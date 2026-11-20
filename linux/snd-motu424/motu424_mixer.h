/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MOTU PCI-424 DSP mixer program generator and downloader.
 *
 * Port of the MixerState / Instruction / Scheduler classes and the
 * ND424::DownloadMixer path of the MOTU macOS kext (x86_64 build).
 * Addresses in comments refer to that binary.  See docs/MIXER.md.
 */
#ifndef MOTU424_MIXER_H
#define MOTU424_MIXER_H

#include <linux/types.h>

#define MOTU424_MIX_PAIRS		48	/* stereo output pairs (96 ch) */
#define MOTU424_MIX_ROUTINGS		180	/* one per DSP fader */
#define MOTU424_MIX_BUF_WORDS		0xa00	/* 0x2800-byte buffer, Drv424::DownloadNewMixer */
#define MOTU424_MIX_DMA_CHUNK_WORDS	0x400	/* ND424::DownloadMixerGuts chunk */
#define MOTU424_TRIM_FACTORS		100	/* 24 inputs + 1 pad per wire */
#define MOTU424_TRIM_UNITY		0x40
#define MOTU424_METER_INPUTS		48
#define MOTU424_METER_VALUES		52

/* MixOutput +0x00 "AlternateChannelType" (set by Drv424::SetupRouting) */
enum motu424_alt_type {
	MOTU424_ALT_STEREO = 0,	/* both channels of the pair are adjacent HW channels */
	MOTU424_ALT_SPLIT = 1,	/* pair split: left/right written by two stores */
	MOTU424_ALT_NONE = 2,	/* no code is generated for this pair */
};

/* MixRouting +0x00 */
enum motu424_routing_state {
	MOTU424_RT_OFF = 0,
	MOTU424_RT_ON = 1,
	MOTU424_RT_FADE_IN = 2,
	MOTU424_RT_FADE_OUT = 3,
};

struct motu424_mix_output {
	u32 alt_type;		/* +0x00 enum motu424_alt_type */
	bool dma_enabled;	/* +0x04 host playback data feeds this pair */
	int nrout;		/* +0x08 number of connected routings */
	s16 first;		/* +0x10 first routing (index) or -1 */
	bool metered;		/* +0x18 */
	u8 store_inc[2];	/* +0x19/+0x1a store post-increments (2 or 3) */
};

struct motu424_mix_routing {
	u32 state;		/* +0x00 enum motu424_routing_state */
	bool flag;		/* +0x04 bool passed to MixerState::Connect */
	s16 output;		/* +0x08 output pair or -1 */
	s16 next, prev;		/* +0x10/+0x18 list links (indices) or -1 */
};

/* MixerState (0x1c570 bytes in the original) */
struct motu424_mixer_state {
	struct motu424_mix_output out[MOTU424_MIX_PAIRS];
	struct motu424_mix_routing rt[MOTU424_MIX_ROUTINGS];
	int metered_output;	/* +0x1c568, -1 = none */
};

/* Opaque scratch area for the instruction graph (~100 KiB). */
struct motu424_mixer_work;
size_t motu424_mixer_work_size(void);

/* MixerState::MixerState() @0x23df0: everything off, no DMA, no routings. */
void motu424_mixer_init(struct motu424_mixer_state *st);
/*
 * Default playback state as produced by Drv424::SetupRouting @0x18770 with
 * the identity channel table: pairs [0, n_dma_pairs) are DMA-fed, stereo,
 * routed 1:1 to the same hardware output pair at unity gain.
 */
void motu424_mixer_init_default(struct motu424_mixer_state *st,
				unsigned int n_dma_pairs);

/* Mutators; the bool ones return true if anything changed. */
bool motu424_mixer_enable_dma(struct motu424_mixer_state *st, int pair, bool en);
bool motu424_mixer_set_alt_type(struct motu424_mixer_state *st, int pair,
				enum motu424_alt_type type);
void motu424_mixer_connect(struct motu424_mixer_state *st, int routing,
			   int pair, bool flag);
void motu424_mixer_disconnect(struct motu424_mixer_state *st, int routing);
void motu424_mixer_complete_fades(struct motu424_mixer_state *st);
void motu424_mixer_meter_output(struct motu424_mixer_state *st, int pair);
void motu424_mixer_disable_output_metering(struct motu424_mixer_state *st);

/*
 * MixerState::GenerateMixer @0x24490.  Emits TI C62x machine code into
 * @out and returns the number of words, or -ENOSPC if @out_words is too
 * small or the DSP program limit would leave instructions unscheduled.
 * @work is caller scratch of motu424_mixer_work_size() bytes.
 */
int motu424_mixer_generate_work(const struct motu424_mixer_state *st,
				struct motu424_mixer_work *work,
				u32 *out, size_t out_words);
/* Same, allocating the scratch with kvmalloc (or malloc in userspace). */
int motu424_mixer_generate(const struct motu424_mixer_state *st,
			   u32 *out, size_t out_words);

/* ---- DSP access ---------------------------------------------------- */

/*
 * Addresses are DSP addresses as used by ND424::DSP::Read/Write @0x29df0 /
 * @0x29d80: if (addr >> 23) == 3 the access goes to the register BAR at
 * (addr & 0x7fffff), otherwise to the memory BAR at (addr & 0x3fffff).
 * delay_us may be NULL (then no delays are performed).
 * warn reports a routing-fade timeout without changing the original switch behaviour.
 */
struct motu424_dsp_ops {
	u32 (*read)(void *ctx, u32 addr);
	void (*write)(void *ctx, u32 addr, u32 val);
	void (*delay_us)(void *ctx, unsigned int us);
	void (*warn)(void *ctx, const char *message);
};

struct motu424_dsp {
	const struct motu424_dsp_ops *ops;
	void *ctx;
	u32 cb;			/* ND+0xb8: DSP address of the control block */
	/* DSP table addresses read from the control block at boot (@0x2d350) */
	u32 fader_input_tbl;	/* ND+0xbc = [cb+0x00] */
	u32 fader_gain_tbl;	/* ND+0xc0 = [cb+0x04] */
	u32 fader_inc_tbl;	/* ND+0xc4 = [cb+0x08] */
	u32 tbl_c8;		/* ND+0xc8 = [cb+0x14]: input routing table (96 B) */
	u32 trim_tbl;		/* ND+0xcc = [cb+0x18] */
	unsigned int bank;	/* ND+0xac: program bank, toggled per download */
	unsigned int rate_mult;	/* ND+0x1ec: 1, 2 or 4 (44.1/48k = 1) */
	bool is_424b;		/* ND424b (PIO download) instead of ND424 (DMA) */
	/* ND424 only: 4 KiB host buffer the DSP DMAs from (ND+0x2d0/0x2d8) */
	u32 *dma_virt;
	u32 dma_bus;
	/* Fader input table cache (ND+0x130) and dirty dword range */
	u8 fader_input[MOTU424_MIX_ROUTINGS];
	int fi_dirty_lo, fi_dirty_hi;	/* ND+0x1e4/0x1e8, -1 = clean */
	u8 trim_factors[MOTU424_TRIM_FACTORS];
	unsigned int meter_bank, meter_input_bank;
	bool meter_fault;
};

/* Control-block word offsets (relative to cb) */
#define MOTU424_CB_FADER_INPUT_TBL	0x00	/* -> ND+0xbc */
#define MOTU424_CB_FADER_GAIN_TBL	0x04	/* -> ND+0xc0 */
#define MOTU424_CB_FADER_INC_TBL	0x08	/* -> ND+0xc4 */
#define MOTU424_CB_MIXER_BANK0		0x0c	/* DSP address of program bank 0 */
#define MOTU424_CB_MIXER_BANK1		0x10
#define MOTU424_CB_INPUT_ROUTE_TBL	0x14	/* -> ND+0xc8 */
#define MOTU424_CB_TRIM_TBL		0x18	/* -> ND+0xcc */
#define MOTU424_CB_METER_BANK0		0x1c
#define MOTU424_CB_METER_INPUT_BANK0	0x24
#define MOTU424_CB_METER_BUSY		0x30
#define MOTU424_CB_METER_SELECT		0x7c
#define MOTU424_CB_ROUTE_FADE_STEP	0x70
#define MOTU424_CB_DMA_GO		0x74
#define MOTU424_CB_MIXER_ADDR		0x78
#define MOTU424_CB_DMA_HOST_ADDR	0xf8
#define MOTU424_CB_DMA_DSP_ADDR		0xfc
#define MOTU424_CB_DMA_BYTES		0x100
#define MOTU424_CB_MIXER_SWITCH		0x104
#define MOTU424_CB_ROUTE_FADE_DONE	0x108
#define MOTU424_CB_VOLUME_FADE		0x10c
#define MOTU424_CB_TRIM_UPDATE		0x110

/*
 * ND424::DownloadMixer @0x2c000 (+ DownloadMixerGuts @0x2b5c0 / ND424b
 * @0x2dfc0, WaitForRoutingFadeToComplete @0x2b540).
 * Returns 0, -EINVAL for invalid arguments, or -ETIMEDOUT if a DMA chunk did not complete (the original
 * then silently aborts without switching programs).
 */
int motu424_mixer_download(struct motu424_dsp *dsp, const u32 *words, int n);
int motu424_mixer_wait_routing_fade(struct motu424_dsp *dsp);

/* Fader helpers (ND424::SetFaderImmediately @0x2c440, SetFaderInput @0x2db50,
 * WriteFaderInputTable @0x2c7f0, ComputeLeftAndRightGain @0x2a760). */
void motu424_dsp_read_tables(struct motu424_dsp *dsp);	/* @0x2d32c */
void motu424_compute_lr_gain(u32 gain, u32 trim, u32 pan, u32 *l, u32 *r);
void motu424_fader_set_immediately(struct motu424_dsp *dsp, int fader,
				   u32 gain, u32 trim, u32 pan);
void motu424_fader_input_init(struct motu424_dsp *dsp);
void motu424_fader_set_input(struct motu424_dsp *dsp, int fader, int hw_in);
void motu424_fader_write_input_table(struct motu424_dsp *dsp);
int motu424_fader_wait_volume_fade(struct motu424_dsp *dsp);
bool motu424_fader_set_fade(struct motu424_dsp *dsp, int fader,
			    u32 gain, u32 trim, u32 pan);
void motu424_fader_clear_fade(struct motu424_dsp *dsp, int fader);
void motu424_fader_start_volume_fade(struct motu424_dsp *dsp);
int motu424_dsp_set_trim(struct motu424_dsp *dsp,
			 const u8 factors[MOTU424_TRIM_FACTORS]);
int motu424_dsp_swap_meters(struct motu424_dsp *dsp, const u8 *inputs,
			    u16 peaks[MOTU424_METER_VALUES]);

#endif
