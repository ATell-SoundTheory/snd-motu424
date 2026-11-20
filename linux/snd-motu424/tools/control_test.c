// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exercise the ALSA control glue with a fake ALSA core and Q-Mix backend.
 *
 * cc -std=gnu11 -Wall -Wextra -Werror -I tools/shim -I . \
 *    tools/control_test.c motu424_control.c motu424_iface.c \
 *    -o /tmp/motu424-control-test
 */
#include <assert.h>
#include <errno.h>
#include <linux/workqueue.h>
#include <sound/control.h>

#include "motu424.h"
#include "motu424_qmix.h"
#include "motu424_tables.h"

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

#define CUE_GROUPS (MOTU424_MIX_PAIRS * 6 + 15)
#define IFACE_GROUPS 21
#define CARD_GROUPS 1
#define GROUPS (CUE_GROUPS + IFACE_GROUPS + CARD_GROUPS)

unsigned int kshim_warnings;
int kshim_verbose;
struct work_struct *kshim_pending_work;

static struct snd_kcontrol *registered[GROUPS];
static unsigned int groups, elements;
static int fail_add = -1;
static unsigned int fake_clock_source;
static unsigned int notifications;
static bool inactive[48 * 4 * 96 + 2 * 48 + 2 * 96 + 48 * 2 * 48 + 105 +
		     IFACE_GROUPS * MOTU424_MAX_WIRES + CARD_GROUPS + 1];

static struct {
	enum motu424_cue_control control;
	unsigned int bus, input, get_calls, put_calls, sync_calls, trim_calls;
	long value;
	int get_result, put_result, sync_result, trim_result;
	int meter_result;
	unsigned int meter_calls;
	bool meter_prime_once;
	struct motu424_cue_layout layout;
} backend;

void snd_ctl_notify(struct snd_card *card, unsigned int mask, struct snd_ctl_elem_id *id)
{
	struct snd_ctl_elem_id lookup = *id;
	struct snd_kcontrol *control;

	lookup.numid = 0;
	control = snd_ctl_find_id(card, &lookup);
	assert(control);
	assert(id->numid == control->id.numid + id->index - control->id.index);
	assert(mask == SNDRV_CTL_EVENT_MASK_VALUE);
	assert(id->iface == SNDRV_CTL_ELEM_IFACE_MIXER);
	assert(id->index == 94 || id->index == 95);
	assert(!strcmp(id->name, "Q-Mix Bus 47 Input Playback Volume") ||
	       !strcmp(id->name, "Q-Mix Bus 47 Input Playback Pan"));
	notifications++;
}

struct snd_kcontrol *snd_ctl_new1(const struct snd_kcontrol_new *new,
				  void *private_data)
{
	struct snd_kcontrol *control = calloc(1, sizeof(*control));

	if (!control)
		return NULL;
	assert(new->count && new->count <= 1028);
	snprintf(control->id.name, sizeof(control->id.name), "%s", new->name);
	control->id.iface = new->iface;
	control->iface = new->iface;
	control->access = new->access;
	control->count = new->count;
	control->info = new->info;
	control->get = new->get;
	control->put = new->put;
	control->private_value = new->private_value;
	control->private_data = private_data;
	return control;
}

int snd_ctl_add(struct snd_card *card, struct snd_kcontrol *control)
{
	unsigned int i;

	(void)card;
	if (fail_add >= 0 && groups == (unsigned int)fail_add) {
		free(control);
		return -ENOMEM;
	}
	assert(groups < GROUPS);
	for (i = 0; i < groups; i++)
		assert(strcmp(registered[i]->id.name, control->id.name));
	control->id.numid = elements + 1;
	for (i = 0; i < control->count; i++)
		inactive[control->id.numid + i] =
			!!(control->access & SNDRV_CTL_ELEM_ACCESS_INACTIVE);
	elements += control->count;
	registered[groups++] = control;
	return 0;
}

struct snd_kcontrol *snd_ctl_find_id(struct snd_card *card, struct snd_ctl_elem_id *id)
{
	unsigned int i;

	(void)card;
	assert(!id->numid);
	for (i = 0; i < groups; i++) {
		struct snd_kcontrol *control = registered[i];

		if (id->iface == control->id.iface &&
		    !strcmp(id->name, control->id.name) &&
		    id->index >= control->id.index &&
		    id->index - control->id.index < control->count)
			return control;
	}
	return NULL;
}

int snd_ctl_activate_id(struct snd_card *card, struct snd_ctl_elem_id *id,
			int active)
{
	bool changed;

	(void)card;
	assert(id->numid && id->numid <= elements);
	changed = inactive[id->numid] == !!active;
	inactive[id->numid] = !active;
	return changed;
}

void motu424_qmix_init(struct motu424_qmix *cue,
			 struct motu424_mixer_state *mixer,
			 struct motu424_dsp *dsp)
{
	memset(cue, 0, sizeof(*cue));
	cue->mixer = mixer;
	cue->dsp = dsp;
}

int motu424_qmix_get(const struct motu424_qmix *cue,
		       enum motu424_cue_control control, unsigned int bus,
		       unsigned int input, long *value)
{
	(void)cue;
	backend.control = control;
	backend.bus = bus;
	backend.input = input;
	backend.get_calls++;
	*value = backend.value;
	return backend.get_result;
}

int motu424_qmix_put(struct motu424_qmix *cue,
		       enum motu424_cue_control control, unsigned int bus,
		       unsigned int input, long value,
		       const struct motu424_cue_layout *layout)
{
	(void)cue;
	backend.control = control;
	backend.bus = bus;
	backend.input = input;
	backend.value = value;
	backend.layout = *layout;
	backend.put_calls++;
	return backend.put_result;
}

int motu424_qmix_sync(struct motu424_qmix *cue,
			const struct motu424_cue_layout *layout)
{
	backend.layout = *layout;
	backend.sync_calls++;
	if (!backend.sync_result)
		cue->dma_pairs = layout->dma_pairs;
	return backend.sync_result;
}

int motu424_qmix_update_trim(struct motu424_qmix *cue,
			       const struct motu424_cue_layout *layout)
{
	(void)cue;
	backend.layout = *layout;
	backend.trim_calls++;
	return backend.trim_result;
}

int motu424_qmix_read_meters(struct motu424_qmix *cue,
			       const struct motu424_cue_layout *layout,
			       u16 peaks[MOTU424_METER_VALUES])
{
	unsigned int i;

	(void)cue;
	backend.layout = *layout;
	backend.meter_calls++;
	if (backend.meter_prime_once) {
		backend.meter_prime_once = false;
		return -EAGAIN;
	}
	for (i = 0; i < MOTU424_METER_VALUES; i++)
		peaks[i] = 0x100 + i;
	return backend.meter_result;
}

static unsigned int fake_set_mult_calls, fake_set_mult_mask;

void motu424_clock_set_mult(struct motu424 *m, unsigned int mult,
			    unsigned int mask)
{
	m->mult = mult;
	m->wire_mask = mask;
	fake_set_mult_calls++;
	fake_set_mult_mask = mask;
}

int motu424_qmix_set_faders(struct motu424_qmix *cue, unsigned int faders,
			      bool quad)
{
	(void)cue;
	(void)faders;
	(void)quad;
	return 0;
}

bool motu424_qmix_fits(const struct motu424_qmix *cue, unsigned int faders)
{
	(void)cue;
	(void)faders;
	return true;
}

unsigned int motu424_qmix_budget(bool is_424b, unsigned int mult)
{
	(void)is_424b;
	return mult;
}

int motu424_clock_set_source(struct motu424 *m, unsigned int source)
{
	(void)m;
	fake_clock_source = source;
	return 0;
}

bool motu424_clock_source_valid(unsigned int source)
{
	return source < MOTU424_WS_COUNT &&
	       ((MOTU424_WS_ALWAYS_MASK | MOTU424_WS_VIDEO_MASK) & BIT(source));
}

static unsigned int fake_smpte_refreshes;

void motu424_smpte_refresh(struct motu424 *m)
{
	(void)m;
	fake_smpte_refreshes++;
}

static void run_pending_work(void)
{
	struct work_struct *work = kshim_pending_work;

	assert(work);
	kshim_pending_work = NULL;
	work->func(work);
}

static void free_controls(struct motu424 *m)
{
	unsigned int i;

	motu424_qmix_cancel(m);
	for (i = 0; i < groups; i++)
		free(registered[i]);
	free(m->controls);
	m->controls = NULL;
	groups = elements = 0;
}

static void test_registration(struct motu424 *m)
{
	static const char *suffixes[] = {
		"Input Playback Volume", "Input Playback Pan",
		"Input Playback Switch", "Input Solo Switch",
	};
	struct snd_ctl_elem_info info = { 0 };
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	unsigned int bus, i;

	assert(motu424_qmix_create(m) == 0);
	assert(groups == GROUPS);
	assert(elements == 48 * 4 * 96 + 2 * 48 + 2 * 96 + 48 * 2 * 48 + 105 +
	       IFACE_GROUPS * MOTU424_MAX_WIRES + CARD_GROUPS);
	for (bus = 0; bus < MOTU424_MIX_PAIRS; bus++)
		for (i = 0; i < 4; i++) {
			struct snd_kcontrol *control = registered[4 * bus + i];

			snprintf(name, sizeof(name), "Q-Mix Bus %u %s", bus,
				 suffixes[i]);
			assert(!strcmp(control->id.name, name));
			assert(control->count == MOTU424_CUE_INPUTS);
			assert(control->iface == SNDRV_CTL_ELEM_IFACE_MIXER);
			assert(control->access == SNDRV_CTL_ELEM_ACCESS_READWRITE);
			assert(control->private_data == m);
		}
	assert(!strcmp(registered[192]->id.name, "Q-Mix Bus Playback Volume"));
	assert(!strcmp(registered[193]->id.name, "Q-Mix Bus Playback Switch"));
	assert(registered[192]->count == 48 && registered[193]->count == 48);
	assert(!strcmp(registered[194]->id.name, "Q-Mix Input Trim Volume"));
	assert(!strcmp(registered[195]->id.name, "Q-Mix Input Switch"));
	assert(registered[194]->count == 96 && registered[195]->count == 96);
	assert(!strcmp(registered[CUE_GROUPS]->id.name,
		       "Interface 2408 Analog Bank"));
	assert(!strcmp(registered[CUE_GROUPS + 1]->id.name,
		       "Interface 2408 Bank A Format"));
	assert(!strcmp(registered[CUE_GROUPS + 2]->id.name,
		       "Interface 2408 Bank B Format"));
	assert(!strcmp(registered[CUE_GROUPS + 3]->id.name,
		       "Interface 2408 Bank C Format"));
	assert(!strcmp(registered[CUE_GROUPS + 4]->id.name,
		       "Interface 2408 Clock Source"));
	assert(!strcmp(registered[CUE_GROUPS + 5]->id.name,
		       "Interface AES Input Pair"));
	assert(!strcmp(registered[CUE_GROUPS + 6]->id.name,
		       "Interface AES Output SRC Mode"));
	assert(!strcmp(registered[CUE_GROUPS + 7]->id.name,
		       "Interface AES Output Clock"));
	assert(!strcmp(registered[CUE_GROUPS + 8]->id.name,
		       "Interface AES Input SRC"));
	assert(!strcmp(registered[CUE_GROUPS + 9]->id.name,
		       "Interface Peak Hold Time"));
	assert(!strcmp(registered[CUE_GROUPS + 10]->id.name,
		       "Interface Clip Hold Time"));
	assert(!strcmp(registered[CUE_GROUPS + 11]->id.name,
		       "Interface 2408 Analog Mirror Bank"));
	assert(!strcmp(registered[CUE_GROUPS + 12]->id.name,
		       "Interface Word Clock Out Rate"));
	assert(!strcmp(registered[CUE_GROUPS + 15]->id.name,
		       "Interface 24I/O Analog 17-24 -10dBV Switch"));
	assert(!strcmp(registered[CUE_GROUPS + 19]->id.name,
		       "Interface 2408mk3 Analog 7-8 -10dBV Switch"));
	assert(!strcmp(registered[CUE_GROUPS + 20]->id.name,
		       "Interface 1296 Clock Source"));
	assert(!strcmp(registered[CUE_GROUPS + IFACE_GROUPS]->id.name,
		       "Card Clock Source"));
	for (i = CUE_GROUPS; i < CUE_GROUPS + IFACE_GROUPS; i++)
		assert(registered[i]->access == (SNDRV_CTL_ELEM_ACCESS_READWRITE |
						 SNDRV_CTL_ELEM_ACCESS_INACTIVE));
	assert(registered[GROUPS - 1]->access == SNDRV_CTL_ELEM_ACCESS_READWRITE);
	for (i = CUE_GROUPS; i < CUE_GROUPS + IFACE_GROUPS; i++)
		assert(registered[i]->count == MOTU424_MAX_WIRES);
	assert(registered[CUE_GROUPS + IFACE_GROUPS]->count == 1);
	assert(registered[CUE_GROUPS]->info(registered[CUE_GROUPS], &info) == 0);
	assert(info.type == SNDRV_CTL_ELEM_TYPE_INTEGER &&
	       info.value.integer.max == MOTU424_2408_ANALOG_NONE);
	assert(registered[CUE_GROUPS + 8]->info(registered[CUE_GROUPS + 8],
						&info) == 0);
	assert(info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN);
	for (i = CUE_GROUPS + 11; i <= CUE_GROUPS + 12; i++) {
		assert(registered[i]->info(registered[i], &info) == 0);
		assert(info.type == SNDRV_CTL_ELEM_TYPE_INTEGER &&
		       info.value.integer.max == 2);
	}
	for (i = CUE_GROUPS + 13; i < CUE_GROUPS + 20; i++) {
		assert(registered[i]->info(registered[i], &info) == 0);
		assert(info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN);
	}
	assert(registered[CUE_GROUPS + 20]->info(registered[CUE_GROUPS + 20],
						 &info) == 0);
	assert(info.type == SNDRV_CTL_ELEM_TYPE_INTEGER &&
	       info.value.integer.max == MOTU424_1296_UICLK_CARD);
	assert(registered[0]->info(registered[0], &info) == 0);
	assert(info.type == SNDRV_CTL_ELEM_TYPE_INTEGER && info.count == 1 &&
	       info.value.integer.min == 0 && info.value.integer.max == 0x8000);
	assert(registered[1]->info(registered[1], &info) == 0);
	assert(info.value.integer.max == 0x80);
	assert(registered[2]->info(registered[2], &info) == 0);
	assert(info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN && info.count == 1 &&
	       info.value.integer.min == 0 && info.value.integer.max == 1);
	assert(registered[194]->info(registered[194], &info) == 0);
	assert(info.type == SNDRV_CTL_ELEM_TYPE_INTEGER && info.count == 1 &&
	       info.value.integer.min == 0x40 && info.value.integer.max == 0xff);
	assert(registered[195]->info(registered[195], &info) == 0);
	assert(info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN && info.count == 1);
}

static void test_callbacks(struct motu424 *m)
{
	struct snd_kcontrol *route = registered[4 * 47 + 1];
	struct snd_kcontrol *bus = registered[193];
	struct snd_ctl_elem_value value = { 0 };
	unsigned int calls, warnings;
	u64 pairs = BIT_ULL(0) | BIT_ULL(12) | BIT_ULL(47);

	m->booted = true;
	m->running = true;
	m->audio = true;
	m->layout_frozen = true;
	m->wire[0].state = MOTU424_WIRE_READY;
	m->wire[1].state = MOTU424_WIRE_QUERY;
	m->wire[3].state = MOTU424_WIRE_READY;
	m->wire[0].info = &motu424_ifaces[0];
	m->wire[3].info = &motu424_ifaces[5];
	m->n_in = 4;
	m->in_hw[0] = 0;
	m->in_hw[1] = 23;
	m->in_hw[2] = 24;
	m->in_hw[3] = 95;
	m->out_pairs = pairs;

	backend.value = 64;
	value.id.numid = route->id.numid + 95;
	assert(route->get(route, &value) == 0 && value.value.integer.value[0] == 64);
	assert(backend.control == MOTU424_CUE_PAN && backend.bus == 47 &&
	       backend.input == 95);
	value.id.numid = 0;
	value.id.index = 95;
	value.value.integer.value[0] = 40;
	backend.put_result = 1;
	assert(route->put(route, &value) == 1);
	assert(backend.control == MOTU424_CUE_PAN && backend.bus == 47 &&
	       backend.input == 95 && backend.value == 40);
	assert(backend.layout.inputs[0] && backend.layout.inputs[23] &&
	       !backend.layout.inputs[24] && backend.layout.inputs[95]);
	assert(backend.layout.outputs == (BIT_ULL(0) | BIT_ULL(47)));
	assert(backend.layout.dma_pairs == pairs);

	/* A replacement box can have fewer channels while PCM remains frozen. */
	m->wire[3].info = &motu424_ifaces[2];
	m->wire[3].avail[7] = BIT(1);
	assert(route->put(route, &value) == 1);
	assert(!backend.layout.inputs[95] && backend.layout.outputs == BIT_ULL(0));
	assert(backend.layout.dma_pairs == pairs);
	m->wire[3].info = &motu424_ifaces[5];

	value.id.numid = bus->id.numid + 47;
	value.id.index = 0;
	value.value.integer.value[0] = 1;
	assert(bus->put(bus, &value) == 1);
	assert(backend.control == MOTU424_CUE_BUS_SWITCH && backend.bus == 47 &&
	       backend.input == 0);
	value.id.numid = registered[194]->id.numid + 95;
	value.value.integer.value[0] = 0x80;
	assert(registered[194]->put(registered[194], &value) == 1);
	assert(backend.control == MOTU424_CUE_INPUT_TRIM && backend.bus == 0 &&
	       backend.input == 95 && backend.value == 0x80);
	value.id.numid = registered[195]->id.numid + 24;
	value.value.integer.value[0] = 0;
	assert(registered[195]->put(registered[195], &value) == 1);
	assert(backend.control == MOTU424_CUE_INPUT_SWITCH && backend.bus == 0 &&
	       backend.input == 24);
	value.id.numid = bus->id.numid + 47;
	value.value.integer.value[0] = 1;
	backend.put_result = -ENOSPC;
	assert(bus->put(bus, &value) == -ENOSPC);
	backend.put_result = -ETIMEDOUT;
	warnings = kshim_warnings;
	assert(bus->put(bus, &value) == -ETIMEDOUT);
	assert(kshim_warnings == warnings + 1);
	backend.get_result = -EIO;
	value.id.numid = route->id.numid + 95;
	assert(route->get(route, &value) == -EIO);
	backend.get_result = 0;
	value.id.numid = bus->id.numid + 47;
	calls = backend.put_calls;
	m->booted = false;
	assert(bus->put(bus, &value) == -ENODEV && backend.put_calls == calls);
	m->booted = true;
	{
		struct snd_kcontrol *analog_bank = registered[CUE_GROUPS];
		struct snd_kcontrol *bank_a = registered[CUE_GROUPS + 1];
		struct snd_kcontrol *bank_c = registered[CUE_GROUPS + 3];
		struct snd_kcontrol *clock_source = registered[CUE_GROUPS + 4];
		struct snd_kcontrol *input_pair = registered[CUE_GROUPS + 5];
		struct snd_kcontrol *output_mode = registered[CUE_GROUPS + 6];
		struct snd_kcontrol *output_clock = registered[CUE_GROUPS + 7];
		struct snd_kcontrol *input_src = registered[CUE_GROUPS + 8];
		struct snd_kcontrol *peak_hold = registered[CUE_GROUPS + 9];
		struct snd_kcontrol *clip_hold = registered[CUE_GROUPS + 10];

		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].info = &motu424_ifaces[3];
		m->wire[0].model = MOTU424_IFACE_ID_1296;
		m->wire[0].state_len = MOTU424_1296_STATE_LEN;
		m->wire[0].state_msg[7] = 0x49;
		m->wire[0].state_msg[10] = MOTU424_1296_C3_DEFAULT;
		m->wire[0].aes_input_pair = 0;
		m->wire[0].aes_output_src_mode = 0;
		m->wire[0].aes_output_clock = 0;
		m->wire[0].aes_input_src = 0;
		m->wire[0].peak_hold_time = 4;
		m->wire[0].clip_hold_time = 1;
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].info = &motu424_ifaces[0];
		m->wire[0].state_len = MOTU424_2408_STATE_LEN;
		m->wire[0].state_msg[10] = MOTU424_2408_CFG0_DEFAULT;
		m->wire[0].state_msg[11] = MOTU424_2408_CFG1_DEFAULT;
		m->wire[0].clock_source = MOTU424_2408_CLK_CARD;
		m->layout_frozen = false;
		value.id.numid = analog_bank->id.numid;
		value.value.integer.value[0] = 1;
		assert(analog_bank->put(analog_bank, &value) == 1);
		assert(m->wire[0].state_msg[11] == 0x28);
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = clock_source->id.numid;
		value.value.integer.value[0] = MOTU424_2408_CLK_BANK_A;
		assert(clock_source->put(clock_source, &value) == 1);
		assert(m->wire[0].state_msg[10] == 0x41 &&
		       m->wire[0].external_clock);
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.value.integer.value[0] = MOTU424_2408_CLK_CARD;
		assert(clock_source->put(clock_source, &value) == 1);
		assert(m->wire[0].state_msg[10] == 0x42 &&
		       !m->wire[0].external_clock);
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = bank_a->id.numid;
		value.value.integer.value[0] = 1;
		assert(bank_a->put(bank_a, &value) == 1);
		assert(m->wire[0].state_msg[11] == 0x29);
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = bank_c->id.numid;
		value.value.integer.value[0] = 2;
		assert(bank_c->put(bank_c, &value) == 1);
		assert(m->wire[0].state_msg[11] == 0x69);
		m->layout_frozen = true;
		value.id.numid = bank_a->id.numid;
		value.value.integer.value[0] = 0;
		assert(bank_a->put(bank_a, &value) == -EBUSY);
		m->layout_frozen = false;
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = analog_bank->id.numid;
		value.value.integer.value[0] = 2;
		assert(analog_bank->put(analog_bank, &value) == -EINVAL);
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = bank_c->id.numid;
		value.value.integer.value[0] = 0;
		assert(bank_c->put(bank_c, &value) == 1);
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = analog_bank->id.numid;
		value.value.integer.value[0] = 2;
		assert(analog_bank->put(analog_bank, &value) == 1);
		assert(m->wire[0].state_msg[11] == 0x39);

		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		m->wire[0].info = &motu424_ifaces[3];
		m->layout_frozen = true;
		m->wire[0].state_len = MOTU424_1296_STATE_LEN;
		m->wire[0].state_msg[11] = 0;
		m->wire[0].state_msg[7] = 0;
		m->wire[0].state_msg[8] = 0;
		m->wire[0].state_msg[9] = 0;
		m->wire[0].state_msg[10] = 0;
		m->wire[0].aes_input_pair = 0;
		m->wire[0].aes_output_src_mode = 0;
		m->wire[0].aes_output_clock = 0;
		m->wire[0].aes_input_src = 0;
		m->wire[0].peak_hold_time = 4;
		m->wire[0].clip_hold_time = 1;
		value.id.numid = input_pair->id.numid;
		assert(input_pair->get(input_pair, &value) == 0 &&
		       value.value.integer.value[0] == 0);
		value.value.integer.value[0] = 3;
		assert(input_pair->put(input_pair, &value) == 1);
		assert(m->wire[0].state == MOTU424_WIRE_CONFIG &&
		       m->wire[0].state_msg[8] == 0x30 &&
		       m->wire[0].state_msg[9] == MOTU424_1296_C2_UPDATE &&
		       m->wire[0].tx_len == 23);
		assert(input_pair->put(input_pair, &value) == -EBUSY);

		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = output_mode->id.numid;
		value.value.integer.value[0] = 7;
		assert(output_mode->put(output_mode, &value) == -EINVAL);
		value.value.integer.value[0] = 8;
		assert(output_mode->put(output_mode, &value) == 1);
		assert(m->wire[0].state_msg[8] == 0x38);

		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = output_clock->id.numid;
		value.value.integer.value[0] = 11;
		assert(output_clock->put(output_clock, &value) == -EINVAL);
		value.value.integer.value[0] = 3;
		assert(output_clock->put(output_clock, &value) == 1);
		assert(m->wire[0].state_msg[9] == 0x13);
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.value.integer.value[0] = 4;
		assert(output_clock->put(output_clock, &value) == 1);
		assert(m->wire[0].state_msg[9] == MOTU424_1296_C2_UPDATE);

		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = input_src->id.numid;
		value.value.integer.value[0] = 1;
		assert(input_src->put(input_src, &value) == 1);
		assert(!(m->wire[0].state_msg[7] & MOTU424_1296_C0_NO_AES_IN_SRC));

		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = peak_hold->id.numid;
		value.value.integer.value[0] = 7;
		assert(peak_hold->put(peak_hold, &value) == 1);
		assert(m->wire[0].state_msg[10] == ((7 << 3) | 1));

		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].tx_pos = m->wire[0].tx_len;
		value.id.numid = clip_hold->id.numid;
		value.value.integer.value[0] = 2;
		assert(clip_hold->put(clip_hold, &value) == 1);
		assert(m->wire[0].state_msg[10] == ((7 << 3) | 2));

		/* 1296 clock: AW_1296::state::GetClockFromUIClock */
		m->wire[0].clock_source = MOTU424_1296_UICLK_CARD;
		m->wire[0].mult = 1;
		{
			static const struct {
				long ui;
				unsigned int rate;
				u8 code;
			} clocks[] = {
				{ MOTU424_1296_UICLK_INTERNAL, 48000,
				  MOTU424_1296_CLK_INT_48K },
				{ MOTU424_1296_UICLK_WORD, 48000,
				  MOTU424_1296_CLK_WORD },
				{ MOTU424_1296_UICLK_INTERNAL, 44100,
				  MOTU424_1296_CLK_INT_44K1 },
				{ MOTU424_1296_UICLK_AES, 44100,
				  MOTU424_1296_CLK_AES },
				{ MOTU424_1296_UICLK_CARD, 48000,
				  MOTU424_1296_CLK_CARD },
			};
			struct snd_kcontrol *clock_1296 =
				registered[CUE_GROUPS + 20];
			unsigned int i;

			value.id.numid = clock_1296->id.numid;
			value.value.integer.value[0] = 4;
			assert(clock_1296->put(clock_1296, &value) == -EINVAL);
			for (i = 0; i < ARRAY_SIZE(clocks); i++) {
				m->rate = clocks[i].rate;
				m->wire[0].state = MOTU424_WIRE_READY;
				m->wire[0].tx_pos = m->wire[0].tx_len;
				value.value.integer.value[0] = clocks[i].ui;
				assert(clock_1296->put(clock_1296, &value) == 1);
				assert((m->wire[0].state_msg[7] &
					MOTU424_1296_C0_CLK_MASK) ==
				       clocks[i].code);
				assert(m->wire[0].external_clock ==
				       (clocks[i].ui != MOTU424_1296_UICLK_CARD));
				assert(clock_1296->get(clock_1296, &value) == 0 &&
				       value.value.integer.value[0] ==
				       clocks[i].ui);
			}
			m->rate = 48000;
			m->wire[0].state = MOTU424_WIRE_READY;
			m->wire[0].tx_pos = m->wire[0].tx_len;
			m->open_count = 1;
			value.value.integer.value[0] = MOTU424_1296_UICLK_WORD;
			assert(clock_1296->put(clock_1296, &value) == -EBUSY);
			m->open_count = 0;
			m->word_source = MOTU424_WS_WIRE0;
			assert(clock_1296->put(clock_1296, &value) == -EBUSY);
			m->word_source = MOTU424_WS_INTERNAL;

			/* HD192 at 4x has no AES input: it runs internal */
			m->wire[0].info = &motu424_ifaces[4];
			m->wire[0].mult = 4;
			value.value.integer.value[0] = MOTU424_1296_UICLK_AES;
			assert(clock_1296->put(clock_1296, &value) == 1);
			assert((m->wire[0].state_msg[7] &
				MOTU424_1296_C0_CLK_MASK) ==
			       MOTU424_1296_CLK_INT_48K);
			m->wire[0].mult = 1;
			m->wire[0].info = &motu424_ifaces[3];
			m->wire[0].clock_source = MOTU424_1296_UICLK_CARD;
		}
		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].info = &motu424_ifaces[0];
		value.id.numid = input_pair->id.numid;
		value.value.integer.value[0] = -1;
		assert(input_pair->get(input_pair, &value) == 0 &&
		       value.value.integer.value[0] == 0);
		value.value.integer.value[0] = 1;
		assert(input_pair->put(input_pair, &value) == -ENODEV);
		value.id.numid = analog_bank->id.numid + 1;
		value.value.integer.value[0] = -1;
		assert(analog_bank->get(analog_bank, &value) == 0 &&
		       value.value.integer.value[0] == 0);
	}
	{
		struct snd_kcontrol *analog_bank = registered[CUE_GROUPS];
		struct snd_kcontrol *clock_source = registered[CUE_GROUPS + 4];
		struct snd_kcontrol *mirror = registered[CUE_GROUPS + 11];
		struct snd_kcontrol *word_out = registered[CUE_GROUPS + 12];
		struct snd_kcontrol *level_17 = registered[CUE_GROUPS + 15];
		struct snd_kcontrol *mk3_level_7 = registered[CUE_GROUPS + 19];
		struct motu424_wire *w = &m->wire[0];

		/* 24I/O: no layout change, so allowed while streaming */
		memcpy(w->state_msg, motu424_sysex_24io_state, 17);
		w->state_len = 17;
		w->info = &motu424_ifaces[5];
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		w->input_levels = 0;
		w->word_out_rate = 0;
		m->layout_frozen = true;
		m->open_count = 1;
		value.id.numid = level_17->id.numid;
		value.value.integer.value[0] = 1;
		assert(level_17->put(level_17, &value) == 1);
		assert(w->state == MOTU424_WIRE_CONFIG && w->state_msg[11] == 0x04);
		assert(level_17->get(level_17, &value) == 0 &&
		       value.value.integer.value[0] == 1);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.id.numid = word_out->id.numid;
		value.value.integer.value[0] = 2;
		assert(word_out->put(word_out, &value) == 1);
		assert(w->state_msg[15] == 1 && w->state_msg[11] == 0x04);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		w->state_msg[10] |= MOTU424_2408_CFG0_2X;
		value.value.integer.value[0] = 1;
		assert(word_out->put(word_out, &value) == 1);
		assert(w->state_msg[15] == 0);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.value.integer.value[0] = 0;
		assert(word_out->put(word_out, &value) == 1);
		assert(w->state_msg[15] == 1);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.value.integer.value[0] = 3;
		assert(word_out->put(word_out, &value) == -EINVAL);
		value.id.numid = mirror->id.numid;
		value.value.integer.value[0] = 1;
		assert(mirror->put(mirror, &value) == -ENODEV);
		value.id.numid = mk3_level_7->id.numid;
		assert(mk3_level_7->put(mk3_level_7, &value) == -ENODEV);
		m->open_count = 0;

		/* 2408: no analog bank; the analog outputs mirror a bank */
		memset(w->state_msg, 0, sizeof(w->state_msg));
		w->info = &motu424_ifaces[0];
		w->state_len = MOTU424_2408_STATE_LEN;
		w->state_msg[10] = MOTU424_2408_CFG0_DEFAULT;
		w->state_msg[11] = MOTU424_2408_CFG1_DEFAULT;
		w->clock_source = MOTU424_2408_CLK_CARD;
		w->analog_mirror = 0;
		w->input_levels = 0;
		w->word_out_rate = 0;
		m->layout_frozen = false;
		m->word_source = MOTU424_WS_INTERNAL;
		value.id.numid = analog_bank->id.numid;
		value.value.integer.value[0] = MOTU424_2408_ANALOG_NONE;
		assert(analog_bank->put(analog_bank, &value) == 1);
		assert(w->state_msg[11] == 0x10);
		assert(analog_bank->get(analog_bank, &value) == 0 &&
		       value.value.integer.value[0] == MOTU424_2408_ANALOG_NONE);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.id.numid = mirror->id.numid;
		value.value.integer.value[0] = 1;
		assert(mirror->put(mirror, &value) == 1);
		assert(w->state_msg[11] == 0x20);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.id.numid = clock_source->id.numid;
		value.value.integer.value[0] = MOTU424_2408_CLK_SPDIF;
		assert(clock_source->put(clock_source, &value) == -EINVAL);
		value.id.numid = analog_bank->id.numid;
		value.value.integer.value[0] = 0;
		assert(analog_bank->put(analog_bank, &value) == 1);
		assert(w->state_msg[11] == 0x18);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		m->layout_frozen = true;
		value.value.integer.value[0] = MOTU424_2408_ANALOG_NONE;
		assert(analog_bank->put(analog_bank, &value) == -EBUSY);
		value.id.numid = mirror->id.numid;
		value.value.integer.value[0] = 2;
		assert(mirror->put(mirror, &value) == 1);
		assert(w->state_msg[11] == 0x18 && w->analog_mirror == 2);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.id.numid = word_out->id.numid;
		assert(word_out->put(word_out, &value) == -ENODEV);
		m->layout_frozen = false;

		/* 2408mk3: levels in byte 14; word out and card clock in 15 */
		w->info = &motu424_ifaces[1];
		w->state_len = MOTU424_2408MK3_STATE_LEN;
		w->state_msg[14] = 0;
		w->state_msg[15] = MOTU424_2408MK3_B15_CARD_CLOCK |
				   MOTU424_2408MK3_B15_WORD_MATCH;
		value.id.numid = mk3_level_7->id.numid;
		value.value.integer.value[0] = 1;
		assert(mk3_level_7->put(mk3_level_7, &value) == 1);
		assert(w->state_msg[14] == 0x08 && w->state_msg[15] == 0x30);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.id.numid = word_out->id.numid;
		value.value.integer.value[0] = 2;
		assert(word_out->put(word_out, &value) == 1);
		assert(w->state_msg[15] == 0x21);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.id.numid = clock_source->id.numid;
		value.value.integer.value[0] = MOTU424_2408_CLK_WORD;
		assert(clock_source->put(clock_source, &value) == 1);
		assert(w->state_msg[15] == 0x01 && w->state_msg[14] == 0x08);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.value.integer.value[0] = MOTU424_2408_CLK_CARD;
		assert(clock_source->put(clock_source, &value) == 1);
		assert(w->state_msg[15] == 0x21);
		w->state = MOTU424_WIRE_READY;
		w->tx_pos = w->tx_len;
		value.id.numid = level_17->id.numid;
		value.value.integer.value[0] = 1;
		assert(level_17->put(level_17, &value) == -ENODEV);
		w->info = &motu424_ifaces[0];
		w->state_len = MOTU424_2408_STATE_LEN;
	}
	{
		struct snd_kcontrol *card_clock =
			registered[CUE_GROUPS + IFACE_GROUPS];
		unsigned int refreshes;

		m->wire[0].state = MOTU424_WIRE_READY;
		m->wire[0].info = &motu424_ifaces[0];
		m->wire[0].external_clock = true;
		m->word_source = MOTU424_WS_INTERNAL;
		value.id.numid = card_clock->id.numid;
		assert(card_clock->get(card_clock, &value) == 0 &&
		       value.value.integer.value[0] == MOTU424_WS_INTERNAL);
		value.value.integer.value[0] = 1;
		assert(card_clock->put(card_clock, &value) == -EINVAL);
		value.value.integer.value[0] = MOTU424_WS_ADAT;
		assert(card_clock->put(card_clock, &value) == 0);
		assert(m->word_source == MOTU424_WS_ADAT &&
		       fake_clock_source == MOTU424_WS_ADAT);
		m->word_source = MOTU424_WS_INTERNAL;
		value.value.integer.value[0] = MOTU424_WS_WIRE0;
		assert(card_clock->put(card_clock, &value) == 0);
		assert(m->word_source == MOTU424_WS_WIRE0 &&
		       fake_clock_source == MOTU424_WS_WIRE0);
		m->open_count = 1;
		value.value.integer.value[0] = MOTU424_WS_INTERNAL;
		assert(card_clock->put(card_clock, &value) == -EBUSY);
		m->open_count = 0;

		/* SMPTE always; video only from a 2408mk3 */
		m->word_source = MOTU424_WS_INTERNAL;
		refreshes = fake_smpte_refreshes;
		value.value.integer.value[0] = MOTU424_WS_SMPTE;
		assert(card_clock->put(card_clock, &value) == 0);
		assert(m->word_source == MOTU424_WS_SMPTE &&
		       fake_smpte_refreshes == refreshes + 1);
		value.value.integer.value[0] = MOTU424_WS_VIDEO_W0;
		assert(card_clock->put(card_clock, &value) == -ENODEV);
		m->wire[0].info = &motu424_ifaces[1];
		m->wire[0].state_len = MOTU424_2408MK3_STATE_LEN;
		assert(card_clock->put(card_clock, &value) == 0);
		assert(m->word_source == MOTU424_WS_VIDEO_W0 &&
		       fake_clock_source == MOTU424_WS_VIDEO_W0);
		m->wire[0].info = &motu424_ifaces[0];
		m->wire[0].state_len = MOTU424_2408_STATE_LEN;
		m->word_source = MOTU424_WS_INTERNAL;
	}
}

static void test_extended_callbacks(struct motu424 *m)
{
	struct snd_kcontrol *balance = registered[196 + 47 * 2];
	struct snd_kcontrol *width = registered[196 + 47 * 2 + 1];
	struct snd_kcontrol *peak = registered[CUE_GROUPS - 1];
	struct snd_ctl_elem_value value = { 0 };
	struct snd_ctl_elem_info info = { 0 };
	unsigned int calls;

	m->booted = true;
	backend.put_result = 1;
	value.id.numid = balance->id.numid + 47;
	value.value.integer.value[0] = 64;
	assert(balance->put(balance, &value) == 1);
	assert(backend.control == MOTU424_CUE_BALANCE &&
	       backend.bus == 47 && backend.input == 94 && notifications == 4);
	value.id.numid = 0;
	value.id.index = 47;
	assert(width->put(width, &value) == 1);
	assert(backend.control == MOTU424_CUE_WIDTH && backend.input == 94 &&
	       notifications == 8);
	assert(balance->info(balance, &info) == 0 && info.value.integer.max == 128);
	value.id.numid = registered[295]->id.numid + 47;
	value.id.index = 0;
	value.value.integer.value[0] = 1;
	assert(registered[295]->put(registered[295], &value) == 1);
	assert(backend.control == MOTU424_CUE_TALKBACK_BUS && backend.bus == 47);
	value.id.numid = registered[297]->id.numid;
	value.value.integer.value[0] = 95;
	assert(registered[297]->put(registered[297], &value) == 1);
	assert(backend.control == MOTU424_CUE_LISTENBACK_INPUT &&
	       !backend.bus && !backend.input && backend.value == 95);
	assert(registered[297]->info(registered[297], &info) == 0 &&
	       info.value.integer.max == 96);
	assert(registered[301]->info(registered[301], &info) == 0 &&
	       info.value.integer.max == 48);
	assert(peak->access == (SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE) &&
	       !peak->put);
	assert(peak->info(peak, &info) == 0 && info.count == 52 &&
	       info.value.integer.max == 65535);
	backend.meter_prime_once = true;
	calls = backend.meter_calls;
	assert(peak->get(peak, &value) == 0 && backend.meter_calls == calls + 2);
	assert(value.value.integer.value[0] == 256 && value.value.integer.value[51] == 307);
	backend.meter_result = -ETIMEDOUT;
	assert(peak->get(peak, &value) == -ETIMEDOUT);
	backend.meter_result = 0;
}

static void test_topology_work(struct motu424 *m)
{
	unsigned int calls = backend.sync_calls, warnings, i;

	assert(motu424_mixer_sync(m, false) == 0);
	assert(backend.sync_calls == ++calls && backend.layout.dma_pairs == 0);
	m->wire[3].state = MOTU424_WIRE_ABSENT;
	motu424_qmix_schedule(m);
	run_pending_work();
	assert(backend.sync_calls == ++calls);
	assert(backend.layout.outputs == BIT_ULL(0) &&
	       backend.layout.dma_pairs == m->out_pairs && !backend.layout.inputs[95]);

	backend.trim_result = -ETIMEDOUT;
	warnings = kshim_warnings;
	motu424_qmix_schedule(m);
	run_pending_work();
	assert(backend.sync_calls == calls && kshim_warnings == warnings + 1);
	backend.trim_result = 0;

	backend.sync_result = -ETIMEDOUT;
	warnings = kshim_warnings;
	motu424_qmix_schedule(m);
	run_pending_work();
	assert(backend.sync_calls == ++calls && kshim_warnings == warnings + 1);
	backend.sync_result = 0;

	/* 2408 on port 0, 1296 on port 1, 24I/O on port 2, 2408mk3 on port 3 */
	m->wire[0].state_len = MOTU424_2408_STATE_LEN;
	m->wire[1].info = &motu424_ifaces[3];
	m->wire[2].info = &motu424_ifaces[5];
	m->wire[3].info = &motu424_ifaces[1];
	m->wire[3].state_len = MOTU424_2408MK3_STATE_LEN;
	motu424_qmix_schedule(m);
	run_pending_work();
	assert(backend.sync_calls == ++calls);
	for (i = 0; i < IFACE_GROUPS; i++) {
		const struct snd_kcontrol *kctl = registered[CUE_GROUPS + i];
		bool is_2408 = i < 5 || i == 11;
		bool is_1296 = (i >= 5 && i < 11) || i == 20;
		bool is_24io = i >= 12 && i < 16;
		bool is_mk3 = is_2408 || i == 12 || (i >= 16 && i < 20);

		assert(inactive[kctl->id.numid] == !is_2408);
		assert(inactive[kctl->id.numid + 1] == !is_1296);
		assert(inactive[kctl->id.numid + 2] == !is_24io);
		assert(inactive[kctl->id.numid + 3] == !is_mk3);
	}
	m->wire[0].info = NULL;
	m->wire[1].info = NULL;
	m->wire[2].info = NULL;
	m->wire[3].info = NULL;

	m->booted = false;
	motu424_qmix_schedule(m);
	run_pending_work();
	assert(backend.sync_calls == calls);
	for (i = 0; i < IFACE_GROUPS; i++)
		assert(inactive[registered[CUE_GROUPS + i]->id.numid]);
	m->booted = true;
	motu424_qmix_schedule(m);
	motu424_qmix_cancel(m);
	assert(!kshim_pending_work);
}

int main(void)
{
	struct snd_card card = { 0 };
	struct pci_dev pci = { 0 };
	struct motu424 m = { .card = &card, .pci = &pci, .rate = 48000, .mult = 1 };

	test_registration(&m);
	test_callbacks(&m);
	test_extended_callbacks(&m);
	test_topology_work(&m);
	free_controls(&m);

	fail_add = 2;
	assert(motu424_qmix_create(&m) == -ENOMEM);
	assert(groups == 2);
	free_controls(&m);
	puts("control test: ok");
	return 0;
}
