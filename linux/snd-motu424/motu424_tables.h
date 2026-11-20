/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MOTU PCI-424: tables for the AudioWire interfaces, clocking and the status slot.
 *
 * Every value here was recovered by static analysis of MOTU's macOS kext.
 * docs/INTERFACES.md explains each one and cites the kext address it
 * comes from. Comments marked "uncertain" are inferences.
 */
#ifndef MOTU424_TABLES_H
#define MOTU424_TABLES_H

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/compiler.h>

#define MOTU424_NUM_WIRES		4
#define MOTU424_CHANS_PER_WIRE		24
#define MOTU424_MAX_HW_CHANS		(MOTU424_NUM_WIRES * MOTU424_CHANS_PER_WIRE)
#define MOTU424_HW_CHAN(wire, pos)	((wire) * MOTU424_CHANS_PER_WIRE + (pos))

/* ---------------------------------------------------------------------- */
/* Control-block word offsets (relative to cb)                            */
/* ---------------------------------------------------------------------- */

#define MOTU424_CB_SAMPLES_PER_IRQ	0x060	/* frames: 16 << (2 * n) */
#define MOTU424_CB_IRQS_PER_BUFFER	0x064
#define MOTU424_CB_ALTERA_CONFIG	0x080
#define MOTU424_CB_ADDRESS_SOURCE	0x084
#define MOTU424_CB_ONEX_CLK0		0x088
#define MOTU424_CB_ONEX_CLK1		0x08c
#define MOTU424_CB_ONEX_CLK2		0x090
#define MOTU424_CB_RATE_CONST		0x098
#define MOTU424_CB_ADDRESS_PARAM	0x0a8	/* ND+0x278 (uncertain meaning) */
#define MOTU424_CB_VIDEO_SYNC		0x0ac	/* 25 * (ws - 4) + 0x18, for ws 4..7 */
#define MOTU424_CB_COMMIT		0x114	/* written as 1 after +0x84/+0xa8 */
#define MOTU424_VIDEO_SYNC_PARAM(w)	(25 * (w) + 0x18)

/* DSP channel numbers of the SMPTE paths: 25 per wire (docs/SMPTE.md 2.1) */
#define MOTU424_SMPTE_DSP_CHAN(w, ch)	(25 * (w) + 1 + (ch))
#define MOTU424_SMPTE_DSP_JACK(w)	(25 * (w) + 25)

/* SMPTE reader and generator (docs/SMPTE.md) */
#define MOTU424_CB_SMPTE_ADDR_STATE	0x034	/* LTC address reader state */
#define MOTU424_CB_SMPTE_CLOCK_STATE	0x038	/* clock/video reference state */
#define MOTU424_CB_SMPTE_DETECTED	0x040	/* 24/25/29/30, host clears */
#define MOTU424_CB_SMPTE_CONST_K	0x044	/* DSP constants, read only */
#define MOTU424_CB_SMPTE_CONST_V	0x048
#define MOTU424_CB_SMPTE_GEN_FRAME	0x04c
#define MOTU424_CB_SMPTE_FPS		0x09c
#define MOTU424_CB_SMPTE_FRAMES_MIN	0x0a0
#define MOTU424_CB_SMPTE_FRAMES_10MIN	0x0a4
#define MOTU424_CB_SMPTE_COUNT_FPS	0x0b0
#define MOTU424_CB_SMPTE_BIT_RATE	0x0b4
#define MOTU424_CB_SMPTE_K_SAMPLES	0x0b8
#define MOTU424_CB_SMPTE_FRAME_INT	0x0bc
#define MOTU424_CB_SMPTE_FRAME_FRAC	0x0c0
#define MOTU424_CB_SMPTE_FW_CLOCK	0x0c4
#define MOTU424_CB_SMPTE_FW_ADDRESS	0x0c8
#define MOTU424_CB_SMPTE_DEST_A		0x0cc
#define MOTU424_CB_SMPTE_DEST_B		0x0d0
#define MOTU424_CB_SMPTE_DEST_JACK	0x0d4
#define MOTU424_CB_SMPTE_CLOBBER	0x0d8
#define MOTU424_CB_SMPTE_LEVEL		0x0dc
#define MOTU424_CB_SMPTE_V_HALF		0x0e0
#define MOTU424_CB_SMPTE_V_SAMPLES	0x0e4
#define MOTU424_CB_SMPTE_START_FRAME	0x0e8
#define MOTU424_CB_SMPTE_START_OFFSET	0x0ec
#define MOTU424_CB_SMPTE_F0		0x0f0
#define MOTU424_CB_SMPTE_STOP		0x118
#define MOTU424_CB_SMPTE_CLOBBERED	0x11c

/* S/MUX option masks of port p (C6412 only): +4p, same value in both */
#define MOTU424_CB_SMUX_A		0x178
#define MOTU424_CB_SMUX_B		0x188

/* The kext always uses n = 3 (1024 frames) and 3 interrupts per buffer. */
#define MOTU424_SAMPLES_PER_IRQ(n)	(16u << (2 * (n)))
#define MOTU424_DEFAULT_SPI_LOG		3
#define MOTU424_DEFAULT_IRQS_PER_BUF	3
#define MOTU424_BLOCK_FRAMES		16

/* cb+0x98 */
#define MOTU424_RATE_CONST_48K		0x05355555u
#define MOTU424_RATE_CONST_44K1		0x05acbf46u

/* cb+0x80 = altera | kModes[ws] | (settled ? SETTLED : 0) */
#define MOTU424_ALTERA_BASE		0x80000000u
#define MOTU424_ALTERA_MULTI		0x00001000u
#define MOTU424_ALTERA_SETTLED		0x000f0000u
#define MOTU424_ALTERA_SETTLE_FRAMES	40000
/* mask bit w set: wire w is not natively 2x, so the card doubles its channels */
#define MOTU424_ALTERA_CONFIG(mult, mask)				\
	((mult) <= 1 ? MOTU424_ALTERA_BASE :				\
	 (MOTU424_ALTERA_BASE | MOTU424_ALTERA_MULTI |			\
	  (((u32)(mask) & 0xf) << 24) | ((((u32)(mask) ^ 0xf) & 0xf) << 20)))

/* cb+0x88/0x8c/0x90 ("one-x clock bits") */
#define MOTU424_ONEX_DEFAULT_0		1u
#define MOTU424_ONEX_DEFAULT_1		0x80000u
#define MOTU424_ONEX_ADAT_1		0x40000u
#define MOTU424_ONEX_WIRE_DBL_1(w)	(1u << ((w) + 14))
#define MOTU424_ONEX_WIRE_NAT_0(w)	(25u * (w) + 0x1c)
#define MOTU424_ONEX_WIRE_NAT_2		0x20000u

/* ---------------------------------------------------------------------- */
/* Card word (clock) source: ND+0x38                                      */
/* ---------------------------------------------------------------------- */

enum motu424_word_source {
	MOTU424_WS_INTERNAL	= 0,
	MOTU424_WS_CONTROL_TRK	= 1,	/* not supported on the 424 */
	MOTU424_WS_ADAT		= 2,	/* ADAT 9-pin sync */
	MOTU424_WS_SMPTE	= 3,	/* LTC */
	MOTU424_WS_VIDEO_W0	= 4,	/* video sync input of the box on wire 0..3 */
	MOTU424_WS_VIDEO_W1	= 5,
	MOTU424_WS_VIDEO_W2	= 6,
	MOTU424_WS_VIDEO_W3	= 7,
	MOTU424_WS_WIRE0	= 8,	/* AudioWire clock from the box on wire 0..3 */
	MOTU424_WS_WIRE1	= 9,
	MOTU424_WS_WIRE2	= 10,
	MOTU424_WS_WIRE3	= 11,
	MOTU424_WS_COUNT	= 12,
};

/* kModes[] (kext 0x3e440); index = word source */
static const u32 motu424_ws_modes[16] __maybe_unused = {
	0x200, 0x200, 0x800, 0x201, 0x202, 0x202, 0x202, 0x202,
	0x400, 0x500, 0x600, 0x700, 0, 0, 0, 0,
};

/* Always supported: 0, 2, 3, 8..11. 4..7 also need a box with video sync. */
#define MOTU424_WS_ALWAYS_MASK		0x0f0du
#define MOTU424_WS_VIDEO_MASK		0x00f0u

static const char * const motu424_ws_names[MOTU424_WS_COUNT] __maybe_unused = {
	"Internal", "Control Track", "ADAT", "SMPTE",
	"Video (port 1)", "Video (port 2)", "Video (port 3)", "Video (port 4)",
	"AudioWire 1", "AudioWire 2", "AudioWire 3", "AudioWire 4",
};

/* cb+0x84 */
#define MOTU424_ADDR_SRC_NONE		0
#define MOTU424_ADDR_SRC_ADAT		1
#define MOTU424_ADDR_SRC_SMPTE_A	2	/* uncertain */
#define MOTU424_ADDR_SRC_SMPTE_B	3	/* uncertain */

/* ---------------------------------------------------------------------- */
/* Sample rates                                                           */
/* ---------------------------------------------------------------------- */

struct motu424_rate {
	u32 rate;
	u32 base;
	u8 mult;	/* Drv+0x40 */
	u8 card_doubles;	/* Drv+0x44: the card itself doubles (4x) */
	u8 wire_mult;	/* framing on natively capable wires */
	u32 rate_const;	/* cb+0x98 */
};

static const struct motu424_rate motu424_rates[] __maybe_unused = {
	{  44100, 44100, 1, 0, 1, MOTU424_RATE_CONST_44K1 },
	{  48000, 48000, 1, 0, 1, MOTU424_RATE_CONST_48K  },
	{  88200, 44100, 2, 0, 2, MOTU424_RATE_CONST_44K1 },
	{  96000, 48000, 2, 0, 2, MOTU424_RATE_CONST_48K  },
	{ 176400, 44100, 4, 1, 2, MOTU424_RATE_CONST_44K1 },
	{ 192000, 48000, 4, 1, 2, MOTU424_RATE_CONST_48K  },
};

/* ---------------------------------------------------------------------- */
/* Status slot: last 12 dwords of every 16-frame block                    */
/* ---------------------------------------------------------------------- */

#define MOTU424_STATUS_DWORDS		12
#define MOTU424_ST_MIDI(wire)		(wire)	/* s[0..3] */
#define MOTU424_ST_MIDI_DEST4		4
#define MOTU424_ST_TS_HI		5
#define MOTU424_ST_TS_LO		6
#define MOTU424_ST_TS_ADAT		7	/* s[7] - 0x57e400 - 0x2d when ws == ADAT */
#define MOTU424_ST_FLAGS		8

#define MOTU424_ST_ADAT_TS_OFFSET	(0x57e400u + 0x2du)

#define MOTU424_ST_MIDI_HI_VALID	BIT(24)
#define MOTU424_ST_MIDI_HI_BYTE(v)	(((v) >> 16) & 0xff)
#define MOTU424_ST_MIDI_LO_VALID	BIT(8)
#define MOTU424_ST_MIDI_LO_BYTE(v)	((v) & 0xff)

#define MOTU424_ST_PRESENT_MASK(s8)	((s8) & 0xf)
/* in doubled-MIDI mode: the byte for this wire is in the low half */
#define MOTU424_ST_MIDI_LOW_SEL(wire)	BIT(6 + (wire))

/* Wires must be present for this many consecutive periods (debounce). */
#define MOTU424_PRESENT_DEBOUNCE	100

/* ---------------------------------------------------------------------- */
/* MIDI out: packet in slot 0 of the matching block in the next period    */
/* ---------------------------------------------------------------------- */

#define MOTU424_MIDI_DESTS		5
#define MOTU424_MIDIOUT_DWORDS		11
#define MOTU424_MIDIOUT_IDX(dest)	(1 + 2 * (dest))
#define MOTU424_MIDIOUT_IDX2(dest)	(2 + 2 * (dest))	/* doubled mode, dest <= 3 */

static const u32 motu424_midi_dest_valid[MOTU424_MIDI_DESTS] __maybe_unused = {
	0x20000, 0x40000, 0x80000, 0x100000, 0x10000,
};

#define MOTU424_MIDIOUT_WORD(dest, byte)				\
	((((u32)(byte) & 0xff) << 8) | motu424_midi_dest_valid[(dest)])

/* ---------------------------------------------------------------------- */
/* Bank personalities                                                     */
/* ---------------------------------------------------------------------- */

enum motu424_2408_personality {
	MOTU424_P2408_ADAT	= 0,
	MOTU424_P2408_TDIF	= 1,
	MOTU424_P2408_ANALOG	= 2,
	MOTU424_P2408_SPDIF	= 3,
};

static const char * const motu424_2408_personality_names[] __maybe_unused = {
	"ADAT", "TDIF", "Analog", "SPDIF",
};

/* Allowed personalities per 2408 bank; 0xff = none (kext 0x4d27c). */
static const u8 motu424_2408_bank_personalities[3][4] __maybe_unused = {
	{ 0, 1, 2, 0xff },
	{ 0, 1, 2, 0xff },
	{ 0, 1, 2, 3 },
};

/* Personality codes reported by family-2 ("simple") boxes. 0x7f = custom string. */
static const char * const motu424_simple_personality_names[] __maybe_unused = {
	"Analog", "ADAT", "TDIF", "SPDIF", "AES/EBU", "Sony", "TOS-Link",
};
#define MOTU424_SIMPLE_PERSONALITY_CUSTOM	0x7f

/* ---------------------------------------------------------------------- */
/* Interface types                                                        */
/* ---------------------------------------------------------------------- */

enum motu424_iface_class {
	MOTU424_IFC_NONE = 0,
	MOTU424_IFC_2408,
	MOTU424_IFC_SIMPLE,
	MOTU424_IFC_1296,
	MOTU424_IFC_24IO,
	MOTU424_IFC_GENERIC,
};

#define MOTU424_FAMILY_CLASSIC		1
#define MOTU424_FAMILY_SIMPLE		2
#define MOTU424_MODEL_ANY		0xffff

#define MOTU424_IFACE_ID_2408		2408
#define MOTU424_IFACE_ID_2408MK3	2403
#define MOTU424_IFACE_ID_24IO		2410
#define MOTU424_IFACE_ID_1296		1296
#define MOTU424_IFACE_ID_HD192		192
#define MOTU424_IFACE_ID_SIMPLE		1224
#define MOTU424_IFACE_ID_GENERIC	9999

#define MOTU424_MAX_BANKS		3

/*
 * Default channel counts per multiplier (index 0 = 1x, 1 = 2x, 2 = 4x),
 * per direction; 0 = rate not supported. 2408: bank A analog, B/C ADAT.
 * Family-2 boxes report their layout at runtime (all zero here).
 */
struct motu424_iface_info {
	u16 family;
	u16 model;	/* MOTU424_MODEL_ANY = wildcard */
	u16 id;
	u8 cls;		/* enum motu424_iface_class */
	u8 mult_mask;	/* BIT(m) for each supported multiplier m */
	u8 hw_mult_mask;	/* natively capable on the wire */
	u8 num_banks;
	u8 bank_chans[MOTU424_MAX_BANKS];
	u8 chans[3];
	const char *name;
};

/* Matching order follows Drv::AddDetectedInterface; GENERIC is the fallback. */
static const struct motu424_iface_info motu424_ifaces[] __maybe_unused = {
	{ 1, 2, MOTU424_IFACE_ID_2408, MOTU424_IFC_2408,
	  BIT(1), BIT(1), 3, { 8, 8, 8 }, { 24, 0, 0 }, "2408" },
	{ 1, 4, MOTU424_IFACE_ID_2408MK3, MOTU424_IFC_2408,
	  BIT(1) | BIT(2), BIT(1) | BIT(2), 3, { 8, 8, 8 }, { 24, 16, 0 },
	  "2408mk3" },
	{ 2, MOTU424_MODEL_ANY, MOTU424_IFACE_ID_SIMPLE, MOTU424_IFC_SIMPLE,
	  BIT(1), BIT(1), 0, { 0, 0, 0 }, { 0, 0, 0 }, "interface" },
	{ 1, 3, MOTU424_IFACE_ID_1296, MOTU424_IFC_1296,
	  BIT(1) | BIT(2), BIT(1), 1, { 24, 0, 0 }, { 12, 12, 0 }, "1296" },
	{ 1, 6, MOTU424_IFACE_ID_HD192, MOTU424_IFC_1296,
	  BIT(1) | BIT(2) | BIT(4), BIT(1) | BIT(2), 1, { 24, 0, 0 },
	  { 12, 12, 12 }, "HD192" },
	/* 2x channel count: uncertain (IsChannelAvailable allows all 24) */
	{ 1, 5, MOTU424_IFACE_ID_24IO, MOTU424_IFC_24IO,
	  BIT(1) | BIT(2), BIT(1) | BIT(2), 1, { 24, 0, 0 }, { 24, 24, 0 },
	  "24I/O" },
	{ 0, MOTU424_MODEL_ANY, MOTU424_IFACE_ID_GENERIC, MOTU424_IFC_GENERIC,
	  BIT(1), BIT(1), 1, { 24, 0, 0 }, { 24, 0, 0 }, "unknown" },
};

/* 1296/HD192: positions 0..11 are used. One stereo pair can be switched to AES. */
#define MOTU424_1296_CHANS		12
#define MOTU424_1296_AES_IN_NAME	"AES/EBU"
#define MOTU424_1296_AES_OUT_NAME	"Analog/AES"

/* 2408: is position c (0..23) available? cfg0 = byte 0, cfg1 = byte 1. */
#define MOTU424_2408_CFG0_CLK_MASK	0x07
#define MOTU424_2408_CFG0_CLK_WORD	0x00
#define MOTU424_2408_CFG0_CLK_DIGITAL	0x01
#define MOTU424_2408_CFG0_CLK_CARD	0x02
#define MOTU424_2408_CFG0_DIGSRC_SHIFT	3	/* 0 A, 1 B, 2 C, 3 SPDIF */
#define MOTU424_2408_CFG0_2X		0x20
#define MOTU424_2408_CFG0_ALWAYS	0x40
#define MOTU424_2408_CFG0_DEFAULT	0x42
#define MOTU424_2408_CFG1_TDIF(bank)	BIT(bank)
#define MOTU424_2408_CFG1_HAS_ANALOG	0x08
#define MOTU424_2408_CFG1_ANALOG_SHIFT	4	/* analog bank index + 1 */
#define MOTU424_2408_CFG1_ANALOG_MASK	0x30
#define MOTU424_2408_CFG1_C_SPDIF	0x40
#define MOTU424_2408_CFG1_DEFAULT	0x18
#define MOTU424_2408_CLK_WORD		0
#define MOTU424_2408_CLK_BANK_A		1
#define MOTU424_2408_CLK_BANK_B		2
#define MOTU424_2408_CLK_BANK_C		3
#define MOTU424_2408_CLK_SPDIF		4
#define MOTU424_2408_CLK_CARD		5

/* 2408 clock sources (index for SetWordClockSource) -> cfg0 low bits */
static const u8 motu424_2408_clock_bits[5] __maybe_unused = {
	0x00, 0x01, 0x09, 0x11, 0x19,
};
static const char * const motu424_2408_clock_names[5] __maybe_unused = {
	"Word Clock In", "Bank A Optical", "Bank B Optical",
	"Bank C Optical", "SPDIF",
};

static const char * const motu424_1296_clock_names[3] __maybe_unused = {
	"Internal", "Word Clock In", "AES/EBU",
};

/* 1296 c0 clock code */
#define MOTU424_1296_CLK_AES		0
#define MOTU424_1296_CLK_CARD		1
#define MOTU424_1296_CLK_WORD		2
#define MOTU424_1296_CLK_INT_44K1	3
#define MOTU424_1296_CLK_INT_48K	4

/* "Interface 1296 Clock Source" (UI clock +0x77; the kext's -1 is CARD) */
#define MOTU424_1296_UICLK_INTERNAL	0
#define MOTU424_1296_UICLK_WORD		1
#define MOTU424_1296_UICLK_AES		2
#define MOTU424_1296_UICLK_CARD		3

/* ---------------------------------------------------------------------- */
/* SysEx messages (sent through the wire's MIDI destination)              */
/* ---------------------------------------------------------------------- */

static const u8 motu424_sysex_dev_inquiry[] __maybe_unused = {
	0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7,
};

/* Device-inquiry reply: family = b[8] | b[9] << 8, model = b[10] | b[11] << 8 */
#define MOTU424_INQ_REPLY_MIN		12
#define MOTU424_INQ_FAMILY(b)		((u16)((b)[8] | ((b)[9] << 8)))
#define MOTU424_INQ_MODEL(b)		((u16)((b)[10] | ((b)[11] << 8)))

/* 2408: [10] = cfg0, [11] = cfg1. Non-mk3 = 13 bytes (F7 at 12); mk3 = 17 bytes. */
static const u8 motu424_sysex_2408_state[17] __maybe_unused = {
	0xf0, 0x00, 0x00, 0x33, 0x20, 0x7f, 0x02, 0x01,
	0x01, 0x00, 0x00, 0x00, 0x7f, 0x7f, 0x00, 0x00, 0xf7,
};
#define MOTU424_2408_STATE_LEN		13
#define MOTU424_2408MK3_STATE_LEN	17

static const u8 motu424_sysex_2408_request[] __maybe_unused = {
	0xf0, 0x00, 0x00, 0x33, 0x20, 0x00, 0x02, 0x20, 0x01, 0xf7,
};

/*
 * 24I/O: [10] = cfg0 (default 0x42, bit 5 = 2x), [11] = InputLevels & 7
 * (bit n = Analog 8n+1..8n+8 at -10 dBV), [15] = word clock out at 2x:
 * WordOutRange 1 -> 0, 2 -> 1, otherwise cfg0 bit 5.
 */
#define MOTU424_24IO_B15_WORD_2X	1
static const u8 motu424_sysex_24io_state[17] __maybe_unused = {
	0xf0, 0x00, 0x00, 0x33, 0x20, 0x7f, 0x02, 0x01,
	0x01, 0x00, 0x42, 0x00, 0x7f, 0x7f, 0x7f, 0x00, 0xf7,
};

/* 1296/HD192: config bytes c0..c3 at [7..10] (AW_1296::state::PackState) */
static const u8 motu424_sysex_1296_state[16] __maybe_unused = {
	0xf0, 0x00, 0x00, 0x33, 0x22, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf7,
};

static const u8 motu424_sysex_1296_request[] __maybe_unused = {
	0xf0, 0x00, 0x00, 0x33, 0x23, 0x01, 0xf7,
};

#define MOTU424_1296_STATE_LEN		16
#define MOTU424_1296_REPLY_MIN		11	/* ReplyGood: length > 10 */
/* c0: clock code (MOTU424_1296_CLK_*) in bits 0..2 */
#define MOTU424_1296_C0_CLK_MASK	0x07
#define MOTU424_1296_C0_NO_AES_IN_SRC	0x08	/* AESInputSRC off */
#define MOTU424_1296_C0_WIRE_2X		0x10	/* 1296 at 2x, HD192 at 4x */
#define MOTU424_1296_C0_2X		0x20	/* 1296 at 2x, HD192 above 1x */
#define MOTU424_1296_C0_ACTIVE		0x40	/* cleared by Breakdown */
/* c1: AESOutputSRCMode in bits 0..3, AESInputSteal pair (0 = none) in 4..6 */
/* c2: computed AES output clock in bits 0..3 */
#define MOTU424_1296_C2_UPDATE		0x10	/* meaning unknown; not echoed */
#define MOTU424_1296_C2_HIGH_RATE	0x20	/* HD192 above 1x */
/* c3: PeakHoldTime << 3 | ClipHoldTime; the kext defaults are 4 and 1 */
#define MOTU424_1296_C3_DEFAULT		((4 << 3) | 1)

/* family 2 description request: [8] = step */
static const u8 motu424_sysex_simple_request[] __maybe_unused = {
	0xf0, 0x00, 0x00, 0x33, 0x20, 0x7f, 0x02, 0x20, 0x00, 0xf7,
};

/* family 2 name request: [8] = list (7 banks, 8 clocks), [9] = index */
static const u8 motu424_sysex_simple_str_request[] __maybe_unused = {
	0xf0, 0x00, 0x00, 0x33, 0x20, 0x7f, 0x02, 0x20, 0x00, 0x00, 0xf7,
};

/* Description steps, in the order AW_SimpleInterface::info asks for them. */
#define MOTU424_SIMPLE_STEP_GENERAL	2	/* bank count, clock count, name */
#define MOTU424_SIMPLE_STEP_BANK_TYPES	3
#define MOTU424_SIMPLE_STEP_BANK_CHANS	4
#define MOTU424_SIMPLE_STEP_CLOCK_TYPES	5
#define MOTU424_SIMPLE_STEP_AVAILABLE	6	/* channel masks: 4 in, 4 out */
#define MOTU424_SIMPLE_STEP_DIGI_CONS	9	/* only if version > 0x300 */
#define MOTU424_SIMPLE_STR_BANKS	7
#define MOTU424_SIMPLE_STR_CLOCKS	8
#define MOTU424_SIMPLE_VERSION_DIGI_CONS 0x300

/* Clock types a family 2 box can report; the state byte is the list index. */
#define MOTU424_SIMPLE_CLK_CARD		0
#define MOTU424_SIMPLE_CLK_INT_44K1	1
#define MOTU424_SIMPLE_CLK_INT_48K	2
#define MOTU424_SIMPLE_CLK_INT_96K	3
#define MOTU424_SIMPLE_CLK_WORD		4
#define MOTU424_SIMPLE_CLK_BANK_A	5	/* .. 12 = bank H */

/* family 2 state: [10] = clock index | MOTU424_SIMPLE_STATE_HOST */
static const u8 motu424_sysex_simple_state[] __maybe_unused = {
	0xf0, 0x00, 0x00, 0x33, 0x20, 0x7f, 0x02, 0x01, 0x01, 0x00, 0x00, 0xf7,
};
#define MOTU424_SIMPLE_STATE_HOST	0x40

/* 2408mk3 state byte 15 (AW_2408::SetMessageBytes) */
#define MOTU424_2408MK3_B15_CARD_CLOCK	0x20	/* clock mode is "card" */
#define MOTU424_2408MK3_B15_WORD_MATCH	0x10	/* WordOutRange 0 (default) */
#define MOTU424_2408MK3_B15_SMPTE_OUT	0x02	/* dedicated SMPTE output */
#define MOTU424_2408MK3_B15_WORD_2X	0x01	/* WordOutRange 2 */
/* byte 14 above the input levels (AW_2408::SetVideoSyncMode) */
#define MOTU424_2408MK3_B14_VIDEO	0x10	/* card clock is video */
#define MOTU424_2408MK3_B14_VIDEO_MASTER 0x20	/* from this box */
#define MOTU424_2408MK3_B14_LTC		0x40	/* LTC is read on this wire */
/* bank C S/PDIF: the dedicated SMPTE output uses wire slots 16 and 17 */
#define MOTU424_2408MK3_SMPTE_OUT_SLOT	16

/* Config sent when the driver lets go of a box (Breakdown) */
#define MOTU424_24IO_CFG_RELEASE	0x03
#define MOTU424_2408_CFG0_RELEASE	0x04

static const u8 motu424_sysex_powerup[] __maybe_unused = {
	0xf0, 0x00, 0x00, 0x33, 0x20, 0x7f, 0x02, 0x03, 0x52, 0xf7,
};

#endif /* MOTU424_TABLES_H */
