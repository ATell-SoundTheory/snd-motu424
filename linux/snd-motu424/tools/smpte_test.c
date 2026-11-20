// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMPTE reader/generator, video sync bits and S/MUX against a fake DSP
 * control block. Expected register values are the kext's (docs/SMPTE.md).
 *
 * cc -std=gnu11 -Wall -Wextra -Werror -I tools/shim -I . \
 *    tools/smpte_test.c motu424_smpte.c motu424_iface.c \
 *    -o /tmp/motu424-smpte-test
 */
#include <assert.h>
#include <errno.h>
#include <linux/workqueue.h>
#include <sound/control.h>

#include "motu424.h"
#include "motu424_tables.h"

#define CB	0x4000

unsigned int kshim_warnings;
int kshim_verbose;
struct work_struct *kshim_pending_work;

static u32 regs[0x200 / 4];
static unsigned int writes[0x200 / 4];
static u32 smux[MOTU424_MAX_WIRES];
static unsigned int smux_writes, notifies;
static struct snd_kcontrol *ctls[MOTU424_SMPTE_CTL_COUNT + 1];
static unsigned int nctls;

u32 motu424_read(struct motu424 *m, u32 addr)
{
	(void)m;
	assert(addr >= CB && addr < CB + sizeof(regs));
	return regs[(addr - CB) / 4];
}

void motu424_write(struct motu424 *m, u32 addr, u32 val)
{
	(void)m;
	assert(addr >= CB && addr < CB + sizeof(regs));
	regs[(addr - CB) / 4] = val;
	writes[(addr - CB) / 4]++;
}

void motu424_smux_write(struct motu424 *m, unsigned int port, u32 mask)
{
	(void)m;
	smux[port] = mask;
	smux_writes++;
}

void motu424_qmix_schedule(struct motu424 *m)
{
	(void)m;
}

void motu424_rawmidi_receive(struct motu424 *m, unsigned int port, u8 byte)
{
	(void)m;
	(void)port;
	(void)byte;
}

bool motu424_rawmidi_transmit(struct motu424 *m, unsigned int port, u8 *byte)
{
	(void)m;
	(void)port;
	(void)byte;
	return false;
}

void snd_ctl_notify(struct snd_card *card, unsigned int mask,
		    struct snd_ctl_elem_id *id)
{
	(void)card;
	(void)mask;
	(void)id;
	notifies++;
}

struct snd_kcontrol *snd_ctl_new1(const struct snd_kcontrol_new *new,
				  void *private_data)
{
	struct snd_kcontrol *k = calloc(1, sizeof(*k));

	assert(k);
	snprintf((char *)k->id.name, sizeof(k->id.name), "%s", new->name);
	k->iface = new->iface;
	k->access = new->access;
	k->count = new->count;
	k->info = new->info;
	k->get = new->get;
	k->put = new->put;
	k->private_value = new->private_value;
	k->private_data = private_data;
	return k;
}

int snd_ctl_add(struct snd_card *card, struct snd_kcontrol *k)
{
	(void)card;
	assert(nctls < ARRAY_SIZE(ctls));
	k->id.numid = nctls + 1;
	ctls[nctls++] = k;
	return 0;
}

#define R(off)	regs[(off) / 4]
#define W(off)	writes[(off) / 4]

static const struct motu424_params params_c6412 = {
	.name = "test", .dsp = MOTU424_DSP_C6412,
};
static struct snd_card card;
static struct pci_dev pci;
static struct motu424 m = {
	.card = &card, .pci = &pci, .params = &params_c6412, .cb = CB,
	.rate = 48000, .mult = 1, .booted = true,
};

static int put(unsigned int ctl, long v0)
{
	struct snd_ctl_elem_value v = { 0 };

	v.value.integer.value[0] = v0;
	return ctls[ctl]->put(ctls[ctl], &v);
}

static long get(unsigned int ctl, unsigned int i)
{
	struct snd_ctl_elem_value v = { 0 };

	assert(ctls[ctl]->get(ctls[ctl], &v) == 0);
	return v.value.integer.value[i];
}

static int put_tc(unsigned int ctl, unsigned int h, unsigned int mi,
		  unsigned int s, unsigned int f)
{
	struct snd_ctl_elem_value v = { 0 };

	v.value.integer.value[0] = h;
	v.value.integer.value[1] = mi;
	v.value.integer.value[2] = s;
	v.value.integer.value[3] = f;
	return ctls[ctl]->put(ctls[ctl], &v);
}

static void poll(void)
{
	assert(m.smpte.poll.queued);
	m.smpte.poll.queued = false;
	m.smpte.poll.work.func(&m.smpte.poll.work);
}

static void set_wire(unsigned int i, unsigned int info, u8 cfg1)
{
	struct motu424_wire *w = &m.wire[i];

	w->info = &motu424_ifaces[info];
	w->state = MOTU424_WIRE_READY;
	w->state_len = info == 1 ? MOTU424_2408MK3_STATE_LEN :
		       info == 0 ? MOTU424_2408_STATE_LEN : 17;
	w->state_msg[10] = MOTU424_2408_CFG0_DEFAULT;
	w->state_msg[11] = cfg1;
	w->mult = 1;
	w->tx_pos = w->tx_len;
}

static void ready(unsigned int i)
{
	m.wire[i].state = MOTU424_WIRE_READY;
	m.wire[i].tx_pos = m.wire[i].tx_len;
}

static void test_timecode(void)
{
	static const unsigned int fps[] = { 24, 25, 30, 30, 30 };
	unsigned int f;
	u8 tc[4];
	u64 n;

	for (f = 0; f < MOTU424_SMPTE_FORMATS; f++) {
		u64 day = 24ull * 3600 * fps[f];

		if (f == MOTU424_SMPTE_2997_DF)
			day = 24 * 6 * 17982;
		for (n = 0; n < day; n++) {
			motu424_smpte_frames_to_tc(n, f, tc);
			assert(motu424_smpte_tc_to_frames(tc, f) == n);
		}
	}
	/* drop-frame: 00:01:00:00/01 do not exist */
	motu424_smpte_frames_to_tc(1799, MOTU424_SMPTE_2997_DF, tc);
	assert(tc[1] == 0 && tc[2] == 59 && tc[3] == 29);
	motu424_smpte_frames_to_tc(1800, MOTU424_SMPTE_2997_DF, tc);
	assert(tc[1] == 1 && tc[2] == 0 && tc[3] == 2);
	motu424_smpte_frames_to_tc(17982, MOTU424_SMPTE_2997_DF, tc);
	assert(tc[1] == 10 && tc[2] == 0 && tc[3] == 0);
}

static void check_params(u32 b4, u32 bc, u32 c0)
{
	assert(R(MOTU424_CB_SMPTE_BIT_RATE) == b4);
	assert(R(MOTU424_CB_SMPTE_FRAME_INT) == bc);
	assert(R(MOTU424_CB_SMPTE_FRAME_FRAC) == c0);
}

static void test_create_and_params(void)
{
	unsigned int i;

	R(MOTU424_CB_SMPTE_CONST_K) = 100;
	R(MOTU424_CB_SMPTE_CONST_V) = 2000;
	motu424_smpte_defaults(&m);
	motu424_smpte_hw_init(&m);
	check_params(0xcccd, 1600, 0);
	assert(R(MOTU424_CB_SMPTE_FPS) == 30 &&
	       R(MOTU424_CB_SMPTE_FRAMES_10MIN) == 18000 &&
	       R(MOTU424_CB_SMPTE_FRAMES_MIN) == 1800 &&
	       R(MOTU424_CB_SMPTE_COUNT_FPS) == 30);
	assert(R(MOTU424_CB_SMPTE_K_SAMPLES) == 100 * 10 * 48000 / 24000);
	assert(R(MOTU424_CB_SMPTE_V_HALF) == 2000);
	assert(R(MOTU424_CB_SMPTE_F0) == 0x40000 / (2000 * 48000 / 64000000));
	assert(R(MOTU424_CB_SMPTE_STOP) == 1 && R(MOTU424_CB_COMMIT) == 1);

	assert(motu424_smpte_create(&m) == 0);
	assert(nctls == MOTU424_SMPTE_CTL_COUNT + 1);
	assert(!strcmp((char *)ctls[MOTU424_SMPTE_CTL_COUNT]->id.name,
		       "Card S/MUX Type"));
	for (i = 0; i < MOTU424_SMPTE_CTL_COUNT; i++)
		assert(ctls[i]->access & SNDRV_CTL_ELEM_ACCESS_VOLATILE);
	assert(ctls[MOTU424_SMPTE_CTL_STATE]->access ==
	       (SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE));
	assert(put(MOTU424_SMPTE_CTL_STATE, 1) == -EPERM);
	assert(m.smpte.poll.queued);

	assert(put(MOTU424_SMPTE_CTL_FORMAT, MOTU424_SMPTE_2997_DF) == 1);
	check_params(0xcc98, 1601, 0x9a029004);
	assert(put(MOTU424_SMPTE_CTL_FORMAT, MOTU424_SMPTE_25) == 1);
	check_params(0xaaab, 1920, 0);
	assert(put(MOTU424_SMPTE_CTL_FORMAT, MOTU424_SMPTE_24) == 1);
	check_params(0xa3d7, 2000, 0);
	assert(put(MOTU424_SMPTE_CTL_FORMAT, 5) == -EINVAL);

	m.rate = 44100;
	motu424_smpte_rate_changed(&m);
	check_params(R(MOTU424_CB_SMPTE_BIT_RATE), 1837, 0x80000000);
	assert(put(MOTU424_SMPTE_CTL_FORMAT, MOTU424_SMPTE_30) == 1);
	check_params(0xdee9, 1470, 0);
	assert(put(MOTU424_SMPTE_CTL_FORMAT, MOTU424_SMPTE_2997_ND) == 1);
	check_params(0xdeb0, 1471, 0x78b25ab6);
	assert(m.smpte.last30 == MOTU424_SMPTE_2997_ND);
	m.rate = 96000;
	m.mult = 2;
	motu424_smpte_rate_changed(&m);
	assert(R(MOTU424_CB_SMPTE_V_HALF) == 1000);
	m.rate = 48000;
	m.mult = 1;
	motu424_smpte_rate_changed(&m);

	/* freewheel: ms * (R / 100) / 10 samples, -1 forever */
	assert(put(MOTU424_SMPTE_CTL_FW_CLOCK, 500) == 1);
	assert(R(MOTU424_CB_SMPTE_FW_CLOCK) == 24000);
	assert(put(MOTU424_SMPTE_CTL_FW_ADDRESS, -1) == 1);
	assert(R(MOTU424_CB_SMPTE_FW_ADDRESS) == 0xffffffff);
	assert(put(MOTU424_SMPTE_CTL_FW_ADDRESS, -1) == 0);
}

static void test_reader(void)
{
	/* 24I/O on wire 0, 2408mk3 on wire 1 (bank A analog, C S/PDIF) */
	set_wire(0, 5, 0);
	set_wire(1, 1, MOTU424_2408_CFG1_DEFAULT | MOTU424_2408_CFG1_C_SPDIF);
	m.word_source = MOTU424_WS_SMPTE;

	/* 2408mk3 bank B (ADAT) is not analog; the dedicated jack is valid */
	assert(put(MOTU424_SMPTE_CTL_SOURCE, MOTU424_CH_PER_WIRE + 9) == -EINVAL);
	assert(put(MOTU424_SMPTE_CTL_SOURCE, MOTU424_SMPTE_JACK(0)) == -EINVAL);
	assert(put(MOTU424_SMPTE_CTL_SOURCE, 3) == 1);
	assert(R(MOTU424_CB_ADDRESS_SOURCE) == MOTU424_ADDR_SRC_SMPTE_A &&
	       R(MOTU424_CB_ADDRESS_PARAM) == MOTU424_SMPTE_DSP_CHAN(0, 2));
	/* the box reading LTC is told so; wire 0 is not a 2408mk3 */
	assert(m.wire[1].smpte_b14 == 0 && m.wire[1].state_msg[14] == 0);

	assert(put(MOTU424_SMPTE_CTL_SOURCE, MOTU424_SMPTE_JACK(1)) == 1);
	assert(R(MOTU424_CB_ADDRESS_SOURCE) == MOTU424_ADDR_SRC_SMPTE_B &&
	       R(MOTU424_CB_ADDRESS_PARAM) == MOTU424_SMPTE_DSP_JACK(1));
	assert(m.wire[1].smpte_b14 == MOTU424_2408MK3_B14_LTC &&
	       (m.wire[1].state_msg[14] & MOTU424_2408MK3_B14_LTC) &&
	       m.wire[1].state == MOTU424_WIRE_CONFIG);
	ready(1);

	/* ADAT sync and internal clock: address source follows the clock */
	motu424_smpte_address_write(&m, MOTU424_WS_ADAT);
	assert(R(MOTU424_CB_ADDRESS_SOURCE) == MOTU424_ADDR_SRC_ADAT);
	motu424_smpte_address_write(&m, MOTU424_WS_INTERNAL);
	assert(R(MOTU424_CB_ADDRESS_SOURCE) == MOTU424_ADDR_SRC_NONE &&
	       R(MOTU424_CB_ADDRESS_PARAM) == MOTU424_SMPTE_DSP_JACK(1));

	/* transport state (ND424::GetSMPTEState) */
	R(MOTU424_CB_SMPTE_ADDR_STATE) = 2;
	poll();
	assert(get(MOTU424_SMPTE_CTL_STATE, 0) == 4 &&
	       get(MOTU424_SMPTE_CTL_STATUS, 0) == 2);
	R(MOTU424_CB_SMPTE_ADDR_STATE) = 0;
	poll();
	assert(get(MOTU424_SMPTE_CTL_STATE, 0) == 1);

	/* format detection with follow: 30 -> Last30fpsRate */
	assert(put(MOTU424_SMPTE_CTL_FORMAT, MOTU424_SMPTE_25) == 1);
	R(MOTU424_CB_SMPTE_DETECTED) = 30;
	poll();
	assert(R(MOTU424_CB_SMPTE_DETECTED) == 0 &&
	       get(MOTU424_SMPTE_CTL_DETECTED, 0) == 4 &&
	       get(MOTU424_SMPTE_CTL_FORMAT, 0) == MOTU424_SMPTE_2997_ND);
	R(MOTU424_CB_SMPTE_DETECTED) = 24;
	poll();
	assert(get(MOTU424_SMPTE_CTL_FORMAT, 0) == MOTU424_SMPTE_24);
	assert(put(MOTU424_SMPTE_CTL_FOLLOW, 0) == 1);
	R(MOTU424_CB_SMPTE_DETECTED) = 25;
	poll();
	assert(get(MOTU424_SMPTE_CTL_FORMAT, 0) == MOTU424_SMPTE_24 &&
	       get(MOTU424_SMPTE_CTL_DETECTED, 0) == 2);
	assert(put(MOTU424_SMPTE_CTL_FORMAT, MOTU424_SMPTE_30) == 1);

	/* a box being reconfigured keeps the source until it confirms */
	m.wire[1].state = MOTU424_WIRE_CONFIG;
	motu424_smpte_refresh(&m);
	assert(get(MOTU424_SMPTE_CTL_SOURCE, 0) == MOTU424_SMPTE_JACK(1) &&
	       m.smpte.box_retry);
	ready(1);
	poll();
	assert(!m.smpte.box_retry);

	/* the box goes away: the source is dropped */
	m.wire[1].state = MOTU424_WIRE_ABSENT;
	motu424_smpte_refresh(&m);
	assert(get(MOTU424_SMPTE_CTL_SOURCE, 0) == 0 &&
	       R(MOTU424_CB_ADDRESS_PARAM) == 0);
	set_wire(1, 1, MOTU424_2408_CFG1_DEFAULT | MOTU424_2408_CFG1_C_SPDIF);
	motu424_smpte_refresh(&m);
	assert(get(MOTU424_SMPTE_CTL_SOURCE, 0) == MOTU424_SMPTE_JACK(1));
	ready(1);
	assert(put(MOTU424_SMPTE_CTL_SOURCE, 0) == 1);
	assert(m.wire[1].smpte_b14 == 0);
	ready(1);
}

static void test_generator(void)
{
	u32 stops;

	/* destination: wire 0 channel 2 */
	assert(put(MOTU424_SMPTE_CTL_DEST, 3) == 1);
	assert(R(MOTU424_CB_SMPTE_DEST_A) == 3 && R(MOTU424_CB_SMPTE_DEST_B) == 3 &&
	       R(MOTU424_CB_SMPTE_DEST_JACK) == 0 &&
	       R(MOTU424_CB_SMPTE_CLOBBER) == ((3 << 16) | 3) &&
	       R(MOTU424_CB_SMPTE_LEVEL) == 0x7fff);
	assert(put(MOTU424_SMPTE_CTL_LEVEL, 0x100) == 1 &&
	       R(MOTU424_CB_SMPTE_LEVEL) == 0x100);

	/* 01:00:00:00 at 30 fps, 48 kHz: frame 108000 */
	assert(put_tc(MOTU424_SMPTE_CTL_START, 1, 0, 0, 30) == -EINVAL);
	assert(put_tc(MOTU424_SMPTE_CTL_START, 1, 0, 0, 0) == 1);
	assert(put(MOTU424_SMPTE_CTL_GENERATE, 1) == 1);
	assert(R(MOTU424_CB_SMPTE_START_FRAME) == 108001 &&
	       R(MOTU424_CB_SMPTE_START_OFFSET) == 1600u << 16);
	R(MOTU424_CB_SMPTE_GEN_FRAME) = 108000 + 30 * 61 + 7;
	poll();
	assert(get(MOTU424_SMPTE_CTL_GEN_TIME, 0) == 1 &&
	       get(MOTU424_SMPTE_CTL_GEN_TIME, 1) == 1 &&
	       get(MOTU424_SMPTE_CTL_GEN_TIME, 2) == 1 &&
	       get(MOTU424_SMPTE_CTL_GEN_TIME, 3) == 7);

	/* clobbered audio is latched once and cleared by writing 0 */
	R(MOTU424_CB_SMPTE_CLOBBERED) = 1;
	poll();
	assert(get(MOTU424_SMPTE_CTL_CLOBBERED, 0) == 1 &&
	       R(MOTU424_CB_SMPTE_CLOBBERED) == 0);
	assert(put(MOTU424_SMPTE_CTL_CLOBBERED, 1) == 0);
	assert(put(MOTU424_SMPTE_CTL_CLOBBERED, 0) == 1);
	assert(put(MOTU424_SMPTE_CTL_CLOBBERED, 1) == -EINVAL);

	/* 4x: the second register is twelve DSP slots on */
	m.mult = 4;
	m.rate = 192000;
	m.wire[0].mult = 4;
	m.wire[0].info = &motu424_ifaces[4];	/* HD192 */
	stops = W(MOTU424_CB_SMPTE_STOP);
	motu424_smpte_rate_changed(&m);
	assert(R(MOTU424_CB_SMPTE_DEST_B) == 3 + 12 &&
	       W(MOTU424_CB_SMPTE_STOP) == stops + 1);
	/* restarted for the new rate: offset halved at 4x */
	assert(R(MOTU424_CB_SMPTE_START_FRAME) == 108001 &&
	       R(MOTU424_CB_SMPTE_START_OFFSET) == (6400u << 16) / 2);
	m.mult = 1;
	m.rate = 48000;
	m.wire[0].mult = 1;
	m.wire[0].info = &motu424_ifaces[5];
	motu424_smpte_rate_changed(&m);

	/* the 2408mk3's dedicated output: no level, S/PDIF slots clobbered */
	assert(put(MOTU424_SMPTE_CTL_DEST, MOTU424_SMPTE_JACK(1)) == 1);
	assert(R(MOTU424_CB_SMPTE_DEST_A) == 0 &&
	       R(MOTU424_CB_SMPTE_DEST_JACK) == 2 &&
	       R(MOTU424_CB_SMPTE_CLOBBER) == ((42u << 16) | 43));
	assert(m.wire[1].smpte_out &&
	       (m.wire[1].state_msg[15] & MOTU424_2408MK3_B15_SMPTE_OUT));
	ready(1);
	assert(put(MOTU424_SMPTE_CTL_LEVEL, 0x200) == 1 &&
	       R(MOTU424_CB_SMPTE_LEVEL) == 0x100);

	/* bank C back to ADAT: no slots used */
	m.wire[1].state_msg[11] = MOTU424_2408_CFG1_DEFAULT;
	motu424_smpte_refresh(&m);
	assert(R(MOTU424_CB_SMPTE_CLOBBER) == 0);

	stops = W(MOTU424_CB_SMPTE_STOP);
	assert(put(MOTU424_SMPTE_CTL_GENERATE, 0) == 1);
	assert(W(MOTU424_CB_SMPTE_STOP) == stops + 1);
	assert(get(MOTU424_SMPTE_CTL_GEN_TIME, 0) == 0);
	assert(put(MOTU424_SMPTE_CTL_DEST, 0) == 1);
	assert(!m.wire[1].smpte_out);
	ready(1);
}

static void test_video_and_smux(void)
{
	struct snd_kcontrol *k = ctls[MOTU424_SMPTE_CTL_COUNT];
	struct snd_ctl_elem_value v = { 0 };

	m.word_source = MOTU424_WS_VIDEO_W1;
	motu424_smpte_refresh(&m);
	assert(m.wire[1].smpte_b14 == (MOTU424_2408MK3_B14_VIDEO |
				       MOTU424_2408MK3_B14_VIDEO_MASTER));
	assert((m.wire[1].state_msg[14] & 0x30) == 0x30);

	/* a box busy sending keeps its bits until the poll retries */
	m.wire[1].tx_len = 10;
	m.wire[1].tx_pos = 0;
	m.word_source = MOTU424_WS_INTERNAL;
	motu424_smpte_refresh(&m);
	assert(m.smpte.box_retry && m.wire[1].smpte_b14 != 0);
	ready(1);
	poll();
	assert(!m.smpte.box_retry && m.wire[1].smpte_b14 == 0);
	ready(1);

	/* S/MUX Type I: the 2408mk3's ADAT banks (B, C) */
	smux_writes = 0;
	v.value.integer.value[0] = 1;
	assert(k->put(k, &v) == 1);
	assert(smux[1] == 0xffff00 && smux[0] == 0 && smux_writes == 1);
	assert(k->put(k, &v) == 0);
	m.wire[1].state_msg[11] |= MOTU424_2408_CFG1_TDIF(1);
	motu424_smpte_refresh(&m);
	assert(smux[1] == 0xff0000 && smux_writes == 2);
	m.wire[1].state = MOTU424_WIRE_CONFIG;
	m.wire[1].state_msg[11] &= ~MOTU424_2408_CFG1_TDIF(1);
	motu424_smpte_refresh(&m);
	assert(smux[1] == 0xff0000 && m.smpte.box_retry);
	ready(1);
	poll();
	assert(smux[1] == 0xffff00 && !m.smpte.box_retry);
	v.value.integer.value[0] = 2;
	assert(k->put(k, &v) == -EINVAL);
	v.value.integer.value[0] = 0;
	assert(k->put(k, &v) == 1 && smux[1] == 0);
}

int main(void)
{
	unsigned int i;

	test_timecode();
	test_create_and_params();
	test_reader();
	test_generator();
	test_video_and_smux();

	m.booted = false;
	assert(put(MOTU424_SMPTE_CTL_FORMAT, MOTU424_SMPTE_25) == -ENODEV);
	motu424_smpte_cancel(&m);
	assert(!m.smpte.poll.queued && m.smpte.stopped);
	for (i = 0; i < nctls; i++)
		free(ctls[i]);
	printf("smpte test: ok (%u notifications)\n", notifies);
	return 0;
}
