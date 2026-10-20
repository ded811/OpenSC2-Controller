/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * cal.c: the calibration helpers of <opensc2/cal.h>, from the rules in the protocol documentation
 * (not public yet; docs/protocol.md lists the pages). Pure logic in integers: the real controller's
 * factors (0.9, 0.98, 0.05) are applied as exact fractions, so a value that lands exactly on a
 * whole number is not off by one the way a float can be.
 */
#include <string.h>
#include <opensc2/cal.h>
#include <opensc2/codec.h>

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
	uint64_t c;

	if (full_mv < 2) {
		return 0;
	}
	c = (uint64_t)mv * OPENSC2_CAL_ADC_MAX / (full_mv - 1u);
	return (uint16_t)(c > 0xFFFFu ? 0xFFFFu : c);
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

/* ---- sticks ---------------------------------------------------------------------------------- */

void opensc2_stick_setup(struct opensc2_stick_proc *p, const struct opensc2_cal_stick *cal)
{
	const struct opensc2_cal_axis *axes[2];

	p->valid = false;
	memset(p->axis, 0, sizeof(p->axis));
	if (!cal || !analog_format(cal->format)) {
		return;
	}
	axes[0] = &cal->x;
	axes[1] = &cal->y;
	for (int i = 0; i < 2; i++) {
		int32_t min = opensc2_cal_mv_to_counts(axes[i]->min, OPENSC2_CAL_STICK_FULL_MV);
		int32_t max = opensc2_cal_mv_to_counts(axes[i]->max, OPENSC2_CAL_STICK_FULL_MV);
		int32_t low = opensc2_cal_mv_to_counts(axes[i]->rest_low, OPENSC2_CAL_STICK_FULL_MV);
		int32_t high = opensc2_cal_mv_to_counts(axes[i]->rest_high, OPENSC2_CAL_STICK_FULL_MV);

		if (abs32(max - high) < STICK_MIN_HALF || abs32(min - low) < STICK_MIN_HALF) {
			memset(p->axis, 0, sizeof(p->axis));
			return;
		}
		p->axis[i].centre = (low + high) / 2;   /* both are positive: rounded down */
		p->axis[i].min = min;
		p->axis[i].max = max;
	}
	p->valid = true;
}

/* July 2026's run-time range: a low-pass copy of the reading (factor 0.15, counts x 256) pushes a
 * half's outer end out when it goes past it. Only for an axis that is not turned over. */
static void follow_range(struct opensc2_stick_axis *a, int32_t r)
{
	int32_t copy;

	if (!a->follow_on) {
		a->follow = r * 256;
		a->follow_on = true;
	} else {
		a->follow += (r * 256 - a->follow) * 15 / 100;
	}
	copy = a->follow / 256;
	if (a->max > a->centre && copy > a->max) {
		a->max = copy;
	}
	if (a->min < a->centre && copy < a->min) {
		a->min = copy;
	}
}

int16_t opensc2_stick_axis_value(struct opensc2_stick_proc *p, int axis, uint16_t counts)
{
	struct opensc2_stick_axis *a;
	int32_t r = counts, v;
	int64_t num, den;

	if (!p->valid || axis < 0 || axis > 1) {
		return 0;
	}
	a = &p->axis[axis];
	if (p->grow_range) {
		follow_range(a, r);
	}
	/* 32767 x (r - c) / (0.9 x (end - c)), each half on its own, toward zero. */
	if (r > a->centre) {
		num = (int64_t)AXIS_FULL * 10 * (r - a->centre);
		den = (int64_t)9 * (a->max - a->centre);
	} else {
		num = -(int64_t)AXIS_FULL * 10 * (a->centre - r);
		den = (int64_t)9 * (a->centre - a->min);
	}
	v = den == 0 ? 0 : clamp32(num / den, -AXIS_FULL, AXIS_FULL);

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

void opensc2_trigger_setup(struct opensc2_trigger_proc *p, const struct opensc2_cal_trigger *cal)
{
	memset(p, 0, sizeof(*p));
	if (!cal || !analog_format(cal->format)) {
		return;
	}
	p->a = opensc2_cal_mv_to_counts(cal->a_mv, OPENSC2_CAL_TRIGGER_FULL_MV);
	p->b = opensc2_cal_mv_to_counts(cal->b_mv, OPENSC2_CAL_TRIGGER_FULL_MV);
	p->falls = cal->direction != 0;
	p->valid = abs32(p->a - p->b) >= TRIGGER_MIN_SPAN;
}

uint16_t opensc2_trigger_value(const struct opensc2_trigger_proc *p, uint16_t counts)
{
	int64_t num, den, out;
	int32_t r = counts;

	if (!p->valid) {
		return 0;
	}
	/* d = 100 (r - B) / (98 A - 100 B) rising, 98 (A - r) / (98 A - 100 B) falling: the pulled
	 * end moved 2 % toward the released one (A x 0.98, or B / 0.98). */
	den = (int64_t)98 * p->a - (int64_t)100 * p->b;
	if (den == 0) {
		return 0;
	}
	num = p->falls ? (int64_t)98 * (p->a - r) : (int64_t)100 * (r - p->b);
	/* 32767 x (d - 0.05) / 0.9 = 32767 x (20 d - 1) / 18 */
	out = (int64_t)AXIS_FULL * (20 * num - den) / (18 * den);
	return (uint16_t)clamp32(out, 0, OPENSC2_TRIGGER_MAX);
}

/* ---- pad pressure ---------------------------------------------------------------------------- */

void opensc2_pressure_setup(struct opensc2_pressure_proc *p, const struct opensc2_cal_pressure *cal)
{
	int32_t lo, hi;

	p->usable = false;
	p->lo = p->range = 0;
	p->gain = 0;
	p->click = false;
	p->click_setting = INT16_MIN;   /* the next sample's setting counts as a change */
	if (!cal || cal->gain == 0) {
		return;
	}
	if (cal->format == OPENSC2_CAL_FORMAT_PRESSURE) {
		if (cal->a == 0x7FFF) {
			return;
		}
		lo = cal->a;
		hi = cal->b;
	} else if (cal->format == OPENSC2_CAL_FORMAT_PRESSURE_SPAN) {
		lo = 0;
		hi = (int32_t)cal->b - cal->a;
	} else {
		return;
	}
	if (hi == lo) {
		return;   /* no range: OpenSC2 reads it as uncalibrated (protocol.md, gaps) */
	}
	p->lo = lo;
	p->range = hi - lo;
	p->gain = cal->gain;
	p->usable = true;
}

/* v = (p - lo - 0.05 R) / (0.9 R) = (20 (p - lo) - R) / (18 R), with a positive denominator. */
static void pressure_fraction(const struct opensc2_pressure_proc *p, int64_t *num, int64_t *den)
{
	*num = (int64_t)20 * ((int64_t)p->filtered - p->lo) - p->range;
	*den = (int64_t)18 * p->range;
	if (*den < 0) {
		*num = -*num;
		*den = -*den;
	}
}

void opensc2_pressure_sample(struct opensc2_pressure_proc *p, int16_t raw, int16_t click_setting)
{
	int64_t num, den;
	int32_t s;

	if (click_setting != p->click_setting) {
		p->click = false;
		p->click_setting = click_setting;
	}
	if (!p->usable) {
		/* p only follows the raw word while the calibration is usable; without one it is left
		 * as it is, so a calibration that arrives later starts from the p it had */
		p->click = false;
		return;
	}
	p->filtered = (3 * p->filtered + raw) / 4;
	s = click_setting < 0 || click_setting > 100 ? 100 : click_setting;   /* -1 means 100 */
	pressure_fraction(p, &num, &den);
	/* On when v passes (s + 8) %, off below (s - 8) %. */
	if (100 * num > (int64_t)(s + 8) * den) {
		p->click = true;
	} else if (100 * num < (int64_t)(s - 8) * den) {
		p->click = false;
	}
}

uint16_t opensc2_pressure_value(const struct opensc2_pressure_proc *p)
{
	int64_t num, den;

	if (!p->usable) {
		return 0;
	}
	pressure_fraction(p, &num, &den);
	/* 32767 x (C / 2000) x v */
	return (uint16_t)clamp32((int64_t)AXIS_FULL * p->gain * num / (2000 * den), 0,
				 OPENSC2_TRIGGER_MAX);
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
	r->step[kind] = 1;
}

static uint32_t save_bits(enum opensc2_cal_rec_kind kind, uint8_t sides)
{
	static const uint32_t left[3] = { OPENSC2_CAL_SAVE_STICK_L, OPENSC2_CAL_SAVE_TRIGGER_L,
					  OPENSC2_CAL_SAVE_PRESSURE_L };
	static const uint32_t right[3] = { OPENSC2_CAL_SAVE_STICK_R, OPENSC2_CAL_SAVE_TRIGGER_R,
					   OPENSC2_CAL_SAVE_PRESSURE_R };

	return ((sides & 1) ? left[kind] : 0) | ((sides & 2) ? right[kind] : 0);
}

uint32_t opensc2_cal_rec_step(struct opensc2_cal_rec *r, enum opensc2_cal_rec_kind kind,
			      uint8_t step)
{
	if (kind > OPENSC2_CAL_REC_PRESSURE || step > 3) {
		return 0;   /* the command still succeeds */
	}
	if (step == 1) {
		rec_start(r, kind);
		return 0;
	}
	if (kind == OPENSC2_CAL_REC_PRESSURE) {
		if (!r->active[kind]) {
			return 0;
		}
		if (step == 3) {
			/* Saved and loaded again inside the command. */
			r->active[kind] = false;
			r->step[kind] = 0;
			return save_bits(kind, r->seen[kind]);
		}
		r->step[kind] = step;   /* 2 records, 0 pauses */
		return 0;
	}
	if (step == 0) {
		r->active[kind] = false;   /* ends without saving */
		r->step[kind] = 0;
		return 0;
	}
	if (r->active[kind]) {
		r->step[kind] = step;      /* 2, or 3: saved at the next frame */
	}
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

/* Steps 3 of sticks and triggers: the records go at the frame after the command. */
static uint32_t rec_finish(struct opensc2_cal_rec *r, enum opensc2_cal_rec_kind kind)
{
	uint32_t bits = save_bits(kind, r->seen[kind]);

	r->active[kind] = false;
	r->step[kind] = 0;
	return bits;
}

uint32_t opensc2_cal_rec_frame(struct opensc2_cal_rec *r, const uint16_t stick_mv[2][2],
			       uint8_t stick_sides, const uint16_t trigger_mv[2],
			       uint8_t trigger_sides, const int16_t pressure_raw[2],
			       uint8_t pressure_sides)
{
	uint32_t bits = 0;

	if (r->active[OPENSC2_CAL_REC_STICKS]) {
		uint8_t step = r->step[OPENSC2_CAL_REC_STICKS];

		if (step == 3) {
			bits |= rec_finish(r, OPENSC2_CAL_REC_STICKS);
		} else if (stick_mv) {
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
	}

	if (r->active[OPENSC2_CAL_REC_TRIGGERS]) {
		uint8_t step = r->step[OPENSC2_CAL_REC_TRIGGERS];

		if (step == 3) {
			bits |= rec_finish(r, OPENSC2_CAL_REC_TRIGGERS);
		} else if (trigger_mv) {
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
	}

	if (r->active[OPENSC2_CAL_REC_PRESSURE] && pressure_raw) {
		uint8_t step = r->step[OPENSC2_CAL_REC_PRESSURE];

		for (int side = 0; side < 2 && step != 0; side++) {
			int16_t raw = pressure_raw[side] < 0 ? 0 : pressure_raw[side];

			if (!(pressure_sides & (1u << side))) {
				continue;
			}
			if (step == 2 && raw > r->pressure[side].b) {
				r->pressure[side].b = raw;
			}
			r->seen[OPENSC2_CAL_REC_PRESSURE] |= (uint8_t)(1u << side);
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
	int32_t span_rise = (int32_t)rise->a_mv - rise->b_mv;
	int32_t span_fall = (int32_t)fall->a_mv - fall->b_mv;

	/* The candidate whose A and B lie further apart; direction 0 on a tie. */
	*out = span_fall > span_rise ? *fall : *rise;
}

void opensc2_cal_rec_pressure(const struct opensc2_cal_rec *r, enum opensc2_side side,
			      struct opensc2_cal_pressure *out)
{
	*out = r->pressure[side & 1];
}
