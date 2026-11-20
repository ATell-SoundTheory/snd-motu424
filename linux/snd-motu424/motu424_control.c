// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALSA Q-Mix controls and deferred interface-layout reconciliation.
 */
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <sound/control.h>

#include "motu424.h"
#include "motu424_qmix.h"
#include "motu424_tables.h"

struct motu424_controls {
	struct motu424 *m;
	struct motu424_qmix cue;
	struct work_struct work;
	struct snd_kcontrol *route_volume[MOTU424_MIX_PAIRS];
	struct snd_kcontrol *route_pan[MOTU424_MIX_PAIRS];
	struct snd_kcontrol *iface_kctl[MOTU424_IFACE_COUNT];
};

/*
 * The routes Q-Mix may program: what the streams were opened with,
 * restricted to the boxes that are still there. The DMA pairs and pair
 * types follow the open streams. At 4x each pair p of a port also feeds its
 * partner pair p + 6 (Drv424::SetupRouting).
 */
static void cue_layout_snapshot(struct motu424 *m,
				struct motu424_cue_layout *layout, bool audio)
{
	struct motu424_layout now;
	unsigned long flags;
	unsigned int i;

	memset(layout, 0, sizeof(*layout));
	spin_lock_irqsave(&m->lock, flags);
	motu424_layout_compute(m, m->mult, &now);
	layout->outputs = m->out_pairs & now.out_pairs &
			  ~(m->split_pairs ^ now.split_pairs);
	layout->split_pairs = m->split_pairs;
	layout->skip_pairs = m->skip_pairs;
	for (i = 0; i < m->n_in; i++)
		layout->inputs[m->in_hw[i]] = 1;
	for (i = 0; i < now.n_in; i++)
		layout->inputs[now.in_hw[i]] |= 2;
	for (i = 0; i < MOTU424_CUE_INPUTS; i++)
		layout->inputs[i] = layout->inputs[i] == 3;
	if (audio) {
		layout->dma_pairs = m->out_pairs;
		if (m->mult == 4)
			for (i = 0; i < MOTU424_MIX_PAIRS; i++)
				if ((m->out_pairs & BIT_ULL(i)) &&
				    i % 12 < MOTU424_CUE_QUAD_PAIR_OFFSET)
					layout->dma_pairs |= BIT_ULL(i +
						MOTU424_CUE_QUAD_PAIR_OFFSET);
	}
	spin_unlock_irqrestore(&m->lock, flags);
}

int motu424_mixer_sync(struct motu424 *m, bool audio)
{
	struct motu424_cue_layout layout;
	u64 previous = m->controls->cue.dma_pairs;
	int err;

	cue_layout_snapshot(m, &layout, audio);
	err = motu424_qmix_update_trim(&m->controls->cue, &layout);
	if (err)
		return err;
	err = motu424_qmix_sync(&m->controls->cue, &layout);
	if (!err && previous != layout.dma_pairs)
		dev_info(&m->pci->dev, "mixer switch requested: %u stereo DMA pairs\n",
			 (unsigned int)hweight64(layout.dma_pairs));
	return err;
}

static void iface_controls_activate(struct motu424_controls *controls);

static void cue_layout_work(struct work_struct *work)
{
	struct motu424_controls *controls =
		container_of(work, struct motu424_controls, work);
	struct motu424 *m = controls->m;
	int err;

	mutex_lock(&m->mutex);
	if (m->booted) {
		unsigned int mask = motu424_iface_wire_mask(m, m->mult);
		unsigned long flags;
		bool resettle;

		spin_lock_irqsave(&m->lock, flags);
		resettle = m->resettle;
		m->resettle = false;
		spin_unlock_irqrestore(&m->lock, flags);
		/*
		 * Above 1x: a port came or went (the Altera settle timer starts
		 * again), or a box that cannot run at 2x itself was identified.
		 */
		if (m->mult > 1 && (resettle || mask != m->wire_mask))
			motu424_clock_set_mult(m, m->mult, mask);
		err = motu424_mixer_sync(m, m->running && m->audio);
		if (err)
			dev_err_ratelimited(&m->pci->dev,
					    "Q-Mix layout update failed: %d\n", err);
		motu424_smpte_refresh(m);
	}
	mutex_unlock(&m->mutex);
	/* takes controls_rwsem, which put callbacks hold while taking m->mutex */
	iface_controls_activate(controls);
}

/* every fader freed before the multiplier changes; m->mutex */
int motu424_qmix_release(struct motu424 *m)
{
	struct motu424_cue_layout empty;

	if (!m->controls)
		return 0;
	memset(&empty, 0, sizeof(empty));
	return motu424_qmix_sync(&m->controls->cue, &empty);
}

/* whether the unmuted Q-Mix routes fit the fader budget at mult; m->mutex */
bool motu424_qmix_mult_fits(struct motu424 *m, unsigned int mult)
{
	if (!m->controls || !m->mix_dsp)
		return true;
	return motu424_qmix_fits(&m->controls->cue,
			motu424_qmix_budget(m->mix_dsp->is_424b, mult));
}

/* Drv424::SetSampleRateMultiplier, mixer part; after qmix_release */
void motu424_qmix_set_mult(struct motu424 *m, unsigned int mult)
{
	int err;

	if (m->mix_dsp)
		m->mix_dsp->rate_mult = mult;
	if (!m->controls)
		return;
	err = motu424_qmix_set_faders(&m->controls->cue,
			motu424_qmix_budget(m->mix_dsp->is_424b, mult),
			mult == 4);
	if (err)
		dev_err(&m->pci->dev, "Q-Mix fader budget: %d\n", err);
}

void motu424_qmix_schedule(struct motu424 *m)
{
	if (m->controls)
		schedule_work(&m->controls->work);
}

void motu424_qmix_cancel(struct motu424 *m)
{
	if (m->controls)
		cancel_work_sync(&m->controls->work);
}

static enum motu424_cue_control cue_control(struct snd_kcontrol *kcontrol)
{
	return kcontrol->private_value & 0xff;
}

static void cue_control_index(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_id *id,
			      unsigned int *bus, unsigned int *input)
{
	enum motu424_cue_control control = cue_control(kcontrol);
	unsigned int index = snd_ctl_get_ioff(kcontrol, id);

	if (control == MOTU424_CUE_BUS_VOLUME || control == MOTU424_CUE_BUS_SWITCH ||
	    control == MOTU424_CUE_TALKBACK_BUS || control == MOTU424_CUE_LISTENBACK_BUS) {
		*bus = index;
		*input = 0;
	} else if (control == MOTU424_CUE_INPUT_TRIM ||
		   control == MOTU424_CUE_INPUT_SWITCH) {
		*bus = 0;
		*input = index;
	} else if (control >= MOTU424_CUE_TALKBACK_SWITCH) {
		*bus = *input = 0;
	} else {
		*bus = kcontrol->private_value >> 8;
		*input = control == MOTU424_CUE_BALANCE || control == MOTU424_CUE_WIDTH ?
			 2 * index : index;
	}
}

static int cue_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *info)
{
	enum motu424_cue_control control = cue_control(kcontrol);

	if (control == MOTU424_CUE_SWITCH || control == MOTU424_CUE_SOLO_SWITCH ||
	    control == MOTU424_CUE_BUS_SWITCH || control == MOTU424_CUE_INPUT_SWITCH ||
	    control == MOTU424_CUE_TALKBACK_SWITCH || control == MOTU424_CUE_LISTENBACK_SWITCH ||
	    control == MOTU424_CUE_TALKBACK_BUS || control == MOTU424_CUE_LISTENBACK_BUS ||
	    control == MOTU424_CUE_METER_INPUT_BANK)
		return snd_ctl_boolean_mono_info(kcontrol, info);
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = control == MOTU424_CUE_INPUT_TRIM ?
				  MOTU424_TRIM_UNITY : 0;
	switch (control) {
	case MOTU424_CUE_INPUT_TRIM:
		info->value.integer.max = 0xff;
		break;
	case MOTU424_CUE_PAN:
	case MOTU424_CUE_BALANCE:
	case MOTU424_CUE_WIDTH:
	case MOTU424_CUE_TALKBACK_DIM:
	case MOTU424_CUE_LISTENBACK_DIM:
		info->value.integer.max = 128;
		break;
	case MOTU424_CUE_TALKBACK_INPUT:
	case MOTU424_CUE_LISTENBACK_INPUT:
		info->value.integer.max = MOTU424_CUE_INPUTS;
		break;
	case MOTU424_CUE_METER_BUS:
		info->value.integer.max = MOTU424_MIX_PAIRS;
		break;
	default:
		info->value.integer.max = 0x8000;
		break;
	}
	info->value.integer.step = 1;
	return 0;
}

static int cue_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);
	unsigned int bus, input;
	long v;
	int err;

	cue_control_index(kcontrol, &value->id, &bus, &input);
	mutex_lock(&m->mutex);
	err = motu424_qmix_get(&m->controls->cue, cue_control(kcontrol),
				 bus, input, &v);
	if (!err)
		value->value.integer.value[0] = v;
	mutex_unlock(&m->mutex);
	return err;
}

static int cue_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);
	struct motu424_cue_layout layout;
	unsigned int bus, input;
	int err;

	cue_control_index(kcontrol, &value->id, &bus, &input);
	mutex_lock(&m->mutex);
	if (!m->booted) {
		err = -ENODEV;
		goto out;
	}
	cue_layout_snapshot(m, &layout, m->running && m->audio);
	err = motu424_qmix_put(&m->controls->cue, cue_control(kcontrol),
				 bus, input, value->value.integer.value[0], &layout);
	if (err > 0 && (cue_control(kcontrol) == MOTU424_CUE_BALANCE ||
			cue_control(kcontrol) == MOTU424_CUE_WIDTH)) {
		unsigned int i, route;

		for (route = 0; route < 2; route++) {
			struct snd_kcontrol *mono = route ?
				m->controls->route_pan[bus] :
				m->controls->route_volume[bus];
			struct snd_ctl_elem_id id;

			if (WARN_ON(!mono))
				continue;
			for (i = 0; i < 2; i++)
				snd_ctl_notify(m->card, SNDRV_CTL_EVENT_MASK_VALUE,
					       snd_ctl_build_ioff(&id, mono, input + i));
		}
	}
	if (err < 0 && err != -EINVAL && err != -ENOSPC)
		dev_err_ratelimited(&m->pci->dev, "Q-Mix update failed: %d\n", err);
out:
	mutex_unlock(&m->mutex);
	return err;
}

static int cue_peak_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *info)
{
	(void)kcontrol;
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = MOTU424_METER_VALUES;
	info->value.integer.min = 0;
	info->value.integer.max = 0xffff;
	info->value.integer.step = 1;
	return 0;
}

static int cue_peak_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);
	struct motu424_cue_layout layout;
	u16 peaks[MOTU424_METER_VALUES];
	unsigned int i;
	int err;

	mutex_lock(&m->mutex);
	if (!m->booted) {
		err = -ENODEV;
		goto out;
	}
	cue_layout_snapshot(m, &layout, m->running && m->audio);
	err = motu424_qmix_read_meters(&m->controls->cue, &layout, peaks);
	if (err == -EAGAIN) {
		msleep(20);
		err = motu424_qmix_read_meters(&m->controls->cue, &layout, peaks);
	}
	if (!err)
		for (i = 0; i < MOTU424_METER_VALUES; i++)
			value->value.integer.value[i] = peaks[i];
	else
		dev_err_ratelimited(&m->pci->dev, "Q-Mix meter read failed: %d\n", err);
out:
	mutex_unlock(&m->mutex);
	return err;
}

static int cue_add_peak_control(struct motu424 *m)
{
	struct snd_kcontrol_new new = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Q-Mix DSP Peak",
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.count = 1,
		.info = cue_peak_info,
		.get = cue_peak_get,
	};
	struct snd_kcontrol *control = snd_ctl_new1(&new, m);

	if (!control)
		return -ENOMEM;
	return snd_ctl_add(m->card, control);
}

static int cue_add_control(struct motu424 *m, const char *name,
			   enum motu424_cue_control control, unsigned int bus,
			   unsigned int count, struct snd_kcontrol **added)
{
	struct snd_kcontrol_new new = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = name,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.count = count,
		.info = cue_info,
		.get = cue_get,
		.put = cue_put,
		.private_value = control | (bus << 8),
	};
	struct snd_kcontrol *kcontrol = snd_ctl_new1(&new, m);
	int err;

	if (!kcontrol)
		return -ENOMEM;
	err = snd_ctl_add(m->card, kcontrol);
	if (!err && added)
		*added = kcontrol;
	return err;
}

static int iface_control_info(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_info *info)
{
	enum motu424_iface_control control = kcontrol->private_value & 0xff;

	switch (control) {
	case MOTU424_IFACE_AES_INPUT_SRC:
	case MOTU424_IFACE_24IO_LEVEL_1_8 ... MOTU424_IFACE_24IO_LEVEL_17_24:
	case MOTU424_IFACE_2408MK3_LEVEL_1_2 ... MOTU424_IFACE_2408MK3_LEVEL_7_8:
		return snd_ctl_boolean_mono_info(kcontrol, info);
	default:
		break;
	}
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.step = 1;
	switch (control) {
	case MOTU424_IFACE_2408_ANALOG_BANK:
		info->value.integer.max = MOTU424_2408_ANALOG_NONE;
		break;
	case MOTU424_IFACE_2408_BANK_C_FORMAT:
	case MOTU424_IFACE_2408_ANALOG_MIRROR:
	case MOTU424_IFACE_WORD_OUT_RATE:
		info->value.integer.max = 2;
		break;
	case MOTU424_IFACE_2408_BANK_A_FORMAT:
	case MOTU424_IFACE_2408_BANK_B_FORMAT:
		info->value.integer.max = 1;
		break;
	case MOTU424_IFACE_2408_CLOCK_SOURCE:
		info->value.integer.max = MOTU424_2408_CLK_CARD;
		break;
	case MOTU424_IFACE_1296_CLOCK_SOURCE:
		info->value.integer.max = MOTU424_1296_UICLK_CARD;
		break;
	case MOTU424_IFACE_AES_INPUT_PAIR:
		info->value.integer.max = 6;
		break;
	case MOTU424_IFACE_AES_OUTPUT_SRC_MODE:
		info->value.integer.max = 13;
		break;
	case MOTU424_IFACE_AES_OUTPUT_CLOCK:
		info->value.integer.max = 10;
		break;
	case MOTU424_IFACE_PEAK_HOLD_TIME:
	case MOTU424_IFACE_CLIP_HOLD_TIME:
		info->value.integer.max = 7;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int iface_control_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);
	enum motu424_iface_control control = kcontrol->private_value & 0xff;
	unsigned int port = snd_ctl_get_ioff(kcontrol, &value->id);
	long v;
	int err;

	err = motu424_iface_control_get(m, port, control, &v);
	/* absent or different box: readable as 0, flagged inactive */
	if (err == -ENODEV) {
		v = 0;
		err = 0;
	}
	if (!err)
		value->value.integer.value[0] = v;
	return err;
}

static int iface_control_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);
	enum motu424_iface_control control = kcontrol->private_value & 0xff;
	unsigned int port = snd_ctl_get_ioff(kcontrol, &value->id);
	int err;

	mutex_lock(&m->mutex);
	if (!m->booted) {
		err = -ENODEV;
		goto out;
	}
	err = motu424_iface_control_put(m, port, control,
					value->value.integer.value[0]);
out:
	mutex_unlock(&m->mutex);
	return err;
}

static void iface_controls_activate(struct motu424_controls *controls)
{
	struct snd_ctl_elem_id id;
	unsigned int control, port;

	for (control = 0; control < ARRAY_SIZE(controls->iface_kctl); control++) {
		struct snd_kcontrol *kctl = controls->iface_kctl[control];

		if (!kctl)
			continue;
		for (port = 0; port < MOTU424_MAX_WIRES; port++)
			snd_ctl_activate_id(controls->m->card,
				snd_ctl_build_ioff(&id, kctl, port),
				motu424_iface_control_available(controls->m,
								port, control));
	}
}

static int iface_add_control(struct motu424 *m, const char *name,
			     enum motu424_iface_control control)
{
	struct snd_kcontrol_new new = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = name,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_INACTIVE,
		.count = MOTU424_MAX_WIRES,
		.info = iface_control_info,
		.get = iface_control_get,
		.put = iface_control_put,
		.private_value = control,
	};
	struct snd_kcontrol *kcontrol = snd_ctl_new1(&new, m);
	int err;

	if (!kcontrol)
		return -ENOMEM;
	err = snd_ctl_add(m->card, kcontrol);
	if (!err)
		m->controls->iface_kctl[control] = kcontrol;
	return err;
}

static int clock_source_info(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_info *info)
{
	(void)kcontrol;
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = MOTU424_WS_INTERNAL;
	info->value.integer.max = MOTU424_WS_WIRE3;
	info->value.integer.step = 1;
	return 0;
}

static int clock_source_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);

	mutex_lock(&m->mutex);
	if (!m->booted) {
		mutex_unlock(&m->mutex);
		return -ENODEV;
	}
	value->value.integer.value[0] = m->word_source;
	mutex_unlock(&m->mutex);
	return 0;
}

static int clock_source_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *value)
{
	struct motu424 *m = snd_kcontrol_chip(kcontrol);
	unsigned int source = value->value.integer.value[0];
	unsigned long flags;
	int err = 0;

	if (!motu424_clock_source_valid(source))
		return -EINVAL;
	mutex_lock(&m->mutex);
	if (!m->booted) {
		err = -ENODEV;
		goto out;
	}
	if (m->open_count) {
		err = -EBUSY;
		goto out;
	}
	if (source == m->word_source)
		goto out;
	if (source >= MOTU424_WS_WIRE0) {
		unsigned int port = source - MOTU424_WS_WIRE0;
		struct motu424_wire *w = &m->wire[port];

		spin_lock_irqsave(&m->lock, flags);
		if (!w->info || w->state != MOTU424_WIRE_READY)
			err = -ENODEV;
		else if (!w->external_clock)
			err = -EINVAL;
		spin_unlock_irqrestore(&m->lock, flags);
		if (err)
			goto out;
	} else if (source >= MOTU424_WS_VIDEO_W0 &&
		   source <= MOTU424_WS_VIDEO_W3) {
		/* Drv424::IsWordSourceSupported: HasVideoSyncInput */
		spin_lock_irqsave(&m->lock, flags);
		if (!motu424_wire_is_2408mk3(
				&m->wire[source - MOTU424_WS_VIDEO_W0]))
			err = -ENODEV;
		spin_unlock_irqrestore(&m->lock, flags);
		if (err)
			goto out;
	}
	dev_warn(&m->pci->dev,
		 "switching clock source to %s; external lock and rate are not detected (expected %u Hz)\n",
		 motu424_ws_names[source], m->rate);
	err = motu424_clock_set_source(m, source);
	if (!err) {
		m->word_source = source;
		motu424_smpte_refresh(m);
	}
out:
	mutex_unlock(&m->mutex);
	return err;
}

static int clock_source_add(struct motu424 *m)
{
	static const struct snd_kcontrol_new new = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Card Clock Source",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.count = 1,
		.info = clock_source_info,
		.get = clock_source_get,
		.put = clock_source_put,
	};
	struct snd_kcontrol *kcontrol = snd_ctl_new1(&new, m);

	if (!kcontrol)
		return -ENOMEM;
	return snd_ctl_add(m->card, kcontrol);
}

int motu424_qmix_create(struct motu424 *m)
{
	static const struct {
		const char *name;
		enum motu424_cue_control control;
	} route_controls[] = {
		{ "Input Playback Volume", MOTU424_CUE_VOLUME },
		{ "Input Playback Pan", MOTU424_CUE_PAN },
		{ "Input Playback Switch", MOTU424_CUE_SWITCH },
		{ "Input Solo Switch", MOTU424_CUE_SOLO_SWITCH },
	};
	static const struct {
		const char *name;
		enum motu424_iface_control control;
	} iface_controls[] = {
		{ "Interface 2408 Analog Bank",
		  MOTU424_IFACE_2408_ANALOG_BANK },
		{ "Interface 2408 Bank A Format",
		  MOTU424_IFACE_2408_BANK_A_FORMAT },
		{ "Interface 2408 Bank B Format",
		  MOTU424_IFACE_2408_BANK_B_FORMAT },
		{ "Interface 2408 Bank C Format",
		  MOTU424_IFACE_2408_BANK_C_FORMAT },
		{ "Interface 2408 Clock Source",
		  MOTU424_IFACE_2408_CLOCK_SOURCE },
		{ "Interface AES Input Pair", MOTU424_IFACE_AES_INPUT_PAIR },
		{ "Interface AES Output SRC Mode",
		  MOTU424_IFACE_AES_OUTPUT_SRC_MODE },
		{ "Interface AES Output Clock", MOTU424_IFACE_AES_OUTPUT_CLOCK },
		{ "Interface AES Input SRC", MOTU424_IFACE_AES_INPUT_SRC },
		{ "Interface Peak Hold Time", MOTU424_IFACE_PEAK_HOLD_TIME },
		{ "Interface Clip Hold Time", MOTU424_IFACE_CLIP_HOLD_TIME },
		{ "Interface 2408 Analog Mirror Bank",
		  MOTU424_IFACE_2408_ANALOG_MIRROR },
		{ "Interface Word Clock Out Rate", MOTU424_IFACE_WORD_OUT_RATE },
		{ "Interface 24I/O Analog 1-8 -10dBV Switch",
		  MOTU424_IFACE_24IO_LEVEL_1_8 },
		{ "Interface 24I/O Analog 9-16 -10dBV Switch",
		  MOTU424_IFACE_24IO_LEVEL_9_16 },
		{ "Interface 24I/O Analog 17-24 -10dBV Switch",
		  MOTU424_IFACE_24IO_LEVEL_17_24 },
		{ "Interface 2408mk3 Analog 1-2 -10dBV Switch",
		  MOTU424_IFACE_2408MK3_LEVEL_1_2 },
		{ "Interface 2408mk3 Analog 3-4 -10dBV Switch",
		  MOTU424_IFACE_2408MK3_LEVEL_3_4 },
		{ "Interface 2408mk3 Analog 5-6 -10dBV Switch",
		  MOTU424_IFACE_2408MK3_LEVEL_5_6 },
		{ "Interface 2408mk3 Analog 7-8 -10dBV Switch",
		  MOTU424_IFACE_2408MK3_LEVEL_7_8 },
		{ "Interface 1296 Clock Source",
		  MOTU424_IFACE_1296_CLOCK_SOURCE },
	};
	struct motu424_controls *controls;
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	unsigned int bus, i;
	int err;

	controls = devm_kzalloc(&m->pci->dev, sizeof(*controls), GFP_KERNEL);
	if (!controls)
		return -ENOMEM;
	controls->m = m;
	INIT_WORK(&controls->work, cue_layout_work);
	motu424_qmix_init(&controls->cue, m->mix_state, m->mix_dsp);
	m->controls = controls;
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++)
		for (i = 0; i < ARRAY_SIZE(route_controls); i++) {
			snprintf(name, sizeof(name), "Q-Mix Bus %u %s",
				 bus, route_controls[i].name);
			err = cue_add_control(m, name, route_controls[i].control,
					      bus, MOTU424_CUE_INPUTS,
					      route_controls[i].control ==
						      MOTU424_CUE_VOLUME ?
						      &controls->route_volume[bus] :
					      route_controls[i].control ==
						      MOTU424_CUE_PAN ?
						      &controls->route_pan[bus] :
						      NULL);
			if (err)
				return err;
		}
	err = cue_add_control(m, "Q-Mix Bus Playback Volume",
			      MOTU424_CUE_BUS_VOLUME, 0, MOTU424_MIX_PAIRS, NULL);
	if (err)
		return err;
	err = cue_add_control(m, "Q-Mix Bus Playback Switch",
			      MOTU424_CUE_BUS_SWITCH, 0, MOTU424_MIX_PAIRS, NULL);
	if (err)
		return err;
	err = cue_add_control(m, "Q-Mix Input Trim Volume",
			      MOTU424_CUE_INPUT_TRIM, 0, MOTU424_CUE_INPUTS, NULL);
	if (err)
		return err;
	err = cue_add_control(m, "Q-Mix Input Switch",
			      MOTU424_CUE_INPUT_SWITCH, 0, MOTU424_CUE_INPUTS, NULL);
	if (err)
		return err;
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++) {
		snprintf(name, sizeof(name), "Q-Mix Bus %u Stereo Balance", bus);
		err = cue_add_control(m, name, MOTU424_CUE_BALANCE, bus,
				      MOTU424_CUE_INPUTS / 2, NULL);
		if (err)
			return err;
		snprintf(name, sizeof(name), "Q-Mix Bus %u Stereo Width", bus);
		err = cue_add_control(m, name, MOTU424_CUE_WIDTH, bus,
				      MOTU424_CUE_INPUTS / 2, NULL);
		if (err)
			return err;
	}
	{
		static const struct {
			const char *name;
			enum motu424_cue_control control;
			unsigned int count;
		} extra[] = {
			{ "Q-Mix Talkback Switch", MOTU424_CUE_TALKBACK_SWITCH, 1 },
			{ "Q-Mix Talkback Input", MOTU424_CUE_TALKBACK_INPUT, 1 },
			{ "Q-Mix Talkback Dim", MOTU424_CUE_TALKBACK_DIM, 1 },
			{ "Q-Mix Talkback Bus Switch", MOTU424_CUE_TALKBACK_BUS, 48 },
			{ "Q-Mix Listenback Switch", MOTU424_CUE_LISTENBACK_SWITCH, 1 },
			{ "Q-Mix Listenback Input", MOTU424_CUE_LISTENBACK_INPUT, 1 },
			{ "Q-Mix Listenback Dim", MOTU424_CUE_LISTENBACK_DIM, 1 },
			{ "Q-Mix Listenback Bus Switch", MOTU424_CUE_LISTENBACK_BUS, 48 },
			{ "Q-Mix Meter Input Bank", MOTU424_CUE_METER_INPUT_BANK, 1 },
			{ "Q-Mix Meter Bus", MOTU424_CUE_METER_BUS, 1 },
		};

		for (i = 0; i < ARRAY_SIZE(extra); i++) {
			err = cue_add_control(m, extra[i].name, extra[i].control,
					      0, extra[i].count, NULL);
			if (err)
				return err;
		}
	}
	err = cue_add_peak_control(m);
	if (err)
		return err;
	for (i = 0; i < ARRAY_SIZE(iface_controls); i++) {
		err = iface_add_control(m, iface_controls[i].name,
					iface_controls[i].control);
		if (err)
			return err;
	}
	return clock_source_add(m);
}
