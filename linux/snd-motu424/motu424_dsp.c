// SPDX-License-Identifier: GPL-2.0-only
/*
 * MOTU PCI-424 family: DSP host interface, FPGA configuration and boot.
 *
 * Mirrors ND424::DSP, ND424::GateArrayProgrammer6205,
 * ND424b::GateArrayProgrammer6412 and ND424::StartDSP of the Mac driver.
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/swab.h>
#include "motu424.h"
#include "motu424_tables.h"

static bool fpga_delays = true;
module_param(fpga_delays, bool, 0444);
MODULE_PARM_DESC(fpga_delays,
		 "Add conservative delays around FPGA nCONFIG (default: on)");

static bool trace_boot;
module_param(trace_boot, bool, 0644);
MODULE_PARM_DESC(trace_boot, "Verbose boot tracing");

#define tdbg(m, fmt, ...) do {						\
	if (trace_boot)							\
		dev_info(&(m)->pci->dev, fmt, ##__VA_ARGS__);		\
	else								\
		dev_dbg(&(m)->pci->dev, fmt, ##__VA_ARGS__);		\
} while (0)

/* ------------------------------------------------------------ DSP access */

/*
 * ND424::DSP::Write/Read (0x29d80/0x29df0). Memory accesses go through the
 * BAR0 window using the *current* page; the Mac driver never changes the
 * page implicitly, so neither do we.
 */
void motu424_write(struct motu424 *m, u32 addr, u32 val)
{
	if (MOTU424_IS_PERIPH(addr))
		iowrite32(val, m->periph + (addr & MOTU424_PERIPH_MASK));
	else
		iowrite32(val, m->mem + (addr & MOTU424_MEM_MASK));
}

/*
 * Note: the Mac driver byte-swaps peripheral reads (but not writes), most
 * likely a leftover from the PowerPC version. We return the raw value and
 * let callers deal with it explicitly.
 */
u32 motu424_read(struct motu424 *m, u32 addr)
{
	if (MOTU424_IS_PERIPH(addr))
		return ioread32(m->periph + (addr & MOTU424_PERIPH_MASK));
	return ioread32(m->mem + (addr & MOTU424_MEM_MASK));
}

void motu424_set_page(struct motu424 *m, u32 page)
{
	m->page = page;
	iowrite32(page, m->hctl + MOTU424_DSPP);
}

void motu424_reset(struct motu424 *m)
{
	iowrite32(MOTU424_HDCR_WARMRESET, m->hctl + MOTU424_HDCR);
}

void motu424_irq_mask(struct motu424 *m, bool masked)
{
	iowrite32(masked ? MOTU424_HSR_INTAM : 0, m->hctl + MOTU424_HSR);
}

/* ND424::FilterInterrupt: check INTAVAL, ack through RSTSRC */
bool motu424_irq_pending_ack(struct motu424 *m)
{
	u32 hsr = ioread32(m->hctl + MOTU424_HSR);

	if (hsr == ~0u || !(hsr & MOTU424_HSR_INTAVAL))
		return false;
	motu424_write(m, m->params->rstsrc, MOTU424_RSTSRC_INTRST);
	return true;
}

static inline u32 cb_read(struct motu424 *m, u32 off)
{
	return motu424_read(m, m->cb + off);
}

static inline void cb_write(struct motu424 *m, u32 off, u32 val)
{
	motu424_write(m, m->cb + off, val);
}

/* ND424::SetInterruptsEnabled (0x2bad0) */
int motu424_set_interrupts(struct motu424 *m, bool enable)
{
	unsigned long timeout;

	if (enable) {
		cb_write(m, CB_IRQ_ENABLE, 1);
		return 0;
	}
	if (!cb_read(m, CB_IRQ_ENABLE))
		return 0;
	cb_write(m, CB_STOP_ACK, 0);
	cb_write(m, CB_IRQ_ENABLE, 0);
	timeout = jiffies + msecs_to_jiffies(60);
	while (!cb_read(m, CB_STOP_ACK)) {
		if (time_after(jiffies, timeout)) {
			dev_warn(&m->pci->dev, "DSP did not acknowledge stop\n");
			return -ETIMEDOUT;
		}
		udelay(5);
	}
	return 0;
}

/* ------------------------------------------------------------ FPGA */

struct motu424_fpga_ops {
	void (*start)(struct motu424 *m);
	void (*put_byte)(struct motu424 *m, u8 b);
	void (*end)(struct motu424 *m);
	bool (*check)(struct motu424 *m);
};

/*
 * C6205: the FPGA configuration pins hang off the McBSP pins used as GPIO.
 * McBSP0 PCR: DX_STAT(bit5)=DATA, bit3=nCONFIG, CLKS_STAT(bit6)=CONF_DONE.
 * McBSP1 PCR bit0 = DCLK.
 */
#define MCBSP0_PCR_BASE	0x3800
#define MCBSP1_PCR_BASE	0x3100

static void fpga6205_pcr0(struct motu424 *m)
{
	motu424_write(m, C6X_MCBSP0_BASE + C6X_MCBSP_PCR,
		      m->fpga6205_nconfig | MCBSP0_PCR_BASE | 0x40 |
		      m->fpga6205_data);
}

static void fpga6205_strobe(struct motu424 *m)
{
	motu424_write(m, C6X_MCBSP1_BASE + C6X_MCBSP_PCR, MCBSP1_PCR_BASE | 1);
	motu424_write(m, C6X_MCBSP1_BASE + C6X_MCBSP_PCR, MCBSP1_PCR_BASE);
}

static void fpga6205_nconfig(struct motu424 *m, bool v)
{
	m->fpga6205_nconfig = v ? 0x08 : 0;
	fpga6205_pcr0(m);
	if (fpga_delays) {
		ioread32(m->hctl + MOTU424_HSR);	/* flush posted writes */
		udelay(v ? 50 : 10);
	}
}

/* GateArrayProgrammer6205::Config_McBSP (0x2a050) */
static void fpga6205_config_mcbsp(struct motu424 *m, u32 base, u32 pcr)
{
	static const u32 regs[8] = { 0, 0, 0, 0x20000001, 0, 0, 0, 0 };
	int i;

	for (i = 0; i < 7; i++)
		motu424_write(m, base + 0x08 + 4 * i, regs[i]);
	motu424_write(m, base + 0x24, pcr);
}

static void fpga6205_start(struct motu424 *m)
{
	int i;

	m->fpga6205_data = 0;
	m->fpga6205_nconfig = 0;
	fpga6205_config_mcbsp(m, C6X_MCBSP0_BASE, MCBSP0_PCR_BASE);
	fpga6205_config_mcbsp(m, C6X_MCBSP1_BASE, MCBSP1_PCR_BASE);
	fpga6205_strobe(m);
	for (i = 0; i < 3; i++) {
		fpga6205_nconfig(m, false);
		fpga6205_nconfig(m, true);
	}
}

static void fpga6205_put_byte(struct motu424 *m, u8 b)
{
	int i;

	for (i = 0; i < 8; i++) {
		m->fpga6205_data = (b >> i) & 1 ? 0x20 : 0;
		fpga6205_pcr0(m);
		fpga6205_strobe(m);
	}
}

static bool fpga6205_check(struct motu424 *m)
{
	u32 raw = motu424_read(m, C6X_MCBSP0_BASE + C6X_MCBSP_PCR);

	tdbg(m, "6205 McBSP0 PCR = %08x (swab %08x)\n", raw, swab32(raw));
	/* Mac driver tests bit 6 of the byte-swapped value; accept either. */
	return (raw & 0x40) || (swab32(raw) & 0x40);
}

/* GateArrayProgrammer6205::EndProgram (0x2a480) */
static void fpga6205_end(struct motu424 *m)
{
	int tries, i;

	for (tries = 0; tries < 100; tries++) {
		for (i = 0; i < 100; i++)
			fpga6205_strobe(m);
		if (fpga6205_check(m)) {
			for (i = 0; i < 100; i++)
				fpga6205_strobe(m);
			return;
		}
	}
}

static const struct motu424_fpga_ops fpga6205_ops = {
	.start = fpga6205_start,
	.put_byte = fpga6205_put_byte,
	.end = fpga6205_end,
	.check = fpga6205_check,
};

/* C6412: GPIO bit5 = DATA, bit6 = nCONFIG, bit7 = DCLK */
static void fpga6412_start(struct motu424 *m)
{
	motu424_write(m, C6X_GPIO_GPEN, 0xe0);
	motu424_write(m, C6X_GPIO_GPDIR, 0xe0);
	motu424_write(m, C6X_GPIO_GPVAL, 0);
	ioread32(m->hctl + MOTU424_HSR);
	udelay(fpga_delays ? 10 : 1);
	motu424_write(m, C6X_GPIO_GPVAL, 0x40);
	if (fpga_delays) {
		ioread32(m->hctl + MOTU424_HSR);
		udelay(50);
	}
}

static void fpga6412_put_byte(struct motu424 *m, u8 b)
{
	int i;

	for (i = 0; i < 8; i++) {
		u32 v = (b >> i) & 1 ? 0x60 : 0x40;

		motu424_write(m, C6X_GPIO_GPVAL, v);
		motu424_write(m, C6X_GPIO_GPVAL, v | 0x80);
		motu424_write(m, C6X_GPIO_GPVAL, v);
	}
}

static void fpga6412_end(struct motu424 *m)
{
	int i;

	for (i = 0; i < 1000; i++) {
		motu424_write(m, C6X_GPIO_GPVAL, 0xc0);
		motu424_write(m, C6X_GPIO_GPVAL, 0x40);
	}
	motu424_write(m, C6X_GPIO_GPDIR, 0xc0);
}

static bool fpga6412_check(struct motu424 *m)
{
	return true;	/* the Mac driver has no CONF_DONE readback here */
}

static const struct motu424_fpga_ops fpga6412_ops = {
	.start = fpga6412_start,
	.put_byte = fpga6412_put_byte,
	.end = fpga6412_end,
	.check = fpga6412_check,
};

/* ND424::ConfigureAltera (0x2d0a0) */
static int motu424_configure_fpga(struct motu424 *m, const struct firmware *fw)
{
	const struct motu424_fpga_ops *ops =
		m->params->dsp == MOTU424_DSP_C6205 ? &fpga6205_ops : &fpga6412_ops;
	size_t i;

	ops->start(m);
	for (i = 0; i < fw->size; i++) {
		ops->put_byte(m, fw->data[i]);
		if ((i & 0xfff) == 0xfff)
			cond_resched();
	}
	ops->end(m);
	if (!ops->check(m)) {
		dev_err(&m->pci->dev, "FPGA configuration failed (no CONF_DONE)\n");
		return -EIO;
	}
	tdbg(m, "FPGA configured (%zu bytes)\n", fw->size);
	return 0;
}

/*
 * ND424::ConfigureDSP (0x2cf30): image is a stream of big-endian 32-bit
 * words; clear the region, select its page and copy it in.
 */
static void motu424_load_image(struct motu424 *m, const struct firmware *fw,
			       u32 dest, u32 clear)
{
	size_t words = DIV_ROUND_UP(fw->size, 4);
	size_t i;

	motu424_set_page(m, dest >> 22);
	for (i = 0; i < clear; i += 4)
		motu424_write(m, dest + i, 0);
	for (i = 0; i < words; i++) {
		u8 b[4] = { 0 };

		memcpy(b, fw->data + 4 * i, min_t(size_t, 4, fw->size - 4 * i));
		motu424_write(m, dest + 4 * i,
			      (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
	}
	tdbg(m, "loaded %zu bytes at %08x (page %x)\n", fw->size, dest,
	     dest >> 22);
}

static int motu424_request_fw(struct motu424 *m, const char *name,
			      const struct firmware **fw)
{
	int err = request_firmware(fw, name, &m->pci->dev);

	if (err)
		dev_err(&m->pci->dev,
			"cannot load %s (%d); extract it with tools/extract_firmware.py\n",
			name, err);
	return err;
}

/* ND424::MakeAlteraConfiguration | kModes[ws] (ND424::SendAlteraConfiguration) */
static u32 motu424_altera_word(struct motu424 *m, unsigned int source,
			       bool settled)
{
	return MOTU424_ALTERA_CONFIG(m->mult, m->wire_mask) |
	       motu424_ws_modes[source] |
	       (settled ? MOTU424_ALTERA_SETTLED : 0);
}

/* ND424::SendOneXClockBits */
static void motu424_onex_write(struct motu424 *m, unsigned int source)
{
	u32 onex0 = MOTU424_ONEX_DEFAULT_0;
	u32 onex1 = MOTU424_ONEX_DEFAULT_1;
	u32 onex2 = 0;

	if (source == MOTU424_WS_ADAT) {
		onex1 = MOTU424_ONEX_ADAT_1;
	} else if (source >= MOTU424_WS_WIRE0) {
		unsigned int wire = source - MOTU424_WS_WIRE0;

		if (m->wire_mask & BIT(wire)) {
			onex1 = MOTU424_ONEX_WIRE_DBL_1(wire);
		} else {
			onex0 = MOTU424_ONEX_WIRE_NAT_0(wire);
			onex2 = MOTU424_ONEX_WIRE_NAT_2;
		}
	}
	cb_write(m, MOTU424_CB_ONEX_CLK0, onex0);
	cb_write(m, MOTU424_CB_ONEX_CLK1, onex1);
	cb_write(m, MOTU424_CB_ONEX_CLK2, onex2);
}

/*
 * Internal clock at the current rate (docs/INTERFACES.md section 5):
 * SampleRateChanged writes the rate-family constant; SetSampleRateMultiplier
 * writes the Altera word (without the settled bits) and the one-x clock bits;
 * WordSourceChanged(internal) writes the address source and commits.
 */
void motu424_clock_init(struct motu424 *m)
{
	cb_write(m, MOTU424_CB_RATE_CONST, m->rate % 11025 ?
		 MOTU424_RATE_CONST_48K : MOTU424_RATE_CONST_44K1);
	cb_write(m, MOTU424_CB_ALTERA_CONFIG,
		 motu424_altera_word(m, MOTU424_WS_INTERNAL, false));
	motu424_onex_write(m, MOTU424_WS_INTERNAL);
	cb_write(m, MOTU424_CB_ADDRESS_SOURCE, MOTU424_ADDR_SRC_NONE);
	cb_write(m, MOTU424_CB_ADDRESS_PARAM, 0);
	cb_write(m, MOTU424_CB_COMMIT, 1);
	motu424_smpte_hw_init(m);
	m->smux_written = false;
}

/* ND424::SendAlteraConfiguration: video sources first select the box */
static void motu424_altera_write(struct motu424 *m, unsigned int source,
				 bool settled)
{
	if (source >= MOTU424_WS_VIDEO_W0 && source <= MOTU424_WS_VIDEO_W3)
		cb_write(m, MOTU424_CB_VIDEO_SYNC,
			 MOTU424_VIDEO_SYNC_PARAM(source - MOTU424_WS_VIDEO_W0));
	cb_write(m, MOTU424_CB_ALTERA_CONFIG,
		 motu424_altera_word(m, source, settled));
}

/* after ~40000 frames the kext rewrites cb+0x80 with the settled bits */
void motu424_clock_settled(struct motu424 *m)
{
	motu424_altera_write(m, m->word_source, true);
}

/*
 * ND424::SampleRateChanged for a family change: only cb+0x98 depends on
 * the family; the Altera word and one-x clock bits stay as they are.
 */
void motu424_clock_set_rate(struct motu424 *m, unsigned int rate)
{
	cb_write(m, MOTU424_CB_RATE_CONST, rate % 11025 ?
		 MOTU424_RATE_CONST_48K : MOTU424_RATE_CONST_44K1);
	cb_write(m, MOTU424_CB_COMMIT, 1);
}

/*
 * ND424::SetSampleRateMultiplier: the new Altera word without the settled
 * bits and the one-x clock bits; the settled bits follow 40000 frames later.
 * Also used to settle again when a port comes or goes above 1x.
 * The caller holds m->mutex; m->rate is already the new rate.
 */
void motu424_clock_set_mult(struct motu424 *m, unsigned int mult,
			    unsigned int mask)
{
	unsigned long flags;

	spin_lock_irqsave(&m->lock, flags);
	if (mult != m->mult) {
		m->midi_toggle = 0;
		m->midi_half = false;
	}
	m->mult = mult;
	m->wire_mask = mult > 1 ? mask & 0xf : 0;
	spin_unlock_irqrestore(&m->lock, flags);
	motu424_altera_write(m, m->word_source, false);
	motu424_onex_write(m, m->word_source);
	msleep(DIV_ROUND_UP(MOTU424_ALTERA_SETTLE_FRAMES * 1000, m->rate));
	motu424_clock_settled(m);
}

/* ND424::WordSourceChanged: cb+0x80, one-x bits, address source, commit */
static void motu424_clock_source_write(struct motu424 *m, unsigned int source)
{
	motu424_altera_write(m, source, true);
	motu424_onex_write(m, source);
	motu424_smpte_address_write(m, source);
}

/* Drv424::IsWordSourceSupported without the video-input check */
bool motu424_clock_source_valid(unsigned int source)
{
	return source < MOTU424_WS_COUNT &&
	       ((MOTU424_WS_ALWAYS_MASK | MOTU424_WS_VIDEO_MASK) & BIT(source));
}

int motu424_clock_set_source(struct motu424 *m, unsigned int source)
{
	if (!motu424_clock_source_valid(source))
		return -EINVAL;

	if (source >= MOTU424_WS_WIRE0) {
		/* Hold the card on its internal clock for mult * 48000 frames. */
		motu424_clock_source_write(m, MOTU424_WS_INTERNAL);
		msleep(DIV_ROUND_UP(m->mult * 48000 * 1000, m->rate));
	}
	motu424_clock_source_write(m, source);
	return 0;
}

/* C6412 S/MUX option of port (ND424::DSP::WritePolitely) */
void motu424_smux_write(struct motu424 *m, unsigned int port, u32 mask)
{
	cb_write(m, MOTU424_CB_SMUX_A + 4 * port, mask);
	udelay(1);
	cb_write(m, MOTU424_CB_SMUX_B + 4 * port, mask);
	udelay(1);
}

/* ND424::StartDSP (0x2d1a0) - up to 5 attempts */
int motu424_boot(struct motu424 *m)
{
	const struct motu424_params *p = m->params;
	const struct firmware *fw_altera = NULL, *fw_prog = NULL, *fw_data = NULL;
	int attempt, err, i;
	u32 val = 0;

	err = motu424_request_fw(m, p->fw_altera, &fw_altera);
	if (!err)
		err = motu424_request_fw(m, p->fw_prog, &fw_prog);
	if (!err && p->fw_data)
		err = motu424_request_fw(m, p->fw_data, &fw_data);
	if (err)
		goto out;

	motu424_reset(m);
	m->cb = 0;
	for (attempt = 0; attempt < 5; attempt++) {
		err = motu424_configure_fpga(m, fw_altera);
		if (err)
			continue;
		motu424_load_image(m, fw_prog, p->prog_dest, p->prog_clear);
		if (fw_data)
			motu424_load_image(m, fw_data, p->data_dest,
					   p->data_clear);

		/* the mailbox and control block live in the last loaded page */
		motu424_irq_mask(m, false);
		motu424_write(m, p->mailbox, 0);
		iowrite32(MOTU424_HDCR_DSPINT, m->hctl + MOTU424_HDCR);

		err = read_poll_timeout(motu424_read, val, val != 0, 5,
					250000, false, m, p->mailbox);
		if (!err)
			break;
		dev_warn(&m->pci->dev, "DSP boot attempt %d timed out\n",
			 attempt + 1);
		motu424_reset(m);
	}
	if (err) {
		motu424_reset(m);
		dev_err(&m->pci->dev, "DSP did not start\n");
		goto out;
	}

	m->cb = val;
	m->cb_words[0] = cb_read(m, 0x00);
	m->cb_words[1] = cb_read(m, 0x04);
	m->cb_words[2] = cb_read(m, 0x08);
	m->cb_words[3] = cb_read(m, CB_ROUTING_TABLE_PTR);
	m->cb_words[4] = cb_read(m, 0x18);
	dev_info(&m->pci->dev,
		 "DSP running, control block at %08x [%08x %08x %08x %08x %08x]\n",
		 m->cb, m->cb_words[0], m->cb_words[1], m->cb_words[2],
		 m->cb_words[3], m->cb_words[4]);
	for (i = 0; i < 0x140; i += 16)
		tdbg(m, "cb+%03x: %08x %08x %08x %08x\n", i, cb_read(m, i),
		     cb_read(m, i + 4), cb_read(m, i + 8), cb_read(m, i + 12));

	motu424_clock_init(m);
	cb_write(m, CB_HOST_READY, 1);
	motu424_write(m, C6X_EMIF_CECTL0, p->emif_cectl0);
	m->booted = true;
out:
	release_firmware(fw_data);
	release_firmware(fw_prog);
	release_firmware(fw_altera);
	return err;
}
