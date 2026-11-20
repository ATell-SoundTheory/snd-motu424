// SPDX-License-Identifier: GPL-2.0-only
/*
 * MOTU PCI-424 family: PCM.
 *
 * The card bus-masters a ring of 3 periods in host memory. The ring is a
 * sequence of 16-frame blocks; every block holds one 12-dword slot per
 * channel (16 x 24-bit big-endian samples) - see motu424.h. ALSA sees a
 * normal interleaved S32_LE buffer; each period is converted to/from the
 * ring when the DSP crosses a period boundary.
 */
#include <linux/delay.h>
#include <linux/gcd.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <sound/pcm_params.h>
#include "motu424.h"
#include "motu424_tables.h"

#define MOTU424_MULT_SETTLE_MS 1000

/*
 * 16 samples of one channel <-> one slot. Within each 3-dword group the four
 * 24-bit samples form a big-endian byte stream and each dword is stored
 * little-endian (PS::clipOutputSamples 0x3b500 / PS::ConvertInputSamples
 * 0x388f0).
 */
void motu424_pack16(__le32 *slot, const s32 *src, unsigned int stride)
{
	int g;

	for (g = 0; g < 4; g++, src += 4 * stride, slot += 3) {
		u32 a = src[0], b = src[stride], c = src[2 * stride],
		    d = src[3 * stride];

		slot[0] = cpu_to_le32((a & 0xffffff00) | (b >> 24));
		slot[1] = cpu_to_le32(((b >> 8) & 0xffff) << 16 | (c >> 16));
		slot[2] = cpu_to_le32(((c >> 8) & 0xff) << 24 | (d >> 8));
	}
}

void motu424_unpack16(const __le32 *slot, s32 *dst, unsigned int stride)
{
	int g;

	for (g = 0; g < 4; g++, dst += 4 * stride, slot += 3) {
		u32 w0 = le32_to_cpu(slot[0]), w1 = le32_to_cpu(slot[1]),
		    w2 = le32_to_cpu(slot[2]);

		dst[0] = w0 & 0xffffff00;
		dst[stride] = (w0 << 24) | ((w1 >> 8) & 0xffff00);
		dst[2 * stride] = (w1 << 16) | ((w2 >> 16) & 0xff00);
		dst[3 * stride] = w2 << 8;
	}
}

static __le32 *block_ptr(struct motu424 *m, unsigned int block)
{
	return (__le32 *)m->dma_area + block * m->block_dwords;
}

/*
 * Each engine period moves blocks_per_period blocks between the DMA ring and
 * the ALSA buffer at frame position pos. A stream may have fewer channels
 * than the engine (the rest of its slots stay silent) or more (they read
 * as silence). At 4x each channel has a primary and a partner slot; even
 * frames go to the primary and odd frames to the partner
 * (PS::clipOutputSamples2X and PS::ConvertInputSamples2X).
 */
static void copy_playback_period(struct motu424 *m, unsigned int period,
				 struct snd_pcm_substream *ss, unsigned int pos)
{
	unsigned int b, ch, h, first = period * m->blocks_per_period;
	unsigned int halves = 1 + m->eng_quad, n = 0, used = 0;
	unsigned int frames = MOTU424_FRAMES_PER_BLOCK * halves;

	if (ss) {
		n = ss->runtime->channels;
		used = min(n, m->pcm_out);
	}
	for (b = 0; b < m->blocks_per_period; b++) {
		__le32 *blk = block_ptr(m, first + b);
		const s32 *src = NULL;

		if (ss)
			src = (const s32 *)ss->runtime->dma_area +
			      (pos + b * frames) * n;
		for (ch = 0; ch < m->pcm_out; ch++)
			for (h = 0; h < halves; h++) {
				__le32 *slot = blk + (1 + m->out_slot[h][ch]) *
					       MOTU424_SLOT_DWORDS;

				if (ch < used)
					motu424_pack16(slot, src + h * n + ch,
						       halves * n);
				else
					memset(slot, 0, MOTU424_SLOT_DWORDS * 4);
			}
	}
}

static void copy_capture_period(struct motu424 *m, unsigned int period,
				unsigned int pos)
{
	struct snd_pcm_substream *ss = m->capture;
	unsigned int b, ch, h, first = period * m->blocks_per_period;
	unsigned int halves = 1 + m->eng_quad, n, used;
	unsigned int frames = MOTU424_FRAMES_PER_BLOCK * halves;

	if (!ss)
		return;
	n = ss->runtime->channels;
	used = min(n, m->pcm_in);
	/* channels beyond the engine's keep the zeros from prepare */
	for (b = 0; b < m->blocks_per_period; b++) {
		const __le32 *blk = block_ptr(m, first + b);
		s32 *dst = (s32 *)ss->runtime->dma_area + (pos + b * frames) * n;

		for (ch = 0; ch < used; ch++)
			for (h = 0; h < halves; h++) {
				const __le32 *slot = blk +
					(1 + m->eng_out + m->in_slot[h][ch]) *
					MOTU424_SLOT_DWORDS;

				motu424_unpack16(slot, dst + h * n + ch,
						 halves * n);
			}
	}
}

/* ---------------------------------------------------------------- engine */

#define MOTU424_IDLE_BLOCKS	64

static void motu424_write_routing(struct motu424 *m, bool audio)
{
	u32 table = m->cb_words[3];
	int i;

	/* ND424::SetupRouting(int in, int out, bool idle) 0x2a680 */
	motu424_write(m, m->cb + CB_IDLE, audio ? 0 : 1);
	motu424_write(m, m->cb + CB_NUM_INPUTS, m->eng_in);
	motu424_write(m, m->cb + CB_NUM_OUTPUTS, m->eng_out);
	for (i = 0; i < 24; i++)
		motu424_write(m, table + 4 * i,
			      ((u32 *)m->routing)[i]);
}

/*
 * Input routing: byte (slot ^ 3) of the 96-byte table holds the hardware
 * input code, hw_channel + hw_channel / 24 (ND424::EnableInput 0x2a650).
 */
static void motu424_build_routing(struct motu424 *m, const u8 *slot_hw)
{
	unsigned int s;

	memset(m->routing, 0, sizeof(m->routing));
	for (s = 0; s < m->eng_in; s++) {
		unsigned int hw = slot_hw[s];

		m->routing[s ^ 3] = hw + hw / MOTU424_CH_PER_WIRE;
	}
}

static void motu424_compute_layout(struct motu424 *m, unsigned int blocks)
{
	m->blocks_per_period = blocks;
	m->period_frames = blocks * MOTU424_FRAMES_PER_BLOCK * (1 + m->eng_quad);
	m->block_dwords = (m->eng_out + m->eng_in + 2) * MOTU424_SLOT_DWORDS;
	m->period_dwords = m->blocks_per_period * m->block_dwords;
	m->buffer_dwords = MOTU424_PERIODS * m->period_dwords;
}

/*
 * The engine always runs once the card is up: with the PCM channels while
 * a stream is open, otherwise idle with only the header and status slots,
 * which carry the interface MIDI (Drv::StartIdling 0x139c0 and
 * Drv::StartAudioRunning 0x13a80). Called with m->mutex held.
 */
static int motu424_engine_start(struct motu424 *m, unsigned int blocks,
				bool audio)
{
	u8 slot_hw[MOTU424_MAX_CHANNELS];
	unsigned long flags;
	int err;

	if (!m->booted)
		return -ENODEV;
	if (m->running)
		return 0;
	/* Both audio and idle transitions update the DSP mixer. */
	err = motu424_mixer_sync(m, audio);
	if (err) {
		dev_err(&m->pci->dev, "mixer setup failed: %d\n", err);
		return err;
	}
	m->eng_quad = audio && m->mult == 4;
	m->pcm_out = audio ? m->n_out : 0;
	m->pcm_in = audio ? m->n_in : 0;
	m->eng_out = motu424_build_slots(m->out_hw, m->pcm_out, m->eng_quad,
					 m->out_slot, NULL);
	m->eng_in = motu424_build_slots(m->in_hw, m->pcm_in, m->eng_quad,
					m->in_slot, slot_hw);
	motu424_compute_layout(m, blocks);
	if (m->eng_out > MOTU424_MAX_CHANNELS || m->eng_in > MOTU424_MAX_CHANNELS ||
	    m->buffer_dwords * 4 > m->dma_bytes)
		return -ENOMEM;

	memset(m->dma_area, 0, m->buffer_dwords * 4);
	motu424_build_routing(m, slot_hw);
	motu424_write_routing(m, audio);

	/* SetSamplesPerInterrupt takes n with samples = 16 << 2n */
	motu424_write(m, m->cb + CB_SAMPLES_PER_IRQ,
		      blocks * MOTU424_FRAMES_PER_BLOCK);
	motu424_write(m, m->cb + CB_IRQS_PER_BUFFER, MOTU424_PERIODS);
	motu424_write(m, m->cb + CB_DMA_BUFFER, (u32)m->dma_addr);

	spin_lock_irqsave(&m->lock, flags);
	m->last_period = 0;
	m->audio = audio;
	m->eng_fresh = true;
	motu424_iface_engine_restart(m);
	m->running = true;
	spin_unlock_irqrestore(&m->lock, flags);
	motu424_set_interrupts(m, true);
	dev_dbg(&m->pci->dev, "engine start: %u out, %u in slots, %u blocks, %u dwords\n",
		m->eng_out, m->eng_in, blocks, m->buffer_dwords);
	return 0;
}

static void motu424_engine_stop(struct motu424 *m)
{
	unsigned long flags;

	if (!m->booted || !m->running)
		return;
	spin_lock_irqsave(&m->lock, flags);
	m->running = false;
	spin_unlock_irqrestore(&m->lock, flags);
	motu424_set_interrupts(m, false);
	m->eng_out = 0;
	m->eng_in = 0;
	m->pcm_out = 0;
	m->pcm_in = 0;
	motu424_build_routing(m, NULL);
	motu424_write_routing(m, false);
}

int motu424_engine_idle(struct motu424 *m)
{
	int err;

	mutex_lock(&m->mutex);
	err = motu424_engine_start(m, MOTU424_IDLE_BLOCKS, false);
	mutex_unlock(&m->mutex);
	return err;
}

/* called from the IRQ handler after the interrupt has been acknowledged */
void motu424_pcm_interrupt(struct motu424 *m)
{
	struct snd_pcm_substream *play = NULL, *capt = NULL;
	unsigned int pos, period;
	u32 bus;

	spin_lock(&m->lock);
	if (!m->running) {
		spin_unlock(&m->lock);
		return;
	}
	bus = motu424_read(m, m->cb + CB_DMA_POSITION);
	pos = (bus - (u32)m->dma_addr) / 4;
	if (pos >= m->buffer_dwords) {
		spin_unlock(&m->lock);
		dev_dbg_ratelimited(&m->pci->dev, "DMA position %08x out of range\n",
				    bus);
		return;
	}
	period = pos / m->period_dwords;
	if (period != m->last_period) {
		unsigned int done = m->last_period;
		unsigned int next = (period + 1) % MOTU424_PERIODS;
		unsigned int present = 0, b;

		/* Drv::DoMIDI: status of the completed period, headers of the next */
		for (b = 0; b < m->blocks_per_period; b++) {
			const __le32 *st =
				block_ptr(m, done * m->blocks_per_period + b) +
				(m->block_dwords - MOTU424_SLOT_DWORDS);

			present = MOTU424_ST_PRESENT_MASK(
				le32_to_cpu(st[MOTU424_ST_FLAGS]));
			motu424_midi_block(m, st, block_ptr(m,
					next * m->blocks_per_period + b));
		}
		if (present != m->present_mask) {
			m->present_mask = present;
			/* ND424::SetDevicePresentMask 0x2ad71: restart the settle timer */
			if (m->mult > 1) {
				m->resettle = true;
				motu424_qmix_schedule(m);
			}
			dev_info_ratelimited(&m->pci->dev,
					     "AudioWire present mask %x\n", present);
		}
		motu424_iface_period(m, present);

		/*
		 * Each stream has its own position, counted from its trigger,
		 * and moves by one engine period per interrupt. Both directions
		 * pass over the engine's first period (its content predates the
		 * start), so linked streams stay in step.
		 * Capture: the period that just completed.
		 * Playback: the period after the one now playing. The pointer
		 * reports what has been taken from the ALSA buffer; the two
		 * periods still queued in the ring are reported as delay.
		 */
		if (m->capture && m->capt_skip) {
			m->capt_skip--;
		} else if (m->capture) {
			struct snd_pcm_runtime *rt = m->capture->runtime;

			copy_capture_period(m, done, m->capt_pos);
			m->capt_pos += m->period_frames;
			if (m->capt_pos >= rt->buffer_size)
				m->capt_pos = 0;
			if (!(m->capt_pos % rt->period_size))
				capt = m->capture;
		}
		if (m->playback && m->play_skip) {
			m->play_skip--;
			copy_playback_period(m, next, NULL, 0);
		} else if (m->playback) {
			struct snd_pcm_runtime *rt = m->playback->runtime;

			copy_playback_period(m, next, m->playback, m->play_pos);
			m->play_pos += m->period_frames;
			if (m->play_pos >= rt->buffer_size)
				m->play_pos = 0;
			if (!(m->play_pos % rt->period_size))
				play = m->playback;
		} else {
			copy_playback_period(m, next, NULL, 0);
		}
		m->eng_fresh = false;
		m->last_period = period;
	}
	m->irq_count++;
	spin_unlock(&m->lock);

	if (play)
		snd_pcm_period_elapsed(play);
	if (capt)
		snd_pcm_period_elapsed(capt);
}

/* ---------------------------------------------------------------- ALSA */

static const unsigned int motu424_pcm_rates[] = {
	44100, 48000, 88200, 96000, 176400, 192000
};

/*
 * The engine interrupts every 16 << 2n frames (SetSamplesPerInterrupt
 * 0..3; ALSA frames double at 4x) on a ring of three such periods. The
 * ALSA buffer is separate: its period is any multiple of a 16-frame block
 * (32 ALSA frames at 4x) and the engine runs at the largest of its sizes
 * that divides it.
 */
static const unsigned int engine_blocks[] = { 64, 16, 4, 1 };

#define MOTU424_PERIOD_FRAMES_MAX	8192
#define MOTU424_BUFFER_FRAMES_MAX	16384

static const struct snd_pcm_hardware motu424_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_JOINT_DUPLEX | SNDRV_PCM_INFO_SYNC_START |
		SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
		 SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000,
	.rate_min = 44100,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = MOTU424_MAX_CHANNELS,
	.buffer_bytes_max = MOTU424_BUFFER_FRAMES_MAX * MOTU424_MAX_CHANNELS * 4,
	.period_bytes_min = MOTU424_FRAMES_PER_BLOCK * 4,
	.period_bytes_max = MOTU424_PERIOD_FRAMES_MAX * MOTU424_MAX_CHANNELS * 4,
	.periods_min = 2,
	.periods_max = MOTU424_BUFFER_FRAMES_MAX / MOTU424_FRAMES_PER_BLOCK,
};

static unsigned int rate_mult(unsigned int rate)
{
	return rate > 96000 ? 4 : rate > 48000 ? 2 : 1;
}

static unsigned int mult_index(unsigned int mult)
{
	return mult == 4 ? 2 : mult - 1;
}

/* blocks per engine period for an ALSA period of frames, or 0 */
static unsigned int motu424_engine_blocks(unsigned int frames,
					  unsigned int mult)
{
	unsigned int bf = motu424_block_frames(mult), i;

	for (i = 0; i < ARRAY_SIZE(engine_blocks); i++)
		if (frames && !(frames % (engine_blocks[i] * bf)))
			return engine_blocks[i];
	return 0;
}

static bool interval_has_multiple(const struct snd_interval *i,
				  unsigned int step)
{
	unsigned int lo = i->min + i->openmin, hi = i->max - i->openmax;

	return roundup(lo, step) <= hi;
}

/*
 * The rates this stream can use: the boxes must give the card channels at
 * that multiplier, the period must hold whole blocks, and once the other
 * direction has its hw_params, both share its rate (the card clock).
 */
static int motu424_rule_rate(struct snd_pcm_hw_params *params,
			     struct snd_pcm_hw_rule *rule)
{
	struct snd_pcm_substream *ss = rule->private;
	struct motu424 *m = snd_pcm_substream_chip(ss);
	struct snd_interval *rate = hw_param_interval(params,
						       SNDRV_PCM_HW_PARAM_RATE);
	const struct snd_interval *period = hw_param_interval_c(params,
						SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	unsigned int list[ARRAY_SIZE(motu424_pcm_rates)], n = 0, i;
	bool fixed = READ_ONCE(m->rate_users) & ~BIT(ss->stream);

	for (i = 0; i < ARRAY_SIZE(motu424_pcm_rates); i++) {
		unsigned int r = motu424_pcm_rates[i], mult = rate_mult(r);

		if (fixed && r != READ_ONCE(m->rate))
			continue;
		if (!m->open_ch[ss->stream][mult_index(mult)])
			continue;
		if (interval_has_multiple(period, motu424_block_frames(mult)))
			list[n++] = r;
	}
	return snd_interval_list(rate, n, list, 0);
}

/* whole blocks: multiples of 16 frames, of 32 when only 4x rates remain */
static int motu424_rule_period(struct snd_pcm_hw_params *params,
			       struct snd_pcm_hw_rule *rule)
{
	const struct snd_interval *rate = hw_param_interval_c(params,
						SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *period = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	unsigned int step = motu424_block_frames(rate->min > 96000 ? 4 : 1);
	struct snd_interval t = {
		.min = roundup(period->min + period->openmin, step),
		.max = rounddown(period->max - period->openmax, step),
		.integer = 1,
	};

	return snd_interval_refine(period, &t);
}

/* Drv::SetSampleRate within a family: the family constant, then the boxes */
static void motu424_change_rate(struct motu424 *m, unsigned int rate)
{
	unsigned long flags;

	spin_lock_irqsave(&m->lock, flags);
	m->rate = rate;
	spin_unlock_irqrestore(&m->lock, flags);
	motu424_clock_set_rate(m, rate);
	motu424_iface_rate_changed(m);
}

/*
 * Drv::SetSampleRate with a new multiplier (Drv424::SetSampleRateMultiplier
 * and ND424::SetSampleRateMultiplier). The card runs on its own clock
 * meanwhile; the mixer is emptied, the boxes are told the new multiplier
 * while the engine still carries their MIDI, then the card follows.
 * Called with m->mutex held and no stream prepared.
 */
static int motu424_change_mult(struct motu424 *m, unsigned int rate,
			       unsigned int mult)
{
	unsigned int source = m->word_source;
	int err;

	/* A smaller budget would keep the engine from restarting. */
	if (!motu424_qmix_mult_fits(m, mult)) {
		dev_warn(&m->pci->dev,
			 "too many Q-Mix routes for %u Hz; mute some first\n",
			 rate);
		return -ENOSPC;
	}
	if (m->running && m->audio) {
		motu424_engine_stop(m);
		err = motu424_engine_start(m, MOTU424_IDLE_BLOCKS, false);
		if (err)
			return err;
	}
	if (source != MOTU424_WS_INTERNAL) {
		motu424_clock_set_source(m, MOTU424_WS_INTERNAL);
		m->word_source = MOTU424_WS_INTERNAL;
	}
	err = motu424_qmix_release(m);
	if (err)
		dev_warn(&m->pci->dev, "Q-Mix release failed: %d\n", err);
	if (motu424_iface_drain(m))
		dev_warn(&m->pci->dev, "interface MIDI did not drain\n");
	motu424_iface_set_mult(m, mult);
	if (motu424_iface_drain(m))
		dev_warn(&m->pci->dev, "interface MIDI did not drain\n");
	msleep(100);

	/* the engine stops while the card's clock and block size change */
	motu424_engine_stop(m);
	motu424_change_rate(m, rate);
	motu424_clock_set_mult(m, mult, motu424_iface_wire_mask(m, mult));
	m->mult_settle_pending = true;
	motu424_qmix_set_mult(m, mult);
	if (source != MOTU424_WS_INTERNAL) {
		motu424_clock_set_source(m, source);
		m->word_source = source;
	}
	motu424_layout_refresh(m);
	err = motu424_engine_start(m, MOTU424_IDLE_BLOCKS, false);
	if (err)
		dev_err(&m->pci->dev, "could not restart idle engine: %d\n", err);
	return err;
}

static int motu424_set_rate(struct motu424 *m, unsigned int rate)
{
	unsigned int mult = rate_mult(rate);

	if (m->word_source != MOTU424_WS_INTERNAL)
		dev_warn(&m->pci->dev,
			 "sample rate %u Hz while the card follows %s; the external source must run at this rate\n",
			 rate, motu424_ws_names[m->word_source]);
	if (mult != m->mult) {
		int err = motu424_change_mult(m, rate, mult);

		if (err)
			return err;
	} else {
		motu424_change_rate(m, rate);
	}
	motu424_smpte_rate_changed(m);
	dev_info(&m->pci->dev, "sample rate %u Hz\n", rate);
	return 0;
}

/*
 * Channels per multiplier: at the current one those of the layout the
 * streams were opened with, at the others those of the boxes now present.
 */
static void motu424_open_channels(struct motu424 *m, bool play,
				  unsigned int ch[3])
{
	static const unsigned int mults[3] = { 1, 2, 4 };
	struct motu424_layout *l;
	unsigned long flags;
	unsigned int i;

	l = kmalloc(sizeof(*l), GFP_KERNEL);
	for (i = 0; i < 3; i++) {
		ch[i] = 0;
		if (mults[i] == m->mult) {
			ch[i] = play ? m->n_out : m->n_in;
		} else if (l) {
			spin_lock_irqsave(&m->lock, flags);
			motu424_layout_compute(m, mults[i], l);
			spin_unlock_irqrestore(&m->lock, flags);
			ch[i] = play ? l->n_out : l->n_in;
		}
	}
	kfree(l);
}

static int motu424_pcm_open(struct snd_pcm_substream *ss)
{
	struct motu424 *m = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	bool play = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;
	unsigned int max, i;
	int err;

	if (!m->booted)
		return -ENODEV;

	mutex_lock(&m->mutex);
	/* the channels are those of the interfaces configured at this moment */
	if (!m->open_count)
		motu424_layout_freeze(m, true);
	motu424_open_channels(m, play, m->open_ch[ss->stream]);
	max = 0;
	for (i = 0; i < 3; i++)
		max = max(max, m->open_ch[ss->stream][i]);
	if (!max) {
		err = -ENODEV;
		goto out;
	}
	/*
	 * Any number of channels up to the most the boxes give at any rate,
	 * so that a sound server keeps its channel count across rates;
	 * those the engine lacks at a rate are silent.
	 */
	rt->hw = motu424_hw;
	rt->hw.channels_max = max;

	err = snd_pcm_hw_rule_add(rt, 0, SNDRV_PCM_HW_PARAM_RATE,
				  motu424_rule_rate, ss,
				  SNDRV_PCM_HW_PARAM_RATE,
				  SNDRV_PCM_HW_PARAM_PERIOD_SIZE, -1);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_rule_add(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				  motu424_rule_period, ss,
				  SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				  SNDRV_PCM_HW_PARAM_RATE, -1);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (err < 0)
		goto out;
	if (m->running && m->audio) {
		/* the other direction is running: its rate and engine period */
		err = snd_pcm_hw_constraint_single(rt, SNDRV_PCM_HW_PARAM_RATE,
						   m->rate);
		if (err < 0)
			goto out;
		err = snd_pcm_hw_constraint_step(rt, 0,
				SNDRV_PCM_HW_PARAM_PERIOD_SIZE, m->period_frames);
	}
out:
	/* a successful constraint returns 1 when it narrowed the range */
	if (err >= 0)
		m->open_count++;
	else if (!m->open_count)
		motu424_layout_freeze(m, false);
	mutex_unlock(&m->mutex);
	if (err < 0)
		return err;
	snd_pcm_set_sync(ss);
	return 0;
}

static int motu424_pcm_close(struct snd_pcm_substream *ss)
{
	struct motu424 *m = snd_pcm_substream_chip(ss);
	unsigned long flags;
	int err;

	mutex_lock(&m->mutex);
	spin_lock_irqsave(&m->lock, flags);
	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		m->playback = NULL;
	else
		m->capture = NULL;
	spin_unlock_irqrestore(&m->lock, flags);
	m->rate_users &= ~BIT(ss->stream);
	if (--m->open_count == 0) {
		/* back to idle so that the interfaces stay under control */
		motu424_engine_stop(m);
		motu424_layout_freeze(m, false);
		err = motu424_engine_start(m, MOTU424_IDLE_BLOCKS, false);
		if (err)
			dev_err(&m->pci->dev, "could not restart idle engine: %d\n", err);
	}
	mutex_unlock(&m->mutex);
	return 0;
}

static int motu424_pcm_hw_params(struct snd_pcm_substream *ss,
				 struct snd_pcm_hw_params *params)
{
	struct motu424 *m = snd_pcm_substream_chip(ss);
	unsigned int rate = params_rate(params);
	unsigned int mult = rate_mult(rate);
	unsigned int period = params_period_size(params);
	bool play = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;
	int err = 0;

	if (rate % 11025 && rate % 12000)
		return -EINVAL;
	if (!motu424_engine_blocks(period, mult))
		return -EINVAL;
	mutex_lock(&m->mutex);
	if (!m->booted) {
		err = -ENODEV;
	} else if (rate != m->rate) {
		if (m->rate_users & ~BIT(ss->stream)) {
			err = -EBUSY;
		} else if (mult != m->mult) {
			struct motu424_layout *l = kmalloc(sizeof(*l), GFP_KERNEL);
			unsigned long flags;

			/* the boxes must still give this direction channels */
			if (!l) {
				err = -ENOMEM;
			} else {
				spin_lock_irqsave(&m->lock, flags);
				motu424_layout_compute(m, mult, l);
				spin_unlock_irqrestore(&m->lock, flags);
				if (!(play ? l->n_out : l->n_in))
					err = -EBUSY;
				kfree(l);
			}
		}
		if (!err)
			err = motu424_set_rate(m, rate);
	}
	if (!err) {
		m->rate_users |= BIT(ss->stream);
		m->period_req[ss->stream] = period;
	}
	mutex_unlock(&m->mutex);
	return err;
}

static int motu424_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct motu424 *m = snd_pcm_substream_chip(ss);

	mutex_lock(&m->mutex);
	m->rate_users &= ~BIT(ss->stream);
	m->period_req[ss->stream] = 0;
	mutex_unlock(&m->mutex);
	return 0;
}

static int motu424_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct motu424 *m = snd_pcm_substream_chip(ss);
	unsigned int period = ss->runtime->period_size;
	unsigned long flags;
	int err = 0;

	mutex_lock(&m->mutex);
	if (ss->runtime->rate != m->rate) {
		err = -EBUSY;
	} else if (m->running && m->audio) {
		/* the other direction fixed the engine period when it started */
		if (period % m->period_frames)
			err = -EBUSY;
	} else {
		unsigned int other = m->period_req[!ss->stream], blocks;
		int idle_err;

		/* an engine period that also suits the other direction */
		blocks = motu424_engine_blocks(other ? gcd(period, other) : period,
					       m->mult);
		motu424_engine_stop(m);
		err = blocks ? motu424_engine_start(m, blocks, true) : -EINVAL;
		if (err) {
			idle_err = motu424_engine_start(m, MOTU424_IDLE_BLOCKS, false);
			if (idle_err)
				dev_err(&m->pci->dev,
					"could not restart idle engine: %d\n", idle_err);
		}
	}
	if (!err && m->mult_settle_pending) {
		dev_info(&m->pci->dev,
			 "waiting %u ms for AudioWire to settle after multiplier change\n",
			 MOTU424_MULT_SETTLE_MS);
		msleep(MOTU424_MULT_SETTLE_MS);
		m->mult_settle_pending = false;
	}
	mutex_unlock(&m->mutex);
	if (err)
		return err;

	spin_lock_irqsave(&m->lock, flags);
	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		m->play_pos = 0;
	else
		m->capt_pos = 0;
	spin_unlock_irqrestore(&m->lock, flags);
	/* capture channels the engine does not carry stay silent */
	memset(ss->runtime->dma_area, 0, ss->runtime->dma_bytes);
	return 0;
}

static int motu424_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct motu424 *m = snd_pcm_substream_chip(ss);
	struct snd_pcm_substream *s;

	spin_lock(&m->lock);
	snd_pcm_group_for_each_entry(s, ss) {
		if (snd_pcm_substream_chip(s) != m)
			continue;
		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
			/* streams started together skip the same periods */
			if (s->stream == SNDRV_PCM_STREAM_PLAYBACK) {
				m->play_pos = 0;
				m->play_skip = m->eng_fresh;
				m->playback = s;
			} else {
				m->capt_pos = 0;
				m->capt_skip = m->eng_fresh;
				m->capture = s;
			}
			break;
		case SNDRV_PCM_TRIGGER_STOP:
			if (s->stream == SNDRV_PCM_STREAM_PLAYBACK)
				m->playback = NULL;
			else
				m->capture = NULL;
			break;
		default:
			spin_unlock(&m->lock);
			return -EINVAL;
		}
		snd_pcm_trigger_done(s, ss);
	}
	spin_unlock(&m->lock);
	return 0;
}

static snd_pcm_uframes_t motu424_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct motu424 *m = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;

	if (ss->stream == SNDRV_PCM_STREAM_CAPTURE)
		return m->capt_pos;
	/* taken from the buffer, but still queued in the DMA ring */
	rt->delay = 2 * m->period_frames;
	return m->play_pos;
}

static const struct snd_pcm_ops motu424_pcm_ops = {
	.open = motu424_pcm_open,
	.close = motu424_pcm_close,
	.hw_params = motu424_pcm_hw_params,
	.hw_free = motu424_pcm_hw_free,
	.prepare = motu424_pcm_prepare,
	.trigger = motu424_pcm_trigger,
	.pointer = motu424_pcm_pointer,
};

int motu424_pcm_create(struct motu424 *m)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(m->card, "MOTU 424", 0, 1, 1, &pcm);
	if (err < 0)
		return err;
	pcm->private_data = m;
	strscpy(pcm->name, m->card->shortname, sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &motu424_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &motu424_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);
	m->pcm = pcm;
	return 0;
}
