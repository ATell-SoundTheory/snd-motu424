// SPDX-License-Identifier: GPL-2.0-only
/*
 * MOTU PCI-424 family: ALSA rawMIDI ports for the four AudioWire links.
 */
#include <linux/bitops.h>
#include "motu424.h"

static int motu424_rawmidi_open(struct snd_rawmidi_substream *substream)
{
	struct motu424 *m = substream->rmidi->private_data;
	unsigned int port = substream->number;
	struct snd_rawmidi_substream **ports;
	unsigned long flags;

	if (port >= MOTU424_MAX_WIRES)
		return -EINVAL;
	ports = substream->stream == SNDRV_RAWMIDI_STREAM_INPUT ?
		m->midi_in : m->midi_out;
	spin_lock_irqsave(&m->lock, flags);
	if (ports[port]) {
		spin_unlock_irqrestore(&m->lock, flags);
		return -EBUSY;
	}
	ports[port] = substream;
	spin_unlock_irqrestore(&m->lock, flags);
	return 0;
}

static int motu424_rawmidi_close(struct snd_rawmidi_substream *substream)
{
	struct motu424 *m = substream->rmidi->private_data;
	unsigned int port = substream->number;
	struct snd_rawmidi_substream **ports;
	unsigned long flags;

	if (port >= MOTU424_MAX_WIRES)
		return -EINVAL;
	if (substream->stream == SNDRV_RAWMIDI_STREAM_INPUT) {
		clear_bit(port, &m->midi_in_active);
		ports = m->midi_in;
	} else {
		clear_bit(port, &m->midi_out_active);
		ports = m->midi_out;
	}
	spin_lock_irqsave(&m->lock, flags);
	if (ports[port] == substream)
		ports[port] = NULL;
	spin_unlock_irqrestore(&m->lock, flags);
	return 0;
}

static void motu424_rawmidi_trigger(struct snd_rawmidi_substream *substream,
				   int up)
{
	struct motu424 *m = substream->rmidi->private_data;
	unsigned int port = substream->number;
	unsigned long *active;

	if (port >= MOTU424_MAX_WIRES)
		return;
	active = substream->stream == SNDRV_RAWMIDI_STREAM_INPUT ?
		&m->midi_in_active : &m->midi_out_active;
	if (up)
		set_bit(port, active);
	else
		clear_bit(port, active);
}

static const struct snd_rawmidi_ops motu424_rawmidi_ops = {
	.open = motu424_rawmidi_open,
	.close = motu424_rawmidi_close,
	.trigger = motu424_rawmidi_trigger,
};

int motu424_rawmidi_create(struct motu424 *m)
{
	struct snd_rawmidi *rmidi;
	int err;

	err = snd_rawmidi_new(m->card, "AudioWire MIDI", 0,
			      MOTU424_MAX_WIRES, MOTU424_MAX_WIRES, &rmidi);
	if (err < 0)
		return err;
	rmidi->private_data = m;
	strscpy(rmidi->name, "AudioWire MIDI", sizeof(rmidi->name));
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT |
			    SNDRV_RAWMIDI_INFO_INPUT |
			    SNDRV_RAWMIDI_INFO_DUPLEX;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT,
			    &motu424_rawmidi_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT,
			    &motu424_rawmidi_ops);
	m->rawmidi = rmidi;
	return 0;
}

void motu424_rawmidi_receive(struct motu424 *m, unsigned int port, u8 byte)
{
	struct snd_rawmidi_substream *substream;

	if (port >= MOTU424_MAX_WIRES ||
	    !test_bit(port, &m->midi_in_active))
		return;
	substream = m->midi_in[port];
	if (substream)
		snd_rawmidi_receive(substream, &byte, 1);
}

bool motu424_rawmidi_transmit(struct motu424 *m, unsigned int port, u8 *byte)
{
	struct snd_rawmidi_substream *substream;

	if (port >= MOTU424_MAX_WIRES ||
	    !test_bit(port, &m->midi_out_active))
		return false;
	substream = m->midi_out[port];
	return substream && snd_rawmidi_transmit(substream, byte, 1) == 1;
}
