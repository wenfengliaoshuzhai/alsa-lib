/*
 *  MIDI byte <-> sequencer event coder
 *
 *  Copyright (C) 1998,99,2000 Takashi Iwai <tiwai@suse.de>,
 *			       Jaroslav Kysela <perex@suse.cz>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <malloc.h>
#include <errno.h>
#include "local.h"


/* midi status */
struct snd_midi_event {
	int qlen;	/* queue length */
	int read;	/* chars read */
	int type;	/* current event type */
	unsigned char lastcmd;
	int bufsize;
	unsigned char *buf;	/* input buffer */
};


/* queue type */
/* from 0 to 7 are normal commands (note off, on, etc.) */
#define ST_NOTEOFF	0
#define ST_NOTEON	1
#define ST_SPECIAL	8
#define ST_SYSEX	ST_SPECIAL
/* from 8 to 15 are events for 0xf0-0xf7 */


/* status event types */
typedef void (*event_encode_t)(snd_midi_event_t *dev, snd_seq_event_t *ev);
typedef void (*event_decode_t)(snd_seq_event_t *ev, unsigned char *buf);

/*
 * prototypes
 */
static void note_event(snd_midi_event_t *dev, snd_seq_event_t *ev);
static void one_param_ctrl_event(snd_midi_event_t *dev, snd_seq_event_t *ev);
static void pitchbend_ctrl_event(snd_midi_event_t *dev, snd_seq_event_t *ev);
static void two_param_ctrl_event(snd_midi_event_t *dev, snd_seq_event_t *ev);
static void one_param_event(snd_midi_event_t *dev, snd_seq_event_t *ev);
static void songpos_event(snd_midi_event_t *dev, snd_seq_event_t *ev);
static void note_decode(snd_seq_event_t *ev, unsigned char *buf);
static void one_param_decode(snd_seq_event_t *ev, unsigned char *buf);
static void pitchbend_decode(snd_seq_event_t *ev, unsigned char *buf);
static void two_param_decode(snd_seq_event_t *ev, unsigned char *buf);
static void songpos_decode(snd_seq_event_t *ev, unsigned char *buf);

/*
 * event list
 */
static struct status_event_list_t {
	int event;
	int qlen;
	event_encode_t encode;
	event_decode_t decode;
} status_event[] = {
	/* 0x80 - 0xf0 */
	{SND_SEQ_EVENT_NOTEOFF,		2, note_event, note_decode},
	{SND_SEQ_EVENT_NOTEON,		2, note_event, note_decode},
	{SND_SEQ_EVENT_KEYPRESS,	2, note_event, note_decode},
	{SND_SEQ_EVENT_CONTROLLER,	2, two_param_ctrl_event, two_param_decode},
	{SND_SEQ_EVENT_PGMCHANGE,	1, one_param_ctrl_event, one_param_decode},
	{SND_SEQ_EVENT_CHANPRESS,	1, one_param_ctrl_event, one_param_decode},
	{SND_SEQ_EVENT_PITCHBEND,	2, pitchbend_ctrl_event, pitchbend_decode},
	{SND_SEQ_EVENT_NONE,		0, NULL, NULL}, /* 0xf0 */
	/* 0xf0 - 0xff */
	{SND_SEQ_EVENT_SYSEX,		1, NULL, NULL}, /* sysex: 0xf0 */
	{SND_SEQ_EVENT_QFRAME,		1, one_param_event, one_param_decode}, /* 0xf1 */
	{SND_SEQ_EVENT_SONGPOS,		2, songpos_event, songpos_decode}, /* 0xf2 */
	{SND_SEQ_EVENT_SONGSEL,		1, one_param_event, one_param_decode}, /* 0xf3 */
	{SND_SEQ_EVENT_NONE,		0, NULL, NULL}, /* 0xf4 */
	{SND_SEQ_EVENT_NONE,		0, NULL, NULL}, /* 0xf5 */
	{SND_SEQ_EVENT_TUNE_REQUEST,	0, NULL, NULL},	/* 0xf6 */
	{SND_SEQ_EVENT_NONE,		0, NULL, NULL}, /* 0xf7 */
	{SND_SEQ_EVENT_CLOCK,		0, NULL, NULL}, /* 0xf8 */
	{SND_SEQ_EVENT_NONE,		0, NULL, NULL}, /* 0xf9 */
	{SND_SEQ_EVENT_START,		0, NULL, NULL}, /* 0xfa */
	{SND_SEQ_EVENT_CONTINUE,	0, NULL, NULL}, /* 0xfb */
	{SND_SEQ_EVENT_STOP, 		0, NULL, NULL}, /* 0xfc */
	{SND_SEQ_EVENT_NONE, 		0, NULL, NULL}, /* 0xfd */
	{SND_SEQ_EVENT_SENSING, 	0, NULL, NULL}, /* 0xfe */
	{SND_SEQ_EVENT_RESET, 		0, NULL, NULL}, /* 0xff */
};

static int extra_decode_ctrl14(snd_midi_event_t *dev, unsigned char *buf, int len, snd_seq_event_t *ev);

static struct extra_event_list_t {
	int event;
	int (*decode)(snd_midi_event_t *dev, unsigned char *buf, int len, snd_seq_event_t *ev);
} extra_event[] = {
	{SND_SEQ_EVENT_CONTROL14, extra_decode_ctrl14},
	/*{SND_SEQ_EVENT_NONREGPARAM, extra_decode_nrpn},*/
	/*{SND_SEQ_EVENT_REGPARAM, extra_decode_rpn},*/
};

#define numberof(ary)	(sizeof(ary)/sizeof(ary[0]))

/*
 *  new/delete record
 */

int snd_midi_event_new(int bufsize, snd_midi_event_t **rdev)
{
	snd_midi_event_t *dev;

	*rdev = NULL;
	dev = (snd_midi_event_t *)malloc(sizeof(snd_midi_event_t));
	if (dev == NULL)
		return -ENOMEM;
	if (bufsize > 0) {
		dev->buf = malloc(bufsize);
		if (dev->buf == NULL) {
			free(dev);
			return -ENOMEM;
		}
	}
	dev->bufsize = bufsize;
	*rdev = dev;
	return 0;
}

void snd_midi_event_free(snd_midi_event_t *dev)
{
	if (dev != NULL) {
		if (dev->buf)
			free(dev->buf);
		free(dev);
	}
}

/*
 * initialize record
 */
inline static void reset_encode(snd_midi_event_t *dev)
{
	dev->read = 0;
	dev->qlen = 0;
	dev->type = 0;
}

void snd_midi_event_reset_encode(snd_midi_event_t *dev)
{
	reset_encode(dev);
}

void snd_midi_event_reset_decode(snd_midi_event_t *dev)
{
	dev->lastcmd = 0xff;
}

void snd_midi_event_init(snd_midi_event_t *dev)
{
	snd_midi_event_reset_encode(dev);
	snd_midi_event_reset_decode(dev);
}

/*
 * resize buffer
 */
int snd_midi_event_resize_buffer(snd_midi_event_t *dev, int bufsize)
{
	unsigned char *new_buf, *old_buf;

	if (bufsize == dev->bufsize)
		return 0;
	new_buf = malloc(bufsize);
	if (new_buf == NULL)
		return -ENOMEM;
	old_buf = dev->buf;
	dev->buf = new_buf;
	dev->bufsize = bufsize;
	reset_encode(dev);
	if (old_buf)
		free(old_buf);
	return 0;
}

/*
 *  read bytes and encode to sequencer event if finished
 *  return the size of encoded bytes
 */
long snd_midi_event_encode(snd_midi_event_t *dev, unsigned char *buf, long count, snd_seq_event_t *ev)
{
	long result = 0;
	int rc;

	ev->type = SND_SEQ_EVENT_NONE;

	while (count-- > 0) {
		rc = snd_midi_event_encode_byte(dev, *buf++, ev);
		result++;
		if (rc < 0)
			return rc;
		else if (rc > 0)
			return result;
	}

	return result;
}

/*
 *  read one byte and encode to sequencer event:
 *  return 1 if MIDI bytes are encoded to an event
 *         0 data is not finished
 *         negative for error
 */
int snd_midi_event_encode_byte(snd_midi_event_t *dev, int c, snd_seq_event_t *ev)
{
	int rc = 0;

	c &= 0xff;

	if (c >= MIDI_CMD_COMMON_CLOCK) {
		/* real-time event */
		ev->type = status_event[ST_SPECIAL + c - 0xf0].event;
		ev->flags &= ~SND_SEQ_EVENT_LENGTH_MASK;
		ev->flags |= SND_SEQ_EVENT_LENGTH_FIXED;
		return 1;
	}

	if (dev->qlen > 0) {
		/* rest of command */
		dev->buf[dev->read++] = c;
		if (dev->type != ST_SYSEX)
			dev->qlen--;
	} else {
		/* new command */
		dev->read = 1;
		if (c & 0x80) {
			dev->buf[0] = c;
			if ((c & 0xf0) == 0xf0) /* special events */
				dev->type = (c & 0x0f) + ST_SPECIAL;
			else
				dev->type = (c >> 4) & 0x07;
			dev->qlen = status_event[dev->type].qlen;
		} else {
			/* process this byte as argument */
			dev->buf[dev->read++] = c;
			dev->qlen = status_event[dev->type].qlen - 1;
		}
	}
	if (dev->qlen == 0) {
		ev->type = status_event[dev->type].event;
		ev->flags &= ~SND_SEQ_EVENT_LENGTH_MASK;
		ev->flags |= SND_SEQ_EVENT_LENGTH_FIXED;
		if (status_event[dev->type].encode) /* set data values */
			status_event[dev->type].encode(dev, ev);
		rc = 1;
	} else 	if (dev->type == ST_SYSEX) {
		if (c == MIDI_CMD_COMMON_SYSEX_END ||
		    dev->read >= dev->bufsize) {
			ev->flags &= ~SND_SEQ_EVENT_LENGTH_MASK;
			ev->flags |= SND_SEQ_EVENT_LENGTH_VARIABLE;
			ev->data.ext.len = dev->read;
			ev->data.ext.ptr = dev->buf;
			if (c != MIDI_CMD_COMMON_SYSEX_END)
				dev->read = 0; /* continue to parse */
			else
				reset_encode(dev); /* all parsed */
			rc = 1;
		}
	}

	return rc;
}

/* encode note event */
static void note_event(snd_midi_event_t *dev, snd_seq_event_t *ev)
{
	ev->data.note.channel = dev->buf[0] & 0x0f;
	ev->data.note.note = dev->buf[1];
	ev->data.note.velocity = dev->buf[2];
}

/* encode one parameter controls */
static void one_param_ctrl_event(snd_midi_event_t *dev, snd_seq_event_t *ev)
{
	ev->data.control.channel = dev->buf[0] & 0x0f;
	ev->data.control.value = dev->buf[1];
}

/* encode pitch wheel change */
static void pitchbend_ctrl_event(snd_midi_event_t *dev, snd_seq_event_t *ev)
{
	ev->data.control.channel = dev->buf[0] & 0x0f;
	ev->data.control.value = (int)dev->buf[2] * 128 + (int)dev->buf[1] - 8192;
}

/* encode midi control change */
static void two_param_ctrl_event(snd_midi_event_t *dev, snd_seq_event_t *ev)
{
	ev->data.control.channel = dev->buf[0] & 0x0f;
	ev->data.control.param = dev->buf[1];
	ev->data.control.value = dev->buf[2];
}

/* encode one parameter value*/
static void one_param_event(snd_midi_event_t *dev, snd_seq_event_t *ev)
{
	ev->data.control.value = dev->buf[1];
}

/* encode song position */
static void songpos_event(snd_midi_event_t *dev, snd_seq_event_t *ev)
{
	ev->data.control.value = (int)dev->buf[2] * 128 + (int)dev->buf[1];
}

/*
 * decode from a sequencer event to midi bytes
 * return the size of decoded midi events
 */
long snd_midi_event_decode(snd_midi_event_t *dev, unsigned char *buf, long count, snd_seq_event_t *ev)
{
	int cmd;
	long qlen;
	unsigned int type;

	if (ev->type == SND_SEQ_EVENT_NONE)
		return -ENOENT;

	for (type = 0; type < numberof(status_event); type++) {
		if (ev->type == status_event[type].event)
			goto __found;
	}
	for (type = 0; type < numberof(extra_event); type++) {
		if (ev->type == extra_event[type].event)
			return extra_event[type].decode(dev, buf, count, ev);
	}
	return -ENOENT;

      __found:
	if (type >= ST_SPECIAL)
		cmd = 0xf0 + (type - ST_SPECIAL);
	else
		/* data.note.channel and data.control.channel is identical */
		cmd = 0x80 | (type << 4) | (ev->data.note.channel & 0x0f);


	if (cmd == MIDI_CMD_COMMON_SYSEX) {
		qlen = ev->data.ext.len;
		if (count < qlen)
			return -ENOMEM;
		switch (ev->flags & SND_SEQ_EVENT_LENGTH_MASK) {
		case SND_SEQ_EVENT_LENGTH_FIXED:
		case SND_SEQ_EVENT_LENGTH_VARIPC:
			return -EINVAL;	/* invalid event */
		}
		memcpy(dev->buf, ev->data.ext.ptr, qlen);
		return qlen;
	} else {
		unsigned char xbuf[4];

		if ((cmd & 0xf0) == 0xf0 || dev->lastcmd != cmd) {
			dev->lastcmd = cmd;
			xbuf[0] = cmd;
			if (status_event[type].decode)
				status_event[type].decode(ev, xbuf + 1);
			qlen = status_event[type].qlen + 1;
		} else {
			if (status_event[type].decode)
				status_event[type].decode(ev, xbuf + 0);
			qlen = status_event[type].qlen;
		}
		if (count < qlen)
			return -ENOMEM;
		memcpy(buf, xbuf, qlen);
		return qlen;
	}
}


/* decode note event */
static void note_decode(snd_seq_event_t *ev, unsigned char *buf)
{
	buf[0] = ev->data.note.note & 0x7f;
	buf[1] = ev->data.note.velocity & 0x7f;
}

/* decode one parameter controls */
static void one_param_decode(snd_seq_event_t *ev, unsigned char *buf)
{
	buf[0] = ev->data.control.value & 0x7f;
}

/* decode pitch wheel change */
static void pitchbend_decode(snd_seq_event_t *ev, unsigned char *buf)
{
	int value = ev->data.control.value + 8192;
	buf[0] = value & 0x7f;
	buf[1] = (value >> 7) & 0x7f;
}

/* decode midi control change */
static void two_param_decode(snd_seq_event_t *ev, unsigned char *buf)
{
	buf[0] = ev->data.control.param & 0x7f;
	buf[1] = ev->data.control.value & 0x7f;
}

/* decode song position */
static void songpos_decode(snd_seq_event_t *ev, unsigned char *buf)
{
	buf[0] = ev->data.control.value & 0x7f;
	buf[1] = (ev->data.control.value >> 7) & 0x7f;
}

/* decode 14bit control */
static int extra_decode_ctrl14(snd_midi_event_t *dev, unsigned char *buf, int count, snd_seq_event_t *ev)
{
	if (ev->data.control.param < 32) {
		if (count < 5)
			return -ENOMEM;
		buf[0] = MIDI_CMD_CONTROL|(ev->data.control.channel & 0x0f);
		buf[1] = ev->data.control.param;
		buf[2] = (ev->data.control.value >> 7) & 0x7f;
		buf[3] = ev->data.control.param + 32;
		buf[4] = ev->data.control.value & 0x7f;
		dev->lastcmd = buf[0];
		return 5;
	} else {
		if (count < 3)
			return -ENOMEM;
		buf[0] = MIDI_CMD_CONTROL|(ev->data.control.channel & 0x0f);
		buf[1] = ev->data.control.param & 0x7f;
		buf[4] = ev->data.control.value & 0x7f;
		dev->lastcmd = buf[0];
		return 3;
	}
}
