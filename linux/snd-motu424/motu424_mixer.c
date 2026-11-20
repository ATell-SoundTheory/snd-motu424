// SPDX-License-Identifier: GPL-2.0-only
/*
 * MOTU PCI-424 DSP mixer program generator and downloader.
 *
 * Faithful port of MixerState/MixOutput/MixRouting, the Instruction class
 * family, Scheduler/ExecutePacket and ND424::DownloadMixer from the MOTU
 * macOS kext.  Hex numbers in comments are function addresses in that
 * binary.  The generated program is TI C62x (C6205) machine code that the
 * DSP firmware runs once per sample frame; see docs/MIXER.md.
 */
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>

#include "motu424_mixer.h"

/* ---------------------------------------------------------------------- */
/* Instructions                                                            */

enum m424_kind {
	K_DUMMY, K_LOAD_OUTPUT, K_SHIFT, K_COMBINE, K_ZERO, K_BUMP,
	K_LOAD_INPUT, K_FADE, K_ADD, K_SATURATE, K_STORE, K_SHIFT_RIGHT,
	K_SHIFT_LEFT, K_METER_STORE,
};

/* "Resources": low nibble = functional unit (L/S/D/M), upper = cross path etc. */
#define R_LOAD_OUTPUT	0x04
#define R_SHIFT		0x02
#define R_COMBINE	0xc1
#define R_ZERO		0x02
#define R_BUMP		0x81
#define R_LOAD_INPUT	0x04
#define R_FADE		0x48
#define R_SATURATE	0x11
#define R_STORE		0x14
#define R_SHIFT_RIGHT	0x32
#define R_SHIFT_LEFT	0x22
#define R_METER_STORE	0x14
#define R_D_UNIT	0x04

#define M424_MAX_DEPS	4

struct m424_ins;

struct m424_dep {
	struct m424_ins *ins;
	int lat;
};

struct m424_ins {
	u8 kind;
	u32 res;			/* +0x08 */
	s8 nregs;			/* +0x0c */
	u8 ndeps;
	struct m424_dep deps[M424_MAX_DEPS];	/* +0x10 vector */
	struct m424_ins *src;		/* +0x28 */
	bool scheduled;			/* +0x30 */
	int cycle;			/* +0x34 */
	s8 reg;				/* +0x38 */
	struct m424_ins *next;		/* +0x40 */
	/* subclass fields (+0x48...) */
	int i48;	/* LoadOutput/Shift type, Zero offset, Fade in, Store inc, MeterStore idx */
	u8 b48, b49;	/* LoadInput base/offset, Add x/y, Store c, ShiftRight long */
	int side_a, side_b;	/* Store +0x4c/+0x50 */
	struct m424_ins *a;	/* Add +0x50: accumulator */
};

/* Instruction::Instruction(Resources) @0x20480 */
static void ins_init(struct m424_ins *i, u8 kind, u32 res)
{
	memset(i, 0, sizeof(*i));
	i->kind = kind;
	i->res = res;
	i->reg = -1;
}

/* Instruction::Reset @0x20520 (does not touch res or subclass fields) */
static void ins_reset(struct m424_ins *i)
{
	i->nregs = 0;
	i->ndeps = 0;
	i->src = NULL;
	i->scheduled = false;
	i->reg = -1;
	i->next = NULL;
}

/* DependencyList::push_back @0x21390 */
static void dep_add(struct m424_ins *i, struct m424_ins *d, int lat)
{
	if (WARN_ON(i->ndeps >= M424_MAX_DEPS))
		return;
	i->deps[i->ndeps].ins = d;
	i->deps[i->ndeps].lat = lat;
	i->ndeps++;
}

/* Instruction::CanSchedule @0x20550 */
static bool ins_can_schedule(const struct m424_ins *i, int cycle)
{
	int k;

	for (k = 0; k < i->ndeps; k++) {
		const struct m424_dep *d = &i->deps[k];

		if (!d->ins->scheduled || d->ins->cycle + d->lat > cycle)
			return false;
	}
	return true;
}

/* Instruction::Schedule @0x221d0 */
static void ins_schedule(struct m424_ins *i, int cycle)
{
	i->scheduled = true;
	i->cycle = cycle;
	i->ndeps = 0;
}

/* LoadInputInstruction::SetInputOffset @0x209c0 */
static void load_input_set_offset(struct m424_ins *i, int idx)
{
	if (idx <= 62) {
		i->b48 = 0xc;
		i->b49 = (u8)(idx - 31);
	} else if (idx <= 125) {
		i->b48 = 0xd;
		i->b49 = (u8)(idx - 94);
	} else {
		i->b48 = 0xe;
		i->b49 = (u8)(idx + 99);	/* == idx - 157 as s8 */
	}
}

/* AddInstruction::SetSourceInstructions @0x22130 */
static void add_set_src(struct m424_ins *i, struct m424_ins *acc,
			struct m424_ins *in, bool x, bool y)
{
	i->res = y ? 0x01 : (x ? 0x21 : 0x31);
	i->nregs = -in->nregs;
	i->src = in;
	i->a = acc;
	i->b48 = x;
	i->b49 = y;
}

/* StoreInstruction::Configure(Side, Side, int, int) @0x20ba0 */
static void store_configure(struct m424_ins *i, int side_a, int side_b,
			    int c, int inc)
{
	i->side_a = side_a;
	i->side_b = side_b;
	i->b49 = (u8)c;
	i->i48 = (u8)inc;
}

/* ShiftRightInstruction::SetLong */
static void shift_right_set_long(struct m424_ins *i, bool lng)
{
	i->b48 = lng;
	i->res = lng ? R_SHIFT_RIGHT : 0x02;
}

static inline u32 sreg(const struct m424_ins *i)
{
	return (u32)(int)i->reg;
}

/* <Class>::GenerateOpcodes (0x21dc0..0x22370): returns A-side and B-side word */
static void ins_opcodes(const struct m424_ins *i, u32 *wa, u32 *wb)
{
	u32 r = i->src ? sreg(i->src) : 0, base, v, p, k;
	int t, off;

	switch (i->kind) {
	case K_SHIFT_LEFT:
		base = r << 23 | r << 18;
		*wa = base | 0x104a1;
		*wb = base | 0x104a3;
		break;
	case K_SHIFT_RIGHT:
		base = r << 23 | r << 18 | 0x10000 | (i->b48 ? 0xd20 : 0xda0);
		*wa = base | 1;
		*wb = base | 3;
		break;
	case K_SATURATE:
		base = r << 23 | r << 18;
		*wa = base | 0x819;
		*wb = base | 0x81b;
		break;
	case K_FADE:
		if (i->i48) {
			base = r << 23 | r << 13 | 0x20000000;
			*wa = base | 0x1181;
			*wb = base | 0x183;
		} else {
			base = r << 23 | r << 13;
			*wa = base | 0x1581;
			*wb = base | 0x583;
		}
		break;
	case K_BUMP:
		*wa = 0x00802079;
		*wb = 0x0080307b;
		break;
	case K_ADD: {
		u32 a = sreg(i->a), op;

		op = i->b49 ? 0x278 : (i->b48 ? 0x478 : 0x438);
		base = a << 18 | a << 23 | r << 13 | op;
		*wa = base | 1;
		*wb = base | 3;
		break;
	}
	case K_COMBINE:
		*wa = r << 23 | r << 18 | r << 13 | 0x1ff9;
		*wb = r << 23 | (r + 1) << 18 | (r + 1) << 13 | 0x1ffb;
		break;
	case K_SHIFT:
		t = i->i48;
		v = r + (t == 1);
		*wa = v << 23 | v << 18 | 0x16ce1;
		v = r + (t == 0);
		*wb = v << 23 | v << 18 | 0x169e3;
		break;
	case K_METER_STORE:
		off = (s8)i->i48;
		p = (off >= 0) << 9;
		k = off < 0 ? -off : off;
		base = r << 23 | 0x380000 | k << 13 | p;
		*wa = base | 0x75;
		*wb = base | 0xf7;
		break;
	case K_STORE:
		base = (u32)i->i48 << 13;
		*wa = r << 23 | base | 0x3c1675 | (i->side_a == 1) << 1;
		*wb = (r + i->b49) << 23 | base | 0x3c16f5 | (i->side_b == 1) << 1;
		break;
	case K_LOAD_INPUT:
		off = (s8)i->b49;
		p = (off >= 0) << 9;
		k = off < 0 ? -off : off;
		base = (u32)i->b48 << 18 | sreg(i) << 23 | k << 13 | p;
		*wa = base | 0x64 | 1;
		*wb = base | 0xe4 | 3;
		break;
	case K_LOAD_OUTPUT:
		if (i->i48 == 0) {
			*wa = sreg(i) << 23 | 0x40265;
			*wb = (sreg(i) + 1) << 23 | 0x422e7;
		} else {
			*wa = r << 23 | 0x42267;
			*wb = (r + 1) << 23 | 0x402e5;
		}
		break;
	case K_ZERO:
		v = (i->nregs ? sreg(i) : r) + i->i48;
		base = v << 23 | v << 18 | v << 13 | 0x5e0;
		*wa = base | 1;
		*wb = base | 3;
		break;
	default:
		*wa = 0;
		*wb = 0;
		break;
	}
}

/* ---------------------------------------------------------------------- */
/* Per-output / per-routing instruction storage (embedded in the objects  */
/* in the original; kept in a separate scratch area here).                */

struct m424_out_ins {
	struct m424_ins zero;		/* MixOutput +0x020 */
	struct m424_ins load[2];	/* +0x070/+0x0c0 */
	struct m424_ins shift[2];	/* +0x110/+0x160 */
	struct m424_ins comb;		/* +0x1b0 */
	struct m424_ins bump;		/* +0x1f8 */
	struct m424_ins sr[2];		/* +0x240/+0x290 */
	struct m424_ins ms[2];		/* +0x2e0/+0x330 */
	struct m424_ins sl[2];		/* +0x380/+0x3c8 */
	struct m424_ins sat;		/* +0x410 */
	struct m424_ins zero2;		/* +0x458 */
	struct m424_ins st1;		/* +0x4a8 */
	struct m424_ins st2;		/* +0x500 */
};

struct m424_rt_ins {
	struct m424_ins li;		/* MixRouting +0x20 */
	struct m424_ins fade;		/* +0x70 */
	struct m424_ins add;		/* +0xc0 */
};

#define M424_PKT_MAX	8

struct m424_packet {			/* ExecutePacket */
	u8 count, nops;
	struct m424_ins *ins[M424_PKT_MAX];
};

struct m424_sched {			/* Scheduler, ctor @0x22480 */
	int cycle;			/* +0x00 */
	u32 units;			/* +0x04 */
	int regs_used;			/* +0x08 */
	u32 free_mask;			/* +0x0c */
	int pc;				/* +0x10, bytes */
};

struct motu424_mixer_work {
	struct m424_out_ins o[MOTU424_MIX_PAIRS];
	struct m424_rt_ins r[MOTU424_MIX_ROUTINGS];
	struct m424_ins head;		/* DummyInstruction in GenerateMixer */
	struct m424_ins *tail;		/* MixerState +0x1c560 */
	struct m424_packet pkt[2];
	struct m424_sched sch;
};

size_t motu424_mixer_work_size(void)
{
	return sizeof(struct motu424_mixer_work);
}

/* Equivalent of the constructors of all embedded instruction objects. */
static void work_init(struct motu424_mixer_work *w)
{
	int i;

	for (i = 0; i < MOTU424_MIX_PAIRS; i++) {
		struct m424_out_ins *o = &w->o[i];

		ins_init(&o->zero, K_ZERO, R_ZERO);
		ins_init(&o->load[0], K_LOAD_OUTPUT, R_LOAD_OUTPUT);
		ins_init(&o->load[1], K_LOAD_OUTPUT, R_LOAD_OUTPUT);
		ins_init(&o->shift[0], K_SHIFT, R_SHIFT);
		ins_init(&o->shift[1], K_SHIFT, R_SHIFT);
		ins_init(&o->comb, K_COMBINE, R_COMBINE);
		ins_init(&o->bump, K_BUMP, R_BUMP);
		ins_init(&o->sr[0], K_SHIFT_RIGHT, R_SHIFT_RIGHT);
		ins_init(&o->sr[1], K_SHIFT_RIGHT, R_SHIFT_RIGHT);
		ins_init(&o->ms[0], K_METER_STORE, R_METER_STORE);
		ins_init(&o->ms[1], K_METER_STORE, R_METER_STORE);
		ins_init(&o->sl[0], K_SHIFT_LEFT, R_SHIFT_LEFT);
		ins_init(&o->sl[1], K_SHIFT_LEFT, R_SHIFT_LEFT);
		ins_init(&o->sat, K_SATURATE, R_SATURATE);
		ins_init(&o->zero2, K_ZERO, R_ZERO);
		ins_init(&o->st1, K_STORE, R_STORE);
		ins_init(&o->st2, K_STORE, R_STORE);
	}
	for (i = 0; i < MOTU424_MIX_ROUTINGS; i++) {
		struct m424_rt_ins *r = &w->r[i];

		ins_init(&r->li, K_LOAD_INPUT, R_LOAD_INPUT);
		load_input_set_offset(&r->li, i);	/* MixRouting::SetIndex @0x23d80 */
		ins_init(&r->fade, K_FADE, R_FADE);
		ins_init(&r->add, K_ADD, 0);
	}
	ins_init(&w->head, K_DUMMY, 0);
	w->tail = &w->head;
}

/* MixerState::AppendInstruction @0x23040 */
static void append(struct motu424_mixer_work *w, struct m424_ins *i)
{
	w->tail->next = i;
	w->tail = i;
}

/* MixRouting::ConfigureInstructions @0x23750 */
static void routing_configure(struct motu424_mixer_work *w,
			      const struct motu424_mix_routing *rt,
			      struct m424_rt_ins *ri, struct m424_ins *acc,
			      struct m424_dep *dep, int idx, bool flag)
{
	ins_reset(&ri->li);
	ins_reset(&ri->fade);
	ins_reset(&ri->add);
	append(w, &ri->li);
	ri->li.nregs = 1;
	if (rt->state == MOTU424_RT_ON) {
		dep_add(&ri->add, &ri->li, 5);
	} else {
		append(w, &ri->fade);
		dep_add(&ri->fade, &ri->li, 5);
		ri->fade.src = &ri->li;
		ri->fade.i48 = (rt->state == MOTU424_RT_FADE_IN);
		dep_add(&ri->add, &ri->fade, 2);
	}
	append(w, &ri->add);
	add_set_src(&ri->add, acc, &ri->li, idx == 0, !flag);
	dep_add(&ri->add, dep->ins, dep->lat);
	dep->ins = &ri->add;
	dep->lat = 1;
}

/* MixOutput::ConfigureLoadOrZero @0x23200 */
static struct m424_dep output_load_or_zero(struct motu424_mixer_work *w,
					   const struct motu424_mix_output *mo,
					   struct m424_out_ins *oi,
					   struct m424_ins **dep_ins,
					   struct m424_ins **acc)
{
	struct m424_dep d;

	if (!mo->dma_enabled) {
		int zoff = (mo->nrout > 1 || mo->metered ||
			    mo->alt_type == MOTU424_ALT_SPLIT) ? 2 : 1;

		ins_reset(&oi->zero);
		append(w, &oi->zero);
		oi->zero.nregs = zoff;
		if (*dep_ins)
			dep_add(&oi->zero, *dep_ins, 1);
		*dep_ins = &oi->zero;
		*acc = &oi->zero;
		d.ins = &oi->zero;
		d.lat = 1;
		return d;
	}

	ins_reset(&oi->load[0]);
	ins_reset(&oi->load[1]);
	ins_reset(&oi->shift[0]);
	ins_reset(&oi->shift[1]);
	ins_reset(&oi->comb);
	ins_reset(&oi->bump);

	oi->load[0].nregs = 2;
	append(w, &oi->load[0]);
	append(w, &oi->shift[0]);
	oi->load[0].i48 = 0;
	oi->shift[0].i48 = 0;
	oi->shift[0].src = &oi->load[0];
	if (*dep_ins)
		dep_add(&oi->load[0], *dep_ins, 1);
	dep_add(&oi->shift[0], &oi->load[0], 5);
	dep_add(&oi->comb, &oi->shift[0], 1);

	append(w, &oi->load[1]);
	append(w, &oi->shift[1]);
	oi->load[1].i48 = 1;
	oi->load[1].src = &oi->load[0];
	oi->shift[1].i48 = 1;
	oi->shift[1].src = &oi->load[0];
	if (*dep_ins)
		dep_add(&oi->load[1], *dep_ins, 1);
	dep_add(&oi->shift[1], &oi->load[1], 5);
	dep_add(&oi->comb, &oi->shift[1], 1);
	dep_add(&oi->load[1], &oi->load[0], 0);
	dep_add(&oi->bump, &oi->load[1], 0);

	oi->comb.src = &oi->load[0];
	append(w, &oi->comb);
	append(w, &oi->bump);

	*dep_ins = &oi->bump;
	*acc = &oi->load[0];
	d.ins = &oi->comb;
	d.lat = 1;
	return d;
}

/* MixOutput::MeterOne @0x230b0 */
static void output_meter_one(struct motu424_mixer_work *w,
			     struct m424_out_ins *oi, int i, bool lng,
			     struct m424_ins *src, struct m424_dep *dep)
{
	struct m424_ins *sr = &oi->sr[i], *ms = &oi->ms[i], *sl = &oi->sl[i];

	ins_reset(sr);
	shift_right_set_long(sr, lng);
	append(w, sr);
	sr->src = src;
	dep_add(sr, dep->ins, dep->lat);
	dep->ins = sr;
	dep->lat = 1;

	ins_reset(ms);
	append(w, ms);
	ms->src = src;			/* MeterStoreInstruction::Configure */
	ms->i48 = i + 0x17;
	dep_add(ms, dep->ins, dep->lat);
	dep->ins = ms;
	dep->lat = 0;

	ins_reset(sl);
	append(w, sl);
	sl->src = src;
	dep_add(sl, dep->ins, dep->lat);
	dep->ins = sl;
	dep->lat = 1;
}

/* MixOutput::DoMetering @0x238d0 */
static void output_do_metering(struct motu424_mixer_work *w,
			       const struct motu424_mixer_state *st,
			       const struct motu424_mix_output *mo,
			       struct m424_out_ins *oi, struct m424_ins *acc,
			       struct m424_dep *dep)
{
	int r, idx = 0;

	for (r = mo->first; r >= 0; r = st->rt[r].next)
		if (st->rt[r].flag)
			routing_configure(w, &st->rt[r], &w->r[r], acc, dep,
					  idx++, true);
	output_meter_one(w, oi, 0, idx > 0, acc, dep);
	for (r = mo->first; r >= 0; r = st->rt[r].next)
		if (!st->rt[r].flag)
			routing_configure(w, &st->rt[r], &w->r[r], acc, dep,
					  idx++, true);
	output_meter_one(w, oi, 1, idx > 0, acc, dep);
}

/* MixOutput::ConfigureInstructions @0x239e0 */
static void output_configure(struct motu424_mixer_work *w,
			     const struct motu424_mixer_state *st, int n,
			     struct m424_ins **dep_ins, struct m424_ins **store)
{
	const struct motu424_mix_output *mo = &st->out[n];
	struct m424_out_ins *oi = &w->o[n];
	struct m424_ins *acc = NULL;
	struct m424_dep d;

	if (mo->alt_type == MOTU424_ALT_NONE)
		return;

	d = output_load_or_zero(w, mo, oi, dep_ins, &acc);

	if (mo->metered) {
		output_do_metering(w, st, mo, oi, acc, &d);
	} else {
		int r, idx = 0;

		for (r = mo->first; r >= 0; r = st->rt[r].next)
			routing_configure(w, &st->rt[r], &w->r[r], acc, &d,
					  idx++, mo->nrout > 1);
	}

	if (mo->nrout > 1 || mo->metered) {
		ins_reset(&oi->sat);
		oi->sat.src = acc;
		dep_add(&oi->sat, d.ins, d.lat);
		append(w, &oi->sat);
		d.ins = &oi->sat;
		d.lat = 1;
	}

	if (mo->alt_type != MOTU424_ALT_SPLIT) {
		ins_reset(&oi->st2);
		append(w, &oi->st2);
		oi->st2.nregs = -acc->nregs;
		oi->st2.src = acc;
		store_configure(&oi->st2, 0, 1, 0, mo->store_inc[0]);
		dep_add(&oi->st2, d.ins, d.lat);
		if (*store)
			dep_add(&oi->st2, *store, 1);
		*store = &oi->st2;
		return;
	}

	/* split pair: zero the second register, two stores */
	ins_reset(&oi->zero2);
	append(w, &oi->zero2);
	oi->zero2.src = acc;
	oi->zero2.i48 = 1;		/* ZeroInstruction::SetRegisterOffset */
	dep_add(&oi->zero2, d.ins, d.lat > 0 ? d.lat - 1 : 0);

	ins_reset(&oi->st1);
	append(w, &oi->st1);
	oi->st1.src = acc;
	if (*store)
		dep_add(&oi->st1, *store, 1);
	*store = &oi->st1;

	ins_reset(&oi->st2);
	append(w, &oi->st2);
	oi->st2.nregs = -acc->nregs;
	oi->st2.src = acc;
	if (*store)
		dep_add(&oi->st2, *store, 1);
	*store = &oi->st2;

	dep_add(&oi->st1, d.ins, d.lat);
	dep_add(&oi->st1, &oi->zero2, 1);
	store_configure(&oi->st1, 0, 1, 1, 2);
	store_configure(&oi->st2, 1, 0, 1, mo->store_inc[1]);
}

/* ---------------------------------------------------------------------- */
/* Scheduler                                                              */

/* ExecutePacket::GenerateCode @0x223e0 */
static int packet_emit(const struct m424_packet *p, u32 *out)
{
	int n = 0, k;

	for (k = 0; k < p->count; k++) {
		ins_opcodes(p->ins[k], &out[n], &out[n + 1]);
		n += 2;
	}
	for (k = 0; k < p->nops; k++)
		out[n++] = 1;		/* NOP with p-bit; cleared below */
	if (n)
		out[n - 1] &= ~1u;
	return n;
}

static inline int packet_words(const struct m424_packet *p)
{
	return 2 * p->count + p->nops;
}

/* words left in the current 8-word fetch packet */
static inline int fp_remaining(int pc)
{
	return ((pc & ~31) - pc + 32) >> 2;
}

/* Scheduler::AllocateRegisters @0x224e0 */
static bool sched_alloc(struct m424_sched *s, int n, int *reg)
{
	u32 mask = (1u << n) - 1, m;
	int k, shift;

	for (k = 0; k <= 8; k += n) {
		shift = (n == 1) ? 10 - k : k;
		m = mask << shift;
		if ((m & s->free_mask) == m) {
			*reg = shift;
			s->free_mask &= ~m;
			s->regs_used += n;
			return true;
		}
	}
	return false;
}

/* Scheduler::FreeRegisters @0x22560 */
static void sched_free(struct m424_sched *s, int n, int reg)
{
	s->free_mask |= ((1u << n) - 1) << reg;
	s->regs_used -= n;
}

/* Scheduler::TryToSchedule @0x22780 */
static bool sched_try(struct m424_sched *s, struct m424_ins *i)
{
	int n = i->nregs, reg;

	if (n > 0) {
		if (!sched_alloc(s, n, &reg))
			return false;
		i->reg = (s8)reg;
		ins_schedule(i, s->cycle);
		s->units |= i->res;
		return true;
	}
	ins_schedule(i, s->cycle);
	s->units |= i->res;
	if (n < 0)
		sched_free(s, -n, i->src->reg);
	return true;
}

/* Scheduler::ShouldFinishExecutePacket @0x22580 */
static bool sched_should_finish(const struct m424_sched *s,
				const struct m424_packet *p,
				const struct m424_ins *i)
{
	int k;

	if ((s->units & 0xf) == 0xf)
		return true;
	if (!(s->units & R_D_UNIT))
		return false;
	if (fp_remaining(s->pc) != packet_words(p))
		return false;
	/* finish if one of the next 8 D-unit instructions could go next cycle */
	for (k = 0; k < 8; k++) {
		i = i->next;
		if (!i)
			return false;
		if ((i->res & R_D_UNIT) && ins_can_schedule(i, s->cycle + 1))
			return true;
	}
	return false;
}

/* Scheduler::ScheduleInstructions @0x22850 */
static void sched_packet(struct m424_sched *s, struct m424_ins *head,
			 struct m424_packet *prev, struct m424_packet *cur)
{
	struct m424_ins *pp = head, *i = head->next;
	int fails = 0, rem, size;

	while (i) {
		bool ok = !(s->units & i->res) && ins_can_schedule(i, s->cycle);

		if (ok && i != head->next && !(i->res & R_D_UNIT) &&
		    i->nregs > 0 && i->nregs + s->regs_used > 6)
			ok = false;
		if (ok)
			ok = sched_try(s, i);
		if (!ok) {
			if (++fails > 20)
				break;
			pp = i;
			i = i->next;
			continue;
		}
		if (!WARN_ON(cur->count >= M424_PKT_MAX))
			cur->ins[cur->count++] = i;
		pp->next = i->next;
		if (sched_should_finish(s, cur, i))
			break;
		i = i->next;
	}

	/* never let a packet straddle a fetch packet: pad the previous one */
	rem = fp_remaining(s->pc);
	size = packet_words(cur);
	if (rem < size) {
		if (prev)	/* NULL only for the first packet, where rem == 8 */
			prev->nops += rem;
		s->pc += rem * 4;
	}
	if (size == 0)
		cur->nops += 1;
	s->pc += packet_words(cur) * 4;
	s->cycle++;
	s->units = 0;
}

/* Epilogue appended after the last packet by GenerateMixer @0x24490 */
static const u32 m424_epilogue[3] = { 0x001c00e2, 0x079803e2, 0x00008000 };

/* MixerState::GenerateMixer @0x24490 */
int motu424_mixer_generate_work(const struct motu424_mixer_state *st,
				struct motu424_mixer_work *w,
				u32 *out, size_t out_words)
{
	struct m424_ins *dep_ins = NULL, *store = NULL;
	struct m424_packet *prev, *cur, *tmp;
	size_t pos = 0;
	int total = 0, n;

	work_init(w);
	for (n = 0; n < MOTU424_MIX_PAIRS; n++)
		output_configure(w, st, n, &dep_ins, &store);

	memset(&w->sch, 0, sizeof(w->sch));
	w->sch.cycle = 1;
	w->sch.free_mask = 0x7fc;	/* A2..A10 / B2..B10 */
	memset(w->pkt, 0, sizeof(w->pkt));

	prev = &w->pkt[1];
	cur = &w->pkt[0];
	sched_packet(&w->sch, &w->head, NULL, prev);
	while (w->head.next) {
		sched_packet(&w->sch, &w->head, prev, cur);
		n = packet_words(prev);
		total += n;
		if (pos + n > out_words)
			return -ENOSPC;
		pos += packet_emit(prev, out + pos);
		memset(prev, 0, sizeof(*prev));
		tmp = prev;
		prev = cur;
		cur = tmp;
		if (total > 0x9f4 && w->head.next)
			return -ENOSPC;
	}
	n = packet_words(prev);
	if (pos + n + ARRAY_SIZE(m424_epilogue) > out_words)
		return -ENOSPC;
	pos += packet_emit(prev, out + pos);
	memcpy(out + pos, m424_epilogue, sizeof(m424_epilogue));
	pos += ARRAY_SIZE(m424_epilogue);
	return (int)pos;
}

int motu424_mixer_generate(const struct motu424_mixer_state *st,
			   u32 *out, size_t out_words)
{
	struct motu424_mixer_work *w;
	int ret;

	w = kvmalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	ret = motu424_mixer_generate_work(st, w, out, out_words);
	kvfree(w);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* Mixer state                                                            */

/* MixerState::MixerState @0x23df0 (+ MixOutput/MixRouting ctors) */
void motu424_mixer_init(struct motu424_mixer_state *st)
{
	int i;

	memset(st, 0, sizeof(*st));
	for (i = 0; i < MOTU424_MIX_PAIRS; i++) {
		struct motu424_mix_output *o = &st->out[i];

		o->alt_type = MOTU424_ALT_STEREO;
		o->first = -1;
		/* every 12th pair (24 ch, one interface) skips a slot */
		o->store_inc[0] = ((i + 1) % 12 == 0) ? 3 : 2;
		o->store_inc[1] = ((i + 2) % 12 == 0) ? 3 : 2;
	}
	for (i = 0; i < MOTU424_MIX_ROUTINGS; i++) {
		st->rt[i].state = MOTU424_RT_OFF;
		st->rt[i].output = -1;
		st->rt[i].next = -1;
		st->rt[i].prev = -1;
	}
	st->metered_output = -1;
}

void motu424_mixer_init_default(struct motu424_mixer_state *st,
				unsigned int n_dma_pairs)
{
	unsigned int i;

	motu424_mixer_init(st);
	for (i = 0; i < MOTU424_MIX_PAIRS; i++) {
		/* Drv424::SetupRouting @0x18770 with hw(i) == i */
		motu424_mixer_set_alt_type(st, i, MOTU424_ALT_STEREO);
		motu424_mixer_enable_dma(st, i, i < n_dma_pairs);
	}
}

/* MixerState::EnableDMA @0x23000 */
bool motu424_mixer_enable_dma(struct motu424_mixer_state *st, int pair, bool en)
{
	if (st->out[pair].dma_enabled == en)
		return false;
	st->out[pair].dma_enabled = en;
	return true;
}

/* MixerState::SetAlternateChannelType @0x23020 */
bool motu424_mixer_set_alt_type(struct motu424_mixer_state *st, int pair,
				enum motu424_alt_type type)
{
	if (st->out[pair].alt_type == (u32)type)
		return false;
	st->out[pair].alt_type = type;
	return true;
}

/* MixerState::Connect @0x22de0 -> MixRouting::Connect @0x22aa0 */
void motu424_mixer_connect(struct motu424_mixer_state *st, int routing,
			   int pair, bool flag)
{
	struct motu424_mix_routing *r = &st->rt[routing];
	struct motu424_mix_output *o = &st->out[pair];

	r->state = MOTU424_RT_FADE_IN;
	r->flag = flag;
	r->output = pair;
	o->nrout++;
	r->prev = -1;
	r->next = o->first;		/* MixOutput::SetFirstRouting @0x229f0 */
	o->first = routing;
	if (r->next >= 0)
		st->rt[r->next].prev = routing;
}

/* MixRouting::Disconnect @0x22a00 */
static void routing_unlink(struct motu424_mixer_state *st, int routing)
{
	struct motu424_mix_routing *r = &st->rt[routing];
	struct motu424_mix_output *o = &st->out[r->output];

	r->state = MOTU424_RT_OFF;
	o->nrout--;
	if (r->next >= 0)
		st->rt[r->next].prev = r->prev;
	if (r->prev >= 0)
		st->rt[r->prev].next = r->next;
	else
		o->first = r->next;
	r->output = -1;
	r->next = -1;
	r->prev = -1;
}

/* MixerState::Disconnect @0x22e20 -> MixRouting::FadeOut @0x229e0 */
void motu424_mixer_disconnect(struct motu424_mixer_state *st, int routing)
{
	st->rt[routing].state = MOTU424_RT_FADE_OUT;
}

/* MixerState::CompleteFades @0x22e40 -> MixRouting::CompleteFade @0x22a60 */
void motu424_mixer_complete_fades(struct motu424_mixer_state *st)
{
	int i;

	for (i = 0; i < MOTU424_MIX_ROUTINGS; i++) {
		if (st->rt[i].state == MOTU424_RT_FADE_IN)
			st->rt[i].state = MOTU424_RT_ON;
		else if (st->rt[i].state == MOTU424_RT_FADE_OUT)
			routing_unlink(st, i);
	}
}

/* MixerState::DisableOutputMetering @0x22f90 */
void motu424_mixer_disable_output_metering(struct motu424_mixer_state *st)
{
	if (st->metered_output != -1) {
		st->out[st->metered_output].metered = false;
		st->metered_output = -1;
	}
}

/* MixerState::MeterOutput @0x22fc0 */
void motu424_mixer_meter_output(struct motu424_mixer_state *st, int pair)
{
	motu424_mixer_disable_output_metering(st);
	st->metered_output = pair;
	st->out[pair].metered = true;
}

/* ---------------------------------------------------------------------- */
/* Download                                                               */

static inline u32 dsp_rd(struct motu424_dsp *d, u32 addr)
{
	return d->ops->read(d->ctx, addr);
}

static inline void dsp_wr(struct motu424_dsp *d, u32 addr, u32 val)
{
	d->ops->write(d->ctx, addr, val);
}

static inline void dsp_delay(struct motu424_dsp *d, unsigned int us)
{
	if (d->ops->delay_us)
		d->ops->delay_us(d->ctx, us);
}

/* ND424::DSP::WritePolitely @0x2bbc0: write followed by a 1 us delay */
static void dsp_wr_polite(struct motu424_dsp *d, u32 addr, u32 val)
{
	dsp_wr(d, addr, val);
	dsp_delay(d, 1);
}

/* ND424::DownloadMixerGuts @0x2b5c0: DSP pulls 4 KiB chunks via PCI DMA */
static int download_guts_dma(struct motu424_dsp *d, u32 dsp_addr,
			     const u32 *words, int n)
{
	int done, chunk, k, tries;
	bool busy;

	for (done = 0; done < n; done += MOTU424_MIX_DMA_CHUNK_WORDS) {
		chunk = n - done;
		if (chunk > MOTU424_MIX_DMA_CHUNK_WORDS)
			chunk = MOTU424_MIX_DMA_CHUNK_WORDS;
		for (k = 0; k < chunk; k++)
			d->dma_virt[k] = words[done + k];
		dsp_wr(d, d->cb + MOTU424_CB_DMA_HOST_ADDR, d->dma_bus);
		dsp_wr(d, d->cb + MOTU424_CB_DMA_DSP_ADDR, dsp_addr);
		dsp_wr(d, d->cb + MOTU424_CB_DMA_BYTES, chunk * 4);
		dsp_wr(d, d->cb + MOTU424_CB_DMA_GO, 1);
		busy = chunk != 0;
		for (tries = 0; busy && tries < 10000; tries++) {
			dsp_delay(d, 20);
			busy = dsp_rd(d, d->cb + MOTU424_CB_DMA_BYTES) != 0;
		}
		dsp_wr(d, d->cb + MOTU424_CB_DMA_GO, 0);
		if (busy)
			return -ETIMEDOUT;
		dsp_addr += MOTU424_MIX_DMA_CHUNK_WORDS * 4;
	}
	return 0;
}

/* ND424b::DownloadMixerGuts @0x2dfc0: plain programmed I/O */
static int download_guts_pio(struct motu424_dsp *d, u32 dsp_addr,
			     const u32 *words, int n)
{
	int k;

	for (k = 0; k < n; k++)
		dsp_wr_polite(d, dsp_addr + 4 * k, words[k]);
	return 0;
}

/*
 * ND424::WaitForRoutingFadeToComplete @0x2b540: poll cb+0x108 until non-zero
 * (5 us steps, 50 ms deadline).  Without a delay callback we bound the loop
 * by iteration count instead of time.
 */
int motu424_mixer_wait_routing_fade(struct motu424_dsp *d)
{
	int k;

	for (k = 0; k < 10000; k++) {
		if (dsp_rd(d, d->cb + MOTU424_CB_ROUTE_FADE_DONE))
			return 0;
		dsp_delay(d, 5);
	}
	return -ETIMEDOUT;
}

/* ND424::DownloadMixer @0x2c000 */
int motu424_mixer_download(struct motu424_dsp *d, const u32 *words, int n)
{
	u32 addr, step;
	unsigned int bank;
	int ret;

	if (!words || n <= 0 || n > MOTU424_MIX_BUF_WORDS ||
	    (d->rate_mult != 1 && d->rate_mult != 2 && d->rate_mult != 4) ||
	    d->bank > 1 || (!d->is_424b && !d->dma_virt))
		return -EINVAL;
	bank = 1 - d->bank;
	addr = dsp_rd(d, d->cb + MOTU424_CB_MIXER_BANK0 + 4 * bank);
	if (d->is_424b)
		ret = download_guts_pio(d, addr, words, n);
	else
		ret = download_guts_dma(d, addr, words, n);
	if (ret)
		return ret;

	if (motu424_mixer_wait_routing_fade(d) && d->ops->warn)
		d->ops->warn(d->ctx, "routing fade timed out; proceeding as the original driver does");
	/* 480-sample routing crossfade, scaled by the rate multiplier */
	step = 0x10000 / (d->rate_mult * 480);
	dsp_wr_polite(d, d->cb + MOTU424_CB_ROUTE_FADE_DONE, 0);
	dsp_wr_polite(d, d->cb + MOTU424_CB_MIXER_ADDR, addr);
	dsp_wr_polite(d, d->cb + MOTU424_CB_ROUTE_FADE_STEP,
		      step << 16 | (u16)(-step));
	dsp_wr_polite(d, d->cb + MOTU424_CB_MIXER_SWITCH, 1);
	d->bank = bank;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Faders                                                                 */

/* ND424 boot @0x2d32c..0x2d3cc: reset bank, cache table addresses */
void motu424_dsp_read_tables(struct motu424_dsp *d)
{
	d->bank = 0;
	d->fader_input_tbl = dsp_rd(d, d->cb + MOTU424_CB_FADER_INPUT_TBL);
	d->fader_gain_tbl = dsp_rd(d, d->cb + MOTU424_CB_FADER_GAIN_TBL);
	d->fader_inc_tbl = dsp_rd(d, d->cb + MOTU424_CB_FADER_INC_TBL);
	d->tbl_c8 = dsp_rd(d, d->cb + MOTU424_CB_INPUT_ROUTE_TBL);
	d->trim_tbl = dsp_rd(d, d->cb + MOTU424_CB_TRIM_TBL);
	memset(d->trim_factors, MOTU424_TRIM_UNITY, sizeof(d->trim_factors));
	d->meter_bank = d->meter_input_bank = 0;
	d->meter_fault = false;
}

static int motu424_trim_wait(struct motu424_dsp *d)
{
	int k;

	for (k = 0; k < 10000; k++) {
		if (!dsp_rd(d, d->cb + MOTU424_CB_TRIM_UPDATE))
			return 0;
		dsp_delay(d, 5);
	}
	return -ETIMEDOUT;
}

/* ND424::SetTrimFactors @0x2c4c0: the DSP consumes signed factor deltas. */
int motu424_dsp_set_trim(struct motu424_dsp *d,
			 const u8 factors[MOTU424_TRIM_FACTORS])
{
	unsigned int i;
	int err;

	if (!factors)
		return -EINVAL;
	if (!memcmp(d->trim_factors, factors, sizeof(d->trim_factors)))
		return 0;
	err = motu424_trim_wait(d);
	if (err)
		return err;
	for (i = 0; i < MOTU424_TRIM_FACTORS; i += 2) {
		s16 left = (int)factors[i] - d->trim_factors[i];
		s16 right = (int)factors[i + 1] - d->trim_factors[i + 1];

		dsp_wr_polite(d, d->trim_tbl + 2 * i,
			      (u32)(u16)left << 16 | (u16)right);
	}
	dsp_wr(d, d->cb + MOTU424_CB_TRIM_UPDATE, 0x100);
	memcpy(d->trim_factors, factors, sizeof(d->trim_factors));
	return 0;
}

static int motu424_meter_wait(struct motu424_dsp *d)
{
	int k;

	for (k = 0; k < 10000; k++) {
		if (!dsp_rd(d, d->cb + MOTU424_CB_METER_BUSY))
			return 0;
		dsp_delay(d, 5);
	}
	return -ETIMEDOUT;
}

/* ND424::SwapLevelMeters @0x2caf0; return the bank just retired by the DSP. */
int motu424_dsp_swap_meters(struct motu424_dsp *d, const u8 *inputs,
			    u16 peaks[MOTU424_METER_VALUES])
{
	unsigned int bank = d->meter_bank ^ 1;
	unsigned int input_bank = d->meter_input_bank ^ !!inputs;
	unsigned int i, j;
	u32 addr;
	int err;

	if (d->meter_fault)
		return -EIO;
	if (inputs)
		for (i = 0; i < MOTU424_METER_INPUTS; i++)
			if (inputs[i] >= 96)
				return -EINVAL;
	err = motu424_meter_wait(d);
	if (err)
		return err;
	if (inputs) {
		addr = dsp_rd(d, d->cb + MOTU424_CB_METER_INPUT_BANK0 + 4 * input_bank);
		for (i = 0; i < MOTU424_METER_INPUTS; i += 4) {
			u32 word = 0;

			for (j = 0; j < 4; j++)
				word = (word << 8) | (inputs[i + j] + inputs[i + j] / 24);
			dsp_wr_polite(d, addr + i, word);
		}
	}
	addr = dsp_rd(d, d->cb + MOTU424_CB_METER_BANK0 + 4 * bank);
	for (i = 0; i < MOTU424_METER_VALUES * 2; i += 4)
		dsp_wr_polite(d, addr + i, 0);
	dsp_wr(d, d->cb + MOTU424_CB_METER_SELECT, bank | (input_bank << 1));
	err = motu424_meter_wait(d);
	if (err) {
		d->meter_fault = true;
		if (d->ops->warn)
			d->ops->warn(d->ctx,
				"Q-Mix meter swap timed out; reload before reading meters");
		return err;
	}
	d->meter_bank = bank;
	d->meter_input_bank = input_bank;
	if (peaks) {
		addr = dsp_rd(d, d->cb + MOTU424_CB_METER_BANK0 + 4 * (bank ^ 1));
		for (i = 0; i < MOTU424_METER_VALUES; i += 2) {
			u32 word = dsp_rd(d, addr + 2 * i);

			peaks[i] = word >> 16;
			peaks[i + 1] = word;
		}
	}
	return 0;
}

/* ComputeLeftAndRightGain @0x2a760: pan 0..0x80, 0x40 = centre */
void motu424_compute_lr_gain(u32 gain, u32 trim, u32 pan, u32 *l, u32 *r)
{
	u32 g = gain * trim;
	u64 g2 = (u64)g * g;
	u32 p2 = pan * pan;

	g = (u32)(g2 >> 45);
	*l = (((0x4000 - p2) * g) >> 14) << 16;
	*r = ((((pan << 8) - p2) * g) >> 14) << 16;
}

/* ND424::SetFaderImmediately @0x2c440: L/R gain pair per fader */
void motu424_fader_set_immediately(struct motu424_dsp *d, int fader,
				   u32 gain, u32 trim, u32 pan)
{
	u32 l, r;

	motu424_compute_lr_gain(gain, trim, pan, &l, &r);
	dsp_wr_polite(d, d->fader_gain_tbl + 8 * fader, l);
	dsp_wr_polite(d, d->fader_gain_tbl + 8 * fader + 4, r);
}

int motu424_fader_wait_volume_fade(struct motu424_dsp *d)
{
	int k;

	for (k = 0; k < 10000; k++) {
		if (!dsp_rd(d, d->cb + MOTU424_CB_VOLUME_FADE))
			return 0;
		dsp_delay(d, 5);
	}
	return -ETIMEDOUT;
}

void motu424_fader_clear_fade(struct motu424_dsp *d, int fader)
{
	dsp_wr_polite(d, d->fader_inc_tbl + 4 * fader, 0);
}

/* ND424::SetFaderWithCrossfade @0x2c2d0, with a 240-sample ramp. */
bool motu424_fader_set_fade(struct motu424_dsp *d, int fader,
			    u32 gain, u32 trim, u32 pan)
{
	u32 old_l, old_r, new_l, new_r, adjust_l, adjust_r;
	s16 step_l, step_r;

	motu424_compute_lr_gain(gain, trim, pan, &new_l, &new_r);
	old_l = dsp_rd(d, d->fader_gain_tbl + 8 * fader);
	old_r = dsp_rd(d, d->fader_gain_tbl + 8 * fader + 4);
	if (old_l == new_l && old_r == new_r) {
		motu424_fader_clear_fade(d, fader);
		return false;
	}
	step_l = ((s32)(new_l >> 10) - (s32)(old_l >> 10)) / 240;
	step_r = ((s32)(new_r >> 10) - (s32)(old_r >> 10)) / 240;
	adjust_l = new_l - old_l - (u32)((s32)step_l * 240 * 1024);
	adjust_r = new_r - old_r - (u32)((s32)step_r * 240 * 1024);
	dsp_wr_polite(d, d->fader_inc_tbl + 4 * fader,
		      (u32)(u16)step_l << 16 | (u16)step_r);
	if (adjust_l)
		dsp_wr_polite(d, d->fader_gain_tbl + 8 * fader,
			      old_l + adjust_l);
	if (adjust_r)
		dsp_wr_polite(d, d->fader_gain_tbl + 8 * fader + 4,
			      old_r + adjust_r);
	return true;
}

void motu424_fader_start_volume_fade(struct motu424_dsp *d)
{
	dsp_wr(d, d->cb + MOTU424_CB_VOLUME_FADE, 240);
}

/* Init part of ND424 boot @0x2d3d4: clear table, mark all dirty, write */
void motu424_fader_input_init(struct motu424_dsp *d)
{
	memset(d->fader_input, 0, sizeof(d->fader_input));
	d->fi_dirty_lo = 0;
	d->fi_dirty_hi = 0x2c;
	motu424_fader_write_input_table(d);
}

/* ND424::SetFaderInput @0x2db50: byte f^3 (big-endian packing) = hw + hw/24 */
void motu424_fader_set_input(struct motu424_dsp *d, int fader, int hw_in)
{
	int idx = fader ^ 3, dw = idx >> 2;
	u8 v = (u8)(hw_in + hw_in / 24);

	if (d->fader_input[idx] == v)
		return;
	d->fader_input[idx] = v;
	if (d->fi_dirty_lo < 0) {
		d->fi_dirty_lo = dw;
		d->fi_dirty_hi = dw;
		return;
	}
	if (dw < d->fi_dirty_lo)
		d->fi_dirty_lo = dw;
	if (dw > d->fi_dirty_hi)
		d->fi_dirty_hi = dw;
}

/* ND424::WriteFaderInputTable @0x2c7f0 */
void motu424_fader_write_input_table(struct motu424_dsp *d)
{
	int k;

	if (d->fi_dirty_lo < 0)
		return;
	for (k = d->fi_dirty_lo; k <= d->fi_dirty_hi; k++) {
		const u8 *b = &d->fader_input[4 * k];

		/* host-endian (LE) dword, as the kext does */
		dsp_wr_polite(d, d->fader_input_tbl + 4 * k,
			      b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24);
	}
	d->fi_dirty_lo = -1;	/* only lo is reset by the original */
}
