// SPDX-License-Identifier: GPL-2.0-only
/* Userspace tests for the ALSA rawMIDI callbacks and per-port queues. */
#include "motu424.h"

unsigned int kshim_warnings;
int kshim_verbose;

static struct snd_rawmidi *fake_rawmidi;

int snd_rawmidi_new(struct snd_card *card, char *id, int device,
		    int output_count, int input_count,
		    struct snd_rawmidi **rmidi)
{
	struct snd_rawmidi *r;
	int stream, port, count[2] = { output_count, input_count };

	(void)card;
	(void)id;
	(void)device;
	r = calloc(1, sizeof(*r));
	if (!r)
		return -ENOMEM;
	for (stream = 0; stream < 2; stream++)
		for (port = 0; port < count[stream]; port++) {
			r->substreams[stream][port] =
				calloc(1, sizeof(struct snd_rawmidi_substream));
			if (!r->substreams[stream][port])
				return -ENOMEM;
			r->substreams[stream][port]->rmidi = r;
			r->substreams[stream][port]->number = port;
			r->substreams[stream][port]->stream = stream;
		}
	fake_rawmidi = r;
	*rmidi = r;
	return 0;
}

void snd_rawmidi_set_ops(struct snd_rawmidi *rmidi, int stream,
			 const struct snd_rawmidi_ops *ops)
{
	rmidi->ops[stream] = ops;
	for (int port = 0; port < MOTU424_MAX_WIRES; port++)
		rmidi->substreams[stream][port]->ops = ops;
}

int snd_rawmidi_receive(struct snd_rawmidi_substream *substream,
			const unsigned char *buffer, int count)
{
	if (count < 0 || substream->rx_len + count > sizeof(substream->rx))
		return -ENOMEM;
	memcpy(substream->rx + substream->rx_len, buffer, count);
	substream->rx_len += count;
	return count;
}

int snd_rawmidi_transmit(struct snd_rawmidi_substream *substream,
			 unsigned char *buffer, int count)
{
	int n = substream->tx_len < (unsigned int)count ?
		substream->tx_len : count;

	memcpy(buffer, substream->tx, n);
	memmove(substream->tx, substream->tx + n, substream->tx_len - n);
	substream->tx_len -= n;
	return n;
}

#define CHECK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	exit(1); } } while (0)

int main(void)
{
	struct motu424 m = { 0 };
	struct snd_card card = { 0 };
	struct pci_dev pci = { 0 };
	struct snd_rawmidi_substream *input, *output;
	u8 byte = 0x90;

	m.card = &card;
	m.pci = &pci;
	CHECK(motu424_rawmidi_create(&m) == 0);
	CHECK(m.rawmidi == fake_rawmidi);
	CHECK(!strcmp(fake_rawmidi->name, "AudioWire MIDI"));
	CHECK(fake_rawmidi->info_flags == (SNDRV_RAWMIDI_INFO_OUTPUT |
					 SNDRV_RAWMIDI_INFO_INPUT |
					 SNDRV_RAWMIDI_INFO_DUPLEX));
	CHECK(fake_rawmidi->ops[SNDRV_RAWMIDI_STREAM_INPUT] ==
	      fake_rawmidi->ops[SNDRV_RAWMIDI_STREAM_OUTPUT]);

	input = fake_rawmidi->substreams[SNDRV_RAWMIDI_STREAM_INPUT][2];
	output = fake_rawmidi->substreams[SNDRV_RAWMIDI_STREAM_OUTPUT][2];
	CHECK(input->ops->open(input) == 0);
	CHECK(output->ops->open(output) == 0);
	motu424_rawmidi_receive(&m, 2, 0x91);
	CHECK(input->rx_len == 0);
	input->ops->trigger(input, 1);
	motu424_rawmidi_receive(&m, 2, 0x91);
	CHECK(input->rx_len == 1 && input->rx[0] == 0x91);
	input->ops->trigger(input, 0);
	motu424_rawmidi_receive(&m, 2, 0x92);
	CHECK(input->rx_len == 1);

	output->tx[0] = byte;
	output->tx_len = 1;
	CHECK(!motu424_rawmidi_transmit(&m, 2, &byte));
	output->ops->trigger(output, 1);
	CHECK(motu424_rawmidi_transmit(&m, 2, &byte) && byte == 0x90);
	CHECK(!motu424_rawmidi_transmit(&m, 2, &byte));
	CHECK(input->ops->close(input) == 0);
	CHECK(output->ops->close(output) == 0);
	CHECK(!m.midi_in[2] && !m.midi_out[2]);
	puts("rawmidi test: ok");
	return 0;
}
