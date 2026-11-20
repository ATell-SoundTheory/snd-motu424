/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MOTU PCI-424 / PCI-424X / PCIe-424 ALSA driver - shared definitions.
 *
 * Register/protocol details were recovered by static analysis of MOTU's
 * macOS driver (MOTUPCIAudio.kext 1.6). See docs/HARDWARE.md.
 */
#ifndef MOTU424_H
#define MOTU424_H

#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>

#define MOTU424_DRV_NAME	"snd-motu424"

#define PCI_VENDOR_ID_MOTU	0x137a
#define PCI_DEVICE_ID_MOTU_PCI324	0x0001	/* not supported (see docs) */
#define PCI_DEVICE_ID_MOTU_PCI424	0x0003
#define PCI_DEVICE_ID_MOTU_PCI424X	0x0004
#define PCI_DEVICE_ID_MOTU_PCIE424	0x0005

/* TI XIO2000A PCIe-to-PCI bridge used on the PCIe-424 */
#define PCI_DEVICE_ID_TI_XIO2000A	0x8231

/*
 * DSP address space as seen through the PCI BARs (TMS320C6205 / C6412 HPI-like
 * PCI port):
 *  BAR0: 4 MiB prefetchable window into DSP memory, page selected by DSPP.
 *  BAR1: 8 MiB window onto the peripheral space at 0x01800000.
 */
#define MOTU424_MEM_WINDOW	0x00400000
#define MOTU424_MEM_MASK	0x003fffff
#define MOTU424_PERIPH_MASK	0x007fffff
#define MOTU424_IS_PERIPH(a)	(((a) >> 23) == 3)

/* host control registers, relative to the per-DSP "hsr" offset in BAR1 */
#define MOTU424_HSR		0x0	/* host status */
#define  MOTU424_HSR_INTAVAL	BIT(1)	/* DSP->host interrupt pending */
#define  MOTU424_HSR_INTAM	BIT(2)	/* interrupt mask */
#define MOTU424_HDCR		0x4	/* host-to-DSP control */
#define  MOTU424_HDCR_WARMRESET	BIT(0)
#define  MOTU424_HDCR_DSPINT	BIT(1)
#define MOTU424_DSPP		0x8	/* memory window page (addr >> 22) */

#define MOTU424_RSTSRC_INTRST	0x10

/* C6000 peripheral addresses used during boot */
#define C6X_EMIF_CECTL0		0x01800008
#define C6X_MCBSP0_BASE		0x018c0000
#define C6X_MCBSP1_BASE		0x01900000
#define C6X_MCBSP_PCR		0x24
#define C6X_GPIO_GPEN		0x01b00000
#define C6X_GPIO_GPDIR		0x01b00004
#define C6X_GPIO_GPVAL		0x01b00008

/*
 * Control block ("cb") in DSP memory. Its address is posted by the DSP
 * firmware in the boot mailbox after DSPINT.
 */
#define CB_ROUTING_TABLE_PTR	0x14	/* -> 24 dwords input routing table */
#define CB_DMA_POSITION		0x2c	/* current DMA bus address */
#define CB_HOST_READY		0x50
#define CB_IRQ_ENABLE		0x54
#define CB_IDLE			0x58	/* 1: idle (no audio slots) */
#define CB_DMA_BUFFER		0x5c	/* host buffer bus address */
#define CB_SAMPLES_PER_IRQ	0x60	/* 16 << (2 * n) */
#define CB_IRQS_PER_BUFFER	0x64
#define CB_NUM_INPUTS		0x68	/* active input slots */
#define CB_NUM_OUTPUTS		0x6c	/* active output slots */
#define CB_ALTERA_CONFIG	0x80
#define CB_ADDRESS_SOURCE	0x84
#define CB_RATE_CONSTANT	0x98
#define CB_STOP_ACK		0xf4
#define CB_ADDRESS_SRC_COMMIT	0x114
#define CB_PCI_USAGE		0x128

#define CB_RATE_48K		0x05355555
#define CB_RATE_44K1		0x05acbf46

/*
 * DMA layout: the ring buffer is a sequence of blocks of 16 sample frames.
 * Each block has (n_out + n_in + 2) slots of 12 dwords; each slot carries
 * 16 samples of one channel, packed as 24-bit big-endian in 32-bit words.
 * Slot 0: header, slots 1..n_out: playback, then capture, last: status.
 */
#define MOTU424_FRAMES_PER_BLOCK	16
#define MOTU424_QUAD_PARTNER		12	/* 4x: partner slot = hw + 12 */
#define MOTU424_SLOT_DWORDS		12
#define MOTU424_BLOCKS_PER_PERIOD_MAX	64
#define MOTU424_PERIODS			3	/* engine periods in the DMA ring */
#define MOTU424_MAX_WIRES		4
#define MOTU424_CH_PER_WIRE		24
#define MOTU424_MAX_CHANNELS	(MOTU424_MAX_WIRES * MOTU424_CH_PER_WIRE)
#define MOTU424_DMA_SKEW		0x10

enum motu424_dsp_type {
	MOTU424_DSP_C6205,	/* PCI-424 */
	MOTU424_DSP_C6412,	/* PCI-424X, PCIe-424 */
};

/* per-model boot parameters (kParameters in the Mac driver) */
struct motu424_params {
	const char *name;
	enum motu424_dsp_type dsp;
	u32 hsr_offset;		/* host control block offset in BAR1 */
	u32 rstsrc;		/* DSP address of RSTSRC (interrupt ack) */
	const char *fw_altera;
	const char *fw_prog;
	u32 prog_dest;
	u32 prog_clear;
	const char *fw_data;	/* optional */
	u32 data_dest;
	u32 data_clear;
	u32 mailbox;
	u32 emif_cectl0;
};

struct motu424_iface_info;

struct motu424_layout {
	unsigned int n_in, n_out;
	u8 in_hw[MOTU424_MAX_CHANNELS];
	u8 out_hw[MOTU424_MAX_CHANNELS];
	u64 out_pairs, split_pairs, skip_pairs;
	bool odd;	/* an output without a stereo partner is unused */
};
struct motu424_mixer_state;
struct motu424_dsp;
struct motu424_controls;

/* per AudioWire port; ABSENT must be 0 */
enum motu424_wire_state {
	MOTU424_WIRE_ABSENT,
	MOTU424_WIRE_DETECTED,		/* present, inquiry due at deadline */
	MOTU424_WIRE_QUERY,		/* device inquiry sent */
	MOTU424_WIRE_INFO,		/* family 2: reading the box's description */
	MOTU424_WIRE_CONFIG,		/* state sent, waiting for the echo */
	MOTU424_WIRE_READY,
	MOTU424_WIRE_UNCONFIGURED,	/* identified, state not set */
};

#define MOTU424_SIMPLE_MAX_BANKS	8
#define MOTU424_SIMPLE_MAX_CLOCKS	24
#define MOTU424_SIMPLE_NAME_LEN		50

struct motu424_wire {
	enum motu424_wire_state state;
	const struct motu424_iface_info *info;
	u16 model;			/* from the inquiry reply */
	unsigned int absent_periods;
	unsigned int attempts;
	unsigned int backoff;
	u32 deadline;			/* in MIDI ticks */
	u8 state_msg[17];
	unsigned int state_len;
	u8 aes_input_pair;
	u8 aes_output_src_mode;
	u8 aes_output_clock;
	u8 aes_input_src;
	u8 peak_hold_time;
	u8 clip_hold_time;
	u8 analog_mirror;		/* 2408 AnalogMirror: bank index */
	u8 input_levels;		/* InputLevels: set bit = -10 dBV */
	u8 word_out_rate;		/* WordOutRange: 0 match, 1 1x, 2 2x */
	u8 clock_source;		/* 2408: MOTU424_2408_CLK_*, 1296: _1296_UICLK_* */
	bool external_clock;
	u8 smpte_b14;			/* 2408mk3 video/LTC bits (SetVideoSyncMode) */
	bool smpte_out;			/* 2408mk3 dedicated SMPTE output on */
	u8 mult;			/* multiplier in the state message */
	u8 tx[32];
	unsigned int tx_len, tx_pos;
	unsigned int tx_split;		/* end of the first of two messages */
	u32 tx_next;			/* tick of the next byte */
	u8 rx[128];
	unsigned int rx_len;
	unsigned int rx_bytes;		/* statistics */
	bool rx_active;

	/* family 2 ("simple") boxes describe themselves */
	unsigned int step;
	bool str_mode;			/* reading custom names of this step */
	int str_idx;
	char name[MOTU424_SIMPLE_NAME_LEN];
	u8 nbanks, nclocks;
	u8 bank_type[MOTU424_SIMPLE_MAX_BANKS];
	u8 bank_chans[MOTU424_SIMPLE_MAX_BANKS];
	u8 clock_type[MOTU424_SIMPLE_MAX_CLOCKS];
	u8 avail[8];			/* 7 channels per byte: 4 in, 4 out */
};

enum motu424_iface_control {
	MOTU424_IFACE_2408_ANALOG_BANK,
	MOTU424_IFACE_2408_BANK_A_FORMAT,
	MOTU424_IFACE_2408_BANK_B_FORMAT,
	MOTU424_IFACE_2408_BANK_C_FORMAT,
	MOTU424_IFACE_2408_CLOCK_SOURCE,
	MOTU424_IFACE_AES_INPUT_PAIR,
	MOTU424_IFACE_AES_OUTPUT_SRC_MODE,
	MOTU424_IFACE_AES_OUTPUT_CLOCK,
	MOTU424_IFACE_AES_INPUT_SRC,
	MOTU424_IFACE_PEAK_HOLD_TIME,
	MOTU424_IFACE_CLIP_HOLD_TIME,
	MOTU424_IFACE_2408_ANALOG_MIRROR,
	MOTU424_IFACE_WORD_OUT_RATE,
	MOTU424_IFACE_24IO_LEVEL_1_8,
	MOTU424_IFACE_24IO_LEVEL_9_16,
	MOTU424_IFACE_24IO_LEVEL_17_24,
	MOTU424_IFACE_2408MK3_LEVEL_1_2,
	MOTU424_IFACE_2408MK3_LEVEL_3_4,
	MOTU424_IFACE_2408MK3_LEVEL_5_6,
	MOTU424_IFACE_2408MK3_LEVEL_7_8,
	MOTU424_IFACE_1296_CLOCK_SOURCE,
	MOTU424_IFACE_COUNT
};

/*
 * SMPTE reader source / generator destination codes of the ALSA controls:
 * 0 none, 1..96 channel (port * 24 + position + 1), 97..100 the dedicated
 * SMPTE jack of the 2408mk3 on port 0..3.
 */
#define MOTU424_SMPTE_NONE		0
#define MOTU424_SMPTE_JACK(port)	(MOTU424_MAX_CHANNELS + 1 + (port))
#define MOTU424_SMPTE_CODE_MAX		MOTU424_SMPTE_JACK(MOTU424_MAX_WIRES - 1)

/* "SMPTE Frame Format" */
enum motu424_smpte_format {
	MOTU424_SMPTE_24,
	MOTU424_SMPTE_25,
	MOTU424_SMPTE_2997_DF,
	MOTU424_SMPTE_2997_ND,
	MOTU424_SMPTE_30,
	MOTU424_SMPTE_FORMATS
};

enum motu424_smpte_control {
	MOTU424_SMPTE_CTL_FORMAT,
	MOTU424_SMPTE_CTL_FOLLOW,
	MOTU424_SMPTE_CTL_DETECTED,
	MOTU424_SMPTE_CTL_SOURCE,
	MOTU424_SMPTE_CTL_STATE,
	MOTU424_SMPTE_CTL_STATUS,
	MOTU424_SMPTE_CTL_FW_ADDRESS,
	MOTU424_SMPTE_CTL_FW_CLOCK,
	MOTU424_SMPTE_CTL_DEST,
	MOTU424_SMPTE_CTL_LEVEL,
	MOTU424_SMPTE_CTL_START,
	MOTU424_SMPTE_CTL_GENERATE,
	MOTU424_SMPTE_CTL_GEN_TIME,
	MOTU424_SMPTE_CTL_CLOBBERED,
	MOTU424_SMPTE_CTL_COUNT
};

/* settings and readback; m->mutex */
struct motu424_smpte {
	u8 format;			/* enum motu424_smpte_format */
	u8 last30;			/* format used for a detected 30-frame code */
	bool follow;			/* switch to a detected format */
	u16 source, dest;		/* MOTU424_SMPTE_* codes as set */
	u16 prog_source, prog_dest;	/* as programmed (0 if not available) */
	int fw_address, fw_clock;	/* freewheel, ms; -1 infinite */
	u32 level;
	bool generate;
	u8 start[4];			/* hh mm ss ff */
	u8 state;			/* Drv::SetTransportState value, 0 inactive */
	u8 detected;			/* 0 none, 1 + detected format code */
	u32 status[2];			/* cb+0x34, cb+0x38 */
	u32 gen_frame;
	bool clobbered;
	u32 regs_a, regs_b, regs_jack, regs_clobber;	/* as programmed */
	bool box_retry;			/* a 2408mk3 still needs its bits */
	bool stopped;			/* no more polling */
	struct delayed_work poll;
	struct snd_kcontrol *kctl[MOTU424_SMPTE_CTL_COUNT];
};

/* "Interface 2408 Analog Bank" value for no analog bank (AnalogMirror applies) */
#define MOTU424_2408_ANALOG_NONE	3

struct motu424 {
	struct pci_dev *pci;
	struct snd_card *card;
	const struct motu424_params *params;
	/* Per-card C6205 FPGA bit-bang state. */
	u32 fpga6205_data;
	u32 fpga6205_nconfig;

	void __iomem *mem;	/* BAR0 */
	void __iomem *periph;	/* BAR1 */
	void __iomem *hctl;	/* BAR1 + hsr_offset */
	u32 page;		/* current DSPP */

	u32 cb;			/* control block DSP address */
	u32 cb_words[5];	/* cb+0,4,8,0x14,0x18 at boot */
	bool booted;

	/* DMA ring */
	void *dma_area;
	dma_addr_t dma_addr;	/* bus address handed to the DSP (skewed) */
	void *dma_raw;
	dma_addr_t dma_raw_addr;
	size_t dma_raw_bytes;
	size_t dma_bytes;

	unsigned int n_out;		/* PCM playback channels */
	unsigned int n_in;		/* PCM capture channels */

	/*
	 * Channel layout at the current multiplier: the channels of the
	 * configured interfaces, in ascending wire-slot order (port * 24 +
	 * position). It follows the interfaces, except while a PCM stream is
	 * open (frozen). At 4x each channel also uses the partner slot 12
	 * positions further on its wire.
	 */
	u8 in_hw[MOTU424_MAX_CHANNELS];	/* capture channel -> hardware input */
	u8 out_hw[MOTU424_MAX_CHANNELS];	/* playback channel -> hardware output */
	u64 out_pairs;			/* output pairs with playback channels */
	u64 split_pairs;		/* of those: L to slot 2p, R to slot 2p + 2 */
	u64 skip_pairs;			/* slots taken by a split pair: no mixer code */
	bool layout_frozen;

	/* DSP mixer program; m->mutex */
	struct motu424_mixer_state *mix_state;
	struct motu424_dsp *mix_dsp;
	struct motu424_controls *controls;
	struct snd_rawmidi *rawmidi;
	struct snd_rawmidi_substream *midi_in[MOTU424_MAX_WIRES];
	struct snd_rawmidi_substream *midi_out[MOTU424_MAX_WIRES];
	unsigned long midi_in_active;
	unsigned long midi_out_active;

	/* current engine layout; idle = no audio slots, interface MIDI only */
	bool audio;
	unsigned int eng_out;		/* playback slots */
	unsigned int eng_in;		/* capture slots */
	unsigned int pcm_out, pcm_in;	/* ALSA channels: slots / (1 + quad) */
	bool eng_quad;			/* 4x: a second (partner) slot per channel */
	u8 out_slot[2][MOTU424_MAX_CHANNELS];	/* channel -> primary, partner */
	u8 in_slot[2][MOTU424_MAX_CHANNELS];
	unsigned int blocks_per_period;
	unsigned int period_frames;	/* engine period in ALSA frames */
	unsigned int block_dwords;
	unsigned int period_dwords;
	unsigned int buffer_dwords;
	unsigned int last_period;
	u8 routing[96];			/* input slot -> hw channel code */
	unsigned int open_ch[2][3];	/* per stream: channels at 1x, 2x, 4x */

	unsigned int rate;		/* m->mutex for writes, also m->lock */
	unsigned int mult;		/* 1, 2 or 4 (Drv+0x40); like rate */
	bool mult_settle_pending;	/* m->mutex: wait after a multiplier change */
	unsigned int wire_mask;		/* ports the card doubles (ND+0x1f0) */
	u8 midi_toggle;			/* BIT(port): doubled MIDI phase */
	bool midi_half;			/* mult > 1: second packet of a tick */
	bool resettle;			/* a port appeared/went: settle again */
	unsigned int rate_users;	/* BIT(stream) for PCM directions with hw_params */
	unsigned int word_source;
	struct motu424_smpte smpte;
	bool smux_type1;		/* C6412 S/MUX option: Type I (default Type II) */
	u32 smux_mask[MOTU424_MAX_WIRES];	/* as written to cb+0x178/0x188 */
	bool smux_written;
	unsigned int present_mask;	/* AudioWire ports with an interface */
	struct motu424_wire wire[MOTU424_MAX_WIRES];
	u32 midi_tick;			/* 16-frame blocks processed */

	spinlock_t lock;
	struct mutex mutex;		/* serialises DSP control */
	bool running;
	unsigned int open_count;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *playback;
	struct snd_pcm_substream *capture;
	/* per-stream position in the ALSA buffer (frames), counted from the trigger */
	unsigned int play_pos;
	unsigned int capt_pos;
	unsigned int play_skip;		/* engine periods still to pass over */
	unsigned int capt_skip;
	bool eng_fresh;			/* no engine period completed since the start */
	unsigned int period_req[2];	/* per stream: ALSA period size from hw_params */
	unsigned int irq_count;
};

/* motu424_dsp.c */
u32 motu424_read(struct motu424 *m, u32 addr);
void motu424_write(struct motu424 *m, u32 addr, u32 val);
void motu424_set_page(struct motu424 *m, u32 page);
int motu424_boot(struct motu424 *m);
void motu424_reset(struct motu424 *m);
void motu424_irq_mask(struct motu424 *m, bool masked);
bool motu424_irq_pending_ack(struct motu424 *m);
int motu424_set_interrupts(struct motu424 *m, bool enable);
/* ALSA frames per 16-sample block: 32 at 4x (primary + partner slot) */
static inline unsigned int motu424_block_frames(unsigned int mult)
{
	return MOTU424_FRAMES_PER_BLOCK << (mult == 4);
}

void motu424_clock_init(struct motu424 *m);
void motu424_clock_settled(struct motu424 *m);
int motu424_clock_set_source(struct motu424 *m, unsigned int source);
void motu424_clock_set_rate(struct motu424 *m, unsigned int rate);
void motu424_clock_set_mult(struct motu424 *m, unsigned int mult,
			    unsigned int mask);
bool motu424_clock_source_valid(unsigned int source);
void motu424_smux_write(struct motu424 *m, unsigned int port, u32 mask);

/* motu424_smpte.c */
void motu424_smpte_defaults(struct motu424 *m);
void motu424_smpte_hw_init(struct motu424 *m);
void motu424_smpte_address_write(struct motu424 *m, unsigned int source);
void motu424_smpte_rate_changed(struct motu424 *m);
void motu424_smpte_refresh(struct motu424 *m);
int motu424_smpte_create(struct motu424 *m);
void motu424_smpte_cancel(struct motu424 *m);
u64 motu424_smpte_tc_to_frames(const u8 tc[4], unsigned int format);
void motu424_smpte_frames_to_tc(u64 frames, unsigned int format, u8 tc[4]);

/* motu424_pcm.c */
int motu424_pcm_create(struct motu424 *m);
int motu424_rawmidi_create(struct motu424 *m);
void motu424_rawmidi_receive(struct motu424 *m, unsigned int port, u8 byte);
bool motu424_rawmidi_transmit(struct motu424 *m, unsigned int port, u8 *byte);
int motu424_engine_idle(struct motu424 *m);
void motu424_pcm_interrupt(struct motu424 *m);
void motu424_pack16(__le32 *slot, const s32 *src, unsigned int stride);
void motu424_unpack16(const __le32 *slot, s32 *dst, unsigned int stride);

/* motu424_control.c */
int motu424_qmix_create(struct motu424 *m);
int motu424_mixer_sync(struct motu424 *m, bool audio);
void motu424_qmix_schedule(struct motu424 *m);
void motu424_qmix_cancel(struct motu424 *m);
int motu424_qmix_release(struct motu424 *m);
void motu424_qmix_set_mult(struct motu424 *m, unsigned int mult);
bool motu424_qmix_mult_fits(struct motu424 *m, unsigned int mult);

/* motu424_iface.c */
bool motu424_wire_supports(const struct motu424_wire *w, unsigned int mult);
bool motu424_wire_chan_available(const struct motu424_wire *w,
				 unsigned int pos, bool out, unsigned int mult);
void motu424_layout_compute(struct motu424 *m, unsigned int mult,
			    struct motu424_layout *l);
unsigned int motu424_iface_wire_mask(struct motu424 *m, unsigned int mult);
void motu424_iface_set_mult(struct motu424 *m, unsigned int mult);
int motu424_iface_drain(struct motu424 *m);
void motu424_midi_block(struct motu424 *m, const __le32 *status, __le32 *header);
void motu424_iface_period(struct motu424 *m, unsigned int present);
void motu424_iface_engine_restart(struct motu424 *m);
void motu424_iface_rate_changed(struct motu424 *m);
void motu424_iface_wait_settled(struct motu424 *m);
void motu424_iface_shutdown(struct motu424 *m);
void motu424_iface_proc_init(struct motu424 *m);
int motu424_iface_control_get(struct motu424 *m, unsigned int port,
			      enum motu424_iface_control control, long *value);
int motu424_iface_control_put(struct motu424 *m, unsigned int port,
			      enum motu424_iface_control control, long value);
bool motu424_iface_control_available(struct motu424 *m, unsigned int port,
				     enum motu424_iface_control control);
bool motu424_wire_input_analog(const struct motu424_wire *w, unsigned int pos);
bool motu424_wire_is_2408mk3(const struct motu424_wire *w);
u32 motu424_iface_smux_mask(struct motu424 *m, unsigned int port);
unsigned int motu424_iface_smpte_jack_slots(struct motu424 *m,
					    unsigned int port);
bool motu424_iface_set_smpte(struct motu424 *m,
			     const u8 b14[MOTU424_MAX_WIRES],
			     unsigned int out_mask);
void motu424_layout_freeze(struct motu424 *m, bool freeze);
void motu424_layout_refresh(struct motu424 *m);
unsigned int motu424_build_slots(const u8 *hw, unsigned int n, bool quad,
				 u8 slot[2][MOTU424_MAX_CHANNELS], u8 *slot_hw);

#endif
