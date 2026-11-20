// SPDX-License-Identifier: GPL-2.0-only
/*
 * MOTU PCI-424 family: AudioWire interface control.
 *
 * The boxes are configured with MIDI SysEx messages that travel inside the
 * DMA stream (docs/INTERFACES.md sections 3 and 4): one byte per 16-frame
 * block and port, host -> box in the block's header slot, box -> host in
 * its status slot. A box that has not been told to follow the card stays
 * in its power-on state, without a valid clock.
 *
 * Per port: wait for the present bit, send the Universal Device Inquiry,
 * identify the box from the reply, send its state message followed by a
 * state request and compare the echoed state (MsgSender, kext 0x1a840).
 * Family 2 boxes are first asked for their bank and clock lists, because
 * "follow the card" is an index into the clock list they report.
 *
 * Everything except motu424_iface_shutdown() runs in the interrupt handler
 * with m->lock held.
 */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/swab.h>
#include <sound/info.h>
#include "motu424.h"
#include "motu424_tables.h"

static bool trace_midi;
module_param(trace_midi, bool, 0644);
MODULE_PARM_DESC(trace_midi, "Log the SysEx messages exchanged with the interfaces");

#define IFACE_TIMEOUT_TICKS	0x60e	/* MIDI::CalcStdWireInterfaceTimeout */
#define IFACE_MAX_ATTEMPTS	10	/* MsgSender::SendAgain */
#define IFACE_MAX_BACKOFF	4	/* Drv::ProbeUsingPresentMask */
#define IFACE_ABSENT_PERIODS	10
/*
 * Pacing of the bytes sent to a box (MIDI::SubQueue::SubQueue 0x1ad20 and
 * GetNextMessage 0x1b420): one byte every 4 blocks, and 31 blocks between
 * the end of a message and the next one.
 */
#define IFACE_BYTE_TICKS	4
#define IFACE_MSG_TICKS		31

static const char * const wire_state_names[] = {
	[MOTU424_WIRE_ABSENT] = "not connected",
	[MOTU424_WIRE_DETECTED] = "detected",
	[MOTU424_WIRE_QUERY] = "inquiry sent",
	[MOTU424_WIRE_INFO] = "reading description",
	[MOTU424_WIRE_CONFIG] = "state sent",
	[MOTU424_WIRE_READY] = "configured",
	[MOTU424_WIRE_UNCONFIGURED] = "not configured",
};

static unsigned int wire_port(struct motu424 *m, struct motu424_wire *w)
{
	return w - m->wire;
}

/* family 2 boxes report their own name */
static const char *wire_name(const struct motu424_wire *w)
{
	if (!w->info)
		return "";
	return w->name[0] ? w->name : w->info->name;
}

/* ------------------------------------------------------- channel layout */

/* the box can run at this multiplier (slot 0x58 SupportsSampleRateMultiplier) */
bool motu424_wire_supports(const struct motu424_wire *w, unsigned int mult)
{
	return w->info && (w->info->mult_mask & BIT(mult));
}

/*
 * Ports the card doubles at mult (Drv::SetSampleRate): at 2x and 4x the
 * wires run at 2x framing, except those without a box that does this
 * natively (slot 0x60 SupportsSampleRateMultiplierInHardware).
 */
unsigned int motu424_iface_wire_mask(struct motu424 *m, unsigned int mult)
{
	unsigned int i, mask = 0;

	if (mult <= 1)
		return 0;
	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		const struct motu424_wire *w = &m->wire[i];

		if (!w->info || !(w->info->hw_mult_mask & BIT(2)))
			mask |= BIT(i);
	}
	return mask;
}

/*
 * Caller holds m->lock; pos is the wire slot 0..23. A box that does not
 * support mult keeps running at 1x and all its channels are hidden
 * (Drv::DoRefreshChannelInfo). At 4x the HD192's slots 12..23 carry the
 * second sample of each pair of its channels 0..11.
 */
bool motu424_wire_chan_available(const struct motu424_wire *w, unsigned int pos,
				 bool out, unsigned int mult)
{
	if (w->state != MOTU424_WIRE_READY || !motu424_wire_supports(w, mult))
		return false;
	switch (w->info->cls) {
	case MOTU424_IFC_24IO:
		return true;
	case MOTU424_IFC_2408: {
		unsigned int bank = pos / 8;
		unsigned int channel = pos % 8;
		u8 cfg1 = w->state_msg[11];
		unsigned int analog_bank = (cfg1 & MOTU424_2408_CFG1_ANALOG_MASK) >>
					   4;

		if (bank == 2 && (cfg1 & MOTU424_2408_CFG1_C_SPDIF))
			return channel <= 1;
		if ((cfg1 & MOTU424_2408_CFG1_HAS_ANALOG) &&
		    analog_bank && bank == analog_bank - 1)
			return true;
		/* 2x ADAT/TDIF banks: 4 channels on the even slots */
		return mult == 1 || !(channel & 1);
	}
	case MOTU424_IFC_SIMPLE:
		return w->avail[(out ? 4 : 0) + pos / 7] & BIT(pos % 7);
	case MOTU424_IFC_1296:	/* AW_1296::IsChannelAvailable */
		return pos < MOTU424_1296_CHANS;
	default:
		return false;
	}
}

static bool layout_slot_available(struct motu424 *m, unsigned int hw, bool out,
				  unsigned int mult)
{
	return motu424_wire_chan_available(&m->wire[hw / MOTU424_CH_PER_WIRE],
					   hw % MOTU424_CH_PER_WIRE, out, mult);
}

/*
 * The DMA stream carries the channels of the configured boxes in ascending
 * wire-slot order (Drv::MaybeRebuildActiveInTable 0xbde0 and
 * MaybeRebuildActiveOutTable 0xcc40). Outputs go through the DSP mixer,
 * which handles them as pairs (Drv424::SetupRouting 0x18770): a stereo
 * pair feeds slots 2p and 2p + 1; where only even slots are used, a split
 * pair feeds 2p and 2p + 2 and pair p + 1 gets no code. Caller holds m->lock.
 */
void motu424_layout_compute(struct motu424 *m, unsigned int mult,
			    struct motu424_layout *l)
{
	unsigned int hw;
	u64 taken = 0;

	memset(l, 0, sizeof(*l));
	for (hw = 0; hw < MOTU424_MAX_CHANNELS; hw++)
		if (layout_slot_available(m, hw, false, mult))
			l->in_hw[l->n_in++] = hw;
	for (hw = 0; hw < MOTU424_MAX_CHANNELS; hw += 2) {
		unsigned int pair = hw / 2;
		bool left = layout_slot_available(m, hw, true, mult);

		if (taken & BIT_ULL(pair)) {
			l->skip_pairs |= BIT_ULL(pair);
		} else if (left && layout_slot_available(m, hw + 1, true, mult)) {
			l->out_hw[l->n_out++] = hw;
			l->out_hw[l->n_out++] = hw + 1;
			l->out_pairs |= BIT_ULL(pair);
		} else if (left && hw % MOTU424_CH_PER_WIRE + 2 < MOTU424_CH_PER_WIRE &&
			   layout_slot_available(m, hw + 2, true, mult)) {
			l->out_hw[l->n_out++] = hw;
			l->out_hw[l->n_out++] = hw + 2;
			l->out_pairs |= BIT_ULL(pair);
			l->split_pairs |= BIT_ULL(pair);
			taken |= BIT_ULL(pair + 1);
		} else if (left || layout_slot_available(m, hw + 1, true, mult)) {
			l->odd = true;
		}
	}
}

/*
 * DMA slots of the channels hw[0..n), which ascend: per wire the primaries,
 * then at 4x their partners at hw + 12 in the same order (Drv::
 * MaybeRebuildActiveInTable 0xbde0 / MaybeRebuildActiveOutTable 0xcc40).
 * Returns the slot count; slot_hw (optional) gets each slot's hw channel.
 */
unsigned int motu424_build_slots(const u8 *hw, unsigned int n, bool quad,
				 u8 slot[2][MOTU424_MAX_CHANNELS], u8 *slot_hw)
{
	unsigned int i = 0, j, base = 0;

	while (i < n) {
		unsigned int wire = hw[i] / MOTU424_CH_PER_WIRE, c = 0;

		while (i + c < n && hw[i + c] / MOTU424_CH_PER_WIRE == wire)
			c++;
		for (j = 0; j < c; j++) {
			slot[0][i + j] = slot[1][i + j] = base + j;
			if (slot_hw)
				slot_hw[base + j] = hw[i + j];
			if (!quad)
				continue;
			slot[1][i + j] = base + c + j;
			if (slot_hw)
				slot_hw[base + c + j] = hw[i + j] +
					MOTU424_QUAD_PARTNER;
		}
		base += quad ? 2 * c : c;
		i += c;
	}
	return base;
}

static void layout_rebuild(struct motu424 *m)
{
	struct motu424_layout l;

	motu424_layout_compute(m, m->mult, &l);
	if (l.n_in != m->n_in || l.n_out != m->n_out ||
	    l.out_pairs != m->out_pairs) {
		dev_info(&m->pci->dev, "%u capture and %u playback channels\n",
			 l.n_in, l.n_out);
		if (l.odd)
			dev_warn(&m->pci->dev,
				 "an output without a stereo partner is left unused\n");
	}
	m->n_in = l.n_in;
	m->n_out = l.n_out;
	memcpy(m->in_hw, l.in_hw, sizeof(m->in_hw));
	memcpy(m->out_hw, l.out_hw, sizeof(m->out_hw));
	m->out_pairs = l.out_pairs;
	m->split_pairs = l.split_pairs;
	m->skip_pairs = l.skip_pairs;
}

/* the set of configured boxes changed; called with m->lock held */
static void layout_update(struct motu424 *m)
{
	if (!m->layout_frozen)
		layout_rebuild(m);
	motu424_qmix_schedule(m);
}

/* open streams keep the layout they were opened with */
void motu424_layout_freeze(struct motu424 *m, bool freeze)
{
	unsigned long flags;

	spin_lock_irqsave(&m->lock, flags);
	m->layout_frozen = freeze;
	if (!freeze)
		layout_rebuild(m);
	spin_unlock_irqrestore(&m->lock, flags);
	if (!freeze)
		motu424_qmix_schedule(m);
}

/* after a multiplier change, even while the streams hold the layout */
void motu424_layout_refresh(struct motu424 *m)
{
	unsigned long flags;

	spin_lock_irqsave(&m->lock, flags);
	layout_rebuild(m);
	spin_unlock_irqrestore(&m->lock, flags);
}

/* MIDI ticks per second: at 2x and 4x a tick is every second block */
static u32 ticks_per_second(struct motu424 *m)
{
	return m->rate / m->mult / MOTU424_FRAMES_PER_BLOCK;
}

static bool tick_reached(struct motu424 *m, u32 deadline)
{
	return (s32)(m->midi_tick - deadline) >= 0;
}

/* ticks until the box has received everything queued, plus the reply timeout */
static u32 wire_tx_deadline(struct motu424 *m, struct motu424_wire *w)
{
	return m->midi_tick + w->tx_len * IFACE_BYTE_TICKS +
	       2 * IFACE_MSG_TICKS + IFACE_TIMEOUT_TICKS;
}

/* queue message a, optionally followed by message b */
static void wire_send(struct motu424 *m, struct motu424_wire *w,
		      const u8 *a, unsigned int alen,
		      const u8 *b, unsigned int blen)
{
	if (WARN_ON(alen + blen > sizeof(w->tx)))
		return;
	memcpy(w->tx, a, alen);
	if (blen)
		memcpy(w->tx + alen, b, blen);
	w->tx_len = alen + blen;
	w->tx_split = alen;
	w->tx_pos = 0;
	w->deadline = wire_tx_deadline(m, w);
	if (trace_midi)
		dev_info(&m->pci->dev, "port %u tx: %*ph\n", wire_port(m, w),
			 w->tx_len, w->tx);
}

static void wire_query(struct motu424 *m, struct motu424_wire *w)
{
	wire_send(m, w, motu424_sysex_dev_inquiry,
		  sizeof(motu424_sysex_dev_inquiry), NULL, 0);
	w->state = MOTU424_WIRE_QUERY;
}

/* state message, then the state request; the box answers with its state */
static void wire_configure(struct motu424 *m, struct motu424_wire *w)
{
	if (w->info->cls == MOTU424_IFC_1296)
		wire_send(m, w, w->state_msg, w->state_len,
			  motu424_sysex_1296_request,
			  sizeof(motu424_sysex_1296_request));
	else
		wire_send(m, w, w->state_msg, w->state_len,
			  motu424_sysex_2408_request,
			  sizeof(motu424_sysex_2408_request));
	w->state = MOTU424_WIRE_CONFIG;
}

static void wire_retry(struct motu424 *m, struct motu424_wire *w)
{
	if (++w->attempts >= IFACE_MAX_ATTEMPTS) {
		dev_warn(&m->pci->dev,
			 "port %u: %s did not confirm its state; leaving it unconfigured\n",
			 wire_port(m, w), wire_name(w));
		w->state = MOTU424_WIRE_UNCONFIGURED;
		layout_update(m);
		return;
	}
	wire_configure(m, w);
}

static void wire_start_config(struct motu424_wire *w, const u8 *msg,
			      unsigned int len)
{
	memcpy(w->state_msg, msg, len);
	w->state_len = len;
	w->attempts = 0;
}

/* ------------------------------------------------- family 2 description */

static const char * const simple_clock_names[] = {
	"card", "internal 44.1k", "internal 48k", "internal 96k", "word clock",
	"bank A", "bank B", "bank C", "bank D", "bank E", "bank F", "bank G",
	"bank H",
};

static void wire_info_request(struct motu424 *m, struct motu424_wire *w)
{
	u8 msg[sizeof(motu424_sysex_simple_str_request)];

	if (w->str_mode) {
		memcpy(msg, motu424_sysex_simple_str_request, sizeof(msg));
		msg[8] = w->step == MOTU424_SIMPLE_STEP_BANK_TYPES ?
			MOTU424_SIMPLE_STR_BANKS : MOTU424_SIMPLE_STR_CLOCKS;
		msg[9] = w->str_idx;
		wire_send(m, w, msg, sizeof(msg), NULL, 0);
	} else {
		memcpy(msg, motu424_sysex_simple_request,
		       sizeof(motu424_sysex_simple_request));
		msg[8] = w->step;
		wire_send(m, w, msg, sizeof(motu424_sysex_simple_request),
			  NULL, 0);
	}
	w->state = MOTU424_WIRE_INFO;
}

/* the kext repeats a request without limit (info::TimedOut 0x1f280) */
static void wire_info_retry(struct motu424 *m, struct motu424_wire *w)
{
	if (++w->attempts >= IFACE_MAX_ATTEMPTS) {
		dev_warn(&m->pci->dev,
			 "port %u: %s does not answer description request %u; it keeps its power-on clock setting\n",
			 wire_port(m, w), wire_name(w), w->step);
		w->state = MOTU424_WIRE_UNCONFIGURED;
		return;
	}
	wire_info_request(m, w);
}

/* b[6] = sum of the bytes from b[7] up to the F7, 7 bits (info::CheckSum) */
static bool simple_checksum_ok(const u8 *b, unsigned int len)
{
	unsigned int i, sum = 0;

	for (i = 7; i < len - 1; i++)
		sum += b[i];
	return b[6] == (sum & 0x7f);
}

static void simple_copy_string(char *dst, size_t size, const u8 *src,
			       unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n && i < size - 1 && src[i]; i++)
		dst[i] = src[i] >= 0x20 && src[i] < 0x7f ? src[i] : '?';
	dst[i] = 0;
}

/* next entry with a custom name, which has to be asked for separately */
static int simple_next_custom(const u8 *types, unsigned int n, int from)
{
	int i;

	for (i = from; i < (int)n; i++)
		if (types[i] == MOTU424_SIMPLE_PERSONALITY_CUSTOM)
			return i;
	return -1;
}

static void wire_info_done(struct motu424 *m, struct motu424_wire *w)
{
	unsigned int port = wire_port(m, w), i;
	int card_clock = -1;
	char buf[200] = "";
	int n = 0;
	u8 msg[sizeof(motu424_sysex_simple_state)];

	for (i = 0; i < w->nbanks; i++) {
		u8 t = w->bank_type[i];

		n += scnprintf(buf + n, sizeof(buf) - n, "%s%s %u", i ? ", " : "",
			       t < ARRAY_SIZE(motu424_simple_personality_names) ?
			       motu424_simple_personality_names[t] : "custom",
			       w->bank_chans[i]);
	}
	dev_info(&m->pci->dev,
		 "port %u: %s: %u banks (%s), inputs %*ph, outputs %*ph\n",
		 port, wire_name(w), w->nbanks, buf, 4, w->avail, 4,
		 w->avail + 4);

	n = 0;
	for (i = 0; i < w->nclocks; i++) {
		u8 t = w->clock_type[i];

		if (t == MOTU424_SIMPLE_CLK_CARD)
			card_clock = i;
		if (t < ARRAY_SIZE(simple_clock_names))
			n += scnprintf(buf + n, sizeof(buf) - n, "%s%s",
				       i ? ", " : "", simple_clock_names[t]);
		else
			n += scnprintf(buf + n, sizeof(buf) - n, "%stype %u",
				       i ? ", " : "", t);
	}
	dev_info(&m->pci->dev, "port %u: %s: clock sources: %s\n", port,
		 wire_name(w), n ? buf : "none");

	if (card_clock < 0) {
		dev_warn(&m->pci->dev,
			 "port %u: %s offers no card clock source; it keeps its power-on clock setting\n",
			 port, wire_name(w));
		w->state = MOTU424_WIRE_UNCONFIGURED;
		return;
	}
	/* AW_SimpleInterface::SendCurStateToBox (kext 0x203c0) */
	memcpy(msg, motu424_sysex_simple_state, sizeof(msg));
	msg[10] = card_clock | MOTU424_SIMPLE_STATE_HOST;
	wire_start_config(w, msg, sizeof(msg));
	wire_configure(m, w);
}

/* AW_SimpleInterface::info::ReplyReceived (kext 0x1f5e0) */
static void wire_info_reply(struct motu424 *m, struct motu424_wire *w,
			    const u8 *b, unsigned int len)
{
	unsigned int port = wire_port(m, w);
	const u8 *data = b + 7;
	unsigned int n;		/* data bytes between the checksum and F7 */
	u8 *types = NULL;
	unsigned int ntypes = 0;
	char str[MOTU424_SIMPLE_NAME_LEN];

	if (len < 8 || memcmp(b, motu424_sysex_simple_request, 4) ||
	    !simple_checksum_ok(b, len)) {
		wire_info_retry(m, w);
		return;
	}
	n = len - 8;

	switch (w->step) {
	case MOTU424_SIMPLE_STEP_GENERAL:
		if (n < 2)
			goto bad;
		if (data[0] > MOTU424_SIMPLE_MAX_BANKS ||
		    data[1] > MOTU424_SIMPLE_MAX_CLOCKS) {
			dev_warn(&m->pci->dev,
				 "port %u: unexpected description (%u banks, %u clocks)\n",
				 port, data[0], data[1]);
			w->state = MOTU424_WIRE_UNCONFIGURED;
			return;
		}
		w->nbanks = data[0];
		w->nclocks = data[1];
		simple_copy_string(w->name, sizeof(w->name), data + 2, n - 2);
		w->step = MOTU424_SIMPLE_STEP_BANK_TYPES;
		break;
	case MOTU424_SIMPLE_STEP_BANK_TYPES:
		types = w->bank_type;
		ntypes = w->nbanks;
		break;
	case MOTU424_SIMPLE_STEP_CLOCK_TYPES:
		types = w->clock_type;
		ntypes = w->nclocks;
		break;
	case MOTU424_SIMPLE_STEP_BANK_CHANS:
		if (n < w->nbanks)
			goto bad;
		memcpy(w->bank_chans, data, w->nbanks);
		w->step = MOTU424_SIMPLE_STEP_CLOCK_TYPES;
		break;
	case MOTU424_SIMPLE_STEP_AVAILABLE:
		if (n < sizeof(w->avail))
			goto bad;
		memcpy(w->avail, data, sizeof(w->avail));
		/* the model word of these boxes is a version number */
		if (swab16(w->model) > MOTU424_SIMPLE_VERSION_DIGI_CONS) {
			w->step = MOTU424_SIMPLE_STEP_DIGI_CONS;
			break;
		}
		wire_info_done(m, w);
		return;
	case MOTU424_SIMPLE_STEP_DIGI_CONS:
		if (n < w->nbanks)
			goto bad;
		dev_info(&m->pci->dev, "port %u: digital connectors per bank: %*ph\n",
			 port, w->nbanks, data);
		wire_info_done(m, w);
		return;
	default:
		return;
	}

	/* a type list, then one request per custom name (info::DoGetTypeInfo) */
	if (types) {
		if (w->str_mode) {
			if (n < 1)
				goto bad;
			simple_copy_string(str, sizeof(str), data + 1, n - 1);
			dev_info(&m->pci->dev, "port %u: %s %d is named \"%s\"\n",
				 port, types == w->bank_type ? "bank" : "clock source",
				 w->str_idx, str);
		} else {
			if (n < ntypes)
				goto bad;
			memcpy(types, data, ntypes);
			w->str_idx = -1;
		}
		w->str_idx = simple_next_custom(types, ntypes, w->str_idx + 1);
		w->str_mode = w->str_idx >= 0;
		if (!w->str_mode)
			w->step++;
	}
	w->attempts = 0;
	wire_info_request(m, w);
	return;
bad:
	wire_info_retry(m, w);
}

/* ------------------------------------------------------- identification */

static u8 wire_1296_output_clock(const struct motu424 *m,
				const struct motu424_wire *w);

static bool wire_is_hd192(const struct motu424_wire *w)
{
	return w->info->id == MOTU424_IFACE_ID_HD192;
}

/*
 * AW_1296::state::GetClockFromUIClock (0x1ddd0): the c0 clock code. The
 * internal clock code follows the card's rate family; on the HD192 at 4x
 * (2x framing on the wire) AES/EBU is not offered and runs as internal.
 */
static u8 wire_1296_clock_code(const struct motu424 *m,
			       const struct motu424_wire *w, bool wire2x)
{
	unsigned int ui = w->clock_source;

	if (ui == MOTU424_1296_UICLK_AES && wire_is_hd192(w) && wire2x)
		ui = MOTU424_1296_UICLK_INTERNAL;
	switch (ui) {
	case MOTU424_1296_UICLK_INTERNAL:
		return m->rate % 11025 ? MOTU424_1296_CLK_INT_48K :
					 MOTU424_1296_CLK_INT_44K1;
	case MOTU424_1296_UICLK_WORD:
		return MOTU424_1296_CLK_WORD;
	case MOTU424_1296_UICLK_AES:
		return MOTU424_1296_CLK_AES;
	default:
		return MOTU424_1296_CLK_CARD;
	}
}

/*
 * AW_1296::state::PackState: bytes 7..10 from the settings and the box's
 * multiplier. HD192: bit 5 of bytes 7 and 9 above 1x, bit 4 of byte 7 and
 * no AES input pair at 4x. 1296: bits 4 and 5 of byte 7 at 2x.
 */
static void wire_1296_pack(const struct motu424 *m, struct motu424_wire *w)
{
	u8 *msg = w->state_msg;
	bool hd = wire_is_hd192(w);
	bool high = hd && w->mult > 1;
	bool wire2x = hd ? w->mult == 4 : w->mult == 2;

	msg[7] = wire_1296_clock_code(m, w, wire2x) |
		 (w->aes_input_src ? 0 : MOTU424_1296_C0_NO_AES_IN_SRC) |
		 (wire2x ? MOTU424_1296_C0_WIRE_2X : 0) |
		 ((hd ? high : wire2x) ? MOTU424_1296_C0_2X : 0) |
		 MOTU424_1296_C0_ACTIVE;
	msg[8] = (w->aes_output_src_mode & 0x0f) |
		 (hd && wire2x ? 0 : (w->aes_input_pair & 0x07) << 4);
	msg[9] = wire_1296_output_clock(m, w) | MOTU424_1296_C2_UPDATE |
		 (high ? MOTU424_1296_C2_HIGH_RATE : 0);
	msg[10] = (w->peak_hold_time << 3) | w->clip_hold_time;
}

/* AW_24IO::SetMessageBytes: bytes 11 and 15 from the settings and byte 10 */
static void wire_24io_pack(struct motu424_wire *w)
{
	u8 *msg = w->state_msg;

	msg[11] = w->input_levels & 0x07;
	if (w->word_out_rate == 1)
		msg[15] = 0;
	else if (w->word_out_rate == 2)
		msg[15] = MOTU424_24IO_B15_WORD_2X;
	else
		msg[15] = msg[10] & MOTU424_2408_CFG0_2X ?
			  MOTU424_24IO_B15_WORD_2X : 0;
}

/* AW_2408::SetMessageBytes: mk3 bytes 14 and 15 from the settings and byte 10 */
static void wire_2408mk3_pack(struct motu424_wire *w)
{
	u8 *msg = w->state_msg;

	if (w->state_len <= 15)
		return;
	msg[14] = (w->input_levels & 0x0f) | w->smpte_b14;
	msg[15] = (msg[10] & MOTU424_2408_CFG0_CLK_MASK) ==
		  MOTU424_2408_CFG0_CLK_CARD ? MOTU424_2408MK3_B15_CARD_CLOCK : 0;
	if (w->word_out_rate == 2)
		msg[15] |= MOTU424_2408MK3_B15_WORD_2X;
	else if (w->word_out_rate != 1)
		msg[15] |= MOTU424_2408MK3_B15_WORD_MATCH;
	if (w->smpte_out)
		msg[15] |= MOTU424_2408MK3_B15_SMPTE_OUT;
}

/* ... plus the mirrored bank when no bank is analog (SetPersonalityForBank) */
static void wire_2408_pack(struct motu424_wire *w)
{
	u8 *msg = w->state_msg;

	if (!(msg[11] & MOTU424_2408_CFG1_HAS_ANALOG))
		msg[11] = (msg[11] & ~MOTU424_2408_CFG1_ANALOG_MASK) |
			  ((w->analog_mirror + 1) <<
			   MOTU424_2408_CFG1_ANALOG_SHIFT);
	wire_2408mk3_pack(w);
}

/*
 * The SetSampleRateMultiplier methods: put mult into the state message.
 * Returns true if the message changed. Caller holds m->lock.
 */
static bool wire_mult_apply(struct motu424 *m, struct motu424_wire *w,
			    unsigned int mult)
{
	u8 old[sizeof(w->state_msg)];
	u8 *msg = w->state_msg;

	if (!motu424_wire_supports(w, mult) || w->mult == mult)
		return false;
	memcpy(old, msg, sizeof(old));
	w->mult = mult;
	switch (w->info->cls) {
	case MOTU424_IFC_2408:
		msg[10] = (msg[10] & ~MOTU424_2408_CFG0_2X) |
			  (mult == 2 ? MOTU424_2408_CFG0_2X : 0);
		break;
	case MOTU424_IFC_24IO:
		msg[10] = (msg[10] & ~MOTU424_2408_CFG0_2X) |
			  (mult == 2 ? MOTU424_2408_CFG0_2X : 0);
		wire_24io_pack(w);
		break;
	case MOTU424_IFC_1296:
		wire_1296_pack(m, w);
		break;
	default:
		break;
	}
	return memcmp(old, msg, sizeof(old)) != 0;
}

/* MsgSender::BlindSend: the state message alone, no reply expected */
static void wire_send_blind(struct motu424 *m, struct motu424_wire *w)
{
	wire_send(m, w, w->state_msg, w->state_len, NULL, 0);
}

static const struct motu424_iface_info *iface_lookup(u16 family, u16 model)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(motu424_ifaces) - 1; i++) {
		const struct motu424_iface_info *info = &motu424_ifaces[i];

		if (info->family == family &&
		    (info->model == MOTU424_MODEL_ANY || info->model == model))
			return info;
	}
	return &motu424_ifaces[ARRAY_SIZE(motu424_ifaces) - 1];
}

static void wire_identified(struct motu424 *m, struct motu424_wire *w,
			    const u8 *b, unsigned int len)
{
	unsigned int port = wire_port(m, w);
	u8 msg[sizeof(motu424_sysex_2408_state)];

	w->model = MOTU424_INQ_MODEL(b);
	w->info = iface_lookup(MOTU424_INQ_FAMILY(b), w->model);
	w->name[0] = 0;
	w->nbanks = 0;
	w->nclocks = 0;
	w->clock_source = MOTU424_2408_CLK_CARD;
	w->external_clock = false;
	w->mult = 1;
	/* SetDefaultState */
	w->analog_mirror = 0;
	w->input_levels = 0;
	w->word_out_rate = 0;
	dev_info(&m->pci->dev, "port %u: MOTU %s (family %u, model %u; inquiry reply %*ph)\n",
		 port, w->info->name, MOTU424_INQ_FAMILY(b), w->model, len, b);

	switch (w->info->cls) {
	case MOTU424_IFC_24IO:
		/* AW_24IO::Setup: config 0x42 = follow the card's clock, 1x */
		wire_start_config(w, motu424_sysex_24io_state,
				  sizeof(motu424_sysex_24io_state));
		wire_24io_pack(w);
		wire_configure(m, w);
		break;
	case MOTU424_IFC_2408:
		/*
		 * AW_2408::Setup and SetMessageBytes: follow the card's clock;
		 * bank A analog, banks B and C ADAT optical.
		 */
		memcpy(msg, motu424_sysex_2408_state, sizeof(msg));
		msg[10] = MOTU424_2408_CFG0_DEFAULT;
		msg[11] = MOTU424_2408_CFG1_DEFAULT;
		if (w->model == 4) {
			wire_start_config(w, msg, MOTU424_2408MK3_STATE_LEN);
		} else {
			msg[MOTU424_2408_STATE_LEN - 1] = 0xf7;
			wire_start_config(w, msg, MOTU424_2408_STATE_LEN);
		}
		wire_2408_pack(w);
		wire_configure(m, w);
		break;
	case MOTU424_IFC_1296:
		/*
		 * AW_1296::state defaults, then SendCurStateToBox: follow the
		 * card, 1x, AES/EBU input and output without SRC, no input pair
		 * replaced by AES/EBU. Same in MOTUAW.sys. Not hardware-tested.
		 */
		memcpy(msg, motu424_sysex_1296_state, MOTU424_1296_STATE_LEN);
		w->aes_input_pair = 0;
		w->aes_output_src_mode = 0;
		w->aes_output_clock = 0;
		w->aes_input_src = 0;
		w->peak_hold_time = 4;
		w->clip_hold_time = 1;
		w->clock_source = MOTU424_1296_UICLK_CARD;
		msg[7] = MOTU424_1296_CLK_CARD;
		wire_start_config(w, msg, MOTU424_1296_STATE_LEN);
		wire_1296_pack(m, w);
		wire_configure(m, w);
		break;
	case MOTU424_IFC_SIMPLE:
		/* AW_SimpleInterface::info::GetInfo (kext 0x1f290) */
		w->step = MOTU424_SIMPLE_STEP_GENERAL;
		w->str_mode = false;
		w->attempts = 0;
		wire_info_request(m, w);
		break;
	default:
		dev_warn(&m->pci->dev,
			 "port %u: state setup for the %s is not implemented yet; it keeps its power-on clock setting\n",
			 port, w->info->name);
		w->state = MOTU424_WIRE_UNCONFIGURED;
		break;
	}
}

/* the ReplyGood methods of the three Sender classes */
static bool wire_state_confirmed(struct motu424_wire *w, const u8 *b,
				 unsigned int len)
{
	const u8 *s = w->state_msg;

	/* AW_1296::state::ReplyGood 0x1df80 */
	if (w->info->cls == MOTU424_IFC_1296)
		return len >= MOTU424_1296_REPLY_MIN && b[7] == s[7] &&
		       b[8] == s[8] && b[10] == s[10] &&
		       !((b[9] ^ s[9]) & ~MOTU424_1296_C2_UPDATE);

	if (len < w->state_len || b[10] != s[10])
		return false;
	switch (w->info->cls) {
	case MOTU424_IFC_24IO:
		return b[11] == s[11] && b[15] == s[15];
	case MOTU424_IFC_2408:
		return b[11] == s[11] &&
		       (w->state_len <= 14 || b[14] == s[14]) &&
		       (w->state_len <= 15 || b[15] == s[15]);
	default:
		return true;
	}
}

/* what the kext sends when it closes (the Breakdown methods) */
static void wire_release_msg(struct motu424_wire *w)
{
	u8 *s = w->state_msg;

	switch (w->info->cls) {
	case MOTU424_IFC_24IO:
		s[10] = MOTU424_24IO_CFG_RELEASE;
		wire_24io_pack(w);
		break;
	case MOTU424_IFC_2408:
		s[10] = MOTU424_2408_CFG0_RELEASE;
		s[11] = 0;
		wire_2408mk3_pack(w);
		break;
	case MOTU424_IFC_1296:	/* AW_1296::Breakdown: Send(false) */
		s[7] &= ~MOTU424_1296_C0_ACTIVE;
		s[9] &= ~MOTU424_1296_C2_UPDATE;
		break;
	default:
		s[10] &= ~MOTU424_SIMPLE_STATE_HOST;
		break;
	}
}

static bool wire_sysex(struct motu424 *m, struct motu424_wire *w)
{
	const u8 *b = w->rx;
	unsigned int len = w->rx_len, port = wire_port(m, w);
	bool protocol = len >= 4 &&
		((b[1] == 0x00 && b[2] == 0x00 && b[3] == 0x33) ||
		 (len >= 5 && b[1] == 0x7e && b[3] == 0x06 &&
		  b[4] == 0x02));

	if (trace_midi)
		dev_info(&m->pci->dev, "port %u rx: %*ph\n", port, len, b);
	if (!protocol)
		return false;

	/* a box that was power-cycled announces itself; byte 5 varies */
	if (len == sizeof(motu424_sysex_powerup) &&
	    !memcmp(b, motu424_sysex_powerup, 5) &&
	    !memcmp(b + 6, motu424_sysex_powerup + 6, 4)) {
		dev_info(&m->pci->dev, "port %u: interface powered up\n", port);
		w->info = NULL;
		w->name[0] = 0;
		w->backoff = 0;
		wire_query(m, w);
		layout_update(m);
		return true;
	}

	switch (w->state) {
	case MOTU424_WIRE_QUERY:
		if (len >= MOTU424_INQ_REPLY_MIN && b[1] == 0x7e &&
		    b[3] == 0x06 && b[4] == 0x02)
			wire_identified(m, w, b, len);
		break;
	case MOTU424_WIRE_INFO:
		wire_info_reply(m, w, b, len);
		break;
	case MOTU424_WIRE_CONFIG:
		if (wire_state_confirmed(w, b, len)) {
			w->state = MOTU424_WIRE_READY;
			dev_info(&m->pci->dev,
				 "port %u: %s configured to follow the card clock (state %*ph)\n",
				 port, wire_name(w), len, b);
			/* Drv: a box found above 1x is set up at 1x, then switched */
			if (wire_mult_apply(m, w, m->mult))
				wire_send_blind(m, w);
			layout_update(m);
		} else {
			wire_retry(m, w);
		}
		break;
	default:
		break;
	}
	return true;
}

static void wire_rawmidi_sysex(struct motu424 *m, struct motu424_wire *w)
{
	unsigned int port = wire_port(m, w), i;

	for (i = 0; i < w->rx_len; i++)
		motu424_rawmidi_receive(m, port, w->rx[i]);
}

/* MIDI::Inbuffer::Take reassembles driver SysEx; all other MIDI is a port. */
static void wire_rx_byte(struct motu424 *m, struct motu424_wire *w, u8 c)
{
	unsigned int port = wire_port(m, w);

	if (c >= 0xf8) {
		w->rx_bytes++;
		motu424_rawmidi_receive(m, port, c);
		return;
	}
	w->rx_bytes++;
	if (c == 0xf0) {
		if (w->rx_active)
			wire_rawmidi_sysex(m, w);
		w->rx_len = 0;
		w->rx_active = true;
	} else if (!w->rx_active) {
		motu424_rawmidi_receive(m, port, c);
		return;
	} else if ((c & 0x80) && c != 0xf7) {
		w->rx_active = false;
		wire_rawmidi_sysex(m, w);
		motu424_rawmidi_receive(m, port, c);
		return;
	}
	if (w->rx_len >= sizeof(w->rx)) {
		w->rx_active = false;
		wire_rawmidi_sysex(m, w);
		motu424_rawmidi_receive(m, port, c);
		return;
	}
	w->rx[w->rx_len++] = c;
	if (c == 0xf7) {
		w->rx_active = false;
		if (!wire_sysex(m, w))
			wire_rawmidi_sysex(m, w);
	}
}

/*
 * One 16-frame block: take the MIDI bytes from the status slot of a
 * completed block and fill the header slot of a block that is still to be
 * played (Drv::DoOneMIDIPacket 0x135a0). The header is always rewritten,
 * otherwise a byte would be sent again each time the ring wraps.
 */
/*
 * The byte a port received, or -1. When the card doubles a port, only every
 * second packet is taken, and status flag bit 6 + port selects its low half
 * (Drv424::UnpackStatusBuffer 0x19ee0).
 */
static int status_midi_byte(struct motu424 *m, unsigned int port, u32 v,
			    u32 flags, bool doubled)
{
	if (doubled) {
		m->midi_toggle ^= BIT(port);
		if (m->midi_toggle & BIT(port))
			return -1;
		if (flags & MOTU424_ST_MIDI_LOW_SEL(port))
			return v & MOTU424_ST_MIDI_LO_VALID ?
			       MOTU424_ST_MIDI_LO_BYTE(v) : -1;
	}
	return v & MOTU424_ST_MIDI_HI_VALID ? MOTU424_ST_MIDI_HI_BYTE(v) : -1;
}

void motu424_midi_block(struct motu424 *m, const __le32 *status, __le32 *header)
{
	u32 flags = le32_to_cpu(status[MOTU424_ST_FLAGS]);
	unsigned int present = MOTU424_ST_PRESENT_MASK(flags);
	unsigned int i;

	memset(header, 0, MOTU424_MIDIOUT_DWORDS * sizeof(*header));
	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		struct motu424_wire *w = &m->wire[i];
		u32 v = le32_to_cpu(status[MOTU424_ST_MIDI(i)]);
		/* ND424::IsMIDIDoubled: the card doubles this port */
		bool doubled = m->mult > 1 && (m->wire_mask & BIT(i));
		int c = (present & BIT(i)) ?
			status_midi_byte(m, i, v, flags, doubled) : -1;

		/* Drv424::UnpackStatusBuffer ignores ports without a box */
		if (c >= 0)
			wire_rx_byte(m, w, c);
		if (w->tx_pos < w->tx_len && tick_reached(m, w->tx_next)) {
			__le32 word = cpu_to_le32(
				MOTU424_MIDIOUT_WORD(i, w->tx[w->tx_pos++]));

			header[MOTU424_MIDIOUT_IDX(i)] = word;
			if (doubled)
				header[MOTU424_MIDIOUT_IDX2(i)] = word;
			w->tx_next = m->midi_tick +
				(w->tx_pos == w->tx_split ||
				 w->tx_pos == w->tx_len ?
				 IFACE_MSG_TICKS : IFACE_BYTE_TICKS);
		} else if (w->tx_pos >= w->tx_len &&
			   w->state == MOTU424_WIRE_READY &&
			   (present & BIT(i)) && tick_reached(m, w->tx_next)) {
			u8 byte;

			if (motu424_rawmidi_transmit(m, i, &byte)) {
				__le32 word = cpu_to_le32(
					MOTU424_MIDIOUT_WORD(i, byte));

				header[MOTU424_MIDIOUT_IDX(i)] = word;
				if (doubled)
					header[MOTU424_MIDIOUT_IDX2(i)] = word;
				w->tx_next = m->midi_tick + IFACE_BYTE_TICKS;
			}
		}
	}
	/* Drv::DoOneMIDIPacket: above 1x the tick advances every second packet */
	if (m->mult > 1) {
		m->midi_half = !m->midi_half;
		if (m->midi_half)
			return;
	}
	m->midi_tick++;
}

/* once per period, with the present mask of its last block */
void motu424_iface_period(struct motu424 *m, unsigned int present)
{
	unsigned int i;

	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		struct motu424_wire *w = &m->wire[i];

		/* keep an idle port's pacing tick from going stale */
		if (w->tx_pos >= w->tx_len && tick_reached(m, w->tx_next))
			w->tx_next = m->midi_tick;

		if (!(present & BIT(i))) {
			if (w->state == MOTU424_WIRE_ABSENT ||
			    ++w->absent_periods < IFACE_ABSENT_PERIODS)
				continue;
			if (w->info)
				dev_info(&m->pci->dev,
					 "port %u: %s disconnected\n", i,
					 wire_name(w));
			memset(w, 0, sizeof(*w));
			layout_update(m);
			continue;
		}
		w->absent_periods = 0;

		switch (w->state) {
		case MOTU424_WIRE_ABSENT:
			/* the kext waits a second before the first inquiry */
			w->state = MOTU424_WIRE_DETECTED;
			w->backoff = 0;
			w->deadline = m->midi_tick + ticks_per_second(m);
			break;
		case MOTU424_WIRE_DETECTED:
			if (tick_reached(m, w->deadline))
				wire_query(m, w);
			break;
		case MOTU424_WIRE_QUERY:
			if (!tick_reached(m, w->deadline))
				break;
			if (w->backoff < IFACE_MAX_BACKOFF)
				w->backoff++;
			if (w->backoff == 2)
				dev_warn(&m->pci->dev,
					 "port %u: no reply to the device inquiry yet, retrying\n",
					 i);
			w->state = MOTU424_WIRE_DETECTED;
			w->deadline = m->midi_tick +
				(ticks_per_second(m) << w->backoff);
			break;
		case MOTU424_WIRE_INFO:
			if (tick_reached(m, w->deadline))
				wire_info_retry(m, w);
			break;
		case MOTU424_WIRE_CONFIG:
			if (tick_reached(m, w->deadline))
				wire_retry(m, w);
			break;
		default:
			break;
		}
	}
}

/* the engine was stopped and its buffer cleared: resend what was cut off */
void motu424_iface_engine_restart(struct motu424 *m)
{
	unsigned int i;

	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		struct motu424_wire *w = &m->wire[i];

		w->rx_active = false;
		if (w->state == MOTU424_WIRE_QUERY ||
		    w->state == MOTU424_WIRE_INFO ||
		    w->state == MOTU424_WIRE_CONFIG) {
			w->tx_pos = 0;
			w->deadline = wire_tx_deadline(m, w);
		}
	}
}

static bool iface_tx_pending(struct motu424 *m)
{
	unsigned long flags;
	bool pending = false;
	unsigned int i;

	spin_lock_irqsave(&m->lock, flags);
	for (i = 0; i < MOTU424_MAX_WIRES; i++)
		pending |= m->wire[i].tx_pos < m->wire[i].tx_len;
	spin_unlock_irqrestore(&m->lock, flags);
	return pending;
}

/* true once every port that reports a box has been set up or given up on */
static bool iface_settled(struct motu424 *m)
{
	unsigned long flags;
	bool settled = true;
	unsigned int i;

	spin_lock_irqsave(&m->lock, flags);
	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		enum motu424_wire_state st = m->wire[i].state;

		if (st == MOTU424_WIRE_ABSENT)
			settled &= !(m->present_mask & BIT(i));
		else
			settled &= st == MOTU424_WIRE_READY ||
				   st == MOTU424_WIRE_UNCONFIGURED;
	}
	spin_unlock_irqrestore(&m->lock, flags);
	return settled;
}

/*
 * Give the boxes that are connected at load time the chance to be set up
 * before the card is registered, so that the first open sees their channels.
 */
void motu424_iface_wait_settled(struct motu424 *m)
{
	unsigned int i;

	msleep(500);
	for (i = 0; i < 90 && !iface_settled(m); i++)
		msleep(50);
	if (!iface_settled(m))
		dev_info(&m->pci->dev,
			 "still identifying interfaces; their channels appear when they answer\n");
}

/*
 * Before the card goes away, release the boxes as the kext does when it
 * closes (the Breakdown methods). Needs the engine to be running.
 */
void motu424_iface_shutdown(struct motu424 *m)
{
	unsigned long flags;
	bool queued = false;
	unsigned int i;

	spin_lock_irqsave(&m->lock, flags);
	for (i = 0; m->running && i < MOTU424_MAX_WIRES; i++) {
		struct motu424_wire *w = &m->wire[i];

		if (!w->info || (w->state != MOTU424_WIRE_READY &&
				 w->state != MOTU424_WIRE_CONFIG))
			continue;
		wire_release_msg(w);
		wire_send(m, w, w->state_msg, w->state_len, NULL, 0);
		w->state = MOTU424_WIRE_UNCONFIGURED;
		queued = true;
	}
	spin_unlock_irqrestore(&m->lock, flags);
	if (!queued)
		return;

	for (i = 0; i < 50 && iface_tx_pending(m); i++)
		msleep(10);
	/* the last bytes sit in a period that has yet to be played */
	msleep(3 * 1024 * 1000 / m->rate + 10);
}

static int iface_control_value(const struct motu424_wire *w,
			       enum motu424_iface_control control,
			       long *value)
{
	switch (control) {
	case MOTU424_IFACE_2408_ANALOG_BANK:
		if (!(w->state_msg[11] & MOTU424_2408_CFG1_HAS_ANALOG)) {
			*value = MOTU424_2408_ANALOG_NONE;
			break;
		}
		*value = ((w->state_msg[11] &
			   MOTU424_2408_CFG1_ANALOG_MASK) >> 4);
		if (*value)
			(*value)--;
		break;
	case MOTU424_IFACE_2408_BANK_A_FORMAT:
		*value = !!(w->state_msg[11] & MOTU424_2408_CFG1_TDIF(0));
		break;
	case MOTU424_IFACE_2408_BANK_B_FORMAT:
		*value = !!(w->state_msg[11] & MOTU424_2408_CFG1_TDIF(1));
		break;
	case MOTU424_IFACE_2408_BANK_C_FORMAT:
		if (w->state_msg[11] & MOTU424_2408_CFG1_C_SPDIF)
			*value = 2;
		else
			*value = !!(w->state_msg[11] &
				    MOTU424_2408_CFG1_TDIF(2));
		break;
	case MOTU424_IFACE_2408_CLOCK_SOURCE:
	case MOTU424_IFACE_1296_CLOCK_SOURCE:
		*value = w->clock_source;
		break;
	case MOTU424_IFACE_AES_INPUT_PAIR:
		*value = w->aes_input_pair;
		break;
	case MOTU424_IFACE_AES_OUTPUT_SRC_MODE:
		*value = w->aes_output_src_mode;
		break;
	case MOTU424_IFACE_AES_OUTPUT_CLOCK:
		*value = w->aes_output_clock;
		break;
	case MOTU424_IFACE_AES_INPUT_SRC:
		*value = w->aes_input_src;
		break;
	case MOTU424_IFACE_PEAK_HOLD_TIME:
		*value = w->peak_hold_time;
		break;
	case MOTU424_IFACE_CLIP_HOLD_TIME:
		*value = w->clip_hold_time;
		break;
	case MOTU424_IFACE_2408_ANALOG_MIRROR:
		*value = w->analog_mirror;
		break;
	case MOTU424_IFACE_WORD_OUT_RATE:
		*value = w->word_out_rate;
		break;
	case MOTU424_IFACE_24IO_LEVEL_1_8 ... MOTU424_IFACE_24IO_LEVEL_17_24:
		*value = !!(w->input_levels &
			    BIT(control - MOTU424_IFACE_24IO_LEVEL_1_8));
		break;
	case MOTU424_IFACE_2408MK3_LEVEL_1_2 ... MOTU424_IFACE_2408MK3_LEVEL_7_8:
		*value = !!(w->input_levels &
			    BIT(control - MOTU424_IFACE_2408MK3_LEVEL_1_2));
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static bool iface_control_is_2408(enum motu424_iface_control control)
{
	return (control >= MOTU424_IFACE_2408_ANALOG_BANK &&
		control <= MOTU424_IFACE_2408_CLOCK_SOURCE) ||
	       control == MOTU424_IFACE_2408_ANALOG_MIRROR;
}

static bool iface_control_is_2408mk3(enum motu424_iface_control control)
{
	return control == MOTU424_IFACE_WORD_OUT_RATE ||
	       (control >= MOTU424_IFACE_2408MK3_LEVEL_1_2 &&
		control <= MOTU424_IFACE_2408MK3_LEVEL_7_8);
}

static bool iface_control_is_24io(enum motu424_iface_control control)
{
	return control == MOTU424_IFACE_WORD_OUT_RATE ||
	       (control >= MOTU424_IFACE_24IO_LEVEL_1_8 &&
		control <= MOTU424_IFACE_24IO_LEVEL_17_24);
}

/* controls that change the channel layout or the box's clock */
static bool iface_control_is_2408_layout(enum motu424_iface_control control)
{
	return control >= MOTU424_IFACE_2408_ANALOG_BANK &&
	       control <= MOTU424_IFACE_2408_CLOCK_SOURCE;
}

static bool iface_control_is_1296(enum motu424_iface_control control)
{
	return (control >= MOTU424_IFACE_AES_INPUT_PAIR &&
		control <= MOTU424_IFACE_CLIP_HOLD_TIME) ||
	       control == MOTU424_IFACE_1296_CLOCK_SOURCE;
}

/* controls that set the box's own clock */
static bool iface_control_is_clock(enum motu424_iface_control control)
{
	return control == MOTU424_IFACE_2408_CLOCK_SOURCE ||
	       control == MOTU424_IFACE_1296_CLOCK_SOURCE;
}

static bool iface_control_applies(const struct motu424_wire *w,
				  enum motu424_iface_control control)
{
	if (!w->info)
		return false;
	switch (w->info->cls) {
	case MOTU424_IFC_2408:
		return iface_control_is_2408(control) ||
		       (w->state_len > 15 && iface_control_is_2408mk3(control));
	case MOTU424_IFC_24IO:
		return iface_control_is_24io(control);
	case MOTU424_IFC_1296:
		return iface_control_is_1296(control);
	default:
		return false;
	}
}

/* whether the box on port has this control */
bool motu424_iface_control_available(struct motu424 *m, unsigned int port,
				     enum motu424_iface_control control)
{
	unsigned long flags;
	bool available;

	if (port >= MOTU424_MAX_WIRES)
		return false;
	spin_lock_irqsave(&m->lock, flags);
	available = iface_control_applies(&m->wire[port], control);
	spin_unlock_irqrestore(&m->lock, flags);
	return available;
}

static u8 wire_1296_output_clock(const struct motu424 *m,
				const struct motu424_wire *w)
{
	static const unsigned int rates[] = {
		0, 0, 0, 44100, 48000, 88200, 96000,
		44100, 48000, 88200, 96000,
	};
	unsigned int mode = w->aes_output_clock;

	/* the kext's table has no 176.4/192k entries: keep the last value */
	if (m->rate > 96000)
		return w->state_msg[9] & 0x0f;
	if (mode == 0)
		return 0;
	if (mode == 1)
		return (w->state_msg[7] & MOTU424_1296_C0_CLK_MASK) ? 1 : 0;
	if (mode == 2)
		return 2;
	if (mode <= 10 && rates[mode] == m->rate)
		return 0;
	return mode;
}

int motu424_iface_control_get(struct motu424 *m, unsigned int port,
			      enum motu424_iface_control control, long *value)
{
	struct motu424_wire *w;
	unsigned long flags;
	int err;

	if (port >= MOTU424_MAX_WIRES || !value)
		return -EINVAL;
	spin_lock_irqsave(&m->lock, flags);
	w = &m->wire[port];
	if (!iface_control_applies(w, control))
		err = -ENODEV;
	else
		err = iface_control_value(w, control, value);
	spin_unlock_irqrestore(&m->lock, flags);
	return err;
}

int motu424_iface_control_put(struct motu424 *m, unsigned int port,
			      enum motu424_iface_control control, long value)
{
	struct motu424_wire *w;
	unsigned long flags;
	long old_value;
	int err;

	if (port >= MOTU424_MAX_WIRES)
		return -EINVAL;
	switch (control) {
	case MOTU424_IFACE_2408_ANALOG_BANK:
		if (value < 0 || value > MOTU424_2408_ANALOG_NONE)
			return -EINVAL;
		break;
	case MOTU424_IFACE_2408_BANK_A_FORMAT:
	case MOTU424_IFACE_2408_BANK_B_FORMAT:
		if (value < 0 || value > 1)
			return -EINVAL;
		break;
	case MOTU424_IFACE_2408_BANK_C_FORMAT:
	case MOTU424_IFACE_2408_ANALOG_MIRROR:
	case MOTU424_IFACE_WORD_OUT_RATE:
		if (value < 0 || value > 2)
			return -EINVAL;
		break;
	case MOTU424_IFACE_2408_CLOCK_SOURCE:
		if (value < MOTU424_2408_CLK_WORD ||
		    value > MOTU424_2408_CLK_CARD)
			return -EINVAL;
		break;
	case MOTU424_IFACE_1296_CLOCK_SOURCE:
		if (value < MOTU424_1296_UICLK_INTERNAL ||
		    value > MOTU424_1296_UICLK_CARD)
			return -EINVAL;
		break;
	case MOTU424_IFACE_AES_INPUT_PAIR:
		if (value < 0 || value > 6)
			return -EINVAL;
		break;
	case MOTU424_IFACE_AES_OUTPUT_SRC_MODE:
		if (value < 0 || value > 13 ||
		    (value > 5 && value < 8))
			return -EINVAL;
		break;
	case MOTU424_IFACE_AES_OUTPUT_CLOCK:
		if (value < 0 || value > 10)
			return -EINVAL;
		break;
	case MOTU424_IFACE_AES_INPUT_SRC:
	case MOTU424_IFACE_24IO_LEVEL_1_8 ... MOTU424_IFACE_24IO_LEVEL_17_24:
	case MOTU424_IFACE_2408MK3_LEVEL_1_2 ... MOTU424_IFACE_2408MK3_LEVEL_7_8:
		if (value < 0 || value > 1)
			return -EINVAL;
		break;
	case MOTU424_IFACE_PEAK_HOLD_TIME:
	case MOTU424_IFACE_CLIP_HOLD_TIME:
		if (value < 0 || value > 7)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&m->lock, flags);
	w = &m->wire[port];
	if (!iface_control_applies(w, control)) {
		err = -ENODEV;
		goto out;
	}
	if (w->state != MOTU424_WIRE_READY || w->tx_pos < w->tx_len) {
		err = -EBUSY;
		goto out;
	}
	if (w->info->cls == MOTU424_IFC_2408 &&
	    iface_control_is_2408_layout(control) && m->layout_frozen) {
		err = -EBUSY;
		goto out;
	}
	if (iface_control_is_clock(control) && m->open_count) {
		err = -EBUSY;
		goto out;
	}
	if ((iface_control_is_clock(control) ||
	     (w->info->cls == MOTU424_IFC_2408 &&
	      iface_control_is_2408_layout(control))) &&
	    m->word_source == MOTU424_WS_WIRE0 + port) {
		err = -EBUSY;
		goto out;
	}
	err = iface_control_value(w, control, &old_value);
	if (err || old_value == value)
		goto out;

	switch (control) {
	case MOTU424_IFACE_1296_CLOCK_SOURCE:
		w->clock_source = value;
		w->external_clock = value != MOTU424_1296_UICLK_CARD;
		goto send;
	case MOTU424_IFACE_2408_ANALOG_MIRROR:
		w->analog_mirror = value;
		goto send;
	case MOTU424_IFACE_WORD_OUT_RATE:
		w->word_out_rate = value;
		goto send;
	case MOTU424_IFACE_24IO_LEVEL_1_8 ... MOTU424_IFACE_24IO_LEVEL_17_24:
	case MOTU424_IFACE_2408MK3_LEVEL_1_2 ... MOTU424_IFACE_2408MK3_LEVEL_7_8: {
		unsigned int bit = control >= MOTU424_IFACE_2408MK3_LEVEL_1_2 ?
			control - MOTU424_IFACE_2408MK3_LEVEL_1_2 :
			control - MOTU424_IFACE_24IO_LEVEL_1_8;

		if (value)
			w->input_levels |= BIT(bit);
		else
			w->input_levels &= ~BIT(bit);
		goto send;
	}
	default:
		break;
	}

	if (w->info->cls == MOTU424_IFC_2408) {
		u8 *cfg1 = &w->state_msg[11];

		switch (control) {
		case MOTU424_IFACE_2408_ANALOG_BANK:
			if (value == MOTU424_2408_ANALOG_NONE) {
				/* the outputs mirror AnalogMirror (pack) */
				*cfg1 &= ~MOTU424_2408_CFG1_HAS_ANALOG;
				break;
			}
			if (w->clock_source == value + 1) {
				err = -EINVAL;
				goto out;
			}
			if (value == 2 &&
			    (*cfg1 & MOTU424_2408_CFG1_C_SPDIF)) {
				err = -EINVAL;
				goto out;
			}
			*cfg1 = (*cfg1 & ~(MOTU424_2408_CFG1_HAS_ANALOG |
					  MOTU424_2408_CFG1_ANALOG_MASK)) |
				MOTU424_2408_CFG1_HAS_ANALOG |
				((value + 1) <<
				 MOTU424_2408_CFG1_ANALOG_SHIFT);
			break;
		case MOTU424_IFACE_2408_BANK_A_FORMAT:
			*cfg1 = (*cfg1 & ~MOTU424_2408_CFG1_TDIF(0)) |
				(value ? MOTU424_2408_CFG1_TDIF(0) : 0);
			break;
		case MOTU424_IFACE_2408_BANK_B_FORMAT:
			*cfg1 = (*cfg1 & ~MOTU424_2408_CFG1_TDIF(1)) |
				(value ? MOTU424_2408_CFG1_TDIF(1) : 0);
			break;
		case MOTU424_IFACE_2408_BANK_C_FORMAT:
			if ((value == 2 &&
			     w->clock_source == MOTU424_2408_CLK_BANK_C) ||
			    (value != 2 &&
			     w->clock_source == MOTU424_2408_CLK_SPDIF)) {
				err = -EINVAL;
				goto out;
			}
			if (value == 2 &&
			    (*cfg1 & MOTU424_2408_CFG1_HAS_ANALOG) &&
			    (*cfg1 & MOTU424_2408_CFG1_ANALOG_MASK) ==
			    (3 << MOTU424_2408_CFG1_ANALOG_SHIFT)) {
				err = -EINVAL;
				goto out;
			}
			*cfg1 &= ~(MOTU424_2408_CFG1_TDIF(2) |
				   MOTU424_2408_CFG1_C_SPDIF);
			if (value == 1)
				*cfg1 |= MOTU424_2408_CFG1_TDIF(2);
			else if (value == 2)
				*cfg1 |= MOTU424_2408_CFG1_C_SPDIF;
			break;
		case MOTU424_IFACE_2408_CLOCK_SOURCE:
			if (value >= MOTU424_2408_CLK_BANK_A &&
			    value <= MOTU424_2408_CLK_BANK_C) {
				unsigned int analog_bank =
					(*cfg1 & MOTU424_2408_CFG1_ANALOG_MASK) >> 4;

				if ((*cfg1 & MOTU424_2408_CFG1_HAS_ANALOG) &&
				    analog_bank == value) {
					err = -EINVAL;
					goto out;
				}
				if (value == MOTU424_2408_CLK_BANK_C &&
				    (*cfg1 & MOTU424_2408_CFG1_C_SPDIF)) {
					err = -EINVAL;
					goto out;
				}
			} else if (value == MOTU424_2408_CLK_SPDIF &&
				   !(*cfg1 & MOTU424_2408_CFG1_C_SPDIF)) {
				err = -EINVAL;
				goto out;
			}
			if (value == MOTU424_2408_CLK_CARD)
				w->state_msg[10] =
					(w->state_msg[10] & 0xb8) |
					MOTU424_2408_CFG0_DEFAULT;
			else
				w->state_msg[10] =
					(w->state_msg[10] &
					 MOTU424_2408_CFG0_2X) |
					MOTU424_2408_CFG0_ALWAYS |
					motu424_2408_clock_bits[value];
			w->clock_source = value;
			w->external_clock = value != MOTU424_2408_CLK_CARD;
			break;
		default:
			err = -EINVAL;
			goto out;
		}
		goto send;
	}

	switch (control) {
	case MOTU424_IFACE_AES_INPUT_PAIR:
		w->aes_input_pair = value;
		break;
	case MOTU424_IFACE_AES_OUTPUT_SRC_MODE:
		w->aes_output_src_mode = value;
		break;
	case MOTU424_IFACE_AES_OUTPUT_CLOCK:
		w->aes_output_clock = value;
		break;
	case MOTU424_IFACE_AES_INPUT_SRC:
		w->aes_input_src = value;
		break;
	case MOTU424_IFACE_PEAK_HOLD_TIME:
		w->peak_hold_time = value;
		break;
	case MOTU424_IFACE_CLIP_HOLD_TIME:
		w->clip_hold_time = value;
		break;
	default:
		err = -EINVAL;
		goto out;
	}

send:
	switch (w->info->cls) {
	case MOTU424_IFC_2408:
		wire_2408_pack(w);
		break;
	case MOTU424_IFC_24IO:
		wire_24io_pack(w);
		break;
	default:
		wire_1296_pack(m, w);
		break;
	}
	wire_configure(m, w);
	err = 1;
out:
	spin_unlock_irqrestore(&m->lock, flags);
	return err;
}

/*
 * Drv::SetSampleRate, step 4: the multiplier message to every box that
 * supports mult, sent blind (MsgSender::BlindSend). A box still waiting for
 * the echo of its state compares it with the new bytes and is sent them
 * again on a mismatch; a box that is still being identified gets them when
 * it is ready. The caller has drained the MIDI queues.
 */
void motu424_iface_set_mult(struct motu424 *m, unsigned int mult)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&m->lock, flags);
	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		struct motu424_wire *w = &m->wire[i];

		if (w->state != MOTU424_WIRE_READY &&
		    w->state != MOTU424_WIRE_CONFIG)
			continue;
		if (wire_mult_apply(m, w, mult) && w->state == MOTU424_WIRE_READY)
			wire_send_blind(m, w);
	}
	spin_unlock_irqrestore(&m->lock, flags);
}

/* MIDI::WaitForMIDIToFinish; needs the engine to be running */
int motu424_iface_drain(struct motu424 *m)
{
	unsigned int i;

	for (i = 0; i < 100 && iface_tx_pending(m); i++)
		msleep(10);
	return iface_tx_pending(m) ? -ETIMEDOUT : 0;
}

/*
 * Interface SetSampleRate (slot 0xc0) after a rate change. 24I/O, 2408
 * and self-describing boxes follow the card and have no rate field; only the
 * 1296/HD192 AES output clock is translated using the card rate. A box whose
 * confirmation is pending compares its reply with the new bytes and is sent
 * them again on a mismatch.
 */
void motu424_iface_rate_changed(struct motu424 *m)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&m->lock, flags);
	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		struct motu424_wire *w = &m->wire[i];

		if (!w->info || w->info->cls != MOTU424_IFC_1296 ||
		    (w->state != MOTU424_WIRE_READY &&
		     w->state != MOTU424_WIRE_CONFIG))
			continue;
		wire_1296_pack(m, w);
		if (w->state == MOTU424_WIRE_READY && w->tx_pos >= w->tx_len) {
			w->attempts = 0;
			wire_configure(m, w);
		}
	}
	spin_unlock_irqrestore(&m->lock, flags);
}

/* --------------------------------------------------------- SMPTE, S/MUX */

bool motu424_wire_is_2408mk3(const struct motu424_wire *w)
{
	return w->info && w->info->cls == MOTU424_IFC_2408 &&
	       w->state_len > 15 && w->state == MOTU424_WIRE_READY;
}

/*
 * IsInputAnalog (slot 0x128): the inputs offered as LTC sources. 2408:
 * the analog bank; 24I/O: all; 1296: all but the AES input pair (HD192 at
 * 4x has none); simple boxes: banks of 8 with the "Analog" personality.
 */
bool motu424_wire_input_analog(const struct motu424_wire *w, unsigned int pos)
{
	if (!w->info || w->state != MOTU424_WIRE_READY ||
	    pos >= MOTU424_CH_PER_WIRE)
		return false;
	switch (w->info->cls) {
	case MOTU424_IFC_2408: {
		u8 cfg1 = w->state_msg[11];

		return (cfg1 & MOTU424_2408_CFG1_HAS_ANALOG) &&
		       ((cfg1 & MOTU424_2408_CFG1_ANALOG_MASK) >>
			MOTU424_2408_CFG1_ANALOG_SHIFT) == pos / 8 + 1;
	}
	case MOTU424_IFC_24IO:
		return true;
	case MOTU424_IFC_1296:
		if (wire_is_hd192(w) && w->mult == 4)
			return true;
		return !w->aes_input_pair || w->aes_input_pair != pos / 2 + 1;
	case MOTU424_IFC_SIMPLE:
		return pos / 8 < w->nbanks && w->bank_type[pos / 8] == 0;
	default:
		return false;
	}
}

/*
 * C6412 S/MUX option for port: with Type I selected, byte n is 0xff for
 * each ADAT bank n of a 2408mk3 (meaning of the DSP mask uncertain).
 */
u32 motu424_iface_smux_mask(struct motu424 *m, unsigned int port)
{
	const struct motu424_wire *w;
	unsigned long flags;
	unsigned int bank;
	u32 mask = 0;

	if (!m->smux_type1 || port >= MOTU424_MAX_WIRES)
		return 0;
	spin_lock_irqsave(&m->lock, flags);
	w = &m->wire[port];
	if (motu424_wire_is_2408mk3(w)) {
		u8 cfg1 = w->state_msg[11];

		for (bank = 0; bank < 3; bank++) {
			if ((cfg1 & MOTU424_2408_CFG1_HAS_ANALOG) &&
			    ((cfg1 & MOTU424_2408_CFG1_ANALOG_MASK) >>
			     MOTU424_2408_CFG1_ANALOG_SHIFT) == bank + 1)
				continue;
			if (cfg1 & MOTU424_2408_CFG1_TDIF(bank))
				continue;
			if (bank == 2 && (cfg1 & MOTU424_2408_CFG1_C_SPDIF))
				continue;
			mask |= 0xffu << (8 * bank);
		}
	}
	spin_unlock_irqrestore(&m->lock, flags);
	return mask;
}

/*
 * AW_2408::GetChannelsUsedByDedicatedSMPTEOutput: with bank C on S/PDIF,
 * the 2408mk3's SMPTE output uses wire slots 16 and 17; else none.
 */
unsigned int motu424_iface_smpte_jack_slots(struct motu424 *m,
					    unsigned int port)
{
	unsigned long flags;
	unsigned int n = 0;

	if (port >= MOTU424_MAX_WIRES)
		return 0;
	spin_lock_irqsave(&m->lock, flags);
	if (motu424_wire_is_2408mk3(&m->wire[port]) &&
	    (m->wire[port].state_msg[11] & MOTU424_2408_CFG1_C_SPDIF))
		n = 2;
	spin_unlock_irqrestore(&m->lock, flags);
	return n;
}

/*
 * AW_2408::SetVideoSyncMode and EnableDedicatedSMPTEOutput for every
 * 2408mk3. A box whose state is still being sent or confirmed keeps its
 * old bits and false is returned, so the caller can try again later.
 */
bool motu424_iface_set_smpte(struct motu424 *m,
			     const u8 b14[MOTU424_MAX_WIRES],
			     unsigned int out_mask)
{
	unsigned long flags;
	bool done = true;
	unsigned int i;

	spin_lock_irqsave(&m->lock, flags);
	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		struct motu424_wire *w = &m->wire[i];
		bool out = out_mask & BIT(i);

		if (!w->info || w->info->cls != MOTU424_IFC_2408 ||
		    w->state_len <= 15 || (w->state != MOTU424_WIRE_READY &&
					   w->state != MOTU424_WIRE_CONFIG))
			continue;
		if (w->smpte_b14 == b14[i] && w->smpte_out == out)
			continue;
		if (w->state == MOTU424_WIRE_CONFIG || w->tx_pos < w->tx_len) {
			done = false;
			continue;
		}
		w->smpte_b14 = b14[i];
		w->smpte_out = out;
		wire_2408_pack(w);
		w->attempts = 0;
		wire_configure(m, w);
	}
	spin_unlock_irqrestore(&m->lock, flags);
	return done;
}

struct proc_snapshot {
	struct motu424_wire wire[MOTU424_MAX_WIRES];
	u8 in_hw[MOTU424_MAX_CHANNELS];
	u8 out_hw[MOTU424_MAX_CHANNELS];
	unsigned int n_in, n_out;
};

/* one line per port: PCM channel range and the positions on that port */
static void proc_print_layout(struct snd_info_buffer *buffer, const char *dir,
			      const struct motu424_wire *wire, const u8 *hw,
			      unsigned int n)
{
	unsigned int i, start;

	snd_iprintf(buffer, "%s: %u channels\n", dir, n);
	for (start = 0; start < n; start = i) {
		unsigned int port = hw[start] / MOTU424_CH_PER_WIRE;

		for (i = start; i < n && hw[i] / MOTU424_CH_PER_WIRE == port; i++)
			;
		snd_iprintf(buffer, "  %u-%u: port %u, %s %u-%u\n", start + 1, i,
			    port, wire_name(&wire[port]),
			    hw[start] % MOTU424_CH_PER_WIRE + 1,
			    hw[i - 1] % MOTU424_CH_PER_WIRE + 1);
	}
}

static void motu424_proc_read(struct snd_info_entry *entry,
			      struct snd_info_buffer *buffer)
{
	struct motu424 *m = entry->private_data;
	struct proc_snapshot *snap;
	const struct motu424_wire *wire;
	unsigned int present, irqs, in, out, period, i, b;
	unsigned long flags;
	bool running, audio;

	snap = kmalloc(sizeof(*snap), GFP_KERNEL);
	if (!snap)
		return;
	wire = snap->wire;
	spin_lock_irqsave(&m->lock, flags);
	memcpy(snap->wire, m->wire, sizeof(snap->wire));
	memcpy(snap->in_hw, m->in_hw, sizeof(snap->in_hw));
	memcpy(snap->out_hw, m->out_hw, sizeof(snap->out_hw));
	snap->n_in = m->n_in;
	snap->n_out = m->n_out;
	present = m->present_mask;
	irqs = m->irq_count;
	running = m->running;
	audio = m->audio;
	in = m->eng_in;
	out = m->eng_out;
	period = m->blocks_per_period * motu424_block_frames(m->mult);
	spin_unlock_irqrestore(&m->lock, flags);

	snd_iprintf(buffer, "%s\n", m->card->longname);
	snd_iprintf(buffer, "Sample rate: %u Hz (%ux, doubled ports %x), card clock: %s\n",
		    m->rate, m->mult, m->wire_mask,
		    m->word_source < MOTU424_WS_COUNT ?
		    motu424_ws_names[m->word_source] : "?");
	if (!running)
		snd_iprintf(buffer, "Engine: stopped\n");
	else if (audio)
		snd_iprintf(buffer, "Engine: audio, %u out, %u in, engine period %u frames\n",
			    out, in, period);
	else
		snd_iprintf(buffer, "Engine: idle (interface control only)\n");
	snd_iprintf(buffer, "Interrupts: %u\n", irqs);
	snd_iprintf(buffer, "Present mask: %x\n", present);
	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		const struct motu424_wire *w = &wire[i];

		snd_iprintf(buffer, "Port %u: %s%s%s (%u MIDI bytes received)\n", i,
			    wire_name(w), w->info ? ", " : "",
			    wire_state_names[w->state], w->rx_bytes);
		for (b = 0; b < w->nbanks; b++)
			snd_iprintf(buffer, "  Bank %c: %s, %u channels\n", 'A' + b,
				    w->bank_type[b] <
				    ARRAY_SIZE(motu424_simple_personality_names) ?
				    motu424_simple_personality_names[w->bank_type[b]] :
				    "custom", w->bank_chans[b]);
	}
	proc_print_layout(buffer, "Capture", wire, snap->in_hw, snap->n_in);
	proc_print_layout(buffer, "Playback", wire, snap->out_hw, snap->n_out);
	kfree(snap);
}

void motu424_iface_proc_init(struct motu424 *m)
{
	snd_card_ro_proc_new(m->card, "motu424", m, motu424_proc_read);
}
