// SPDX-License-Identifier: GPL-2.0-only
/*
 * MOTU PCI-424 / PCI-424X / PCIe-424 ALSA driver - PCI glue.
 *
 * Linux implementation informed by reverse-engineering MOTU's macOS driver;
 * firmware must be extracted from MOTU's installer by the user
 * (tools/extract_firmware.py).
 */
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <sound/initval.h>
#include "motu424.h"
#include "motu424_mixer.h"
#include "motu424_tables.h"

MODULE_AUTHOR("MoTu_424 project");
MODULE_DESCRIPTION("MOTU PCI-424 family (AudioWire) driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("motu/pci424-altera.rbf");
MODULE_FIRMWARE("motu/pci424-dsp-prog.bin");
MODULE_FIRMWARE("motu/pci424-dsp-data.bin");
MODULE_FIRMWARE("motu/pci424b-altera.rbf");
MODULE_FIRMWARE("motu/pci424b-dsp.bin");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;
static unsigned int rate = 48000;
static bool boot_only;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for MOTU 424 soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for MOTU 424 soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable MOTU 424 soundcard.");
module_param(rate, uint, 0444);
MODULE_PARM_DESC(rate, "Initial sample rate, 44100 or 48000; PCM streams can change it");
module_param(boot_only, bool, 0444);
MODULE_PARM_DESC(boot_only,
		 "Only boot the DSP and dump state; do not register PCM");

/* kParameters @0x4d4a0 (PCI-424) and @0x58820 (PCI-424X/PCIe-424) */
static const struct motu424_params motu424_params_a = {
	.name = "PCI-424",
	.dsp = MOTU424_DSP_C6205,
	.hsr_offset = 0x27fff0,
	.rstsrc = 0x01a40000,
	.fw_altera = "motu/pci424-altera.rbf",
	.fw_prog = "motu/pci424-dsp-prog.bin",
	.prog_dest = 0x00000000,
	.prog_clear = 0x10000,
	.fw_data = "motu/pci424-dsp-data.bin",
	.data_dest = 0x80000000,
	.data_clear = 0x10000,
	.mailbox = 0x8000fffc,
	.emif_cectl0 = 0x10440120,
};

static const struct motu424_params motu424_params_b = {
	.name = "PCI-424X/PCIe-424",
	.dsp = MOTU424_DSP_C6412,
	.hsr_offset = 0x41fff0,
	.rstsrc = 0x01c00000,
	.fw_altera = "motu/pci424b-altera.rbf",
	.fw_prog = "motu/pci424b-dsp.bin",
	.prog_dest = 0x00000000,
	.prog_clear = 0x40000,
	.mailbox = 0x0003fffc,
	.emif_cectl0 = 0x10914221,
};

static const struct pci_device_id motu424_ids[] = {
	{ PCI_VDEVICE(MOTU, PCI_DEVICE_ID_MOTU_PCI424),
	  .driver_data = (kernel_ulong_t)&motu424_params_a },
	{ PCI_VDEVICE(MOTU, PCI_DEVICE_ID_MOTU_PCI424X),
	  .driver_data = (kernel_ulong_t)&motu424_params_b },
	{ PCI_VDEVICE(MOTU, PCI_DEVICE_ID_MOTU_PCIE424),
	  .driver_data = (kernel_ulong_t)&motu424_params_b },
	{ }
};
MODULE_DEVICE_TABLE(pci, motu424_ids);

static irqreturn_t motu424_interrupt(int irq, void *dev_id)
{
	struct motu424 *m = dev_id;

	if (!motu424_irq_pending_ack(m))
		return IRQ_NONE;
	motu424_pcm_interrupt(m);
	return IRQ_HANDLED;
}

/* ND424b::InitHook (0x2e210): tune the TI XIO2000A PCIe bridge */
static void motu424_bridge_quirk(struct motu424 *m)
{
	struct pci_dev *bridge = m->pci->bus->self;

	if (!bridge || bridge->vendor != PCI_VENDOR_ID_TI ||
	    bridge->device != PCI_DEVICE_ID_TI_XIO2000A)
		return;
	pci_write_config_byte(m->pci, PCI_LATENCY_TIMER, 0xff);
	pci_write_config_byte(bridge, PCI_CACHE_LINE_SIZE, 0x20);
	dev_info(&m->pci->dev, "XIO2000A bridge: latency 0xff, cache line 0x20\n");
}

/*
 * ND::SetupBuffer: 32-bit physical buffer, bus address = 4K aligned + 0x10.
 * Size it for the largest layout: 96 + 96 + 2 slots, 3 x 1024 frames.
 */
static int motu424_alloc_dma(struct motu424 *m)
{
	size_t bytes = MOTU424_PERIODS * MOTU424_BLOCKS_PER_PERIOD_MAX *
		(2 * MOTU424_MAX_CHANNELS + 2) * MOTU424_SLOT_DWORDS * 4;

	m->dma_raw_bytes = PAGE_ALIGN(bytes + MOTU424_DMA_SKEW);
	m->dma_raw = dmam_alloc_coherent(&m->pci->dev, m->dma_raw_bytes,
					 &m->dma_raw_addr, GFP_KERNEL);
	if (!m->dma_raw)
		return -ENOMEM;
	m->dma_area = m->dma_raw + MOTU424_DMA_SKEW;
	m->dma_addr = m->dma_raw_addr + MOTU424_DMA_SKEW;
	m->dma_bytes = bytes;
	return 0;
}

static void motu424_hw_stop(struct motu424 *m)
{
	unsigned long flags;

	if (m->booted) {
		motu424_set_interrupts(m, false);
		motu424_irq_mask(m, true);
		spin_lock_irqsave(&m->lock, flags);
		m->running = false;
		m->playback = NULL;
		m->capture = NULL;
		spin_unlock_irqrestore(&m->lock, flags);
		motu424_reset(m);
		m->booted = false;
	}
}

static u32 mixer_read(void *ctx, u32 addr)
{
	return motu424_read(ctx, addr);
}

static void mixer_write(void *ctx, u32 addr, u32 val)
{
	struct motu424 *m = ctx;

	if (addr == m->cb + MOTU424_CB_DMA_GO && val)
		dma_wmb();
	motu424_write(m, addr, val);
}

static void mixer_delay(void *ctx, unsigned int us)
{
	udelay(us);
}

static void mixer_warn(void *ctx, const char *message)
{
	struct motu424 *m = ctx;

	dev_warn(&m->pci->dev, "%s\n", message);
}

static const struct motu424_dsp_ops mixer_ops = {
	.read = mixer_read,
	.write = mixer_write,
	.delay_us = mixer_delay,
	.warn = mixer_warn,
};

static int motu424_mixer_setup(struct motu424 *m)
{
	struct motu424_dsp *dsp;
	dma_addr_t bus;

	m->mix_state = devm_kzalloc(&m->pci->dev, sizeof(*m->mix_state),
				    GFP_KERNEL);
	dsp = devm_kzalloc(&m->pci->dev, sizeof(*dsp), GFP_KERNEL);
	if (!m->mix_state || !dsp)
		return -ENOMEM;
	dsp->ops = &mixer_ops;
	dsp->ctx = m;
	dsp->cb = m->cb;
	dsp->rate_mult = 1;
	dsp->is_424b = m->params->dsp == MOTU424_DSP_C6412;
	if (!dsp->is_424b) {
		dsp->dma_virt = dmam_alloc_coherent(&m->pci->dev,
			MOTU424_MIX_DMA_CHUNK_WORDS * sizeof(u32), &bus,
			GFP_KERNEL);
		if (!dsp->dma_virt)
			return -ENOMEM;
		dsp->dma_bus = (u32)bus;
	}
	motu424_dsp_read_tables(dsp);
	motu424_fader_input_init(dsp);
	/* the DSP starts without playback pairs, like a new MixerState */
	motu424_mixer_init(m->mix_state);
	m->mix_dsp = dsp;
	return 0;
}

static int motu424_probe(struct pci_dev *pci, const struct pci_device_id *pid)
{
	static int dev;
	struct snd_card *card;
	struct motu424 *m;
	bool irq_requested = false;
	int err;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}
	if (rate != 44100 && rate != 48000) {
		dev_err(&pci->dev, "rate must be 44100 or 48000\n");
		return -EINVAL;
	}

	err = snd_card_new(&pci->dev, index[dev], id[dev], THIS_MODULE,
				sizeof(*m), &card);
	if (err < 0)
		return err;
	m = card->private_data;
	m->card = card;
	m->pci = pci;
	m->params = (const struct motu424_params *)pid->driver_data;
	m->rate = rate;
	m->mult = 1;
	spin_lock_init(&m->lock);
	mutex_init(&m->mutex);
	motu424_smpte_defaults(m);

	err = pcim_enable_device(pci);
	if (err < 0)
		goto err_card;
	pci_set_master(pci);
	err = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
	if (err)
		goto err_card;
	err = pcim_iomap_regions(pci, BIT(0) | BIT(1), MOTU424_DRV_NAME);
	if (err < 0)
		goto err_card;
	m->mem = pcim_iomap_table(pci)[0];
	m->periph = pcim_iomap_table(pci)[1];
	if (pci_resource_len(pci, 0) < MOTU424_MEM_WINDOW ||
	    pci_resource_len(pci, 1) < m->params->hsr_offset + MOTU424_DSPP + 4) {
		dev_err(&pci->dev, "unexpected BAR sizes %pa/%pa\n",
			&pci->resource[0].end, &pci->resource[1].end);
		err = -ENODEV;
		goto err_card;
	}
	m->hctl = m->periph + m->params->hsr_offset;

	if (m->params->dsp == MOTU424_DSP_C6412)
		motu424_bridge_quirk(m);

	err = motu424_alloc_dma(m);
	if (err)
		goto err_card;

	/* keep the DSP quiet until it has been booted */
	motu424_irq_mask(m, true);
	err = devm_request_irq(&pci->dev, pci->irq, motu424_interrupt,
			       IRQF_SHARED, MOTU424_DRV_NAME, m);
	if (err) {
		dev_err(&pci->dev, "cannot get IRQ %d\n", pci->irq);
		goto err_card;
	}
	irq_requested = true;
	card->sync_irq = pci->irq;

	err = motu424_boot(m);
	if (err)
		goto err_card;
	motu424_write(m, m->cb + CB_DMA_BUFFER, (u32)m->dma_addr);
	/* the kext waits 40000 frames before setting the settled bits */
	msleep(DIV_ROUND_UP(MOTU424_ALTERA_SETTLE_FRAMES * 1000, m->rate));
	motu424_clock_settled(m);

	strscpy(card->driver, "MOTU424", sizeof(card->driver));
	strscpy(card->shortname, "MOTU 424", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "MOTU %s at 0x%llx, irq %d", m->params->name,
		 (unsigned long long)pci_resource_start(pci, 0), pci->irq);

	if (!boot_only) {
		err = motu424_mixer_setup(m);
		if (err)
			goto err_card;
		err = motu424_qmix_create(m);
		if (err)
			goto err_card;
		err = motu424_smpte_create(m);
		if (err)
			goto err_card;
		err = motu424_pcm_create(m);
		if (err < 0)
			goto err_card;
		err = motu424_rawmidi_create(m);
		if (err < 0)
			goto err_card;
		/* idle engine: lets the interfaces be identified and set up */
		err = motu424_engine_idle(m);
		if (err < 0)
			goto err_card;
		motu424_iface_wait_settled(m);
	}
	motu424_iface_proc_init(m);
	err = snd_card_register(card);
	if (err < 0)
		goto err_card;
	pci_set_drvdata(pci, card);
	dev++;
	return 0;

err_card:
	mutex_lock(&m->mutex);
	motu424_hw_stop(m);
	mutex_unlock(&m->mutex);
	if (irq_requested)
		devm_free_irq(&pci->dev, pci->irq, m);
	motu424_smpte_cancel(m);
	motu424_qmix_cancel(m);
	snd_card_free(card);
	return err;
}

static void motu424_remove(struct pci_dev *pci)
{
	struct snd_card *card = pci_get_drvdata(pci);
	struct motu424 *m = card->private_data;

	snd_card_disconnect(card);
	motu424_smpte_cancel(m);
	motu424_iface_shutdown(m);
	mutex_lock(&m->mutex);
	motu424_hw_stop(m);
	mutex_unlock(&m->mutex);
	devm_free_irq(&pci->dev, pci->irq, m);
	motu424_qmix_cancel(m);
	snd_card_free(card);
}

static struct pci_driver motu424_driver = {
	.name = MOTU424_DRV_NAME,
	.id_table = motu424_ids,
	.probe = motu424_probe,
	.remove = motu424_remove,
};

module_pci_driver(motu424_driver);
