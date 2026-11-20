// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace test of motu424_iface.c against simulated AudioWire boxes.
 *
 * Bytes travel as in the DMA stream: the driver's byte for a port is taken
 * from the header slot, the box's byte is put into the status slot.
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -I tools/shim -I . \
 *       tools/iface_test.c motu424_iface.c -o /tmp/motu424-iface-test
 */
#include "motu424.h"
#include "motu424_tables.h"

unsigned int kshim_warnings;
int kshim_verbose;
static unsigned int cue_updates;

void motu424_qmix_schedule(struct motu424 *m)
{
	(void)m;
	cue_updates++;
}

#define CHECK(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	exit(1); } } while (0)

enum box_kind {
	BOX_NONE,
	BOX_24IO,
	BOX_2408,
	BOX_2408MK3,
	BOX_SIMPLE,		/* version 3.1: also asked for step 9 */
	BOX_SIMPLE_OLD,		/* version 2.0 */
	BOX_24IO_MUTE_STATE,	/* identifies itself, never echoes its state */
	BOX_SIMPLE_MUTE_INFO,	/* identifies itself, never describes itself */
	BOX_UNKNOWN,		/* family 1, model 99 */
	BOX_1296,
	BOX_HD192,
};

struct box {
	enum box_kind kind;
	u8 rx[64];
	unsigned int rx_len;
	u8 tx[512];
	unsigned int tx_len, tx_pos;
	u8 state[17];		/* last state message received */
	unsigned int state_len, state_msgs;
	u8 steps[32];		/* description requests, strings as 0x80|list */
	unsigned int nsteps;
	bool corrupted;
	bool odd_outputs;	/* family 2: reports three outputs */
	u32 last_tick;
	bool in_msg;
};

static struct motu424 m;
static struct pci_dev pci;
static struct box box[MOTU424_MAX_WIRES];
static u32 tick;
static u8 rawmidi_rx[MOTU424_MAX_WIRES][256];
static unsigned int rawmidi_rx_len[MOTU424_MAX_WIRES];
static u8 rawmidi_tx[MOTU424_MAX_WIRES][256];
static unsigned int rawmidi_tx_len[MOTU424_MAX_WIRES];
static unsigned int rawmidi_tx_pos[MOTU424_MAX_WIRES];

void motu424_rawmidi_receive(struct motu424 *m, unsigned int port, u8 byte)
{
	(void)m;
	if (port < MOTU424_MAX_WIRES &&
	    rawmidi_rx_len[port] < ARRAY_SIZE(rawmidi_rx[port]))
		rawmidi_rx[port][rawmidi_rx_len[port]++] = byte;
}

bool motu424_rawmidi_transmit(struct motu424 *m, unsigned int port, u8 *byte)
{
	(void)m;
	if (port >= MOTU424_MAX_WIRES ||
	    rawmidi_tx_pos[port] >= rawmidi_tx_len[port])
		return false;
	*byte = rawmidi_tx[port][rawmidi_tx_pos[port]++];
	return true;
}

static void box_reply(struct box *b, const u8 *msg, unsigned int len)
{
	CHECK(b->tx_len + len <= sizeof(b->tx));
	memcpy(b->tx + b->tx_len, msg, len);
	b->tx_len += len;
}

/* family 2 reply: header, checksum over the data, data, F7 */
static void box_reply_data(struct box *b, const u8 *data, unsigned int n,
			   bool corrupt)
{
	u8 msg[64] = { 0xf0, 0x00, 0x00, 0x33, 0x20, 0x00 };
	unsigned int i, sum = 0;

	for (i = 0; i < n; i++) {
		msg[7 + i] = data[i];
		sum += data[i];
	}
	msg[6] = (sum + corrupt) & 0x7f;
	msg[7 + n] = 0xf7;
	box_reply(b, msg, n + 8);
}

static void box_simple_request(struct box *b)
{
	static const u8 general[] = { 3, 4, 'T', 'e', 's', 't', ' ', '2', '4', 'i' };
	static const u8 bank_types[] = { 0, 0x7f, 1 };
	static const u8 bank_chans[] = { 8, 8, 8 };
	static const u8 clock_types[] = { 1, 2, 0, 4 };
	static const u8 avail[] = { 0x7f, 0x7f, 0x7f, 0x07, 0x03, 0, 0, 0 };
	static const u8 avail_odd[] = { 0x7f, 0x7f, 0x7f, 0x07, 0x07, 0, 0, 0 };
	static const u8 digi[] = { 0, 0, 1 };
	static const u8 custom[] = { 1, 'M', 'a', 'i', 'n' };
	u8 step = b->rx[8];

	if (b->kind == BOX_SIMPLE_MUTE_INFO)
		return;
	CHECK(b->nsteps < sizeof(b->steps));
	if (b->rx_len == 11) {
		b->steps[b->nsteps++] = 0x80 | step;
		CHECK(step == MOTU424_SIMPLE_STR_BANKS && b->rx[9] == 1);
		box_reply_data(b, custom, sizeof(custom), false);
		return;
	}
	b->steps[b->nsteps++] = step;
	switch (step) {
	case 2:
		box_reply_data(b, general, sizeof(general), false);
		break;
	case 3:
		box_reply_data(b, bank_types, sizeof(bank_types), false);
		break;
	case 4:
		/* the first answer has a wrong checksum */
		box_reply_data(b, bank_chans, sizeof(bank_chans), !b->corrupted);
		b->corrupted = true;
		break;
	case 5:
		box_reply_data(b, clock_types, sizeof(clock_types), false);
		break;
	case 6:
		box_reply_data(b, b->odd_outputs ? avail_odd : avail,
			       sizeof(avail), false);
		break;
	case 9:
		box_reply_data(b, digi, sizeof(digi), false);
		break;
	default:
		CHECK(!"unexpected description step");
	}
}

static void box_message(struct box *b)
{
	static const u8 inquiry[] = { 0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7 };
	static const u8 request[] = {
		0xf0, 0x00, 0x00, 0x33, 0x20, 0x00, 0x02, 0x20, 0x01, 0xf7 };
	static const u8 request_1296[] = {
		0xf0, 0x00, 0x00, 0x33, 0x23, 0x01, 0xf7 };
	u8 id[16] = { 0xf0, 0x7e, 0x00, 0x06, 0x02, 0x00, 0x00, 0x33,
		      0x01, 0x00, 0x05, 0x00, 0x11, 0x10, 0x11, 0xf7 };
	u8 echo[17];

	if (b->rx_len == sizeof(inquiry) && !memcmp(b->rx, inquiry, sizeof(inquiry))) {
		switch (b->kind) {
		case BOX_2408:
			id[10] = 2;
			break;
		case BOX_2408MK3:
			id[10] = 4;
			break;
		case BOX_SIMPLE:
		case BOX_SIMPLE_MUTE_INFO:
			id[8] = 2;
			id[10] = 3;
			id[11] = 1;
			break;
		case BOX_SIMPLE_OLD:
			id[8] = 2;
			id[10] = 2;
			id[11] = 0;
			break;
		case BOX_UNKNOWN:
			id[10] = 99;
			break;
		case BOX_1296:
			id[10] = 3;
			break;
		case BOX_HD192:
			id[10] = 6;
			break;
		default:
			break;
		}
		box_reply(b, id, sizeof(id));
		return;
	}
	if (b->rx_len == sizeof(request) && !memcmp(b->rx, request, sizeof(request))) {
		if (b->kind == BOX_24IO_MUTE_STATE || !b->state_len)
			return;
		/* the box reports its state with its own header bytes */
		memcpy(echo, b->state, b->state_len);
		echo[5] = 0x00;
		echo[7] = 0x00;
		box_reply(b, echo, b->state_len);
		return;
	}
	if (b->rx_len == sizeof(request_1296) &&
	    !memcmp(b->rx, request_1296, sizeof(request_1296))) {
		CHECK(b->kind == BOX_1296 || b->kind == BOX_HD192);
		if (!b->state_len)
			return;
		/* c2 bit 4 is a command bit that ReplyGood ignores */
		memcpy(echo, b->state, b->state_len);
		echo[9] &= ~0x10;
		box_reply(b, echo, b->state_len);
		return;
	}
	if (b->rx_len == 16 && b->rx[4] == 0x22 && b->rx[5] == 0x01) {
		memcpy(b->state, b->rx, b->rx_len);
		b->state_len = b->rx_len;
		b->state_msgs++;
		return;
	}
	if (b->rx_len >= 10 && b->rx[5] == 0x7f && b->rx[7] == 0x20) {
		box_simple_request(b);
		return;
	}
	if (b->rx_len >= 12 && b->rx[5] == 0x7f && b->rx[7] == 0x01) {
		CHECK(b->rx_len <= sizeof(b->state));
		memcpy(b->state, b->rx, b->rx_len);
		b->state_len = b->rx_len;
		b->state_msgs++;
		return;
	}
	CHECK(!"unexpected message");
}

static void box_rx(struct box *b, u8 c)
{
	/* SubQueue pacing: 4 blocks between bytes, 31 between messages */
	if (b->in_msg)
		CHECK(tick - b->last_tick == 4);
	else if (b->last_tick)
		CHECK(tick - b->last_tick >= 31);
	b->last_tick = tick;

	if (c == 0xf0)
		b->rx_len = 0;
	CHECK(b->rx_len < sizeof(b->rx));
	b->rx[b->rx_len++] = c;
	b->in_msg = c != 0xf7;
	if (c == 0xf7)
		box_message(b);
}

static void run(unsigned int blocks)
{
	__le32 status[MOTU424_STATUS_DWORDS], header[MOTU424_SLOT_DWORDS];
	unsigned int present = 0, i;

	for (i = 0; i < MOTU424_MAX_WIRES; i++)
		if (box[i].kind != BOX_NONE)
			present |= BIT(i);

	while (blocks--) {
		memset(status, 0, sizeof(status));
		status[MOTU424_ST_FLAGS] = present;
		for (i = 0; i < MOTU424_MAX_WIRES; i++) {
			struct box *b = &box[i];

			if (b->tx_pos < b->tx_len)
				status[i] = MOTU424_ST_MIDI_HI_VALID |
					    (u32)b->tx[b->tx_pos++] << 16;
		}
		memset(header, 0xff, sizeof(header));
		motu424_midi_block(&m, status, header);
		tick++;
		CHECK(header[0] == 0);
		for (i = 0; i < MOTU424_MAX_WIRES; i++) {
			u32 v = header[MOTU424_MIDIOUT_IDX(i)];

			if (v & motu424_midi_dest_valid[i])
				box_rx(&box[i], v >> 8);
			else
				CHECK(v == 0);
		}
		if (tick % 64 == 0)
			motu424_iface_period(&m, present);
	}
}

static void setup(enum box_kind k0, enum box_kind k1, enum box_kind k2,
		  enum box_kind k3)
{
	memset(&m, 0, sizeof(m));
	memset(box, 0, sizeof(box));
	memset(rawmidi_rx, 0, sizeof(rawmidi_rx));
	memset(rawmidi_rx_len, 0, sizeof(rawmidi_rx_len));
	memset(rawmidi_tx, 0, sizeof(rawmidi_tx));
	memset(rawmidi_tx_len, 0, sizeof(rawmidi_tx_len));
	memset(rawmidi_tx_pos, 0, sizeof(rawmidi_tx_pos));
	m.pci = &pci;
	m.rate = 48000;
	m.mult = 1;
	m.running = true;
	box[0].kind = k0;
	box[1].kind = k1;
	box[2].kind = k2;
	box[3].kind = k3;
	kshim_warnings = 0;
	tick = 0;
}

static void test_configure_and_release(void)
{
	static const u8 steps[] = { 2, 3, 0x87, 4, 4, 5, 6, 9 };
	static const u8 steps_old[] = { 2, 3, 0x87, 4, 4, 5, 6 };
	static const u8 st_2408[] = { 0xf0, 0x00, 0x00, 0x33, 0x20, 0x7f, 0x02,
		0x01, 0x01, 0x00, 0x42, 0x18, 0xf7 };
	static const u8 st_mk3[] = { 0xf0, 0x00, 0x00, 0x33, 0x20, 0x7f, 0x02,
		0x01, 0x01, 0x00, 0x42, 0x18, 0x7f, 0x7f, 0x00, 0x30, 0xf7 };
	static const u8 st_simple[] = { 0xf0, 0x00, 0x00, 0x33, 0x20, 0x7f, 0x02,
		0x01, 0x01, 0x00, 0x42, 0xf7 };
	struct motu424_wire *w = &m.wire[2];
	unsigned int i;

	setup(BOX_24IO, BOX_2408, BOX_SIMPLE, BOX_SIMPLE_OLD);
	run(48000 / 16 * 4);
	for (i = 0; i < MOTU424_MAX_WIRES; i++) {
		CHECK(m.wire[i].state == MOTU424_WIRE_READY);
		CHECK(box[i].state_msgs == 1);
	}
	CHECK(kshim_warnings == 0);
	CHECK(box[0].state_len == 17 && !memcmp(box[0].state,
		motu424_sysex_24io_state, 17));
	CHECK(box[1].state_len == 13 && !memcmp(box[1].state, st_2408, 13));
	/* card clock is the box's third clock source: index 2, host bit set */
	CHECK(box[2].state_len == 12 && !memcmp(box[2].state, st_simple, 12));
	CHECK(box[2].nsteps == sizeof(steps) &&
	      !memcmp(box[2].steps, steps, sizeof(steps)));
	CHECK(box[3].nsteps == sizeof(steps_old) &&
	      !memcmp(box[3].steps, steps_old, sizeof(steps_old)));
	CHECK(!strcmp(w->name, "Test 24i"));
	CHECK(w->nbanks == 3 && w->nclocks == 4);
	CHECK(w->bank_type[1] == 0x7f && w->bank_chans[2] == 8);
	CHECK(w->clock_type[2] == MOTU424_SIMPLE_CLK_CARD);
	CHECK(w->avail[3] == 0x07 && w->avail[4] == 0x03);

	/* channels in hardware order: 24 inputs each; 24, 24, 2 and 2 outputs */
	CHECK(m.n_in == 96 && m.n_out == 52);
	CHECK(m.in_hw[0] == 0 && m.in_hw[95] == 95);
	CHECK(m.out_hw[47] == 47 && m.out_hw[48] == 48 && m.out_hw[49] == 49 &&
	      m.out_hw[50] == 72 && m.out_hw[51] == 73);
	CHECK(m.out_pairs == (0xffffffull | 1ull << 24 | 1ull << 36));

	/* Move the analog bank and select S/PDIF on C; C shrinks to one pair. */
	CHECK(motu424_iface_control_put(&m, 1,
		MOTU424_IFACE_2408_ANALOG_BANK, 1) == 1);
	run(1000);
	CHECK(m.wire[1].state == MOTU424_WIRE_READY &&
	      box[1].state[11] == 0x28);
	CHECK(motu424_iface_control_put(&m, 1,
		MOTU424_IFACE_2408_BANK_C_FORMAT, 2) == 1);
	run(1000);
	CHECK(m.wire[1].state == MOTU424_WIRE_READY &&
	      box[1].state[11] == 0x68);
	CHECK(m.n_in == 90 && m.n_out == 46);
	CHECK(m.in_hw[41] == 41 && m.in_hw[42] == 48);
	CHECK(m.out_hw[41] == 41 && m.out_hw[42] == 48);
	CHECK(m.out_pairs == (0xffffffull & ~(7ull << 21) |
			      1ull << 24 | 1ull << 36));
	/* Restore the default format and analog bank before shutdown. */
	CHECK(motu424_iface_control_put(&m, 1,
		MOTU424_IFACE_2408_BANK_C_FORMAT, 0) == 1);
	run(1000);
	CHECK(motu424_iface_control_put(&m, 1,
		MOTU424_IFACE_2408_ANALOG_BANK, 0) == 1);
	run(1000);
	CHECK(m.n_in == 96 && m.n_out == 52);

	/* No analog bank: all three banks digital, the analog outs mirror one. */
	CHECK(motu424_iface_control_put(&m, 1, MOTU424_IFACE_2408_ANALOG_BANK,
					MOTU424_2408_ANALOG_NONE) == 1);
	run(1000);
	CHECK(m.wire[1].state == MOTU424_WIRE_READY &&
	      box[1].state[11] == 0x10);
	CHECK(m.n_in == 96 && m.n_out == 52);
	CHECK(motu424_iface_control_put(&m, 1,
		MOTU424_IFACE_2408_ANALOG_MIRROR, 2) == 1);
	run(1000);
	CHECK(m.wire[1].state == MOTU424_WIRE_READY &&
	      box[1].state[11] == 0x30);
	CHECK(motu424_iface_control_put(&m, 1,
		MOTU424_IFACE_2408_ANALOG_BANK, 0) == 1);
	run(1000);
	CHECK(box[1].state[11] == 0x18);

	/* 24I/O: Analog 9-16 at -10 dBV, word clock out at 2x */
	CHECK(motu424_iface_control_put(&m, 0,
		MOTU424_IFACE_24IO_LEVEL_9_16, 1) == 1);
	run(1000);
	CHECK(m.wire[0].state == MOTU424_WIRE_READY &&
	      box[0].state[11] == 0x02 && box[0].state[15] == 0);
	CHECK(motu424_iface_control_put(&m, 0,
		MOTU424_IFACE_WORD_OUT_RATE, 2) == 1);
	run(1000);
	CHECK(m.wire[0].state == MOTU424_WIRE_READY &&
	      box[0].state[11] == 0x02 && box[0].state[15] == 1);
	CHECK(kshim_warnings == 0);

	/* unloading: every box gets its release message, once */
	motu424_iface_shutdown(&m);
	run(1000);
	CHECK(box[0].state_msgs == 4 && box[1].state_msgs == 9 &&
	      box[2].state_msgs == 2 && box[3].state_msgs == 2);
	CHECK(box[0].state[10] == 0x03 && box[0].state[11] == 0x02 &&
	      box[0].state[15] == 1);
	CHECK(box[1].state_len == 13 && box[1].state[10] == 0x04 &&
	      box[1].state[11] == 0x00);
	CHECK(box[2].state[10] == 0x02);

	setup(BOX_2408MK3, BOX_NONE, BOX_NONE, BOX_NONE);
	run(48000 / 16 * 4);
	CHECK(m.wire[0].state == MOTU424_WIRE_READY);
	CHECK(box[0].state_len == 17 && !memcmp(box[0].state, st_mk3, 17));
	CHECK(m.wire[1].state == MOTU424_WIRE_ABSENT);
	CHECK(motu424_iface_control_put(&m, 0,
		MOTU424_IFACE_2408MK3_LEVEL_3_4, 1) == 1);
	run(1000);
	CHECK(m.wire[0].state == MOTU424_WIRE_READY &&
	      box[0].state[14] == 0x02 && box[0].state[15] == 0x30);
	CHECK(motu424_iface_control_put(&m, 0,
		MOTU424_IFACE_WORD_OUT_RATE, 1) == 1);
	run(1000);
	CHECK(m.wire[0].state == MOTU424_WIRE_READY &&
	      box[0].state[15] == 0x20);
	motu424_iface_shutdown(&m);
	run(1000);
	CHECK(box[0].state[10] == 0x04 && box[0].state[11] == 0x00 &&
	      box[0].state[14] == 0x02 && box[0].state[15] == 0x00);
}

/* AW_1296 and MOTUAW.sys defaults: follow the card, no SRC, hold times 4/1 */
static void test_1296(void)
{
	static const u8 st_1296[] = { 0xf0, 0x00, 0x00, 0x33, 0x22, 0x01, 0x00,
		0x49, 0x00, 0x10, 0x21, 0x00, 0x00, 0x00, 0x00, 0xf7 };
	unsigned int i;

	setup(BOX_1296, BOX_NONE, BOX_HD192, BOX_NONE);
	run(48000 / 16 * 4);
	CHECK(kshim_warnings == 0);
	for (i = 0; i < MOTU424_MAX_WIRES; i += 2) {
		CHECK(m.wire[i].state == MOTU424_WIRE_READY);
		CHECK(box[i].state_msgs == 1);
		CHECK(box[i].state_len == 16 &&
		      !memcmp(box[i].state, st_1296, 16));
	}
	CHECK(!strcmp(m.wire[0].info->name, "1296"));
	CHECK(!strcmp(m.wire[2].info->name, "HD192"));

	for (i = 0; i < MOTU424_MAX_WIRES; i += 2)
		CHECK(motu424_iface_control_put(&m, i,
			MOTU424_IFACE_AES_INPUT_PAIR, 2) == 1);
	run(1000);
	for (i = 0; i < MOTU424_MAX_WIRES; i += 2) {
		CHECK(m.wire[i].state == MOTU424_WIRE_READY);
		CHECK(box[i].state_msgs == 2 && box[i].state[8] == 0x20);
	}
	for (i = 0; i < MOTU424_MAX_WIRES; i += 2)
		CHECK(motu424_iface_control_put(&m, i,
			MOTU424_IFACE_AES_OUTPUT_CLOCK, 3) == 1);
	run(1000);
	for (i = 0; i < MOTU424_MAX_WIRES; i += 2) {
		CHECK(m.wire[i].state == MOTU424_WIRE_READY);
		CHECK(box[i].state_msgs == 3 && box[i].state[9] == 0x13);
	}

	/* a 44.1k output clock matches the new card rate: send the card clock */
	m.rate = 44100;
	motu424_iface_rate_changed(&m);
	run(1000);
	for (i = 0; i < MOTU424_MAX_WIRES; i += 2) {
		CHECK(m.wire[i].state == MOTU424_WIRE_READY);
		CHECK(box[i].state_msgs == 4 && box[i].state[9] == 0x10);
	}
	m.rate = 48000;
	motu424_iface_rate_changed(&m);
	run(1000);
	for (i = 0; i < MOTU424_MAX_WIRES; i += 2) {
		CHECK(m.wire[i].state == MOTU424_WIRE_READY);
		CHECK(box[i].state_msgs == 5 && box[i].state[9] == 0x13);
	}

	/* 12 inputs and 12 outputs on positions 0..11 of each port */
	CHECK(m.n_in == 24 && m.n_out == 24);
	CHECK(m.in_hw[11] == 11 && m.in_hw[12] == 48 && m.in_hw[23] == 59);
	CHECK(m.out_hw[11] == 11 && m.out_hw[12] == 48);
	CHECK(m.out_pairs == (0x3full | 0x3full << 24));

	/* Breakdown: active bit and c2 bit 4 cleared */
	motu424_iface_shutdown(&m);
	run(1000);
	for (i = 0; i < MOTU424_MAX_WIRES; i += 2) {
		CHECK(box[i].state_msgs == 6);
		CHECK(box[i].state[7] == 0x09 && box[i].state[8] == 0x20 &&
		      box[i].state[9] == 0x03 && box[i].state[10] == 0x21);
	}
}

/* 2x and 4x: layouts, the doubled ports, the boxes' multiplier bytes, MIDI */
static void test_multiplier(void)
{
	static const u8 hw[] = { 0, 1, 2, 3, 48, 49 };
	static struct motu424_layout l;
	u8 slot[2][MOTU424_MAX_CHANNELS], slot_hw[MOTU424_MAX_CHANNELS];
	__le32 status[MOTU424_STATUS_DWORDS], header[MOTU424_SLOT_DWORDS];
	struct motu424_wire *w = &m.wire[1];
	unsigned int i, t;

	setup(BOX_2408MK3, BOX_1296, BOX_HD192, BOX_24IO);
	run(48000 / 16 * 4);
	for (i = 0; i < MOTU424_MAX_WIRES; i++)
		CHECK(m.wire[i].state == MOTU424_WIRE_READY && box[i].state_msgs == 1);

	/* the 1296 is doubled by the card; the 2408mk3 ADAT banks halve */
	CHECK(motu424_iface_wire_mask(&m, 1) == 0);
	CHECK(motu424_iface_wire_mask(&m, 2) == BIT(1));
	motu424_layout_compute(&m, 2, &l);
	CHECK(l.n_in == 16 + 12 + 12 + 24 && l.n_out == 16 + 12 + 12 + 24);
	CHECK(l.in_hw[8] == 8 && l.in_hw[9] == 10 && l.in_hw[15] == 22);
	CHECK(l.out_hw[8] == 8 && l.out_hw[9] == 10 && l.out_hw[10] == 12);
	CHECK(l.split_pairs == (BIT_ULL(4) | BIT_ULL(6) | BIT_ULL(8) | BIT_ULL(10)));
	CHECK(l.skip_pairs == (BIT_ULL(5) | BIT_ULL(7) | BIT_ULL(9) | BIT_ULL(11)));
	CHECK(!l.odd);
	/* 4x: only the HD192, on positions 0..11 */
	motu424_layout_compute(&m, 4, &l);
	CHECK(l.n_in == 12 && l.n_out == 12 && l.in_hw[0] == 48 && l.in_hw[11] == 59);
	CHECK(l.out_pairs == 0x3full << 24 && !l.split_pairs);

	/* the 24I/O's word clock out stays at 1x */
	CHECK(motu424_iface_control_put(&m, 3,
		MOTU424_IFACE_WORD_OUT_RATE, 1) == 1);
	run(1000);
	CHECK(m.wire[3].state == MOTU424_WIRE_READY && box[3].state[15] == 0);

	motu424_iface_set_mult(&m, 2);
	run(1000);
	for (i = 0; i < MOTU424_MAX_WIRES; i++)
		CHECK(m.wire[i].state == MOTU424_WIRE_READY && m.wire[i].mult == 2 &&
		      box[i].state_msgs == (i == 3 ? 3 : 2));
	CHECK(box[0].state[10] == 0x62);
	CHECK(box[1].state[7] == 0x79 && box[1].state[9] == 0x10);
	CHECK(box[2].state[7] == 0x69 && box[2].state[9] == 0x30);
	CHECK(box[3].state[10] == 0x62 && box[3].state[15] == 0);

	/* only the HD192 runs at 4x; the others keep their last message */
	motu424_iface_set_mult(&m, 4);
	run(1000);
	CHECK(box[0].state_msgs == 2 && box[1].state_msgs == 2 &&
	      box[3].state_msgs == 3 && m.wire[0].mult == 2);
	CHECK(box[2].state_msgs == 3 && box[2].state[7] == 0x79 &&
	      box[2].state[8] == 0x00 && box[2].state[9] == 0x30);
	CHECK(kshim_warnings == 0);

	/* a doubled port sends each byte twice; the tick runs at half speed */
	m.mult = 2;
	m.wire_mask = BIT(1);
	w->tx[0] = 0xf8;
	w->tx_pos = 0;
	w->tx_len = w->tx_split = 1;
	w->tx_next = m.midi_tick;
	t = m.midi_tick;
	memset(status, 0, sizeof(status));
	status[MOTU424_ST_FLAGS] = 0xf;
	motu424_midi_block(&m, status, header);
	CHECK(header[MOTU424_MIDIOUT_IDX(1)] != 0 &&
	      header[MOTU424_MIDIOUT_IDX2(1)] == header[MOTU424_MIDIOUT_IDX(1)]);
	CHECK(m.midi_tick == t);
	motu424_midi_block(&m, status, header);
	CHECK(m.midi_tick == t + 1);

	/* 4x DMA slots: per port the primaries, then their partners */
	CHECK(motu424_build_slots(hw, 6, false, slot, NULL) == 6);
	CHECK(slot[0][4] == 4 && slot[1][4] == 4);
	CHECK(motu424_build_slots(hw, 6, true, slot, slot_hw) == 12);
	CHECK(slot[0][3] == 3 && slot[1][3] == 7 && slot[0][4] == 8 &&
	      slot[1][5] == 11);
	CHECK(slot_hw[4] == 12 && slot_hw[9] == 49 && slot_hw[10] == 60);
}

static void test_rawmidi(void)
{
	__le32 status[MOTU424_STATUS_DWORDS] = { 0 };
	__le32 header[MOTU424_SLOT_DWORDS];
	const u8 sysex[] = { 0xf0, 0x7d, 0x01, 0xf7 };
	const u8 control_sysex[] = { 0xf0, 0x00, 0x00, 0x33, 0x55, 0xf7 };
	unsigned int i;

	setup(BOX_24IO, BOX_NONE, BOX_NONE, BOX_NONE);
	m.wire[0].state = MOTU424_WIRE_READY;
	m.wire[0].tx_next = 0;
	rawmidi_tx[0][0] = 0x90;
	rawmidi_tx[0][1] = 0x40;
	rawmidi_tx_len[0] = 2;
	status[MOTU424_ST_FLAGS] = BIT(0);
	status[MOTU424_ST_MIDI(0)] =
		MOTU424_ST_MIDI_HI_VALID | (u32)0x91 << 16;

	motu424_midi_block(&m, status, header);
	CHECK(rawmidi_rx_len[0] == 1 && rawmidi_rx[0][0] == 0x91);
	CHECK(header[MOTU424_MIDIOUT_IDX(0)] ==
	      MOTU424_MIDIOUT_WORD(0, 0x90));
	for (i = 0; i < 3; i++) {
		status[MOTU424_ST_MIDI(0)] = 0;
		motu424_midi_block(&m, status, header);
		CHECK(header[MOTU424_MIDIOUT_IDX(0)] == 0);
	}
	motu424_midi_block(&m, status, header);
	CHECK(header[MOTU424_MIDIOUT_IDX(0)] ==
	      MOTU424_MIDIOUT_WORD(0, 0x40));

	for (i = 0; i < ARRAY_SIZE(sysex); i++) {
		status[MOTU424_ST_MIDI(0)] =
			MOTU424_ST_MIDI_HI_VALID | (u32)sysex[i] << 16;
		motu424_midi_block(&m, status, header);
	}
	CHECK(rawmidi_rx_len[0] == 1 + ARRAY_SIZE(sysex));
	CHECK(!memcmp(rawmidi_rx[0] + 1, sysex, sizeof(sysex)));
	for (i = 0; i < ARRAY_SIZE(control_sysex); i++) {
		status[MOTU424_ST_MIDI(0)] =
			MOTU424_ST_MIDI_HI_VALID | (u32)control_sysex[i] << 16;
		motu424_midi_block(&m, status, header);
	}
	CHECK(rawmidi_rx_len[0] == 1 + ARRAY_SIZE(sysex));
}

static void test_give_up(void)
{
	setup(BOX_24IO_MUTE_STATE, BOX_SIMPLE_MUTE_INFO, BOX_UNKNOWN, BOX_NONE);
	run(30000);
	CHECK(m.wire[0].state == MOTU424_WIRE_UNCONFIGURED);
	CHECK(box[0].state_msgs == 10);
	CHECK(m.wire[1].state == MOTU424_WIRE_UNCONFIGURED);
	CHECK(box[1].state_msgs == 0);
	CHECK(m.wire[2].state == MOTU424_WIRE_UNCONFIGURED);
	CHECK(box[2].state_msgs == 0);
	CHECK(kshim_warnings == 3);
}

/* the layout follows the boxes, but not while a stream is open */
static void test_layout(void)
{
	unsigned int before;

	setup(BOX_24IO, BOX_NONE, BOX_SIMPLE, BOX_UNKNOWN);
	box[2].odd_outputs = true;
	CHECK(m.n_in == 0 && m.n_out == 0);
	run(48000 / 16 * 4);
	/* the unknown box has no channels; the third output has no partner */
	CHECK(m.n_in == 48 && m.n_out == 26);
	CHECK(m.in_hw[23] == 23 && m.in_hw[24] == 48);
	CHECK(m.out_hw[24] == 48 && m.out_hw[25] == 49);
	CHECK(m.out_pairs == (0xfffull | 1ull << 24));
	CHECK(kshim_warnings == 2);

	motu424_layout_freeze(&m, true);
	before = cue_updates;
	box[0].kind = BOX_NONE;			/* unplugged */
	run(64 * 12);
	CHECK(m.wire[0].state == MOTU424_WIRE_ABSENT);
	CHECK(m.n_in == 48 && m.n_out == 26 && m.in_hw[0] == 0);
	CHECK(cue_updates > before);
	before = cue_updates;
	motu424_layout_freeze(&m, false);
	CHECK(cue_updates > before);
	CHECK(m.n_in == 24 && m.n_out == 2);
	CHECK(m.in_hw[0] == 48 && m.out_hw[0] == 48 && m.out_hw[1] == 49);
	CHECK(m.out_pairs == 1ull << 24);
}

int main(int argc, char **argv)
{
	kshim_verbose = argc > 1 && !strcmp(argv[1], "-v");
	test_configure_and_release();
	test_layout();
	test_1296();
	test_multiplier();
	test_rawmidi();
	test_give_up();
	printf("iface test: ok\n");
	return 0;
}
