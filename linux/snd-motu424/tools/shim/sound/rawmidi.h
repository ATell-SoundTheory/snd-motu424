/* Userspace shim for <sound/rawmidi.h> */
#ifndef KSHIM_RAWMIDI_H
#define KSHIM_RAWMIDI_H

#include "../kshim.h"

#define SNDRV_RAWMIDI_INFO_OUTPUT	1
#define SNDRV_RAWMIDI_INFO_INPUT	2
#define SNDRV_RAWMIDI_INFO_DUPLEX	4
#define SNDRV_RAWMIDI_STREAM_OUTPUT	0
#define SNDRV_RAWMIDI_STREAM_INPUT	1

struct snd_rawmidi;
struct snd_rawmidi_substream;
struct snd_rawmidi_ops {
	int (*open)(struct snd_rawmidi_substream *substream);
	int (*close)(struct snd_rawmidi_substream *substream);
	void (*trigger)(struct snd_rawmidi_substream *substream, int up);
};

struct snd_rawmidi_substream {
	int number;
	int stream;
	struct snd_rawmidi *rmidi;
	const struct snd_rawmidi_ops *ops;
	u8 tx[256], rx[256];
	unsigned int tx_len, rx_len;
};

struct snd_rawmidi {
	char name[80];
	unsigned int info_flags;
	void *private_data;
	const struct snd_rawmidi_ops *ops[2];
	struct snd_rawmidi_substream *substreams[2][4];
};

int snd_rawmidi_new(struct snd_card *card, char *id, int device,
		    int output_count, int input_count,
		    struct snd_rawmidi **rmidi);
void snd_rawmidi_set_ops(struct snd_rawmidi *rmidi, int stream,
			 const struct snd_rawmidi_ops *ops);
int snd_rawmidi_receive(struct snd_rawmidi_substream *substream,
			const unsigned char *buffer, int count);
int snd_rawmidi_transmit(struct snd_rawmidi_substream *substream,
			 unsigned char *buffer, int count);

#endif
