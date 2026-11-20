// SPDX-License-Identifier: GPL-2.0-only
/*
 * Q-Mix route allocation for the PCI-424 DSP mixer.
 * The caller serialises settings, topology changes, and DSP access.
 */
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "motu424_qmix.h"

struct motu424_cue_work {
	struct motu424_mixer_state mixer;
	u8 faders[MOTU424_MIX_PAIRS][MOTU424_CUE_INPUTS];
	bool used[MOTU424_MIX_ROUTINGS];
	u32 words[MOTU424_MIX_BUF_WORDS];
};

void motu424_qmix_init(struct motu424_qmix *cue,
			 struct motu424_mixer_state *mixer,
			 struct motu424_dsp *dsp)
{
	unsigned int bus, input;

	memset(cue, 0, sizeof(*cue));
	cue->mixer = mixer;
	cue->dsp = dsp;
	cue->faders = MOTU424_MIX_ROUTINGS;
	cue->talkback.input = cue->listenback.input = MOTU424_CUE_INPUTS;
	cue->talkback.dim = cue->listenback.dim = 128;
	cue->meter_bus = MOTU424_MIX_PAIRS;
	for (input = 0; input < MOTU424_CUE_INPUTS; input++)
		cue->inputs[input].trim = MOTU424_TRIM_UNITY;
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++) {
		cue->buses[bus].volume = 0x8000;
		cue->buses[bus].mute = true;
		for (input = 0; input < MOTU424_CUE_INPUTS; input++) {
			struct motu424_cue_route *route = &cue->routes[bus][input];

			route->volume = 0x8000;
			route->pan = 0x40;
			route->flags = MOTU424_CUE_MUTE;
			route->fader = MOTU424_CUE_NO_FADER;
		}
		for (input = 0; input < MOTU424_CUE_INPUTS / 2; input++) {
			cue->stereo[bus][input].balance = 64;
			cue->stereo[bus][input].width = 64;
		}
	}
}

static bool cue_control_valid(enum motu424_cue_control control, long value)
{
	switch (control) {
	case MOTU424_CUE_VOLUME:
	case MOTU424_CUE_BUS_VOLUME:
		return value >= 0 && value <= 0x8000;
	case MOTU424_CUE_PAN:
	case MOTU424_CUE_BALANCE:
	case MOTU424_CUE_WIDTH:
	case MOTU424_CUE_TALKBACK_DIM:
	case MOTU424_CUE_LISTENBACK_DIM:
		return value >= 0 && value <= 0x80;
	case MOTU424_CUE_TALKBACK_INPUT:
	case MOTU424_CUE_LISTENBACK_INPUT:
		return value >= 0 && value <= MOTU424_CUE_INPUTS;
	case MOTU424_CUE_METER_BUS:
		return value >= 0 && value <= MOTU424_MIX_PAIRS;
	case MOTU424_CUE_INPUT_TRIM:
		return value >= MOTU424_TRIM_UNITY && value <= 0xff;
	case MOTU424_CUE_SWITCH:
	case MOTU424_CUE_SOLO_SWITCH:
	case MOTU424_CUE_BUS_SWITCH:
	case MOTU424_CUE_INPUT_SWITCH:
	case MOTU424_CUE_TALKBACK_SWITCH:
	case MOTU424_CUE_LISTENBACK_SWITCH:
	case MOTU424_CUE_TALKBACK_BUS:
	case MOTU424_CUE_LISTENBACK_BUS:
	case MOTU424_CUE_METER_INPUT_BANK:
		return value == 0 || value == 1;
	}
	return false;
}

int motu424_qmix_get(const struct motu424_qmix *cue,
		       enum motu424_cue_control control, unsigned int bus,
		       unsigned int input, long *value)
{
	const struct motu424_cue_route *route;

	if (!value || bus >= MOTU424_MIX_PAIRS || input >= MOTU424_CUE_INPUTS)
		return -EINVAL;
	route = &cue->routes[bus][input];
	switch (control) {
	case MOTU424_CUE_VOLUME:
		*value = route->volume;
		break;
	case MOTU424_CUE_PAN:
		*value = route->pan;
		break;
	case MOTU424_CUE_SWITCH:
		*value = !(route->flags & MOTU424_CUE_MUTE);
		break;
	case MOTU424_CUE_SOLO_SWITCH:
		*value = !!(route->flags & MOTU424_CUE_SOLO);
		break;
	case MOTU424_CUE_BUS_VOLUME:
		*value = cue->buses[bus].volume;
		break;
	case MOTU424_CUE_BUS_SWITCH:
		*value = !cue->buses[bus].mute;
		break;
	case MOTU424_CUE_INPUT_TRIM:
		*value = cue->inputs[input].trim;
		break;
	case MOTU424_CUE_INPUT_SWITCH:
		*value = !cue->inputs[input].mute;
		break;
	case MOTU424_CUE_BALANCE:
	case MOTU424_CUE_WIDTH:
		if (input & 1)
			return -EINVAL;
		*value = control == MOTU424_CUE_BALANCE ?
			cue->stereo[bus][input / 2].balance :
			cue->stereo[bus][input / 2].width;
		break;
	case MOTU424_CUE_TALKBACK_SWITCH:
	case MOTU424_CUE_LISTENBACK_SWITCH:
		*value = control == MOTU424_CUE_TALKBACK_SWITCH ?
			cue->talkback.active : cue->listenback.active;
		break;
	case MOTU424_CUE_TALKBACK_INPUT:
	case MOTU424_CUE_LISTENBACK_INPUT:
		*value = control == MOTU424_CUE_TALKBACK_INPUT ?
			cue->talkback.input : cue->listenback.input;
		break;
	case MOTU424_CUE_TALKBACK_DIM:
	case MOTU424_CUE_LISTENBACK_DIM:
		*value = control == MOTU424_CUE_TALKBACK_DIM ?
			cue->talkback.dim : cue->listenback.dim;
		break;
	case MOTU424_CUE_TALKBACK_BUS:
	case MOTU424_CUE_LISTENBACK_BUS:
		*value = !!((control == MOTU424_CUE_TALKBACK_BUS ?
			cue->talkback.buses : cue->listenback.buses) & (1ULL << bus));
		break;
	case MOTU424_CUE_METER_INPUT_BANK:
		*value = cue->meter_input_bank;
		break;
	case MOTU424_CUE_METER_BUS:
		*value = cue->meter_bus;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void cue_set(struct motu424_qmix *cue,
		    enum motu424_cue_control control, unsigned int bus,
		    unsigned int input, long value)
{
	struct motu424_cue_route *route = &cue->routes[bus][input];

	switch (control) {
	case MOTU424_CUE_VOLUME:
		route->volume = value;
		break;
	case MOTU424_CUE_PAN:
		route->pan = value;
		break;
	case MOTU424_CUE_SWITCH:
		if (value)
			route->flags &= ~MOTU424_CUE_MUTE;
		else
			route->flags |= MOTU424_CUE_MUTE;
		break;
	case MOTU424_CUE_SOLO_SWITCH:
		if (value)
			route->flags |= MOTU424_CUE_SOLO;
		else
			route->flags &= ~MOTU424_CUE_SOLO;
		break;
	case MOTU424_CUE_BUS_VOLUME:
		cue->buses[bus].volume = value;
		break;
	case MOTU424_CUE_BUS_SWITCH:
		cue->buses[bus].mute = !value;
		break;
	case MOTU424_CUE_INPUT_TRIM:
		cue->inputs[input].trim = value;
		break;
	case MOTU424_CUE_INPUT_SWITCH:
		cue->inputs[input].mute = !value;
		break;
	case MOTU424_CUE_BALANCE:
		cue->stereo[bus][input / 2].balance = value;
		break;
	case MOTU424_CUE_WIDTH:
		cue->stereo[bus][input / 2].width = value;
		break;
	case MOTU424_CUE_TALKBACK_SWITCH:
		cue->talkback.active = value;
		break;
	case MOTU424_CUE_LISTENBACK_SWITCH:
		cue->listenback.active = value;
		break;
	case MOTU424_CUE_TALKBACK_INPUT:
		cue->talkback.input = value;
		break;
	case MOTU424_CUE_LISTENBACK_INPUT:
		cue->listenback.input = value;
		break;
	case MOTU424_CUE_TALKBACK_DIM:
		cue->talkback.dim = value;
		break;
	case MOTU424_CUE_LISTENBACK_DIM:
		cue->listenback.dim = value;
		break;
	case MOTU424_CUE_TALKBACK_BUS:
		if (value)
			cue->talkback.buses |= 1ULL << bus;
		else
			cue->talkback.buses &= ~(1ULL << bus);
		break;
	case MOTU424_CUE_LISTENBACK_BUS:
		if (value)
			cue->listenback.buses |= 1ULL << bus;
		else
			cue->listenback.buses &= ~(1ULL << bus);
		break;
	case MOTU424_CUE_METER_INPUT_BANK:
		cue->meter_input_bank = value;
		break;
	case MOTU424_CUE_METER_BUS:
		cue->meter_bus = value;
		break;
	}
}

static bool cue_mic_route(const struct motu424_cue_talkback *mic,
			  unsigned int bus, unsigned int input)
{
	return mic->active && mic->input == input && (mic->buses & (1ULL << bus));
}

int motu424_qmix_update_trim(struct motu424_qmix *cue,
			       const struct motu424_cue_layout *layout)
{
	u8 factors[MOTU424_TRIM_FACTORS];
	unsigned int input;

	if (!layout)
		return -EINVAL;
	memset(factors, MOTU424_TRIM_UNITY, sizeof(factors));
	for (input = 0; input < MOTU424_CUE_INPUTS; input++) {
		if (layout->inputs[input] > 1)
			return -EINVAL;
		factors[input + input / 24] = cue->inputs[input].mute ?
			0 : cue->inputs[input].trim;
	}
	if (cue->quad)
		for (input = 0; input < MOTU424_CUE_INPUTS; input++)
			if (layout->inputs[input] &&
			    input % 24 < MOTU424_CUE_QUAD_IN_OFFSET) {
				unsigned int partner = input + MOTU424_CUE_QUAD_IN_OFFSET;

				factors[partner + partner / 24] =
					factors[input + input / 24];
			}
	return motu424_dsp_set_trim(cue->dsp, factors);
}

/*
 * Reserve for all unmuted routes, even if a port or solo is inactive, so that
 * a layout change can never need more faders than the budget.
 */
bool motu424_qmix_fits(const struct motu424_qmix *cue, unsigned int faders)
{
	unsigned int bus, input, count = 0;

	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++) {
		if (cue->buses[bus].mute)
			continue;
		for (input = 0; input < MOTU424_CUE_INPUTS; input++)
			if ((!(cue->routes[bus][input].flags & MOTU424_CUE_MUTE) ||
			     cue_mic_route(&cue->talkback, bus, input) ||
			     cue_mic_route(&cue->listenback, bus, input)) &&
			    ++count > faders)
				return false;
	}
	return true;
}

static bool cue_over_capacity(const struct motu424_qmix *cue)
{
	return !motu424_qmix_fits(cue, cue->faders);
}

static void cue_solos(const struct motu424_qmix *cue,
		      const struct motu424_cue_layout *layout,
		      bool solos[MOTU424_MIX_PAIRS])
{
	unsigned int bus, input;

	memset(solos, 0, MOTU424_MIX_PAIRS * sizeof(*solos));
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++)
		for (input = 0; input < MOTU424_CUE_INPUTS; input++)
			if (layout->inputs[input] &&
			    (cue->routes[bus][input].flags & MOTU424_CUE_SOLO)) {
				solos[bus] = true;
				break;
			}
}

static bool cue_wanted(const struct motu424_qmix *cue,
		       const struct motu424_cue_layout *layout,
		       const bool solos[MOTU424_MIX_PAIRS],
		       unsigned int bus, unsigned int input)
{
	const struct motu424_cue_route *route = &cue->routes[bus][input];

	if (cue->quad && (input % 24 >= MOTU424_CUE_QUAD_IN_OFFSET ||
			  bus % 12 >= MOTU424_CUE_QUAD_PAIR_OFFSET))
		return false;
	return (layout->outputs & (1ULL << bus)) && layout->inputs[input] &&
		!cue->buses[bus].mute &&
		(cue_mic_route(&cue->talkback, bus, input) ||
		 cue_mic_route(&cue->listenback, bus, input) ||
		 (!(route->flags & MOTU424_CUE_MUTE) &&
		  (!solos[bus] || (route->flags & MOTU424_CUE_SOLO))));
}

/* Vendor volume update @0x17d60: dim other routes, not the microphone itself. */
static u16 cue_route_volume(const struct motu424_qmix *cue,
			    unsigned int bus, unsigned int input)
{
	u32 volume = cue->routes[bus][input].volume;

	if (cue->talkback.active && (cue->talkback.buses & (1ULL << bus)) &&
	    input != cue->talkback.input)
		return volume * cue->talkback.dim >> 7;
	if (cue->listenback.active && (cue->listenback.buses & (1ULL << bus)) &&
	    input != cue->listenback.input)
		return volume * cue->listenback.dim >> 7;
	return volume;
}

static int cue_meter_target(const struct motu424_qmix *cue,
			     const struct motu424_cue_layout *layout)
{
	if (cue->meter_bus < MOTU424_MIX_PAIRS &&
	    (layout->outputs & (1ULL << cue->meter_bus)))
		return cue->meter_bus;
	return -1;
}

static void cue_fault(struct motu424_qmix *cue)
{
	cue->fault = true;
	if (cue->dsp->ops->warn)
		cue->dsp->ops->warn(cue->dsp->ctx,
			"Q-Mix DSP state uncertain; reload the driver before changing routes");
}

static enum motu424_alt_type cue_alt_type(const struct motu424_cue_layout *layout,
					  unsigned int bus)
{
	if (layout->split_pairs & (1ULL << bus))
		return MOTU424_ALT_SPLIT;
	if (layout->skip_pairs & (1ULL << bus))
		return MOTU424_ALT_NONE;
	return MOTU424_ALT_STEREO;
}

/*
 * Drv424::ConnectFader 0x16410 and DisconnectFader 0x162e0: at 4x the
 * partner fader carries the second sample of each pair.
 */
static void cue_connect(struct motu424_qmix *cue,
			struct motu424_mixer_state *mixer, unsigned int fader,
			unsigned int bus)
{
	motu424_mixer_connect(mixer, fader, bus, false);
	if (cue->quad)
		motu424_mixer_connect(mixer, fader + cue->faders,
				      bus + MOTU424_CUE_QUAD_PAIR_OFFSET, false);
}

static void cue_disconnect(struct motu424_qmix *cue,
			   struct motu424_mixer_state *mixer, unsigned int fader)
{
	motu424_mixer_disconnect(mixer, fader);
	if (cue->quad)
		motu424_mixer_disconnect(mixer, fader + cue->faders);
}

static void cue_fader_init(struct motu424_qmix *cue, unsigned int fader,
			   unsigned int bus, unsigned int input)
{
	const struct motu424_cue_route *route = &cue->routes[bus][input];
	unsigned int i, n = cue->quad ? 2 : 1;

	for (i = 0; i < n; i++) {
		unsigned int f = fader + i * cue->faders;

		motu424_fader_set_immediately(cue->dsp, f, cue->buses[bus].volume,
					      cue_route_volume(cue, bus, input), route->pan);
		motu424_fader_clear_fade(cue->dsp, f);
		motu424_fader_set_input(cue->dsp, f,
					input + i * MOTU424_CUE_QUAD_IN_OFFSET);
	}
}

/*
 * Removed faders remain reserved until their fade has completed and a
 * stable program without them has been downloaded.
 */
static int cue_stage(struct motu424_qmix *cue,
		     const struct motu424_cue_layout *layout,
		     const bool solos[MOTU424_MIX_PAIRS], bool remove, bool add)
{
	struct motu424_cue_work *work;
	unsigned int bus, input, fader;
	bool graph_changed = false;
	int n, err = 0;

	work = kvmalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;
	memset(work->used, 0, sizeof(work->used));
	memcpy(&work->mixer, cue->mixer, sizeof(work->mixer));
	motu424_mixer_disable_output_metering(&work->mixer);
	if (cue_meter_target(cue, layout) >= 0)
		motu424_mixer_meter_output(&work->mixer, cue->meter_bus);
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++) {
		motu424_mixer_set_alt_type(&work->mixer, bus,
					   cue_alt_type(layout, bus));
		motu424_mixer_enable_dma(&work->mixer, bus,
					 !!(layout->dma_pairs & (1ULL << bus)));
		for (input = 0; input < MOTU424_CUE_INPUTS; input++) {
			fader = cue->routes[bus][input].fader;
			work->faders[bus][input] = fader;
			if (fader == MOTU424_CUE_NO_FADER)
				continue;
			if (fader >= cue->faders || work->used[fader]) {
				err = -EIO;
				goto out;
			}
			work->used[fader] = true;
		}
	}
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++) {
		for (input = 0; input < MOTU424_CUE_INPUTS; input++) {
			fader = work->faders[bus][input];
			if (remove && fader != MOTU424_CUE_NO_FADER &&
			    !cue_wanted(cue, layout, solos, bus, input)) {
				cue_disconnect(cue, &work->mixer, fader);
				work->faders[bus][input] = MOTU424_CUE_NO_FADER;
				graph_changed = true;
			}
			if (!add || fader != MOTU424_CUE_NO_FADER ||
			    !cue_wanted(cue, layout, solos, bus, input))
				continue;
			for (fader = 0; fader < cue->faders; fader++)
				if (!work->used[fader])
					break;
			if (fader == cue->faders) {
				err = -ENOSPC;
				goto out;
			}
			work->used[fader] = true;
			work->faders[bus][input] = fader;
			cue_connect(cue, &work->mixer, fader, bus);
			graph_changed = true;
		}
	}
	n = motu424_mixer_generate(&work->mixer, work->words,
				    MOTU424_MIX_BUF_WORDS);
	if (n < 0) {
		err = n;
		goto out;
	}
	if (cue->has_program) {
		err = motu424_mixer_wait_routing_fade(cue->dsp);
		if (err)
			goto out;
	}
	err = motu424_fader_wait_volume_fade(cue->dsp);
	if (err)
		goto out;
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++) {
		for (input = 0; input < MOTU424_CUE_INPUTS; input++) {
			struct motu424_cue_route *route = &cue->routes[bus][input];

			fader = work->faders[bus][input];
			if (route->fader != MOTU424_CUE_NO_FADER ||
			    fader == MOTU424_CUE_NO_FADER)
				continue;
			cue_fader_init(cue, fader, bus, input);
		}
	}
	motu424_fader_write_input_table(cue->dsp);
	err = motu424_mixer_download(cue->dsp, work->words, n);
	if (err)
		goto out;
	if (cue->mixer->metered_output != work->mixer.metered_output)
		cue->meter_primed = false;
	memcpy(cue->mixer, &work->mixer, sizeof(work->mixer));
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++)
		for (input = 0; input < MOTU424_CUE_INPUTS; input++)
			cue->routes[bus][input].fader = work->faders[bus][input];
	cue->dma_pairs = layout->dma_pairs;
	cue->split_pairs = layout->split_pairs;
	cue->skip_pairs = layout->skip_pairs;
	cue->has_program = true;
	cue->generation++;
	if (!graph_changed)
		goto out;

	err = motu424_mixer_wait_routing_fade(cue->dsp);
	if (err)
		goto fault;
	motu424_mixer_complete_fades(cue->mixer);
	n = motu424_mixer_generate(cue->mixer, work->words,
				    MOTU424_MIX_BUF_WORDS);
	if (n < 0) {
		err = n;
		goto fault;
	}
	err = motu424_mixer_download(cue->dsp, work->words, n);
	if (err)
		goto fault;
	goto out;

fault:
	cue_fault(cue);
out:
	kvfree(work);
	return err;
}

static int cue_update_gains(struct motu424_qmix *cue)
{
	unsigned int bus, input;
	bool active = false, changed = false;
	int err;

	for (bus = 0; bus < MOTU424_MIX_PAIRS && !active; bus++)
		for (input = 0; input < MOTU424_CUE_INPUTS; input++)
			if (cue->routes[bus][input].fader != MOTU424_CUE_NO_FADER) {
				active = true;
				break;
			}
	if (!active)
		return 0;
	if (cue->has_program) {
		err = motu424_mixer_wait_routing_fade(cue->dsp);
		if (err)
			return err;
	}
	err = motu424_fader_wait_volume_fade(cue->dsp);
	if (err)
		return err;
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++)
		for (input = 0; input < MOTU424_CUE_INPUTS; input++) {
			const struct motu424_cue_route *route = &cue->routes[bus][input];

			if (route->fader == MOTU424_CUE_NO_FADER)
				continue;
			changed |= motu424_fader_set_fade(cue->dsp, route->fader,
							  cue->buses[bus].volume,
							  cue_route_volume(cue, bus, input),
							  route->pan);
			if (cue->quad)	/* SetFaderWithCrossfade 0x16a70 */
				changed |= motu424_fader_set_fade(cue->dsp,
						route->fader + cue->faders,
						cue->buses[bus].volume,
						cue_route_volume(cue, bus, input), route->pan);
		}
	if (changed) {
		motu424_fader_start_volume_fade(cue->dsp);
		err = motu424_fader_wait_volume_fade(cue->dsp);
		if (err) {
			cue_fault(cue);
			return err;
		}
	}
	return 0;
}

int motu424_qmix_sync(struct motu424_qmix *cue,
			const struct motu424_cue_layout *layout)
{
	bool solos[MOTU424_MIX_PAIRS];
	unsigned int bus, input, allocated_count = 0, additions = 0, removals = 0;
	int err;

	if (!layout || layout->outputs >> MOTU424_MIX_PAIRS ||
	    layout->dma_pairs >> MOTU424_MIX_PAIRS ||
	    layout->split_pairs >> MOTU424_MIX_PAIRS ||
	    layout->skip_pairs >> MOTU424_MIX_PAIRS)
		return -EINVAL;
	if (cue->fault)
		return -EIO;
	for (input = 0; input < MOTU424_CUE_INPUTS; input++)
		if (layout->inputs[input] > 1)
			return -EINVAL;
	cue_solos(cue, layout, solos);
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++)
		for (input = 0; input < MOTU424_CUE_INPUTS; input++) {
			bool allocated = cue->routes[bus][input].fader !=
				MOTU424_CUE_NO_FADER;
			bool wanted = cue_wanted(cue, layout, solos, bus, input);

			allocated_count += allocated;
			additions += !allocated && wanted;
			removals += allocated && !wanted;
		}
	if (allocated_count - removals + additions > cue->faders)
		return -ENOSPC;
	if (additions && allocated_count + additions > cue->faders) {
		err = cue_stage(cue, layout, solos, true, false);
		if (err)
			return err;
		removals = 0;
	}
	if (additions || removals || cue->dma_pairs != layout->dma_pairs ||
	    cue->split_pairs != layout->split_pairs ||
	    cue->skip_pairs != layout->skip_pairs ||
	    cue->mixer->metered_output != cue_meter_target(cue, layout)) {
		err = cue_stage(cue, layout, solos, removals != 0, additions != 0);
		if (err)
			return err;
	}
	return cue_update_gains(cue);
}

int motu424_qmix_put(struct motu424_qmix *cue,
		       enum motu424_cue_control control, unsigned int bus,
		       unsigned int input, long value,
		       const struct motu424_cue_layout *layout)
{
	bool stereo = control == MOTU424_CUE_BALANCE || control == MOTU424_CUE_WIDTH;
	struct motu424_cue_route saved[2];
	unsigned int generation;
	long old;
	int err, rollback;

	if (!cue_control_valid(control, value))
		return -EINVAL;
	err = motu424_qmix_get(cue, control, bus, input, &old);
	if (err)
		return err;
	if (cue->fault)
		return -EIO;
	if (old == value && !stereo)
		return 0;
	if (stereo) {
		struct motu424_cue_route *left = &cue->routes[bus][input];
		struct motu424_cue_route *right = left + 1;
		u32 volume = left->volume > right->volume ? left->volume : right->volume;

		saved[0] = *left;
		saved[1] = *right;
		left->volume = right->volume = volume;
		if (control == MOTU424_CUE_BALANCE) {
			left->pan = 0;
			right->pan = 128;
			if (value > 64)
				left->volume = volume * value * (128 - value) / 4096;
			else if (value < 64)
				right->volume = volume * value * (128 - value) / 4096;
		} else {
			left->pan = value;
			right->pan = 128 - value;
		}
		if (old == value && left->volume == saved[0].volume &&
		    right->volume == saved[1].volume && left->pan == saved[0].pan &&
		    right->pan == saved[1].pan)
			return 0;
	}
	cue_set(cue, control, bus, input, value);
	if (control == MOTU424_CUE_INPUT_TRIM ||
	    control == MOTU424_CUE_INPUT_SWITCH) {
		err = motu424_qmix_update_trim(cue, layout);
		if (!err)
			return 1;
		cue_set(cue, control, bus, input, old);
		return err;
	}
	generation = cue->generation;
	if (cue_over_capacity(cue)) {
		err = -ENOSPC;
		goto restore;
	}
	err = motu424_qmix_sync(cue, layout);
	if (!err) {
		if (control == MOTU424_CUE_METER_BUS ||
		    control == MOTU424_CUE_METER_INPUT_BANK)
			cue->meter_primed = false;
		return 1;
	}
restore:
	cue_set(cue, control, bus, input, old);
	if (stereo) {
		cue->routes[bus][input].volume = saved[0].volume;
		cue->routes[bus][input].pan = saved[0].pan;
		cue->routes[bus][input + 1].volume = saved[1].volume;
		cue->routes[bus][input + 1].pan = saved[1].pan;
	}
	if (!cue->fault && cue->generation != generation) {
		rollback = motu424_qmix_sync(cue, layout);
		if (rollback)
			cue_fault(cue);
	}
	return err;
}

int motu424_qmix_read_meters(struct motu424_qmix *cue,
			       const struct motu424_cue_layout *layout,
			       u16 peaks[MOTU424_METER_VALUES])
{
	unsigned int base = cue->meter_input_bank * MOTU424_METER_INPUTS;
	unsigned int next_base;
	u8 inputs[MOTU424_METER_INPUTS];
	unsigned int i;
	int err;

	if (!layout || !peaks)
		return -EINVAL;
	if (!cue->meter_primed) {
		for (i = 0; i < MOTU424_METER_INPUTS; i++)
			inputs[i] = base + i;
		err = motu424_dsp_swap_meters(cue->dsp, inputs, NULL);
		if (err)
			return err;
		cue->meter_primed = true;
		return -EAGAIN;
	}
	/* Return the current bank's window while selecting the next bank. */
	next_base = (cue->meter_input_bank ^ 1) * MOTU424_METER_INPUTS;
	for (i = 0; i < MOTU424_METER_INPUTS; i++)
		inputs[i] = next_base + i;
	err = motu424_dsp_swap_meters(cue->dsp, inputs, peaks);
	if (err)
		return err;
	cue->meter_input_bank ^= 1;
	for (i = 0; i < MOTU424_METER_INPUTS; i++)
		if (!layout->inputs[base + i])
			peaks[i] = 0;
	if (cue->mixer->metered_output < 0)
		memset(peaks + MOTU424_METER_INPUTS, 0,
		       4 * sizeof(*peaks));
	return 0;
}

int motu424_qmix_set_faders(struct motu424_qmix *cue, unsigned int faders,
			      bool quad)
{
	unsigned int bus, input;

	if (!faders || faders * (quad ? 2 : 1) > MOTU424_MIX_ROUTINGS)
		return -EINVAL;
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++)
		for (input = 0; input < MOTU424_CUE_INPUTS; input++)
			if (cue->routes[bus][input].fader != MOTU424_CUE_NO_FADER)
				return -EBUSY;
	cue->faders = faders;
	cue->quad = quad;
	cue->meter_primed = false;
	return 0;
}

unsigned int motu424_qmix_budget(bool is_424b, unsigned int mult)
{
	/* Drv+0x20ca0: the 2x budget, set by Drv424 and Drv424b */
	unsigned int high = is_424b ? MOTU424_MIX_ROUTINGS : 96;

	if (mult <= 1)
		return MOTU424_MIX_ROUTINGS;
	return mult == 2 ? high : high / 2;
}
