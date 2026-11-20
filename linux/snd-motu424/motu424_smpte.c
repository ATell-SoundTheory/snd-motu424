// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMPTE (LTC) reader and generator, video sync box bits and the C6412
 * S/MUX option. Follows the kext's ND424/Drv424 SMPTE code; see
 * docs/SMPTE.md for the recovered control-block protocol. Untested on
 * hardware.
 */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/workqueue.h>
#include <sound/control.h>

#include "motu424.h"
#include "motu424_tables.h"

#define SMPTE_POLL_MS		100
#define SMPTE_FREEWHEEL_MAX_MS	3600000

/* kFrameRates[] (0x3e2e0), indexed by the kext's FrameRate/FrameFormat */
static const u16 smpte_rates[4] = { 2400, 2500, 2997, 3000 };
/* UI format -> (FrameRate, FrameFormat) */
static const u8 smpte_format_rate[MOTU424_SMPTE_FORMATS] = { 0, 1, 2, 2, 3 };
static const u8 smpte_format_count[MOTU424_SMPTE_FORMATS] = { 0, 1, 2, 3, 3 };
static const u8 smpte_format_fps[MOTU424_SMPTE_FORMATS] = { 24, 25, 30, 30, 30 };

static u32 sm_read(struct motu424 *m, u32 off)
{
	return motu424_read(m, m->cb + off);
}

static void sm_write(struct motu424 *m, u32 off, u32 val)
{
	motu424_write(m, m->cb + off, val);
}

/* ND424::DSP::WritePolitely */
static void sm_write_politely(struct motu424 *m, u32 off, u32 val)
{
	sm_write(m, off, val);
	udelay(1);
}

static unsigned int smpte_base_rate(const struct motu424 *m)
{
	return m->rate % 11025 ? 48000 : 44100;
}

static bool ws_reads_ltc(unsigned int ws)
{
	return ws >= MOTU424_WS_SMPTE && ws <= MOTU424_WS_VIDEO_W3;
}

/* ------------------------------------------------------------ timecode */

u64 motu424_smpte_tc_to_frames(const u8 tc[4], unsigned int format)
{
	u64 minutes = 60ull * tc[0] + tc[1];
	u64 frames = (minutes * 60 + tc[2]) * smpte_format_fps[format] + tc[3];

	if (format == MOTU424_SMPTE_2997_DF)
		frames -= 2 * (minutes - div_u64(minutes, 10));
	return frames;
}

void motu424_smpte_frames_to_tc(u64 frames, unsigned int format, u8 tc[4])
{
	unsigned int fps = smpte_format_fps[format];
	u32 rem;

	if (format == MOTU424_SMPTE_2997_DF) {
		/* 17982 frames per 10 minutes, 1798 per dropping minute */
		u64 tens = div_u64_rem(frames, 17982, &rem);

		frames += 18 * tens;
		if (rem >= 2)
			frames += 2 * ((rem - 2) / 1798);
	}
	frames = div_u64_rem(frames, fps, &rem);
	tc[3] = rem;
	frames = div_u64_rem(frames, 60, &rem);
	tc[2] = rem;
	frames = div_u64_rem(frames, 60, &rem);
	tc[1] = rem;
	div_u64_rem(frames, 24, &rem);
	tc[0] = rem;
}

static bool smpte_tc_valid(const u8 tc[4], unsigned int format)
{
	if (tc[0] > 23 || tc[1] > 59 || tc[2] > 59 ||
	    tc[3] >= smpte_format_fps[format])
		return false;
	/* drop-frame skips frames 0 and 1 of minutes not divisible by 10 */
	return format != MOTU424_SMPTE_2997_DF || tc[2] || tc[3] > 1 ||
	       tc[1] % 10 == 0;
}

/* -------------------------------------------------------- source codes */

/* still offered (Drv::RebuildSMPTESourceAndDestinationLists); m->lock */
static bool smpte_code_valid(struct motu424 *m, unsigned int code, bool out)
{
	const struct motu424_wire *w;
	unsigned int pos;

	if (code == MOTU424_SMPTE_NONE || code > MOTU424_SMPTE_CODE_MAX)
		return false;
	if (code >= MOTU424_SMPTE_JACK(0))
		return motu424_wire_is_2408mk3(
			&m->wire[code - MOTU424_SMPTE_JACK(0)]);
	w = &m->wire[(code - 1) / MOTU424_CH_PER_WIRE];
	pos = (code - 1) % MOTU424_CH_PER_WIRE;
	if (!motu424_wire_chan_available(w, pos, out, m->mult))
		return false;
	return out || motu424_wire_input_analog(w, pos);
}

static unsigned int smpte_code_wire(unsigned int code)
{
	if (code >= MOTU424_SMPTE_JACK(0))
		return code - MOTU424_SMPTE_JACK(0);
	return (code - 1) / MOTU424_CH_PER_WIRE;
}

/* the box of code is waiting for its new state to be confirmed; m->lock */
static bool smpte_code_busy(struct motu424 *m, unsigned int code)
{
	return code && code <= MOTU424_SMPTE_CODE_MAX &&
	       m->wire[smpte_code_wire(code)].state == MOTU424_WIRE_CONFIG;
}

static bool smpte_code_jack(unsigned int code)
{
	return code >= MOTU424_SMPTE_JACK(0);
}

/* X(w, ch) or 25w + 25 (Drv424::SMPTESourceChanged) */
static u32 smpte_dsp_source(unsigned int code)
{
	unsigned int w = smpte_code_wire(code);

	if (code == MOTU424_SMPTE_NONE)
		return 0;
	if (smpte_code_jack(code))
		return MOTU424_SMPTE_DSP_JACK(w);
	return MOTU424_SMPTE_DSP_CHAN(w, (code - 1) % MOTU424_CH_PER_WIRE);
}

/* ----------------------------------------------------- control block */

/* ND424::SendAddressSource: called with the card's (new) word source */
void motu424_smpte_address_write(struct motu424 *m, unsigned int source)
{
	unsigned int code = m->smpte.prog_source;
	u32 mode = MOTU424_ADDR_SRC_NONE;

	if (source == MOTU424_WS_ADAT)
		mode = MOTU424_ADDR_SRC_ADAT;
	else if (code && ws_reads_ltc(source))
		mode = smpte_code_jack(code) ? MOTU424_ADDR_SRC_SMPTE_B :
					       MOTU424_ADDR_SRC_SMPTE_A;
	sm_write(m, MOTU424_CB_ADDRESS_SOURCE, mode);
	sm_write(m, MOTU424_CB_ADDRESS_PARAM, smpte_dsp_source(code));
	sm_write(m, MOTU424_CB_COMMIT, 1);
}

/* ND424::UpdateSMPTEParameters (0x2bc60) */
static void smpte_params_write(struct motu424 *m)
{
	unsigned int f = m->smpte.format;
	u32 a = smpte_rates[smpte_format_rate[f]];
	u32 b = smpte_rates[smpte_format_count[f]];
	u32 r = smpte_base_rate(m);
	u32 k, v, bc, q;

	sm_write(m, MOTU424_CB_SMPTE_FPS, (b + 50) / 100);
	sm_write_politely(m, MOTU424_CB_SMPTE_FRAMES_10MIN, 6 * b);
	sm_write_politely(m, MOTU424_CB_SMPTE_FRAMES_MIN, 6 * b / 10);
	sm_write_politely(m, MOTU424_CB_SMPTE_COUNT_FPS, b / 100);
	sm_write_politely(m, MOTU424_CB_SMPTE_BIT_RATE,
			  (div_u64((u64)8 * a << 20, r) + 5) / 10);
	k = sm_read(m, MOTU424_CB_SMPTE_CONST_K);
	sm_write_politely(m, MOTU424_CB_SMPTE_K_SAMPLES,
			  div_u64((u64)k * 10 * r, 8 * a));
	bc = 100 * r / a;
	sm_write_politely(m, MOTU424_CB_SMPTE_FRAME_INT, bc);
	sm_write_politely(m, MOTU424_CB_SMPTE_FRAME_FRAC,
			  div_u64((u64)(100 * r - bc * a) << 32, a));
	v = sm_read(m, MOTU424_CB_SMPTE_CONST_V);
	sm_write_politely(m, MOTU424_CB_SMPTE_V_HALF, m->mult > 1 ? v >> 1 : v);
	sm_write_politely(m, MOTU424_CB_SMPTE_V_SAMPLES,
			  div_u64((u64)v * 10 * r + 4 * a, 8 * a));
	q = div_u64((u64)v * r, 64000000);
	/* the kext divides unguarded; a DSP that reports V = 0 gets no f0 */
	if (q)
		sm_write_politely(m, MOTU424_CB_SMPTE_F0, 0x40000 / q);
	sm_write_politely(m, MOTU424_CB_COMMIT, 1);
}

static u32 smpte_freewheel_samples(const struct motu424 *m, int ms)
{
	if (ms < 0)
		return 0xffffffff;
	return (u64)ms * (smpte_base_rate(m) / 100) / 10;
}

/* ND424::UpdateFreewheelTimes (0x2b040) */
static void smpte_freewheel_write(struct motu424 *m)
{
	sm_write(m, MOTU424_CB_SMPTE_FW_CLOCK,
		 smpte_freewheel_samples(m, m->smpte.fw_clock));
	sm_write(m, MOTU424_CB_SMPTE_FW_ADDRESS,
		 smpte_freewheel_samples(m, m->smpte.fw_address));
}

/* ND424::SetSMPTEGeneratorStartTime (0x2d8d0) at the configured time */
static void smpte_generator_start(struct motu424 *m)
{
	struct motu424_smpte *s = &m->smpte;
	u64 a = smpte_rates[smpte_format_rate[s->format]];
	u64 sr = (u64)smpte_base_rate(m) * m->mult;
	u64 t, f, off;

	t = div64_u64(motu424_smpte_tc_to_frames(s->start, s->format) * 100 *
		      sr, a);
	f = div64_u64(t * a, 100 * sr) + 1;
	off = div64_u64((f * 100 * sr - t * a) << 16, a);
	if (m->mult == 4)
		off >>= 1;
	sm_write(m, MOTU424_CB_SMPTE_START_FRAME, f);
	sm_write(m, MOTU424_CB_SMPTE_START_OFFSET, off);
}

/* ND424::StopStriping */
static void smpte_generator_stop(struct motu424 *m)
{
	sm_write(m, MOTU424_CB_SMPTE_STOP, 1);
}

struct smpte_dest_regs {
	u32 a, b, jack, clobber;
};

/* SetSMPTEDestination and SetSMPTEClobberChannels for dest */
static void smpte_dest_regs(struct motu424 *m, unsigned int dest,
			    struct smpte_dest_regs *r)
{
	unsigned int w = smpte_code_wire(dest);

	memset(r, 0, sizeof(*r));
	if (dest == MOTU424_SMPTE_NONE)
		return;
	if (smpte_code_jack(dest)) {
		u32 x = MOTU424_SMPTE_DSP_CHAN(w, MOTU424_2408MK3_SMPTE_OUT_SLOT);

		r->jack = w + 1;
		if (motu424_iface_smpte_jack_slots(m, w) == 2)
			r->clobber = (x << 16) | (x + 1);
		return;
	}
	r->a = smpte_dsp_source(dest);
	r->b = r->a + (m->mult == 4 ? MOTU424_CH_PER_WIRE / 2 : 0);
	r->clobber = (r->a << 16) | r->a;
}

/* Drv424::SMPTEGeneratorSettingsChanged (0x19ac0), mode 0 */
static void smpte_generator_apply(struct motu424 *m,
				  const struct smpte_dest_regs *r)
{
	struct motu424_smpte *s = &m->smpte;

	smpte_generator_stop(m);
	sm_write(m, MOTU424_CB_SMPTE_DEST_A, r->a);
	sm_write(m, MOTU424_CB_SMPTE_DEST_B, r->b);
	sm_write(m, MOTU424_CB_SMPTE_DEST_JACK, r->jack);
	if (!r->jack)
		sm_write(m, MOTU424_CB_SMPTE_LEVEL, s->level);
	sm_write(m, MOTU424_CB_SMPTE_CLOBBER, r->clobber);
	sm_write(m, MOTU424_CB_SMPTE_CLOBBERED, 0);
	s->regs_a = r->a;
	s->regs_b = r->b;
	s->regs_jack = r->jack;
	s->regs_clobber = r->clobber;
	if (s->generate && s->prog_dest)
		smpte_generator_start(m);
}

static void smpte_generator_reprogram(struct motu424 *m)
{
	struct smpte_dest_regs r;

	smpte_dest_regs(m, m->smpte.prog_dest, &r);
	smpte_generator_apply(m, &r);
}

/* --------------------------------------------------- layout following */

static void smpte_notify(struct motu424 *m, unsigned int ctl)
{
	struct snd_kcontrol *kctl = m->smpte.kctl[ctl];

	if (kctl)
		snd_ctl_notify(m->card, SNDRV_CTL_EVENT_MASK_VALUE, &kctl->id);
}

/* false if a box being reconfigured has to be looked at again */
static bool smux_refresh(struct motu424 *m)
{
	unsigned int port;
	bool done = true;

	if (m->params->dsp != MOTU424_DSP_C6412)
		return true;
	for (port = 0; port < MOTU424_MAX_WIRES; port++) {
		u32 mask;

		if (m->smux_written &&
		    READ_ONCE(m->wire[port].state) == MOTU424_WIRE_CONFIG) {
			done = false;
			continue;
		}
		mask = motu424_iface_smux_mask(m, port);
		if (m->smux_written && m->smux_mask[port] == mask)
			continue;
		motu424_smux_write(m, port, mask);
		m->smux_mask[port] = mask;
	}
	m->smux_written = true;
	return done;
}

/*
 * After a box, layout, rate or clock change (m->mutex): drop a source or
 * destination that is no longer offered, reprogram what changed, and
 * update the 2408mk3 video/LTC bits (Drv::UpdateSMPTEVideoSyncState) and
 * the S/MUX option.
 */
void motu424_smpte_refresh(struct motu424 *m)
{
	struct motu424_smpte *s = &m->smpte;
	unsigned int ws = m->word_source;
	u8 b14[MOTU424_MAX_WIRES] = { 0 };
	unsigned int out_mask = 0, w;
	struct smpte_dest_regs r;
	unsigned int source, dest;
	unsigned long flags;
	bool busy = false;

	if (!m->booted)
		return;
	/* judge a box being reconfigured once it has confirmed its state */
	spin_lock_irqsave(&m->lock, flags);
	if (smpte_code_busy(m, s->source)) {
		source = s->prog_source;
		busy = true;
	} else {
		source = smpte_code_valid(m, s->source, false) ? s->source : 0;
	}
	if (smpte_code_busy(m, s->dest)) {
		dest = s->prog_dest;
		busy = true;
	} else {
		dest = smpte_code_valid(m, s->dest, true) ? s->dest : 0;
	}
	spin_unlock_irqrestore(&m->lock, flags);

	if (source != s->prog_source) {
		s->prog_source = source;
		motu424_smpte_address_write(m, ws);
		smpte_notify(m, MOTU424_SMPTE_CTL_SOURCE);
	}
	if (dest != s->prog_dest) {
		s->prog_dest = dest;
		smpte_notify(m, MOTU424_SMPTE_CTL_DEST);
	}
	smpte_dest_regs(m, dest, &r);
	if (r.a != s->regs_a || r.b != s->regs_b || r.jack != s->regs_jack ||
	    r.clobber != s->regs_clobber)
		smpte_generator_apply(m, &r);

	for (w = 0; w < MOTU424_MAX_WIRES; w++) {
		if (ws >= MOTU424_WS_VIDEO_W0 && ws <= MOTU424_WS_VIDEO_W3)
			b14[w] |= MOTU424_2408MK3_B14_VIDEO;
		if (ws == MOTU424_WS_VIDEO_W0 + w)
			b14[w] |= MOTU424_2408MK3_B14_VIDEO_MASTER;
		if (ws == MOTU424_WS_SMPTE && source &&
		    smpte_code_wire(source) == w)
			b14[w] |= MOTU424_2408MK3_B14_LTC;
	}
	if (dest && smpte_code_jack(dest))
		out_mask = BIT(smpte_code_wire(dest));
	/* S/MUX first: updating the bits puts the boxes into CONFIG */
	if (!smux_refresh(m))
		busy = true;
	if (!motu424_iface_set_smpte(m, b14, out_mask))
		busy = true;
	s->box_retry = busy;
}

/* boot (motu424_clock_init): the DSP starts with no SMPTE state */
void motu424_smpte_hw_init(struct motu424 *m)
{
	struct motu424_smpte *s = &m->smpte;
	struct smpte_dest_regs none = { 0 };

	smpte_params_write(m);
	smpte_freewheel_write(m);
	s->prog_source = 0;
	s->prog_dest = 0;
	smpte_generator_apply(m, &none);
}

/* after a base rate or multiplier change; m->mutex */
void motu424_smpte_rate_changed(struct motu424 *m)
{
	if (!m->booted)
		return;
	smpte_params_write(m);
	smpte_freewheel_write(m);
	/* reprogrammed by the refresh: new start values for the new rate */
	m->smpte.regs_jack = ~0u;
	motu424_smpte_refresh(m);
}

/* ------------------------------------------------------------- polling */

/* ND424::GetSMPTEState (0x2a830) */
static u8 smpte_transport_state(struct motu424 *m, u32 s34, u32 s38)
{
	unsigned int ws = m->word_source;

	if (!ws_reads_ltc(ws))
		return 0;
	if (ws == MOTU424_WS_SMPTE) {
		if (!m->smpte.prog_source)
			return 1;
		return s34 >= 1 && s34 <= 3 ? s34 + 2 : 1;
	}
	if (s38 == 1)
		return s34 == 1 ? 3 : s34 == 2 ? 4 : 6;
	if (s38 == 2)
		return s34 == 1 || s34 == 2 ? 4 : 5;
	return 1;
}

/* Drv::SMPTEFormatDetected (0x13d10) without a user-space client */
static void smpte_format_detected(struct motu424 *m, unsigned int code)
{
	static const u8 formats[4] = {
		MOTU424_SMPTE_24, MOTU424_SMPTE_25, MOTU424_SMPTE_2997_DF,
		MOTU424_SMPTE_30,
	};
	struct motu424_smpte *s = &m->smpte;

	if (code == smpte_format_count[s->format] || !s->follow)
		return;
	s->format = code == 3 ? s->last30 : formats[code];
	smpte_params_write(m);
	smpte_generator_reprogram(m);
	smpte_notify(m, MOTU424_SMPTE_CTL_FORMAT);
}

static void smpte_poll(struct work_struct *work)
{
	struct motu424 *m = container_of(to_delayed_work(work), struct motu424,
					 smpte.poll);
	struct motu424_smpte *s = &m->smpte;
	u32 status[2], detected;
	u8 state;

	mutex_lock(&m->mutex);
	if (s->stopped) {
		mutex_unlock(&m->mutex);
		return;
	}
	if (!m->booted)
		goto out;
	if (s->box_retry)
		motu424_smpte_refresh(m);

	status[0] = sm_read(m, MOTU424_CB_SMPTE_ADDR_STATE);
	status[1] = sm_read(m, MOTU424_CB_SMPTE_CLOCK_STATE);
	if (status[0] != s->status[0] || status[1] != s->status[1]) {
		s->status[0] = status[0];
		s->status[1] = status[1];
		smpte_notify(m, MOTU424_SMPTE_CTL_STATUS);
	}
	state = smpte_transport_state(m, status[0], status[1]);
	if (state != s->state) {
		s->state = state;
		smpte_notify(m, MOTU424_SMPTE_CTL_STATE);
	}

	/* GetDetectedFrameRate: 24, 25, 29 (drop) or 30; the host clears it */
	detected = sm_read(m, MOTU424_CB_SMPTE_DETECTED);
	if (detected) {
		unsigned int code = detected == 24 ? 0 : detected == 25 ? 1 :
				    detected == 29 ? 2 : detected == 30 ? 3 : 4;

		sm_write(m, MOTU424_CB_SMPTE_DETECTED, 0);
		if (code < 4) {
			if (s->detected != code + 1) {
				s->detected = code + 1;
				smpte_notify(m, MOTU424_SMPTE_CTL_DETECTED);
			}
			smpte_format_detected(m, code);
		}
	}

	if (s->generate && s->prog_dest) {
		u32 frame = sm_read(m, MOTU424_CB_SMPTE_GEN_FRAME);

		if (frame != s->gen_frame) {
			s->gen_frame = frame;
			smpte_notify(m, MOTU424_SMPTE_CTL_GEN_TIME);
		}
	}
	/* DidSMPTEGenClobberAudio, reported once per destination */
	if (!s->clobbered && sm_read(m, MOTU424_CB_SMPTE_CLOBBERED)) {
		sm_write(m, MOTU424_CB_SMPTE_CLOBBERED, 0);
		s->clobbered = true;
		smpte_notify(m, MOTU424_SMPTE_CTL_CLOBBERED);
	}
out:
	schedule_delayed_work(&s->poll, msecs_to_jiffies(SMPTE_POLL_MS));
	mutex_unlock(&m->mutex);
}

/* ------------------------------------------------------------ controls */

struct smpte_ctl_desc {
	const char *name;
	unsigned int count;
	long min, max;
	bool readonly, boolean;
};

#define SMPTE_INT(n, c, lo, hi)	{ .name = n, .count = c, .min = lo, .max = hi }
#define SMPTE_RO(n, c, lo, hi)	{ .name = n, .count = c, .min = lo, .max = hi, \
				  .readonly = true }
#define SMPTE_BOOL(n)		{ .name = n, .count = 1, .max = 1, .boolean = true }

static const struct smpte_ctl_desc smpte_ctls[MOTU424_SMPTE_CTL_COUNT] = {
	[MOTU424_SMPTE_CTL_FORMAT] = SMPTE_INT("SMPTE Frame Format", 1, 0,
					       MOTU424_SMPTE_FORMATS - 1),
	[MOTU424_SMPTE_CTL_FOLLOW] = SMPTE_BOOL("SMPTE Format Follow Switch"),
	[MOTU424_SMPTE_CTL_DETECTED] = SMPTE_RO("SMPTE Detected Format", 1, 0,
						4),
	[MOTU424_SMPTE_CTL_SOURCE] = SMPTE_INT("SMPTE Reader Source", 1, 0,
					       MOTU424_SMPTE_CODE_MAX),
	[MOTU424_SMPTE_CTL_STATE] = SMPTE_RO("SMPTE Reader State", 1, 0, 6),
	[MOTU424_SMPTE_CTL_STATUS] = SMPTE_RO("SMPTE Reader Status", 2, 0,
					      0x7fffffff),
	[MOTU424_SMPTE_CTL_FW_ADDRESS] =
		SMPTE_INT("SMPTE Freewheel Address Time", 1, -1,
			  SMPTE_FREEWHEEL_MAX_MS),
	[MOTU424_SMPTE_CTL_FW_CLOCK] =
		SMPTE_INT("SMPTE Freewheel Clock Time", 1, -1,
			  SMPTE_FREEWHEEL_MAX_MS),
	[MOTU424_SMPTE_CTL_DEST] = SMPTE_INT("SMPTE Generator Destination", 1,
					     0, MOTU424_SMPTE_CODE_MAX),
	[MOTU424_SMPTE_CTL_LEVEL] = SMPTE_INT("SMPTE Generator Level", 1, 0,
					      0xffff),
	[MOTU424_SMPTE_CTL_START] = SMPTE_INT("SMPTE Generator Start Time", 4,
					      0, 59),
	[MOTU424_SMPTE_CTL_GENERATE] = SMPTE_BOOL("SMPTE Generator Switch"),
	[MOTU424_SMPTE_CTL_GEN_TIME] = SMPTE_RO("SMPTE Generator Time", 4, 0,
						59),
	[MOTU424_SMPTE_CTL_CLOBBERED] =
		SMPTE_BOOL("SMPTE Generator Audio Overwritten"),
};

static int smpte_info(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_info *info)
{
	const struct smpte_ctl_desc *d = &smpte_ctls[kcontrol->private_value];

	info->type = d->boolean ? SNDRV_CTL_ELEM_TYPE_BOOLEAN :
				  SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = d->count;
	info->value.integer.min = d->min;
	info->value.integer.max = d->max;
	if (!d->boolean)
		info->value.integer.step = 1;
	return 0;
}

static int smpte_get(struct snd_kcontrol *kcontrol,
		     struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);
	struct motu424_smpte *s = &m->smpte;
	long *v = value->value.integer.value;
	u8 tc[4];
	unsigned int i;

	mutex_lock(&m->mutex);
	switch (kcontrol->private_value) {
	case MOTU424_SMPTE_CTL_FORMAT:
		v[0] = s->format;
		break;
	case MOTU424_SMPTE_CTL_FOLLOW:
		v[0] = s->follow;
		break;
	case MOTU424_SMPTE_CTL_DETECTED:
		v[0] = s->detected;
		break;
	case MOTU424_SMPTE_CTL_SOURCE:
		v[0] = s->prog_source;
		break;
	case MOTU424_SMPTE_CTL_STATE:
		v[0] = s->state;
		break;
	case MOTU424_SMPTE_CTL_STATUS:
		v[0] = s->status[0] & 0x7fffffff;
		v[1] = s->status[1] & 0x7fffffff;
		break;
	case MOTU424_SMPTE_CTL_FW_ADDRESS:
		v[0] = s->fw_address;
		break;
	case MOTU424_SMPTE_CTL_FW_CLOCK:
		v[0] = s->fw_clock;
		break;
	case MOTU424_SMPTE_CTL_DEST:
		v[0] = s->prog_dest;
		break;
	case MOTU424_SMPTE_CTL_LEVEL:
		v[0] = s->level;
		break;
	case MOTU424_SMPTE_CTL_START:
		for (i = 0; i < 4; i++)
			v[i] = s->start[i];
		break;
	case MOTU424_SMPTE_CTL_GENERATE:
		v[0] = s->generate;
		break;
	case MOTU424_SMPTE_CTL_GEN_TIME:
		motu424_smpte_frames_to_tc(s->generate && s->prog_dest ?
					   s->gen_frame : 0, s->format, tc);
		for (i = 0; i < 4; i++)
			v[i] = tc[i];
		break;
	case MOTU424_SMPTE_CTL_CLOBBERED:
		v[0] = s->clobbered;
		break;
	}
	mutex_unlock(&m->mutex);
	return 0;
}

static int smpte_put_locked(struct motu424 *m, unsigned int ctl,
			    const long *v)
{
	struct motu424_smpte *s = &m->smpte;
	unsigned long flags;
	unsigned int i;
	bool valid;
	u8 tc[4];

	switch (ctl) {
	case MOTU424_SMPTE_CTL_FORMAT:
		if (v[0] == s->format)
			return 0;
		s->format = v[0];
		/* Drv::SetFrameRate stores Last30fpsRate with format 3 */
		if (smpte_format_count[s->format] == 3)
			s->last30 = s->format;
		smpte_params_write(m);
		smpte_generator_reprogram(m);
		return 1;
	case MOTU424_SMPTE_CTL_FOLLOW:
		if (!!v[0] == s->follow)
			return 0;
		s->follow = v[0];
		return 1;
	case MOTU424_SMPTE_CTL_SOURCE:
	case MOTU424_SMPTE_CTL_DEST: {
		bool out = ctl == MOTU424_SMPTE_CTL_DEST;
		u16 *code = out ? &s->dest : &s->source;

		spin_lock_irqsave(&m->lock, flags);
		valid = !v[0] || smpte_code_valid(m, v[0], out);
		spin_unlock_irqrestore(&m->lock, flags);
		if (!valid)
			return -EINVAL;
		if (v[0] == *code && v[0] == (out ? s->prog_dest :
						    s->prog_source))
			return 0;
		*code = v[0];
		if (out)
			s->clobbered = false;
		motu424_smpte_refresh(m);
		return 1;
	}
	case MOTU424_SMPTE_CTL_FW_ADDRESS:
	case MOTU424_SMPTE_CTL_FW_CLOCK: {
		int *ms = ctl == MOTU424_SMPTE_CTL_FW_CLOCK ? &s->fw_clock :
							      &s->fw_address;

		if (v[0] == *ms)
			return 0;
		*ms = v[0];
		smpte_freewheel_write(m);
		return 1;
	}
	case MOTU424_SMPTE_CTL_LEVEL:
		if (v[0] == s->level)
			return 0;
		s->level = v[0];
		/* SetSMPTEOutputLevel is ignored for the dedicated output */
		if (!s->regs_jack)
			sm_write(m, MOTU424_CB_SMPTE_LEVEL, s->level);
		return 1;
	case MOTU424_SMPTE_CTL_START:
		for (i = 0; i < 4; i++)
			tc[i] = v[i];
		if (!smpte_tc_valid(tc, s->format))
			return -EINVAL;
		if (!memcmp(tc, s->start, sizeof(tc)))
			return 0;
		memcpy(s->start, tc, sizeof(tc));
		if (s->generate && s->prog_dest) {
			smpte_generator_stop(m);
			smpte_generator_start(m);
		}
		return 1;
	case MOTU424_SMPTE_CTL_GENERATE:
		if (!!v[0] == s->generate)
			return 0;
		s->generate = v[0];
		smpte_generator_stop(m);
		if (s->generate && s->prog_dest) {
			sm_write(m, MOTU424_CB_SMPTE_CLOBBERED, 0);
			smpte_generator_start(m);
		}
		smpte_notify(m, MOTU424_SMPTE_CTL_GEN_TIME);
		return 1;
	case MOTU424_SMPTE_CTL_CLOBBERED:
		/* ClearSMPTEUserNotifications: only clearing is allowed */
		if (v[0] || !s->clobbered)
			return v[0] && !s->clobbered ? -EINVAL : 0;
		s->clobbered = false;
		return 1;
	default:
		return -EPERM;
	}
}

static int smpte_put(struct snd_kcontrol *kcontrol,
		     struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);
	const struct smpte_ctl_desc *d = &smpte_ctls[kcontrol->private_value];
	const long *v = value->value.integer.value;
	unsigned int i;
	int err;

	if (d->readonly)
		return -EPERM;
	for (i = 0; i < d->count; i++)
		if (v[i] < d->min || v[i] > d->max)
			return -EINVAL;
	mutex_lock(&m->mutex);
	if (!m->booted)
		err = -ENODEV;
	else
		err = smpte_put_locked(m, kcontrol->private_value, v);
	mutex_unlock(&m->mutex);
	return err;
}

static int smux_info(struct snd_kcontrol *kcontrol,
		     struct snd_ctl_elem_info *info)
{
	(void)kcontrol;
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = 1;
	info->value.integer.step = 1;
	return 0;
}

static int smux_get(struct snd_kcontrol *kcontrol,
		    struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);

	mutex_lock(&m->mutex);
	value->value.integer.value[0] = m->smux_type1;
	mutex_unlock(&m->mutex);
	return 0;
}

static int smux_put(struct snd_kcontrol *kcontrol,
		    struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);
	long v = value->value.integer.value[0];
	int err = 0;

	if (v < 0 || v > 1)
		return -EINVAL;
	mutex_lock(&m->mutex);
	if (!m->booted) {
		err = -ENODEV;
	} else if (m->smux_type1 != v) {
		m->smux_type1 = v;
		smux_refresh(m);
		err = 1;
	}
	mutex_unlock(&m->mutex);
	return err;
}

/* probe, before the DSP is booted */
void motu424_smpte_defaults(struct motu424 *m)
{
	struct motu424_smpte *s = &m->smpte;

	/* Drv::SetDefaultSMPTESettings(true) */
	memset(s, 0, sizeof(*s));
	s->format = MOTU424_SMPTE_30;
	s->last30 = MOTU424_SMPTE_30;
	s->follow = true;
	s->level = 0x7fff;
	INIT_DELAYED_WORK(&s->poll, smpte_poll);
}

int motu424_smpte_create(struct motu424 *m)
{
	unsigned int i;
	int err;

	for (i = 0; i < MOTU424_SMPTE_CTL_COUNT; i++) {
		const struct smpte_ctl_desc *d = &smpte_ctls[i];
		struct snd_kcontrol_new new = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = d->name,
			.access = (d->readonly ? SNDRV_CTL_ELEM_ACCESS_READ :
				   SNDRV_CTL_ELEM_ACCESS_READWRITE) |
				  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
			.count = 1,
			.info = smpte_info,
			.get = smpte_get,
			.put = smpte_put,
			.private_value = i,
		};
		struct snd_kcontrol *kctl = snd_ctl_new1(&new, m);

		if (!kctl)
			return -ENOMEM;
		err = snd_ctl_add(m->card, kctl);
		if (err)
			return err;
		m->smpte.kctl[i] = kctl;
	}
	if (m->params->dsp == MOTU424_DSP_C6412) {
		static const struct snd_kcontrol_new smux = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = "Card S/MUX Type",
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
			.count = 1,
			.info = smux_info,
			.get = smux_get,
			.put = smux_put,
		};
		struct snd_kcontrol *kctl = snd_ctl_new1(&smux, m);

		if (!kctl)
			return -ENOMEM;
		err = snd_ctl_add(m->card, kctl);
		if (err)
			return err;
	}
	mutex_lock(&m->mutex);
	motu424_smpte_refresh(m);
	mutex_unlock(&m->mutex);
	schedule_delayed_work(&m->smpte.poll, msecs_to_jiffies(SMPTE_POLL_MS));
	return 0;
}

void motu424_smpte_cancel(struct motu424 *m)
{
	mutex_lock(&m->mutex);
	m->smpte.stopped = true;
	mutex_unlock(&m->mutex);
	cancel_delayed_work_sync(&m->smpte.poll);
}
