/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * pad_rules.c: the trackpads' frame-by-frame processing (pad_rules.h), written from the real
 * controller's behaviour as the protocol documentation (not public yet; docs/protocol.md lists the
 * pages) describes it. Pure logic; the arithmetic is the helpers' (single precision where the real
 * controller's is).
 */
#include <string.h>
#include <opensc2/types.h>
#include "pad_rules.h"

#define STICK_TOUCH_RADIUS  12000   /* the deflection that counts as a touched stick */
#define HOLD_RADIUS         4369    /* the stabiliser's (setting 80 = 2) */
#define PAD_RATE_HZ         250.0f  /* the position filter's sample rate */

static const uint8_t watched[10] = { 30, 31, 33, 34, 35, 52, 53, 66, 72, 73 };

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void osc2_pad_start_over(struct osc2_pad_proc *p)
{
	opensc2_cap_reset(&p->stick);
	opensc2_cap_reset(&p->grip);
	opensc2_pad_filter_reset(&p->filter);
	memset(&p->stab, 0, sizeof(p->stab));
	p->pressure.click = false;
	p->pressure.click_setting = INT16_MIN;   /* the stage is set up again at the next frame */
}

void osc2_pad_watch_step(struct osc2_pad_watch *w, struct osc2_pad_proc pads[2],
			 const struct osc2_pad_settings *set, const uint32_t loads[2],
			 const struct opensc2_cal_pressure cal[2])
{
	bool changed = false;

	for (int side = 0; side < 2; side++) {
		struct osc2_pad_proc *p = &pads[side];

		if (!p->set_up || loads[side] != p->pressure_applied) {
			opensc2_pressure_setup(&p->pressure, &cal[side]);   /* the filter is kept */
			osc2_pad_start_over(p);
			p->pressure_applied = loads[side];
			p->set_up = true;
		}
	}
	for (int i = 0; i < 10; i++) {
		int16_t v = set->v[watched[i]];

		changed |= w->started && v != w->settings[i];
		w->settings[i] = v;
	}
	w->started = true;
	if (changed) {
		osc2_pad_start_over(&pads[0]);
		osc2_pad_start_over(&pads[1]);
	}
}

/* Move the output with the thumb's movement since the last input; if it moved at all, close an
 * eighth of the gap to it (rounded down) plus one unit on each axis. */
static void stab_follow(struct osc2_pad_stab *st, int32_t p[2])
{
	int32_t in[2] = { p[0], p[1] };
	int32_t d[2] = { in[0] - st->in[0], in[1] - st->in[1] };

	for (int i = 0; i < 2; i++) {
		p[i] = st->out[i] + d[i];
	}
	if (opensc2_approx_distance(d[0], d[1]) > 0) {
		for (int i = 0; i < 2; i++) {
			if (p[i] < in[i]) {
				p[i] += ((in[i] - p[i]) >> 3) + 1;
			} else if (p[i] > in[i]) {
				p[i] += ((in[i] - p[i]) >> 3) - 1;
			}
		}
	}
}

/* One touched sample through the stabiliser. */
static void stab_step(struct osc2_pad_stab *st, int32_t p[2])
{
	int32_t in[2] = { p[0], p[1] };

	if (!st->in_touch) {
		st->mode = 1;   /* the touch-down point is the output */
	} else if (st->mode == 1) {
		if (opensc2_approx_distance(in[0] - st->out[0], in[1] - st->out[1]) > HOLD_RADIUS) {
			st->mode = 2;
			stab_follow(st, p);
		} else {
			p[0] = st->out[0];
			p[1] = st->out[1];
		}
	} else if (st->mode == 2) {
		stab_follow(st, p);
	} else {
		return;   /* not reached: a held touch is never idle */
	}
	st->out[0] = p[0];
	st->out[1] = p[1];
	st->in[0] = in[0];
	st->in[1] = in[1];
	st->in_touch = true;
}

bool osc2_pad_step(struct osc2_pad_proc *p, int side, const struct opensc2_pad_frame *f,
		   const struct osc2_pad_settings *set, const int16_t stick[2])
{
	const int16_t *v = set->v;
	struct osc2_pad_worked *w = &p->worked;
	bool touch = f->touched;
	int32_t x = 0, y = 0, fx = f->x, fy = f->y;

	side &= 1;
	/* a raw sensor's frame: what the real controller's trackpad driver does first */
	if (f->has & OPENSC2_PAD_HAS_CENTROID) {
		opensc2_pad_centroid(&fx, &fy, v[OPENSC2_SETTING_PAD_CLIP] > 0, p->stretch);
	}
	if (f->has & OPENSC2_PAD_HAS_LEVEL) {
		touch = opensc2_pad_touch_step(&p->touch, f->level, v[OPENSC2_SETTING_PAD_TOUCH_ON],
					       v[OPENSC2_SETTING_PAD_TOUCH_OFF], &fx, &fy);
	}
	memset(w, 0, sizeof(*w));
	w->has = f->has;
	w->touched = touch;
	/* the position */
	if (!touch) {
		opensc2_pad_filter_reset(&p->filter);
		memset(&p->stab, 0, sizeof(p->stab));
	} else {
		int32_t pos[2] = { opensc2_pad_scale(fx), opensc2_pad_scale(fy) };

		if (v[OPENSC2_SETTING_PAD_FILTER] != 0) {
			int16_t px = (int16_t)pos[0], py = (int16_t)pos[1];

			opensc2_pad_filter_step(&p->filter, &px, &py, PAD_RATE_HZ);
			pos[0] = px;
			pos[1] = py;
		}
		(void)opensc2_pad_rotate((enum opensc2_side)side, v[OPENSC2_SETTING_PAD_ROTATION_ANGLE],
					 v[OPENSC2_SETTING_PAD_ROTATION], &pos[0], &pos[1]);
		pos[0] = clamp32(pos[0] * 2, -OPENSC2_AXIS_MAX, OPENSC2_AXIS_MAX);
		pos[1] = clamp32(pos[1] * 2, -OPENSC2_AXIS_MAX, OPENSC2_AXIS_MAX);
		if (v[OPENSC2_SETTING_PAD_STABILIZER] == 2) {
			stab_step(&p->stab, pos);
		}
		x = pos[0];
		y = -pos[1];
	}
	w->x = (int16_t)clamp32(x, INT16_MIN, INT16_MAX);
	w->y = (int16_t)clamp32(y, INT16_MIN, INT16_MAX);

	/* the stick touch: its words, and the stick deflected */
	if (f->has & OPENSC2_PAD_HAS_STICK_TOUCH) {
		w->stick_level = opensc2_cap_sample(
			&p->stick, f->stick_touch,
			opensc2_stick_touch_on_level(v[OPENSC2_SETTING_STICK_TOUCH_ON]),
			opensc2_stick_touch_off_level(v[OPENSC2_SETTING_STICK_TOUCH_OFF]));
	}
	w->deflected = opensc2_approx_distance(stick[0], stick[1]) > STICK_TOUCH_RADIUS;

	/* the grip */
	if (f->has & OPENSC2_PAD_HAS_GRIP) {
		int16_t s34 = v[OPENSC2_SETTING_GRIP_SENSITIVITY];

		w->grip_level = opensc2_cap_sample(
			&p->grip, f->grip, opensc2_grip_on_level(s34),
			opensc2_grip_off_level(s34, v[OPENSC2_SETTING_GRIP_RELEASE]));
	}

	/* the pressure and the click */
	if (f->has & OPENSC2_PAD_HAS_PRESSURE) {
		opensc2_pressure_sample(&p->pressure, f->pressure,
					v[side ? OPENSC2_SETTING_RIGHT_CLICK_PRESSURE :
						 OPENSC2_SETTING_LEFT_CLICK_PRESSURE]);
		w->pressure = p->pressure.last;
		w->click = p->pressure.click;
		w->raw = f->pressure;
		return true;
	}
	return false;
}

static const uint32_t stick_bit[2] = { OPENSC2_TOUCH_LEFT_STICK, OPENSC2_TOUCH_RIGHT_STICK };
static const uint32_t grip_bit[2] = { OPENSC2_TOUCH_LEFT_GRIP, OPENSC2_TOUCH_RIGHT_GRIP };
static const uint32_t touch_bit[2] = { OPENSC2_TOUCH_LEFT_PAD, OPENSC2_TOUCH_RIGHT_PAD };
static const uint32_t click_bit[2] = { OPENSC2_CLICK_LEFT_PAD, OPENSC2_CLICK_RIGHT_PAD };

void osc2_pad_finish(const struct osc2_pad_worked *w, int side, const int16_t *settings,
		     uint32_t buttons, struct opensc2_pad_result *out)
{
	bool touch = w->touched;
	int16_t x = w->x, y = w->y;
	uint16_t pressure = w->pressure;
	bool click, stick_touched, grip;

	side &= 1;
	/* a word the frame does not have: the application's bit (a capacitive key, a click switch) */
	stick_touched = ((w->has & OPENSC2_PAD_HAS_STICK_TOUCH) ? w->stick_level :
								  (buttons & stick_bit[side]) != 0) ||
			w->deflected;
	grip = (w->has & OPENSC2_PAD_HAS_GRIP) ? w->grip_level : (buttons & grip_bit[side]) != 0;
	click = (w->has & OPENSC2_PAD_HAS_PRESSURE) ? w->click : (buttons & click_bit[side]) != 0;

	/* setting 82: a touched stick blanks its pad */
	if (stick_touched && (settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK] & (side ? 2 : 1))) {
		x = 0;
		y = 0;
		touch = false;
	}
	if (!touch) {
		pressure = 0;
		click = false;
	}
	if (settings[OPENSC2_SETTING_RAW_PRESSURE] != 0) {
		/* the raw word, also while not touched */
		pressure = (w->has & OPENSC2_PAD_HAS_PRESSURE) ? opensc2_pressure_raw_value(w->raw) :
								  0;
	}
	out->frames = true;
	out->touched = touch;
	out->clicked = click;
	out->stick_touched = stick_touched;
	out->grip = grip;
	out->x = x;
	out->y = y;
	out->pressure = pressure;
}

void osc2_pad_merge(const struct osc2_pad_worked *w, int side, const int16_t *settings,
		    struct opensc2_state *s)
{
	struct opensc2_pad_result out;
	uint32_t bits, on;

	side &= 1;
	osc2_pad_finish(w, side, settings, s->buttons, &out);
	bits = stick_bit[side] | grip_bit[side] | touch_bit[side] | click_bit[side];
	on = (out.stick_touched ? stick_bit[side] : 0) | (out.grip ? grip_bit[side] : 0) |
	     (out.touched ? touch_bit[side] : 0) | (out.clicked ? click_bit[side] : 0);
	s->pad[side] = (struct opensc2_trackpad){
		.x = out.x,
		.y = out.y,
		.pressure = out.pressure,
		.touched = out.touched,
		.clicked = out.clicked,
	};
	s->buttons = (s->buttons & ~bits) | on;
}
