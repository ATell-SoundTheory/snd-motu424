/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MOTU424_QMIX_H
#define MOTU424_QMIX_H

#include "motu424_mixer.h"

#define MOTU424_CUE_INPUTS	96
#define MOTU424_CUE_NO_FADER	0xff
#define MOTU424_CUE_SOLO	0x01
#define MOTU424_CUE_MUTE	0x02

enum motu424_cue_control {
	MOTU424_CUE_VOLUME,
	MOTU424_CUE_PAN,
	MOTU424_CUE_SWITCH,
	MOTU424_CUE_SOLO_SWITCH,
	MOTU424_CUE_BUS_VOLUME,
	MOTU424_CUE_BUS_SWITCH,
	MOTU424_CUE_INPUT_TRIM,
	MOTU424_CUE_INPUT_SWITCH,
	MOTU424_CUE_BALANCE,
	MOTU424_CUE_WIDTH,
	MOTU424_CUE_TALKBACK_SWITCH,
	MOTU424_CUE_TALKBACK_INPUT,
	MOTU424_CUE_TALKBACK_DIM,
	MOTU424_CUE_TALKBACK_BUS,
	MOTU424_CUE_LISTENBACK_SWITCH,
	MOTU424_CUE_LISTENBACK_INPUT,
	MOTU424_CUE_LISTENBACK_DIM,
	MOTU424_CUE_LISTENBACK_BUS,
	MOTU424_CUE_METER_INPUT_BANK,
	MOTU424_CUE_METER_BUS,
};

struct motu424_cue_route {
	u16 volume;
	u8 pan;
	u8 flags;
	u8 fader;
};

struct motu424_cue_bus {
	u16 volume;
	bool mute;
};

struct motu424_cue_input {
	u8 trim;
	bool mute;
};

struct motu424_cue_stereo {
	u8 balance, width;
};

struct motu424_cue_talkback {
	unsigned int input;	/* 96 = none */
	unsigned int dim;	/* 0..128, applied to non-mic route volumes */
	bool active;
	u64 buses;
};

struct motu424_cue_layout {
	u8 inputs[MOTU424_CUE_INPUTS];
	u64 outputs;
	u64 dma_pairs;
	u64 split_pairs;	/* MOTU424_ALT_SPLIT */
	u64 skip_pairs;		/* MOTU424_ALT_NONE */
};

/* At 4x, outputs and inputs of a port are positions 0..11; 12..23 are partners. */
#define MOTU424_CUE_QUAD_IN_OFFSET	12
#define MOTU424_CUE_QUAD_PAIR_OFFSET	6

struct motu424_qmix {
	struct motu424_mixer_state *mixer;
	struct motu424_dsp *dsp;
	struct motu424_cue_route routes[MOTU424_MIX_PAIRS][MOTU424_CUE_INPUTS];
	struct motu424_cue_bus buses[MOTU424_MIX_PAIRS];
	struct motu424_cue_input inputs[MOTU424_CUE_INPUTS];
	struct motu424_cue_stereo stereo[MOTU424_MIX_PAIRS][MOTU424_CUE_INPUTS / 2];
	struct motu424_cue_talkback talkback, listenback;
	unsigned int meter_input_bank, meter_bus;	/* bus 48 = disabled */
	bool meter_primed;
	u64 dma_pairs;
	u64 split_pairs, skip_pairs;
	unsigned int faders;	/* fader budget (Drv+0x20ca4) */
	bool quad;		/* 4x: fader f + faders mirrors fader f */
	unsigned int generation;
	bool has_program;
	bool fault;
};

void motu424_qmix_init(struct motu424_qmix *cue,
			 struct motu424_mixer_state *mixer,
			 struct motu424_dsp *dsp);
int motu424_qmix_get(const struct motu424_qmix *cue,
		       enum motu424_cue_control control, unsigned int bus,
		       unsigned int input, long *value);
int motu424_qmix_put(struct motu424_qmix *cue,
		       enum motu424_cue_control control, unsigned int bus,
		       unsigned int input, long value,
		       const struct motu424_cue_layout *layout);
int motu424_qmix_sync(struct motu424_qmix *cue,
			const struct motu424_cue_layout *layout);
int motu424_qmix_update_trim(struct motu424_qmix *cue,
			       const struct motu424_cue_layout *layout);
/* Drv424::SetNumberOfFaders; every route must have been freed first. */
int motu424_qmix_set_faders(struct motu424_qmix *cue, unsigned int faders,
			      bool quad);
/* Drv424::SetSampleRateMultiplier: 180 at 1x; 96/48 (PCI-424) or 180/90 */
unsigned int motu424_qmix_budget(bool is_424b, unsigned int mult);
bool motu424_qmix_fits(const struct motu424_qmix *cue, unsigned int faders);
/* First call primes the DSP selection and returns -EAGAIN; retry after 20 ms. */
int motu424_qmix_read_meters(struct motu424_qmix *cue,
			       const struct motu424_cue_layout *layout,
			       u16 peaks[MOTU424_METER_VALUES]);

#endif
