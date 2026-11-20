// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace harness for motu424_mixer.c.
 *
 *   cc -std=gnu11 -Wall -Wextra -I tools/shim -I . \
 *      tools/mixer_test.c motu424_mixer.c motu424_qmix.c -o tools/mixer_test
 *   ./tools/mixer_test [n_dma_pairs] [-q] [-x]
 *
 * Prints the default mixer program (DMA pair N -> output pair N, unity),
 * then simulates the download against a fake DSP and logs every access.
 * -x additionally exercises routings, crossfades, metering and split pairs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "motu424_mixer.h"
#include "motu424_qmix.h"

#define FAKE_CB		0x01800000u	/* arbitrary control-block address */
#define FAKE_BANK0	0x00020000u
#define FAKE_BANK1	0x00024000u
#define FAKE_INPUT	0x00010000u
#define FAKE_GAIN	0x00010100u
#define FAKE_INC	0x00010700u
#define FAKE_TRIM	0x00011000u
#define FAKE_METERS	0x00012000u
#define FAKE_METER_INPUT	0x00013000u

struct fake_dsp {
	int quiet;
	unsigned long nwrites;
	u32 last_addr;
	u32 last_value;
	bool dma_stuck;
	bool dma_stuck_after_switch;
	bool fade_stuck;
	bool fade_stuck_after_switch;
	bool volume_stuck;
	bool volume_stuck_after_start;
	bool trim_stuck;
	bool trim_stuck_after_start;
	bool meter_stuck, meter_stuck_after_start;
	unsigned int warnings;
	unsigned int switches, volume_starts;
	unsigned int trim_writes, trim_starts;
	u32 gains[MOTU424_MIX_ROUTINGS * 2];
	u32 increments[MOTU424_MIX_ROUTINGS];
	u32 trim_deltas[MOTU424_TRIM_FACTORS / 2];
	u8 trim_values[MOTU424_TRIM_FACTORS];
	u32 meters[2][MOTU424_METER_VALUES / 2];
	u32 meter_inputs[2][MOTU424_METER_INPUTS / 4];
	unsigned int meter_select, meter_swaps;
};

static unsigned int fake_decode_meter_input(unsigned int encoded)
{
	unsigned int input;

	for (input = 0; input < 96; input++)
		if (input + input / 24 == encoded)
			return input;
	assert(0 && "invalid fake meter input");
	return 0;
}

static void fake_collect_meters(struct fake_dsp *f)
{
	unsigned int meter_bank = f->meter_select & 1;
	unsigned int input_bank = (f->meter_select >> 1) & 1;
	unsigned int i;

	for (i = 0; i < MOTU424_METER_INPUTS; i += 2) {
		unsigned int word_index = i / 4;
		unsigned int shift_left = 24 - 8 * (i % 4);
		unsigned int shift_right = shift_left - 8;
		u32 word = f->meter_inputs[input_bank][word_index];
		unsigned int left = fake_decode_meter_input((word >> shift_left) & 0xff);
		unsigned int right = fake_decode_meter_input((word >> shift_right) & 0xff);
		unsigned int left_peak = left < 48 ? left + 1 : left + 53;
		unsigned int right_peak = right < 48 ? right + 1 : right + 53;

		f->meters[meter_bank][i / 2] =
			left_peak << 16 | right_peak;
	}
	f->meters[meter_bank][24] = 49 << 16 | 50;
	f->meters[meter_bank][25] = 51 << 16 | 52;
}

static void fake_trim_complete(struct fake_dsp *f)
{
	unsigned int i;

	for (i = 0; i < MOTU424_TRIM_FACTORS / 2; i++) {
		int left = f->trim_values[2 * i] +
			   (s16)(f->trim_deltas[i] >> 16);
		int right = f->trim_values[2 * i + 1] +
			    (s16)f->trim_deltas[i];

		assert(left >= 0 && left <= 0xff);
		assert(right >= 0 && right <= 0xff);
		f->trim_values[2 * i] = left;
		f->trim_values[2 * i + 1] = right;
	}
	f->trim_stuck = false;
}

static u32 fake_read(void *ctx, u32 addr)
{
	struct fake_dsp *f = ctx;
	u32 v = 0;

	if (addr >= FAKE_GAIN && addr < FAKE_GAIN + sizeof(f->gains))
		return f->gains[(addr - FAKE_GAIN) / 4];
	if (addr >= FAKE_INC && addr < FAKE_INC + sizeof(f->increments))
		return f->increments[(addr - FAKE_INC) / 4];
	if (addr >= FAKE_METERS && addr < FAKE_METERS + sizeof(f->meters))
		return f->meters[(addr - FAKE_METERS) / 104][((addr - FAKE_METERS) % 104) / 4];
	switch (addr - FAKE_CB) {
	case MOTU424_CB_FADER_INPUT_TBL: v = FAKE_INPUT; break;
	case MOTU424_CB_FADER_GAIN_TBL:	v = FAKE_GAIN; break;
	case MOTU424_CB_FADER_INC_TBL:	v = FAKE_INC; break;
	case MOTU424_CB_TRIM_TBL:	v = FAKE_TRIM; break;
	case MOTU424_CB_MIXER_BANK0:	v = FAKE_BANK0; break;
	case MOTU424_CB_MIXER_BANK1:	v = FAKE_BANK1; break;
	case MOTU424_CB_DMA_BYTES:	v = f->dma_stuck; break;
	case MOTU424_CB_ROUTE_FADE_DONE: v = !f->fade_stuck; break;
	case MOTU424_CB_VOLUME_FADE:	v = f->volume_stuck; break;
	case MOTU424_CB_TRIM_UPDATE:	v = f->trim_stuck; break;
	case MOTU424_CB_METER_BANK0:	v = FAKE_METERS; break;
	case MOTU424_CB_METER_BANK0 + 4:	v = FAKE_METERS + 104; break;
	case MOTU424_CB_METER_INPUT_BANK0: v = FAKE_METER_INPUT; break;
	case MOTU424_CB_METER_INPUT_BANK0 + 4: v = FAKE_METER_INPUT + 48; break;
	case MOTU424_CB_METER_BUSY:	v = f->meter_stuck; break;
	}
	if (!f->quiet)
		printf("  R %08x -> %08x\n", addr, v);
	return v;
}

static void fake_write(void *ctx, u32 addr, u32 val)
{
	struct fake_dsp *f = ctx;
	unsigned int i;

	f->nwrites++;
	f->last_addr = addr;
	f->last_value = val;
	if (addr >= FAKE_GAIN && addr < FAKE_GAIN + sizeof(f->gains))
		f->gains[(addr - FAKE_GAIN) / 4] = val;
	else if (addr >= FAKE_INC && addr < FAKE_INC + sizeof(f->increments))
		f->increments[(addr - FAKE_INC) / 4] = val;
	else if (addr >= FAKE_TRIM && addr < FAKE_TRIM + sizeof(f->trim_deltas)) {
		f->trim_deltas[(addr - FAKE_TRIM) / 4] = val;
		f->trim_writes++;
	} else if (addr == FAKE_CB + MOTU424_CB_TRIM_UPDATE && val) {
		f->trim_starts++;
		if (f->trim_stuck_after_start) {
			f->trim_stuck = true;
		} else {
			fake_trim_complete(f);
		}
	} else if (addr >= FAKE_METERS && addr < FAKE_METERS + sizeof(f->meters)) {
		f->meters[(addr - FAKE_METERS) / 104][((addr - FAKE_METERS) % 104) / 4] = val;
	} else if (addr >= FAKE_METER_INPUT &&
		   addr < FAKE_METER_INPUT + sizeof(f->meter_inputs)) {
		f->meter_inputs[(addr - FAKE_METER_INPUT) / 48]
			       [((addr - FAKE_METER_INPUT) % 48) / 4] = val;
	} else if (addr == FAKE_CB + MOTU424_CB_METER_SELECT) {
		fake_collect_meters(f);
		f->meter_select = val;
		f->meter_swaps++;
		f->meter_stuck = f->meter_stuck_after_start;
	} else if (addr == FAKE_CB + MOTU424_CB_MIXER_SWITCH && val) {
		f->switches++;
		if (f->fade_stuck_after_switch)
			f->fade_stuck = true;
		if (f->dma_stuck_after_switch)
			f->dma_stuck = true;
	} else if (addr == FAKE_CB + MOTU424_CB_VOLUME_FADE && val) {
		f->volume_starts++;
		if (f->volume_stuck_after_start) {
			f->volume_stuck = true;
		} else {
			for (i = 0; i < MOTU424_MIX_ROUTINGS; i++) {
				s16 l = f->increments[i] >> 16;
				s16 r = f->increments[i] & 0xffff;

				f->gains[2 * i] += (u32)((s32)l * 240 * 1024);
				f->gains[2 * i + 1] += (u32)((s32)r * 240 * 1024);
			}
		}
	}
	if (!f->quiet)
		printf("  W %08x <- %08x\n", addr, val);
}

static void fake_warn(void *ctx, const char *message)
{
	struct fake_dsp *f = ctx;

	assert(message && *message);
	f->warnings++;
}

static const struct motu424_dsp_ops fake_ops = {
	.read = fake_read,
	.write = fake_write,
	.delay_us = NULL,
	.warn = fake_warn,
};

static void dump(const char *title, const u32 *w, int n)
{
	int i;

	printf("== %s: %d words\n", title, n);
	for (i = 0; i < n; i++)
		printf("%08x%s", w[i], (i % 8 == 7 || i == n - 1) ? "\n" : " ");
}

static int gen(const struct motu424_mixer_state *st, u32 *buf, const char *t)
{
	int n = motu424_mixer_generate(st, buf, MOTU424_MIX_BUF_WORDS);

	if (n < 0) {
		fprintf(stderr, "%s: generate failed: %d\n", t, n);
		exit(1);
	}
	dump(t, buf, n);
	return n;
}

static void test_fader_helpers(void)
{
	static const struct {
		int input;
		u8 encoded;
	} mappings[] = {
		{ 6, 6 }, { 7, 7 }, { 23, 23 }, { 24, 25 },
		{ 47, 48 }, { 48, 50 }, { 61, 63 }, { 72, 75 }, { 95, 98 },
	};
	struct fake_dsp fake = { .quiet = 1 };
	struct motu424_dsp dsp = {
		.ops = &fake_ops,
		.ctx = &fake,
		.cb = FAKE_CB,
		.rate_mult = 1,
	};
	unsigned int i;
	u32 left, right;

	motu424_dsp_read_tables(&dsp);
	motu424_compute_lr_gain(0x8000, 0x8000, 0x40, &left, &right);
	assert(left == 0x60000000 && right == 0x60000000);
	motu424_compute_lr_gain(0x8000, 0x8000, 0x00, &left, &right);
	assert(left == 0x80000000 && right == 0);
	motu424_compute_lr_gain(0x8000, 0x8000, 0x80, &left, &right);
	assert(left == 0 && right == 0x80000000);
	motu424_compute_lr_gain(0, 0x8000, 0x40, &left, &right);
	assert(left == 0 && right == 0);

	motu424_fader_input_init(&dsp);
	fake.nwrites = 0;
	motu424_fader_set_immediately(&dsp, 5, 0x8000, 0x8000, 0x40);
	assert(fake.nwrites == 2);
	assert(fake.last_addr == 0x00010100 + 8 * 5 + 4);
	assert(fake.last_value == 0x60000000);

	fake.nwrites = 0;
	motu424_fader_set_input(&dsp, 5, 7);
	motu424_fader_write_input_table(&dsp);
	assert(fake.nwrites == 1);
	assert(fake.last_addr == 0x00010000 + 4);
	assert(fake.last_value == 0x00070000);
	motu424_fader_set_input(&dsp, 5, 7);
	motu424_fader_write_input_table(&dsp);
	assert(fake.nwrites == 1);
	for (i = 0; i < sizeof(mappings) / sizeof(mappings[0]); i++) {
		motu424_fader_set_input(&dsp, i, mappings[i].input);
		assert(dsp.fader_input[i ^ 3] == mappings[i].encoded);
	}
}

static void test_input_trim(void)
{
	static struct motu424_qmix cue;
	static struct motu424_mixer_state mixer;
	struct motu424_cue_layout layout = { 0 };
	struct fake_dsp fake = { .quiet = 1 };
	struct motu424_dsp dsp = {
		.ops = &fake_ops,
		.ctx = &fake,
		.cb = FAKE_CB,
		.rate_mult = 1,
		.is_424b = true,
	};
	unsigned int starts;
	long value;

	memset(fake.trim_values, MOTU424_TRIM_UNITY, sizeof(fake.trim_values));
	motu424_dsp_read_tables(&dsp);
	motu424_mixer_init(&mixer);
	motu424_qmix_init(&cue, &mixer, &dsp);
	assert(motu424_qmix_get(&cue, MOTU424_CUE_INPUT_TRIM, 0, 0, &value) == 0 &&
	       value == 0x40);
	assert(motu424_qmix_get(&cue, MOTU424_CUE_INPUT_SWITCH, 0, 0, &value) == 0 &&
	       value == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 0, 0x3f,
				  &layout) == -EINVAL);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 0, 0x100,
				  &layout) == -EINVAL);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_SWITCH, 0, 96, 0,
				  &layout) == -EINVAL);

	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 0, 0x90,
				  &layout) == 1);
	assert(fake.trim_writes == 50 && fake.trim_starts == 1 &&
	       fake.trim_deltas[0] == 0x00500000 && fake.trim_values[0] == 0x90);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 1, 0xff,
				  &layout) == 1);
	assert(fake.trim_deltas[0] == 0x000000bf && fake.trim_values[1] == 0xff);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 24, 0x44,
				  &layout) == 1);
	assert(fake.trim_deltas[12] == 0x00000004 &&
	       fake.trim_values[24] == 0x40 && fake.trim_values[25] == 0x44);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 95, 0x50,
				  &layout) == 1);
	assert(fake.trim_deltas[49] == 0x00100000 &&
	       fake.trim_values[98] == 0x50 && fake.trim_values[99] == 0x40);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_SWITCH, 0, 0, 0,
				  &layout) == 1);
	assert(fake.trim_deltas[0] == 0xff700000 && fake.trim_values[0] == 0);
	starts = fake.trim_starts;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 0, 0x80,
				  &layout) == 1);
	assert(fake.trim_starts == starts && cue.inputs[0].trim == 0x80);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_SWITCH, 0, 0, 1,
				  &layout) == 1);
	assert(fake.trim_values[0] == 0x80);

	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 12, 0xb0,
				  &layout) == 1);
	layout.inputs[0] = layout.inputs[24] = 1;
	assert(motu424_qmix_set_faders(&cue, 90, true) == 0);
	assert(motu424_qmix_update_trim(&cue, &layout) == 0);
	assert(fake.trim_values[0] == 0x80 && fake.trim_values[12] == 0x80 &&
	       fake.trim_values[25] == 0x44 && fake.trim_values[37] == 0x44);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 0, 0xa0,
				  &layout) == 1);
	assert(fake.trim_values[0] == 0xa0 && fake.trim_values[12] == 0xa0);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_SWITCH, 0, 0, 0,
				  &layout) == 1);
	assert(fake.trim_values[0] == 0 && fake.trim_values[12] == 0);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_SWITCH, 0, 0, 1,
				  &layout) == 1);
	assert(fake.trim_values[0] == 0xa0 && fake.trim_values[12] == 0xa0);
	starts = fake.trim_starts;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 12, 0xc0,
				  &layout) == 1);
	assert(fake.trim_starts == starts && fake.trim_values[12] == 0xa0);
	layout.inputs[0] = 0;
	assert(motu424_qmix_update_trim(&cue, &layout) == 0);
	assert(fake.trim_values[12] == 0xc0);
	layout.inputs[0] = 1;
	assert(motu424_qmix_update_trim(&cue, &layout) == 0);
	assert(fake.trim_values[12] == 0xa0);
	assert(motu424_qmix_set_faders(&cue, 180, false) == 0);
	assert(motu424_qmix_update_trim(&cue, &layout) == 0);
	assert(fake.trim_values[12] == 0xc0);

	fake.trim_stuck = true;
	starts = fake.trim_starts;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 0, 0x70,
				  &layout) == -ETIMEDOUT);
	assert(cue.inputs[0].trim == 0xa0 && fake.trim_starts == starts);
	fake.trim_stuck = false;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 0, 0x70,
				  &layout) == 1);
	fake.trim_stuck_after_start = true;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_TRIM, 0, 0, 0x60,
				  &layout) == 1);
	starts = fake.trim_starts;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_SWITCH, 0, 0, 0,
				  &layout) == -ETIMEDOUT);
	assert(cue.inputs[0].trim == 0x60 && !cue.inputs[0].mute &&
	       fake.trim_starts == starts);
	fake.trim_stuck_after_start = false;
	fake_trim_complete(&fake);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_INPUT_SWITCH, 0, 0, 0,
				  &layout) == 1);
	assert(fake.trim_values[0] == 0);
}

static void test_trim_delta_table(void)
{
	struct fake_dsp fake = { .quiet = 1 };
	struct motu424_dsp dsp = {
		.ops = &fake_ops,
		.ctx = &fake,
		.cb = FAKE_CB,
	};
	u8 factors[MOTU424_TRIM_FACTORS];
	unsigned int i;

	memset(fake.trim_values, MOTU424_TRIM_UNITY, sizeof(fake.trim_values));
	motu424_dsp_read_tables(&dsp);
	for (i = 0; i < MOTU424_TRIM_FACTORS; i++)
		factors[i] = i % 2 ? 0 : 0xff;
	assert(motu424_dsp_set_trim(&dsp, factors) == 0);
	assert(fake.trim_deltas[0] == 0x00bfffc0);
	assert(!memcmp(fake.trim_values, factors, sizeof(factors)));
	for (i = 0; i < MOTU424_TRIM_FACTORS; i++)
		factors[i] ^= 0xff;
	assert(motu424_dsp_set_trim(&dsp, factors) == 0);
	assert(fake.trim_deltas[0] == 0xff0100ff);
	assert(!memcmp(fake.trim_values, factors, sizeof(factors)));
}

static void test_stereo_and_talkback(void)
{
	static struct motu424_qmix cue;
	static struct motu424_mixer_state mixer;
	struct motu424_cue_layout layout = { .outputs = 3 };
	struct fake_dsp fake = { .quiet = 1 };
	struct motu424_dsp dsp = { .ops = &fake_ops, .ctx = &fake,
		.cb = FAKE_CB, .rate_mult = 1, .is_424b = true };
	unsigned int f, other;

	motu424_dsp_read_tables(&dsp);
	motu424_fader_input_init(&dsp);
	motu424_mixer_init(&mixer);
	motu424_qmix_init(&cue, &mixer, &dsp);
	layout.inputs[0] = layout.inputs[1] = layout.inputs[2] = 1;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_VOLUME, 0, 0, 16384, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BALANCE, 0, 0, 96, &layout) == 1);
	assert(cue.routes[0][0].volume == 24576 && cue.routes[0][1].volume == 32768);
	assert(cue.routes[0][0].pan == 0 && cue.routes[0][1].pan == 128);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BALANCE, 0, 0, 96, &layout) == 0);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_PAN, 0, 1, 64, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BALANCE, 0, 0, 96, &layout) == 1);
	assert(cue.routes[0][1].pan == 128);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_WIDTH, 0, 0, 64, &layout) == 1);
	assert(cue.routes[0][0].pan == 64 && cue.routes[0][1].pan == 64 &&
	       cue.routes[0][0].volume == 32768);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_WIDTH, 0, 0, 128, &layout) == 1);
	assert(cue.routes[0][0].pan == 128 && cue.routes[0][1].pan == 0);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BALANCE, 0, 0, 0, &layout) == 1);
	assert(cue.routes[0][0].volume == 32768 && !cue.routes[0][1].volume);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BALANCE, 0, 0, 128, &layout) == 1);
	assert(!cue.routes[0][0].volume && cue.routes[0][1].volume == 32768);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_WIDTH, 0, 1, 64, &layout) == -EINVAL);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_WIDTH, 0, 0, 129, &layout) == -EINVAL);

	assert(motu424_qmix_put(&cue, MOTU424_CUE_VOLUME, 0, 0, 32768, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_PAN, 0, 0, 0, &layout) == 0);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_PAN, 0, 2, 128, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 2, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 0, 0, 1, &layout) == 1);
	other = cue.routes[0][2].fader;
	assert(fake.gains[2 * other + 1] == 0x80000000);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_TALKBACK_INPUT, 0, 0, 0, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_TALKBACK_BUS, 0, 0, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_TALKBACK_DIM, 0, 0, 64, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_TALKBACK_SWITCH, 0, 0, 1, &layout) == 1);
	f = cue.routes[0][0].fader;
	assert(f < MOTU424_MIX_ROUTINGS && (cue.routes[0][0].flags & MOTU424_CUE_MUTE));
	assert(fake.gains[2 * f] == 0x80000000 &&
	       fake.gains[2 * other + 1] == 0x20000000);
	assert(!motu424_qmix_fits(&cue, 1));
	assert(cue.routes[1][0].fader == MOTU424_CUE_NO_FADER);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SOLO_SWITCH, 0, 2, 1, &layout) == 1);
	assert(cue.routes[0][0].fader == f);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_TALKBACK_SWITCH, 0, 0, 0, &layout) == 1);
	assert(cue.routes[0][0].fader == MOTU424_CUE_NO_FADER &&
	       fake.gains[2 * cue.routes[0][2].fader + 1] == 0x80000000);
	assert(motu424_qmix_set_faders(&cue, 1, false) == -EBUSY);
	cue.faders = 1;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_TALKBACK_SWITCH, 0, 0, 1, &layout) == -ENOSPC);
	assert(!cue.talkback.active);
	cue.faders = MOTU424_MIX_ROUTINGS;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_LISTENBACK_INPUT, 0, 0, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_LISTENBACK_BUS, 0, 0, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_LISTENBACK_DIM, 0, 0, 0, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_LISTENBACK_SWITCH, 0, 0, 1, &layout) == 1);
	assert(cue.routes[0][1].fader < MOTU424_MIX_ROUTINGS &&
	       !fake.gains[2 * cue.routes[0][2].fader + 1]);
	layout.inputs[1] = 0;
	assert(motu424_qmix_sync(&cue, &layout) == 0);
	assert(cue.routes[0][1].fader == MOTU424_CUE_NO_FADER);
}

static void test_meters(void)
{
	static struct motu424_qmix cue;
	static struct motu424_mixer_state mixer;
	struct motu424_cue_layout layout = { .outputs = 1 };
	struct fake_dsp fake = { .quiet = 1 };
	struct motu424_dsp dsp = { .ops = &fake_ops, .ctx = &fake,
		.cb = FAKE_CB, .rate_mult = 1, .is_424b = true };
	u16 peaks[MOTU424_METER_VALUES];
	unsigned int writes;

	motu424_dsp_read_tables(&dsp);
	motu424_mixer_init(&mixer);
	motu424_qmix_init(&cue, &mixer, &dsp);
	memset(layout.inputs, 1, sizeof(layout.inputs));
	assert(motu424_qmix_put(&cue, MOTU424_CUE_METER_BUS, 0, 0, 0, &layout) == 1);
	assert(mixer.metered_output == 0);
	assert(motu424_qmix_read_meters(&cue, &layout, peaks) == -EAGAIN);
	assert(fake.meter_swaps == 1 && fake.meter_select == 3);
	assert(fake.meter_inputs[1][0] == 0x00010203 &&
	       fake.meter_inputs[1][6] == 0x191a1b1c &&
	       fake.meter_inputs[1][11] == 0x2d2e2f30);
	layout.inputs[0] = 0;
	assert(motu424_qmix_read_meters(&cue, &layout, peaks) == 0);
	assert(cue.meter_input_bank == 1);
	assert(fake.meter_inputs[0][0] == 0x32333435 &&
	       fake.meter_inputs[0][11] == 0x5f606162);
	assert(!peaks[0] && peaks[1] == 2 && peaks[47] == 48 &&
	       peaks[48] == 49 && peaks[51] == 52);
	assert(fake.meter_swaps == 2 && fake.meter_select == 0);

	layout.inputs[48] = 0;
	assert(motu424_qmix_read_meters(&cue, &layout, peaks) == 0);
	assert(cue.meter_input_bank == 0);
	assert(fake.meter_inputs[1][0] == 0x00010203 &&
	       fake.meter_inputs[1][11] == 0x2d2e2f30);
	assert(!peaks[0] && peaks[1] == 102 && peaks[47] == 148 &&
	       peaks[48] == 49 && peaks[51] == 52);
	assert(fake.meter_swaps == 3 && fake.meter_select == 3);

	assert(motu424_qmix_put(&cue, MOTU424_CUE_METER_INPUT_BANK, 0, 0, 1, &layout) == 1);
	assert(motu424_qmix_read_meters(&cue, &layout, peaks) == -EAGAIN);
	assert(fake.meter_inputs[0][0] == 0x32333435 &&
	       fake.meter_inputs[0][11] == 0x5f606162);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_METER_BUS, 0, 0, 48, &layout) == 1);
	assert(mixer.metered_output == -1);
	assert(motu424_qmix_read_meters(&cue, &layout, peaks) == -EAGAIN);
	fake.meters[dsp.meter_bank][24] = 0x12345678;
	assert(motu424_qmix_read_meters(&cue, &layout, peaks) == 0);
	assert(!peaks[48] && !peaks[49]);
	fake.meter_stuck = true;
	writes = fake.nwrites;
	assert(motu424_qmix_read_meters(&cue, &layout, peaks) == -ETIMEDOUT);
	assert(fake.nwrites == writes);
	fake.meter_stuck = false;
	fake.meter_stuck_after_start = true;
	assert(motu424_qmix_read_meters(&cue, &layout, peaks) == -ETIMEDOUT);
	assert(dsp.meter_fault && fake.warnings == 1);
	assert(motu424_qmix_read_meters(&cue, &layout, peaks) == -EIO);
}

/* Drv424::SetSampleRateMultiplier budgets; 4x partner faders and pair types */
static void test_qmix_mult(void)
{
	static struct motu424_qmix cue;
	static struct motu424_mixer_state mixer;
	struct motu424_cue_layout layout = {
		.outputs = (1ULL << 0) | (1ULL << 1),
		.dma_pairs = (1ULL << 0) | (1ULL << 1) | (1ULL << 6) | (1ULL << 7),
	};
	struct fake_dsp fake = { .quiet = 1 };
	struct motu424_dsp dsp = {
		.ops = &fake_ops,
		.ctx = &fake,
		.cb = FAKE_CB,
		.rate_mult = 4,
		.is_424b = true,
	};
	unsigned int f;

	assert(motu424_qmix_budget(false, 1) == 180 &&
	       motu424_qmix_budget(false, 2) == 96 &&
	       motu424_qmix_budget(false, 4) == 48 &&
	       motu424_qmix_budget(true, 2) == 180 &&
	       motu424_qmix_budget(true, 4) == 90);

	motu424_dsp_read_tables(&dsp);
	motu424_fader_input_init(&dsp);
	motu424_mixer_init(&mixer);
	motu424_qmix_init(&cue, &mixer, &dsp);
	assert(motu424_qmix_set_faders(&cue, 91, true) == -EINVAL);
	assert(motu424_qmix_set_faders(&cue, 90, true) == 0);
	layout.inputs[3] = layout.inputs[15] = 1;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 1, 3, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 1, 0, 1, &layout) == 1);
	f = cue.routes[1][3].fader;
	assert(f < 90);
	/* the partner fader mixes hw 15 into pair 7 */
	assert(mixer.rt[f].output == 1 && mixer.rt[f + 90].output == 7);
	assert(dsp.fader_input[f ^ 3] == 3 &&
	       dsp.fader_input[(f + 90) ^ 3] == 15);
	/* partner inputs and pairs are not routed on their own */
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 1, 15, 1, &layout) == 1);
	assert(cue.routes[1][15].fader == MOTU424_CUE_NO_FADER);
	assert(motu424_qmix_set_faders(&cue, 48, true) == -EBUSY);
	/* every unmuted route counts against a new budget, even hidden ones */
	assert(motu424_qmix_fits(&cue, 2) && !motu424_qmix_fits(&cue, 1));

	/* a split pair writes slots 0 and 2; pair 1 then has no code */
	layout.outputs = (1ULL << 0);
	layout.split_pairs = (1ULL << 0);
	layout.skip_pairs = (1ULL << 1);
	assert(motu424_qmix_sync(&cue, &layout) == 0);
	assert(mixer.out[0].alt_type == MOTU424_ALT_SPLIT &&
	       mixer.out[1].alt_type == MOTU424_ALT_NONE &&
	       mixer.out[6].alt_type == MOTU424_ALT_STEREO);
	assert(mixer.rt[f].output == -1 && mixer.rt[f + 90].output == -1);

	layout.inputs[27] = layout.inputs[39] = 1;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 27, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 0, 0, 1, &layout) == 1);
	f = cue.routes[0][27].fader;
	assert(f < 90);
	assert(dsp.fader_input[f ^ 3] == 28 &&
	       dsp.fader_input[(f + 90) ^ 3] == 40);

	memset(&layout, 0, sizeof(layout));
	assert(motu424_qmix_sync(&cue, &layout) == 0);
	assert(motu424_qmix_set_faders(&cue, 180, false) == 0);
}

static void test_qmix(void)
{
	static struct motu424_qmix cue;
	static struct motu424_mixer_state mixer;
	static u32 dmabuf[MOTU424_MIX_DMA_CHUNK_WORDS];
	struct motu424_cue_layout layout = { .outputs = 3 };
	struct fake_dsp fake = { .quiet = 1 };
	struct motu424_dsp dsp = {
		.ops = &fake_ops,
		.ctx = &fake,
		.cb = FAKE_CB,
		.rate_mult = 1,
		.is_424b = true,
		.dma_virt = dmabuf,
		.dma_bus = 0x12345000,
	};
	unsigned int bus, i, fader, switches;
	long value;

	motu424_dsp_read_tables(&dsp);
	motu424_fader_input_init(&dsp);
	motu424_mixer_init(&mixer);
	motu424_qmix_init(&cue, &mixer, &dsp);
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++) {
		assert(cue.buses[bus].mute);
		for (i = 0; i < MOTU424_CUE_INPUTS; i++)
			assert((cue.routes[bus][i].flags & MOTU424_CUE_MUTE) &&
			       cue.routes[bus][i].fader == MOTU424_CUE_NO_FADER);
	}
	layout.inputs[0] = layout.inputs[7] = 1;
	assert(motu424_qmix_get(&cue, MOTU424_CUE_SWITCH, 0, 0, &value) == 0);
	assert(value == 0 && cue.buses[0].mute);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 0, 1, &layout) == 1);
	assert(!cue.has_program && cue.routes[0][0].fader == MOTU424_CUE_NO_FADER);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 0, 0, 1, &layout) == 1);
	fader = cue.routes[0][0].fader;
	assert(fader < MOTU424_MIX_ROUTINGS && mixer.rt[fader].state == MOTU424_RT_ON);
	assert(fake.gains[2 * fader] == 0x60000000);
	assert(fake.gains[2 * fader + 1] == 0x60000000);

	switches = fake.switches;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_PAN, 0, 0, 0, &layout) == 1);
	assert(fake.switches == switches && fake.volume_starts == 1);
	assert(fake.gains[2 * fader] == 0x80000000 && fake.gains[2 * fader + 1] == 0);
	assert(fake.increments[fader] == ((u32)(u16)2184 << 16 | (u16)-6553));
	assert(motu424_qmix_put(&cue, MOTU424_CUE_PAN, 0, 0, 0x81, &layout) == -EINVAL);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_VOLUME, 0, 0, -1, &layout) == -EINVAL);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 48, 0, 1, &layout) == -EINVAL);

	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 7, 1, &layout) == 1);
	fader = cue.routes[0][7].fader;
	assert(fader < MOTU424_MIX_ROUTINGS && dsp.fader_input[fader ^ 3] == 7);
	switches = fake.switches;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_VOLUME, 0, 0, 0x4000, &layout) == 1);
	assert(fake.switches == switches);
	assert(fake.gains[2 * cue.routes[0][0].fader] == 0x20000000);
	assert(fake.gains[2 * fader] == 0x18000000 && fake.gains[2 * fader + 1] == 0x18000000);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SOLO_SWITCH, 0, 0, 1, &layout) == 1);
	assert(cue.routes[0][0].fader != MOTU424_CUE_NO_FADER);
	assert(cue.routes[0][7].fader == MOTU424_CUE_NO_FADER);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 0, 0, &layout) == 1);
	assert(cue.routes[0][7].fader == MOTU424_CUE_NO_FADER);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SOLO_SWITCH, 0, 0, 0, &layout) == 1);
	fader = cue.routes[0][7].fader;
	assert(fader != MOTU424_CUE_NO_FADER);
	layout.inputs[7] = 0;
	assert(motu424_qmix_sync(&cue, &layout) == 0);
	assert(cue.routes[0][7].fader == MOTU424_CUE_NO_FADER);
	assert(mixer.rt[fader].state == MOTU424_RT_OFF);
	layout.inputs[7] = 1;
	assert(motu424_qmix_sync(&cue, &layout) == 0);
	assert(cue.routes[0][7].fader == fader);

	dsp.is_424b = false;
	fake.dma_stuck = true;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 0, 1, &layout) == -ETIMEDOUT);
	assert(cue.routes[0][0].flags & MOTU424_CUE_MUTE);
	assert(cue.routes[0][0].fader == MOTU424_CUE_NO_FADER && !cue.fault);
	fake.dma_stuck = false;
	dsp.is_424b = true;

	memset(&fake, 0, sizeof(fake));
	fake.quiet = 1;
	motu424_dsp_read_tables(&dsp);
	motu424_fader_input_init(&dsp);
	motu424_mixer_init(&mixer);
	motu424_qmix_init(&cue, &mixer, &dsp);
	memset(layout.inputs, 1, sizeof(layout.inputs));
	for (i = 0; i < 96; i++)
		assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, i, 1, &layout) == 1);
	for (i = 0; i < 96; i++)
		assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 1, i, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 0, 0, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 1, 0, 1, &layout) == -ENOSPC);
	assert(cue.buses[1].mute && mixer.out[1].nrout == 0);
	for (i = 84; i < 96; i++)
		assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 1, i, 0, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 1, 0, 1, &layout) == 1);
	assert(mixer.out[0].nrout == 96 && mixer.out[1].nrout == 84);
	switches = fake.switches;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 1, 84, 1, &layout) == -ENOSPC);
	assert(cue.routes[1][84].flags & MOTU424_CUE_MUTE && fake.switches == switches);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SOLO_SWITCH, 0, 0, 1, &layout) == 1);
	assert(mixer.out[0].nrout == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 1, 84, 1, &layout) == -ENOSPC);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SOLO_SWITCH, 0, 0, 0, &layout) == 1);
	layout.inputs[95] = 0;
	assert(motu424_qmix_sync(&cue, &layout) == 0);
	assert(mixer.out[0].nrout == 95);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 1, 84, 1, &layout) == -ENOSPC);
	layout.inputs[95] = 1;
	assert(motu424_qmix_sync(&cue, &layout) == 0);
	assert(mixer.out[0].nrout == 96);
	layout.dma_pairs = (1ULL << MOTU424_MIX_PAIRS) - 1;
	assert(motu424_qmix_sync(&cue, &layout) == 0);
	assert(mixer.out[0].dma_enabled && mixer.out[1].dma_enabled &&
	       mixer.out[47].dma_enabled);
	layout.dma_pairs = 0;
	assert(motu424_qmix_sync(&cue, &layout) == 0);
	assert(!mixer.out[0].dma_enabled && !mixer.out[1].dma_enabled);
	fake.fade_stuck = true;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_VOLUME, 0, 0, 0, &layout) == -ETIMEDOUT);
	assert(cue.routes[0][0].volume == 0x8000 && !cue.fault);

	memset(&fake, 0, sizeof(fake));
	fake.quiet = 1;
	fake.fade_stuck_after_switch = true;
	motu424_dsp_read_tables(&dsp);
	motu424_fader_input_init(&dsp);
	motu424_mixer_init(&mixer);
	motu424_qmix_init(&cue, &mixer, &dsp);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 0, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 0, 0, 1, &layout) == -ETIMEDOUT);
	assert(cue.fault && cue.buses[0].mute && fake.warnings);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 0, 0, &layout) == -EIO);

	memset(&fake, 0, sizeof(fake));
	fake.quiet = 1;
	motu424_dsp_read_tables(&dsp);
	motu424_fader_input_init(&dsp);
	motu424_mixer_init(&mixer);
	motu424_qmix_init(&cue, &mixer, &dsp);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 0, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 0, 0, 1, &layout) == 1);
	fake.volume_stuck_after_start = true;
	assert(motu424_qmix_put(&cue, MOTU424_CUE_VOLUME, 0, 0, 0, &layout) == -ETIMEDOUT);
	assert(cue.fault && cue.routes[0][0].volume == 0x8000 && fake.warnings);

	memset(&fake, 0, sizeof(fake));
	fake.quiet = 1;
	fake.dma_stuck_after_switch = true;
	dsp.is_424b = false;
	motu424_dsp_read_tables(&dsp);
	motu424_fader_input_init(&dsp);
	motu424_mixer_init(&mixer);
	motu424_qmix_init(&cue, &mixer, &dsp);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_SWITCH, 0, 0, 1, &layout) == 1);
	assert(motu424_qmix_put(&cue, MOTU424_CUE_BUS_SWITCH, 0, 0, 1, &layout) == -ETIMEDOUT);
	assert(cue.fault && cue.buses[0].mute && fake.warnings && fake.switches == 1);
}

static void test_incomplete_program(void)
{
	static struct motu424_mixer_state mixer;
	static u32 words[MOTU424_MIX_BUF_WORDS];
	unsigned int i;

	motu424_mixer_init_default(&mixer, MOTU424_MIX_PAIRS);
	for (i = 0; i < MOTU424_MIX_ROUTINGS; i++)
		motu424_mixer_connect(&mixer, i, 0, i & 1);
	for (i = 0; i < MOTU424_MIX_PAIRS; i++)
		motu424_mixer_set_alt_type(&mixer, i, MOTU424_ALT_SPLIT);
	motu424_mixer_meter_output(&mixer, 0);
	assert(motu424_mixer_generate(&mixer, words, MOTU424_MIX_BUF_WORDS) == -ENOSPC);
}

int main(int argc, char **argv)
{
	static struct motu424_mixer_state st;
	static u32 buf[MOTU424_MIX_BUF_WORDS];
	static u32 dmabuf[MOTU424_MIX_DMA_CHUNK_WORDS];
	struct fake_dsp fake = { 0 };
	struct motu424_dsp dsp;
	unsigned int pairs = MOTU424_MIX_PAIRS;
	int n, i, extra = 0, ret;

	test_fader_helpers();
	test_input_trim();
	test_trim_delta_table();
	test_stereo_and_talkback();
	test_meters();
	test_qmix();
	test_qmix_mult();
	test_incomplete_program();
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-q"))
			fake.quiet = 1;
		else if (!strcmp(argv[i], "-x"))
			extra = 1;
		else
			pairs = (unsigned int)strtoul(argv[i], NULL, 0);
	}

	motu424_mixer_init_default(&st, pairs);
	n = gen(&st, buf, "default mixer");

	memset(&dsp, 0, sizeof(dsp));
	dsp.ops = &fake_ops;
	dsp.ctx = &fake;
	dsp.cb = FAKE_CB;
	dsp.rate_mult = 1;
	dsp.dma_virt = dmabuf;
	dsp.dma_bus = 0x12345000;
	motu424_dsp_read_tables(&dsp);

	printf("== download (ND424, host DMA)\n");
	ret = motu424_mixer_download(&dsp, buf, n);
	assert(ret == 0 && dsp.bank == 1);
	assert(fake.nwrites == 5UL * ((n + 1023) / 1024) + 4);
	printf("   ret=%d bank=%u writes=%lu\n", ret, dsp.bank, fake.nwrites);

	printf("== download (ND424b, PIO, quiet)\n");
	dsp.is_424b = true;
	fake.quiet++;
	fake.nwrites = 0;
	ret = motu424_mixer_download(&dsp, buf, n);
	assert(ret == 0 && dsp.bank == 0);
	assert(fake.nwrites == (unsigned long)n + 4);
	fake.quiet--;
	printf("   ret=%d bank=%u writes=%lu (expect %d)\n", ret, dsp.bank,
	       fake.nwrites, n + 4);

	fake.quiet = 1;
	fake.dma_stuck = true;
	dsp.is_424b = false;
	fake.nwrites = 0;
	assert(motu424_mixer_download(&dsp, buf, n) == -ETIMEDOUT);
	assert(dsp.bank == 0 && fake.nwrites == 5);
	fake.dma_stuck = false;
	fake.fade_stuck = true;
	assert(motu424_mixer_download(&dsp, buf, n) == 0);
	assert(dsp.bank == 1 && fake.warnings == 1);
	fake.fade_stuck = false;
	fake.nwrites = 0;
	assert(motu424_mixer_download(&dsp, buf, 0) == -EINVAL);
	dsp.rate_mult = 0;
	assert(motu424_mixer_download(&dsp, buf, n) == -EINVAL);
	assert(fake.nwrites == 0);
	dsp.rate_mult = 1;
	assert(motu424_mixer_generate(&st, buf, 1) == -ENOSPC);

	if (extra) {
		/* fader 0 -> pair 0 (fading in), fader 1 + 2 -> pair 1 */
		motu424_mixer_connect(&st, 0, 0, false);
		motu424_mixer_connect(&st, 1, 1, true);
		motu424_mixer_connect(&st, 2, 1, false);
		gen(&st, buf, "routings fading in");
		motu424_mixer_complete_fades(&st);
		motu424_mixer_meter_output(&st, 1);
		motu424_mixer_set_alt_type(&st, 2, MOTU424_ALT_SPLIT);
		motu424_mixer_set_alt_type(&st, 3, MOTU424_ALT_NONE);
		gen(&st, buf, "routings on, pair 1 metered, pair 2 split, pair 3 none");
		motu424_mixer_disconnect(&st, 1);
		motu424_mixer_disable_output_metering(&st);
		gen(&st, buf, "fader 1 fading out");
		motu424_mixer_complete_fades(&st);
		gen(&st, buf, "after complete_fades");

		/* all 180 faders connected: exercises the size limit */
		motu424_mixer_init_default(&st, pairs);
		for (i = 0; i < MOTU424_MIX_ROUTINGS; i++)
			motu424_mixer_connect(&st, i, i % MOTU424_MIX_PAIRS, i & 1);
		motu424_mixer_complete_fades(&st);
		n = gen(&st, buf, "180 routings");
		fake.nwrites = 0;
		assert(motu424_mixer_download(&dsp, buf, n) == 0);
		assert(fake.nwrites == 5UL * ((n + 1023) / 1024) + 4);

		fake.quiet = 0;
		motu424_fader_input_init(&dsp);
		motu424_fader_set_input(&dsp, 5, 7);
		motu424_fader_write_input_table(&dsp);
		motu424_fader_set_immediately(&dsp, 5, 0x8000, 0x8000, 0x40);
	}
	return 0;
}
