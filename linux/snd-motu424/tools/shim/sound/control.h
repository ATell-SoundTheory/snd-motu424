/* Minimal userspace shim for <sound/control.h>. */
#ifndef SHIM_SOUND_CONTROL_H
#define SHIM_SOUND_CONTROL_H
#include <assert.h>
#include "../kshim.h"

#define SNDRV_CTL_ELEM_ID_NAME_MAXLEN 44
#define SNDRV_CTL_ELEM_IFACE_MIXER 2
#define SNDRV_CTL_ELEM_ACCESS_READWRITE 3
#define SNDRV_CTL_ELEM_ACCESS_READ 1
#define SNDRV_CTL_ELEM_ACCESS_VOLATILE 4
#define SNDRV_CTL_ELEM_ACCESS_INACTIVE 0x100
#define SNDRV_CTL_EVENT_MASK_VALUE 1
#define SNDRV_CTL_ELEM_TYPE_BOOLEAN 1
#define SNDRV_CTL_ELEM_TYPE_INTEGER 2

struct snd_ctl_elem_id {
	unsigned int numid, index, iface;
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
};

struct snd_ctl_elem_info {
	unsigned int type, count;
	union {
		struct {
			long min, max, step;
		} integer;
	} value;
};

struct snd_ctl_elem_value {
	struct snd_ctl_elem_id id;
	union {
		struct {
			long value[128];
		} integer;
	} value;
};

struct snd_kcontrol;
typedef int snd_kcontrol_info_t(struct snd_kcontrol *, struct snd_ctl_elem_info *);
typedef int snd_kcontrol_get_t(struct snd_kcontrol *, struct snd_ctl_elem_value *);
typedef int snd_kcontrol_put_t(struct snd_kcontrol *, struct snd_ctl_elem_value *);

struct snd_kcontrol_new {
	unsigned int iface, access, count;
	const char *name;
	snd_kcontrol_info_t *info;
	snd_kcontrol_get_t *get;
	snd_kcontrol_put_t *put;
	unsigned long private_value;
};

struct snd_kcontrol {
	struct snd_ctl_elem_id id;
	unsigned int iface, access, count;
	snd_kcontrol_info_t *info;
	snd_kcontrol_get_t *get;
	snd_kcontrol_put_t *put;
	unsigned long private_value;
	void *private_data;
};

#define snd_kcontrol_chip(control) ((control)->private_data)

static inline unsigned int snd_ctl_get_ioff(struct snd_kcontrol *control,
					    struct snd_ctl_elem_id *id)
{
	unsigned int offset = id->numid ? id->numid - control->id.numid :
		id->index - control->id.index;

	assert(offset < control->count);
	return offset;
}

static inline int snd_ctl_boolean_mono_info(struct snd_kcontrol *control,
					    struct snd_ctl_elem_info *info)
{
	(void)control;
	info->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = 1;
	return 0;
}

struct snd_kcontrol *snd_ctl_new1(const struct snd_kcontrol_new *control,
				  void *private_data);
int snd_ctl_add(struct snd_card *card, struct snd_kcontrol *control);
struct snd_kcontrol *snd_ctl_find_id(struct snd_card *card, struct snd_ctl_elem_id *id);
void snd_ctl_notify(struct snd_card *card, unsigned int mask,
		    struct snd_ctl_elem_id *id);
int snd_ctl_activate_id(struct snd_card *card, struct snd_ctl_elem_id *id,
			int active);

static inline struct snd_ctl_elem_id *snd_ctl_build_ioff(struct snd_ctl_elem_id *id,
							struct snd_kcontrol *control,
							unsigned int offset)
{
	assert(offset < control->count);
	*id = control->id;
	id->index += offset;
	id->numid += offset;
	return id;
}
#endif
