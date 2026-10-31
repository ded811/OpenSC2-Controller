/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * cal.c: the calibration helpers of <opensc2/cal.h>, from the rules in the protocol documentation
 * (not public yet; docs/protocol.md lists the pages). Pure logic. The sticks, triggers and pad
 * pressure go through the real controller's normalising stage in single-precision floating point,
 * in the same order of operations, fused multiply-adds (fmaf()) where its build fuses them, and
 * float-to-integer conversions as its floating-point unit does them (float_rules.h): the results
 * are the real controller's to the last count. Built with -ffp-contract=off (CMakeLists.txt), so
 * the compiler fuses nothing else.
 */
#include <math.h>
#include <string.h>
#include <opensc2/cal.h>
#include <opensc2/codec.h>
#include "float_rules.h"

#define STICK_MIN_HALF     200   /* counts between an extreme and its end of the rest band */
#define TRIGGER_MIN_SPAN   100   /* counts between A and B */
#define REC_EMPTY_LOW      0x7FFF
#define AXIS_FULL          32767

static int32_t clamp32(int64_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : (int32_t)v;
}

static int32_t abs32(int32_t v)
{
	return v < 0 ? -v : v;
}

/* ---- millivolts ------------------------------------------------------------------------------ */

uint16_t opensc2_cal_counts_to_mv(uint16_t counts, uint16_t full_mv)
{
	uint32_t c = counts > OPENSC2_CAL_ADC_MAX ? OPENSC2_CAL_ADC_MAX : counts;

	return (uint16_t)(c * full_mv / 4096u);
}

uint16_t opensc2_cal_mv_to_counts(uint32_t mv, uint16_t full_mv)
{
	/* the millivolts of a full-scale reading (4095), as the ADC driver converts it */
	uint32_t top = (uint32_t)OPENSC2_CAL_ADC_MAX * full_mv / 4096u;
	float c;

	if (top == 0) {
		return 0;
	}
	c = (float)mv / ((float)top / (float)OPENSC2_CAL_ADC_MAX);
	if (c <= 0.0f) {
		return 0;
	}
	return c < 65535.0f ? (uint16_t)osc2_f32_to_u32(c) : 0xFFFFu;
}

bool opensc2_cal_raw_mode(int16_t mode, uint16_t counts, uint16_t full_mv, uint16_t *out)
{
	if (mode == 1) {
		*out = opensc2_cal_counts_to_mv(counts, full_mv);
		return true;
	}
	if (mode == 2) {
		*out = counts > OPENSC2_CAL_ADC_MAX ? OPENSC2_CAL_ADC_MAX : counts;
		return true;
	}
	return false;
}

/* ---- the keys -------------------------------------------------------------------------------- */

static void axis_pack(const struct opensc2_cal_axis *a, uint8_t *out)
{
	opensc2_put_le16(&out[0], a->min);
	opensc2_put_le16(&out[2], a->max);
	opensc2_put_le16(&out[4], a->rest_low);
	opensc2_put_le16(&out[6], a->rest_high);
}

static void axis_unpack(const uint8_t *in, struct opensc2_cal_axis *a)
{
	a->min = opensc2_get_le16(&in[0]);
	a->max = opensc2_get_le16(&in[2]);
	a->rest_low = opensc2_get_le16(&in[4]);
	a->rest_high = opensc2_get_le16(&in[6]);
}

void opensc2_cal_stick_pack(const struct opensc2_cal_stick *c, uint8_t out[OPENSC2_CAL_STICK_LEN])
{
	opensc2_put_le16(&out[0], c->format);
	axis_pack(&c->x, &out[2]);
	axis_pack(&c->y, &out[10]);
}

void opensc2_cal_stick_unpack(const uint8_t in[OPENSC2_CAL_STICK_LEN], struct opensc2_cal_stick *c)
{
	c->format = opensc2_get_le16(&in[0]);
	axis_unpack(&in[2], &c->x);
	axis_unpack(&in[10], &c->y);
}

void opensc2_cal_trigger_pack(const struct opensc2_cal_trigger *c,
			      uint8_t out[OPENSC2_CAL_TRIGGER_LEN])
{
	opensc2_put_le16(&out[0], c->format);
	opensc2_put_le16(&out[2], c->a_mv);
	opensc2_put_le16(&out[4], c->b_mv);
	out[6] = c->direction;
}

void opensc2_cal_trigger_unpack(const uint8_t in[OPENSC2_CAL_TRIGGER_LEN],
				struct opensc2_cal_trigger *c)
{
	c->format = opensc2_get_le16(&in[0]);
	c->a_mv = opensc2_get_le16(&in[2]);
	c->b_mv = opensc2_get_le16(&in[4]);
	c->direction = in[6];
}

void opensc2_cal_pressure_pack(const struct opensc2_cal_pressure *c,
			       uint8_t out[OPENSC2_CAL_PRESSURE_LEN])
{
	opensc2_put_le16(&out[0], c->format);
	opensc2_put_le16(&out[2], (uint16_t)c->a);
	opensc2_put_le16(&out[4], (uint16_t)c->b);
	opensc2_put_le16(&out[6], c->gain);
}

void opensc2_cal_pressure_unpack(const uint8_t in[OPENSC2_CAL_PRESSURE_LEN],
				 struct opensc2_cal_pressure *c)
{
	c->format = opensc2_get_le16(&in[0]);
	c->a = (int16_t)opensc2_get_le16(&in[2]);
	c->b = (int16_t)opensc2_get_le16(&in[4]);
	c->gain = opensc2_get_le16(&in[6]);
}

static bool analog_format(uint16_t format)
{
	return format == OPENSC2_CAL_FORMAT_ANALOG || format == OPENSC2_CAL_FORMAT_ANALOG_ALT;
}

/* ---- the normalising stage ------------------------------------------------------------------- */

void opensc2_norm_setup(struct opensc2_norm *n, int32_t lo, int32_t hi, float band_pct,
			bool outer_only, float alpha)
{
	n->outer_only = outer_only;
	n->hysteresis = 10.0f;
	n->lo = (float)lo;
	n->hi = (float)hi;
	n->band = band_pct;
	n->alpha = alpha;
	n->smooth = 0.0f;
	n->limit = true;
	n->threshold = 0.0f;
	n->on = false;
	n->grow = true;
}

float opensc2_norm_step(struct opensc2_norm *n, int32_t reading)
{
	float x = (float)reading;
	float s = n->smooth;
	float range, band, level, out, t;
	bool on;

	/* The smoothed copy: the reading itself to start with, and after a copy of exactly 0. */
	s = s != 0.0f ? fmaf(n->alpha, x - s, s) : x;
	if (n->hi == 0.0f) {
		n->lo = s;   /* an end of 0 is "not set": both from the copy */
		n->hi = s;
	}
	n->smooth = s;
	if (n->grow) {
		if (n->hi < s && !n->flip) {
			n->hi = s;
		}
		if (n->lo > s && n->flip) {
			n->lo = s;
		}
	}
	range = n->hi - n->lo;
	if (range == 0.0f) {
		return 0.0f;
	}
	level = n->lo;
	band = n->band * 0.01f * range;
	range = n->outer_only ? range - band : fmaf(-band, 2.0f, range);
	if (!n->outer_only || n->flip) {
		level += band;
	}
	out = (x - level) / range;
	if (n->limit) {
		if (out > 1.0f) {
			out = 1.0f;
		} else if (out < 0.0f) {
			out = 0.0f;
		}
	}
	/* The digital state, from the other end when flipped. */
	on = n->on;
	t = n->threshold;
	if (n->flip) {
		t = 1.0f - t;
		on = !on;
	}
	if (on) {
		if (t - n->hysteresis > out) {
			n->on = n->flip;
		}
	} else if (t + n->hysteresis < out) {
		n->on = !n->flip;
	}
	return n->flip ? 1.0f - out : out;
}

/* ---- sticks ---------------------------------------------------------------------------------- */

#define STICK_BAND   10.0f    /* percent, at the outer end of each half */
#define STICK_ALPHA  0.15f    /* the halves' smoothed copies */

static void stick_axis_setup(struct opensc2_stick_axis *a, const uint16_t c[4], bool grow)
{
	/* c: min, max, rest low, rest high in counts */
	a->centre = (c[3] + c[2]) >> 1;
	opensc2_norm_setup(&a->upper, a->centre, c[1], STICK_BAND, true, STICK_ALPHA);
	a->upper.flip = false;
	a->upper.grow = grow;
	opensc2_norm_setup(&a->lower, c[0], a->centre, STICK_BAND, true, STICK_ALPHA);
	a->lower.flip = true;
	a->lower.grow = grow;
}

void opensc2_stick_setup(struct opensc2_stick_proc *p, const struct opensc2_cal_stick *cal)
{
	const struct opensc2_cal_axis *axes[2];
	uint16_t c[2][4];

	p->valid = false;
	memset(p->axis, 0, sizeof(p->axis));
	if (!cal || !analog_format(cal->format)) {
		return;
	}
	axes[0] = &cal->x;
	axes[1] = &cal->y;
	for (int i = 0; i < 2; i++) {
		c[i][0] = opensc2_cal_mv_to_counts(axes[i]->min, OPENSC2_CAL_STICK_FULL_MV);
		c[i][1] = opensc2_cal_mv_to_counts(axes[i]->max, OPENSC2_CAL_STICK_FULL_MV);
		c[i][2] = opensc2_cal_mv_to_counts(axes[i]->rest_low, OPENSC2_CAL_STICK_FULL_MV);
		c[i][3] = opensc2_cal_mv_to_counts(axes[i]->rest_high, OPENSC2_CAL_STICK_FULL_MV);
		if (abs32((int32_t)c[i][1] - c[i][3]) < STICK_MIN_HALF ||
		    abs32((int32_t)c[i][2] - c[i][0]) < STICK_MIN_HALF) {
			return;
		}
	}
	for (int i = 0; i < 2; i++) {
		stick_axis_setup(&p->axis[i], c[i], p->grow_range);
	}
	p->valid = true;
}

int16_t opensc2_stick_axis_value(struct opensc2_stick_proc *p, int axis, uint16_t counts)
{
	struct opensc2_stick_axis *a;
	float f;
	int32_t v;

	if (!p->valid || axis < 0 || axis > 1) {
		return 0;
	}
	a = &p->axis[axis];
	f = (int32_t)counts > a->centre ? opensc2_norm_step(&a->upper, counts) :
					   -opensc2_norm_step(&a->lower, counts);
	v = (int16_t)osc2_f32_to_s32(f * (float)AXIS_FULL);

	if (p->dead_zone > 0 && p->dead_zone < AXIS_FULL) {
		int32_t mag = abs32(v);
		int32_t dz = p->dead_zone;

		mag = mag <= dz ? 0 : (int32_t)((int64_t)(mag - dz) * AXIS_FULL / (AXIS_FULL - dz));
		v = v < 0 ? -mag : mag;
	}
	return (int16_t)v;
}

void opensc2_stick_value(struct opensc2_stick_proc *p, uint16_t x_counts, uint16_t y_counts,
			 struct opensc2_stick *out)
{
	out->x = opensc2_stick_axis_value(p, 0, x_counts);
	out->y = opensc2_stick_axis_value(p, 1, y_counts);
}

/* ---- triggers -------------------------------------------------------------------------------- */

#define TRIGGER_BAND        5.0f    /* percent, at both ends */
#define TRIGGER_HYSTERESIS  5.0f    /* percent */
#define TRIGGER_EARLY       0.98f   /* the pulled end moved 2 % toward the released one */
#define TRIGGER_LEVEL       90      /* setting 68's default */

void opensc2_trigger_setup_level(struct opensc2_trigger_proc *p,
				 const struct opensc2_cal_trigger *cal, int16_t setting68)
{
	float a, b;

	memset(p, 0, sizeof(*p));
	if (!cal || !analog_format(cal->format)) {
		return;
	}
	p->a = opensc2_cal_mv_to_counts(cal->a_mv, OPENSC2_CAL_TRIGGER_FULL_MV);
	p->b = opensc2_cal_mv_to_counts(cal->b_mv, OPENSC2_CAL_TRIGGER_FULL_MV);
	p->falls = cal->direction != 0;
	p->valid = abs32(p->a - p->b) >= TRIGGER_MIN_SPAN;
	if (!p->valid) {
		return;
	}
	a = (float)(uint32_t)p->a;
	b = (float)(uint32_t)p->b;
	if (p->falls) {
		b = b / TRIGGER_EARLY;
	} else {
		a = a * TRIGGER_EARLY;
	}
	opensc2_norm_setup(&p->stage, osc2_f32_to_s32(b), osc2_f32_to_s32(a), TRIGGER_BAND, false,
			   1.0f);
	p->stage.grow = false;
	p->stage.flip = p->falls;
	/* setting 68 as an unsigned number, in percent */
	p->stage.threshold = (float)(uint32_t)(int32_t)setting68 / 100.0f;
	p->stage.hysteresis = TRIGGER_HYSTERESIS / 100.0f;
}

void opensc2_trigger_setup(struct opensc2_trigger_proc *p, const struct opensc2_cal_trigger *cal)
{
	opensc2_trigger_setup_level(p, cal, TRIGGER_LEVEL);
}

bool opensc2_trigger_step(struct opensc2_trigger_proc *p, uint16_t counts, uint16_t *value)
{
	if (!p->valid) {
		*value = 0;
		return false;
	}
	*value = (uint16_t)osc2_f32_to_u32(opensc2_norm_step(&p->stage, counts) * (float)AXIS_FULL);
	return p->stage.on;
}

uint16_t opensc2_trigger_value(const struct opensc2_trigger_proc *p, uint16_t counts)
{
	struct opensc2_trigger_proc copy = *p;
	uint16_t v;

	(void)opensc2_trigger_step(&copy, counts, &v);
	return v;
}

/* ---- pad pressure ---------------------------------------------------------------------------- */

#define PRESSURE_BAND        5.0f   /* percent, at both ends */
#define PRESSURE_HYSTERESIS  8.0f   /* percent */
#define PRESSURE_UNSET       INT16_MIN

/* The stage over the calibration's range with the click level of setting 52 or 53. */
static void pressure_stage(struct opensc2_pressure_proc *p, int16_t click_setting)
{
	opensc2_norm_setup(&p->stage, p->lo, p->hi, PRESSURE_BAND, false, 1.0f);
	p->stage.threshold = (click_setting >= 0 ? (float)click_setting : 100.0f) / 100.0f;
	p->stage.hysteresis = PRESSURE_HYSTERESIS / 100.0f;
	p->stage.limit = false;
	p->stage.grow = false;
	p->click = false;
}

void opensc2_pressure_setup(struct opensc2_pressure_proc *p, const struct opensc2_cal_pressure *cal)
{
	p->usable = false;
	p->click = false;
	p->last = 0;
	p->click_setting = PRESSURE_UNSET;   /* the next sample sets the stage up */
	if (!cal || cal->gain == 0) {
		return;
	}
	if (cal->format == OPENSC2_CAL_FORMAT_PRESSURE) {
		if (cal->a == 0x7FFF) {
			return;
		}
		p->lo = cal->a;
		p->hi = cal->b;
	} else if (cal->format == OPENSC2_CAL_FORMAT_PRESSURE_SPAN) {
		p->lo = 0;
		p->hi = (int16_t)(uint16_t)((uint16_t)cal->b - (uint16_t)cal->a);
	} else {
		return;
	}
	p->gain = cal->gain;
	p->scale = (float)(uint32_t)cal->gain / 2000.0f;
	p->usable = true;
}

static uint16_t pressure_of(const struct opensc2_pressure_proc *p, float v)
{
	return (uint16_t)clamp32(osc2_f32_to_s32(p->scale * 32767.0f * v), 0, OPENSC2_TRIGGER_MAX);
}

void opensc2_pressure_sample(struct opensc2_pressure_proc *p, int16_t raw, int16_t click_setting)
{
	if (click_setting != p->click_setting) {
		p->click_setting = click_setting;
		if (p->usable) {
			pressure_stage(p, click_setting);
		}
		p->click = false;
	}
	if (!p->usable) {
		/* p only follows the raw words while the calibration is usable */
		p->click = false;
		p->last = 0;
		return;
	}
	p->filtered = (int16_t)((3 * (int32_t)p->filtered + raw) / 4);
	p->last = pressure_of(p, opensc2_norm_step(&p->stage, p->filtered));
	p->click = p->stage.on;
}

uint16_t opensc2_pressure_value(const struct opensc2_pressure_proc *p)
{
	struct opensc2_norm copy;

	if (!p->usable) {
		return 0;
	}
	copy = p->stage;
	if (p->click_setting == PRESSURE_UNSET) {
		/* not sampled since the set-up: the stage as the next sample would have it */
		struct opensc2_pressure_proc q = *p;

		pressure_stage(&q, 40);
		copy = q.stage;
	}
	return pressure_of(p, opensc2_norm_step(&copy, p->filtered));
}

uint16_t opensc2_pressure_raw_value(int16_t raw)
{
	return raw < 0 ? 0 : (uint16_t)raw;
}

/* ---- the factory recordings ------------------------------------------------------------------ */

void opensc2_cal_rec_init(struct opensc2_cal_rec *r)
{
	memset(r, 0, sizeof(*r));
}

static void rec_start(struct opensc2_cal_rec *r, enum opensc2_cal_rec_kind kind)
{
	for (int side = 0; side < 2; side++) {
		switch (kind) {
		case OPENSC2_CAL_REC_STICKS: {
			struct opensc2_cal_axis empty = {
				.min = REC_EMPTY_LOW, .max = 0,
				.rest_low = REC_EMPTY_LOW, .rest_high = 0,
			};

			r->stick[side].format = OPENSC2_CAL_FORMAT_ANALOG;
			r->stick[side].x = empty;
			r->stick[side].y = empty;
			break;
		}
		case OPENSC2_CAL_REC_TRIGGERS:
			for (int dir = 0; dir < 2; dir++) {
				r->trigger[side][dir] = (struct opensc2_cal_trigger){
					.format = OPENSC2_CAL_FORMAT_ANALOG,
					.a_mv = 0,
					.b_mv = REC_EMPTY_LOW,
					.direction = (uint8_t)dir,
				};
			}
			break;
		case OPENSC2_CAL_REC_PRESSURE:
			r->pressure[side] = (struct opensc2_cal_pressure){
				.format = OPENSC2_CAL_FORMAT_PRESSURE,
				.a = 0,
				.b = 0,
				.gain = OPENSC2_CAL_PRESSURE_GAIN_ONE,
			};
			break;
		}
	}
	r->active[kind] = true;
	r->seen[kind] = 0;
}

static uint32_t save_bits(enum opensc2_cal_rec_kind kind, uint8_t sides)
{
	static const uint32_t left[3] = { OPENSC2_CAL_SAVE_STICK_L, OPENSC2_CAL_SAVE_TRIGGER_L,
					  OPENSC2_CAL_SAVE_PRESSURE_L };
	static const uint32_t right[3] = { OPENSC2_CAL_SAVE_STICK_R, OPENSC2_CAL_SAVE_TRIGGER_R,
					   OPENSC2_CAL_SAVE_PRESSURE_R };

	return ((sides & 1) ? left[kind] : 0) | ((sides & 2) ? right[kind] : 0);
}

/* The sides a finished recording saves: those the board has, or that got samples. */
static uint32_t rec_sides(const struct opensc2_cal_rec *r, enum opensc2_cal_rec_kind kind)
{
	return save_bits(kind, (uint8_t)(r->seen[kind] | r->supplied[kind]));
}

uint32_t opensc2_cal_rec_step(struct opensc2_cal_rec *r, enum opensc2_cal_rec_kind kind,
			      uint8_t step)
{
	if (kind > OPENSC2_CAL_REC_PRESSURE || step > 3) {
		return 0;   /* the command still succeeds */
	}
	r->step[kind] = step;
	if (step == 1) {
		rec_start(r, kind);
		return 0;
	}
	if (kind == OPENSC2_CAL_REC_PRESSURE && step == 3) {
		/* Unhooked, saved and loaded again inside the command, hooked or not: the records
		 * as they stand (zeros when no step 1 came since boot). */
		r->active[kind] = false;
		r->step[kind] = 0;
		return rec_sides(r, kind);
	}
	/* Sticks and triggers: 2 records further, 3 saves at the next frame, 0 unhooks there. The
	 * pressure: 2 records, 0 pauses. */
	return 0;
}

static uint16_t min16(uint16_t a, uint16_t b)
{
	return a < b ? a : b;
}

static uint16_t max16(uint16_t a, uint16_t b)
{
	return a > b ? a : b;
}

static void widen_rest(struct opensc2_cal_axis *a, uint16_t mv)
{
	a->rest_low = min16(a->rest_low, mv);
	a->rest_high = max16(a->rest_high, mv);
}

static void widen_range(struct opensc2_cal_axis *a, uint16_t mv)
{
	a->min = min16(a->min, mv);
	a->max = max16(a->max, mv);
}

/* A hooked stick or trigger recording at a frame: 3 hands the records over and unhooks, 0
 * unhooks. True when it is to take this frame's samples. */
static bool rec_frame_step(struct opensc2_cal_rec *r, enum opensc2_cal_rec_kind kind,
			   uint32_t *bits)
{
	uint8_t step = r->step[kind];

	if (!r->active[kind]) {
		return false;
	}
	if (step == 3) {
		*bits |= rec_sides(r, kind);
		r->step[kind] = 0;
		r->active[kind] = false;
		return false;
	}
	if (step == 0) {
		r->active[kind] = false;
		return false;
	}
	return true;
}

void opensc2_cal_rec_pressure_sample(struct opensc2_cal_rec *r, enum opensc2_side side, int16_t raw)
{
	int s = side & 1;

	r->supplied[OPENSC2_CAL_REC_PRESSURE] |= (uint8_t)(1u << s);
	if (!r->active[OPENSC2_CAL_REC_PRESSURE]) {
		return;
	}
	r->seen[OPENSC2_CAL_REC_PRESSURE] |= (uint8_t)(1u << s);
	if (r->step[OPENSC2_CAL_REC_PRESSURE] == 2 && (raw < 0 ? 0 : raw) > r->pressure[s].b) {
		r->pressure[s].b = raw < 0 ? 0 : raw;
	}
}

uint32_t opensc2_cal_rec_frame(struct opensc2_cal_rec *r, const uint16_t stick_mv[2][2],
			       uint8_t stick_sides, const uint16_t trigger_mv[2],
			       uint8_t trigger_sides, const int16_t pressure_raw[2],
			       uint8_t pressure_sides)
{
	uint32_t bits = 0;

	r->supplied[OPENSC2_CAL_REC_STICKS] |= (uint8_t)(stick_mv ? stick_sides & 3 : 0);
	r->supplied[OPENSC2_CAL_REC_TRIGGERS] |= (uint8_t)(trigger_mv ? trigger_sides & 3 : 0);

	if (rec_frame_step(r, OPENSC2_CAL_REC_STICKS, &bits) && stick_mv) {
		uint8_t step = r->step[OPENSC2_CAL_REC_STICKS];

		for (int side = 0; side < 2; side++) {
			struct opensc2_cal_stick *s = &r->stick[side];

			if (!(stick_sides & (1u << side))) {
				continue;
			}
			if (step == 1) {
				widen_rest(&s->x, stick_mv[side][0]);
				widen_rest(&s->y, stick_mv[side][1]);
			} else {
				widen_range(&s->x, stick_mv[side][0]);
				widen_range(&s->y, stick_mv[side][1]);
			}
			r->seen[OPENSC2_CAL_REC_STICKS] |= (uint8_t)(1u << side);
		}
	}

	if (rec_frame_step(r, OPENSC2_CAL_REC_TRIGGERS, &bits) && trigger_mv) {
		uint8_t step = r->step[OPENSC2_CAL_REC_TRIGGERS];

		for (int side = 0; side < 2; side++) {
			struct opensc2_cal_trigger *rise = &r->trigger[side][0];
			struct opensc2_cal_trigger *fall = &r->trigger[side][1];
			uint16_t mv = trigger_mv[side];

			if (!(trigger_sides & (1u << side))) {
				continue;
			}
			if (step == 1) {   /* released */
				rise->b_mv = min16(rise->b_mv, mv);
				fall->a_mv = max16(fall->a_mv, mv);
			} else {           /* pulled */
				rise->a_mv = max16(rise->a_mv, mv);
				fall->b_mv = min16(fall->b_mv, mv);
			}
			r->seen[OPENSC2_CAL_REC_TRIGGERS] |= (uint8_t)(1u << side);
		}
	}

	for (int side = 0; pressure_raw && side < 2; side++) {
		if (pressure_sides & (1u << side)) {
			opensc2_cal_rec_pressure_sample(r, (enum opensc2_side)side, pressure_raw[side]);
		}
	}
	return bits;
}

void opensc2_cal_rec_stick(const struct opensc2_cal_rec *r, enum opensc2_side side,
			   struct opensc2_cal_stick *out)
{
	*out = r->stick[side & 1];
}

void opensc2_cal_rec_trigger(const struct opensc2_cal_rec *r, enum opensc2_side side,
			     struct opensc2_cal_trigger *out)
{
	const struct opensc2_cal_trigger *rise = &r->trigger[side & 1][0];
	const struct opensc2_cal_trigger *fall = &r->trigger[side & 1][1];
	int32_t span_rise = abs32((int32_t)rise->a_mv - rise->b_mv);
	int32_t span_fall = abs32((int32_t)fall->a_mv - fall->b_mv);

	/* The candidate whose A and B lie further apart; direction 0 on a tie. */
	*out = span_rise >= span_fall ? *rise : *fall;
}

void opensc2_cal_rec_pressure(const struct opensc2_cal_rec *r, enum opensc2_side side,
			      struct opensc2_cal_pressure *out)
{
	*out = r->pressure[side & 1];
}
