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
		   const struct osc2_pad_settings *set, const int16_t stick[2], bool app_stick,
		   bool app_grip, bool app_click, int16_t *raw)
{
	const int16_t *v = set->v;
	bool touch = f->touched;
	bool level, stick_touched, grip, click, record = false;
	int32_t x = 0, y = 0;
	uint16_t pressure = 0;

	side &= 1;
	/* the position */
	if (!touch) {
		opensc2_pad_filter_reset(&p->filter);
		memset(&p->stab, 0, sizeof(p->stab));
	} else {
		int32_t pos[2] = { opensc2_pad_scale(f->x), opensc2_pad_scale(f->y) };

		if (v[OPENSC2_SETTING_PAD_FILTER] != 0) {
			int16_t fx = (int16_t)pos[0], fy = (int16_t)pos[1];

			opensc2_pad_filter_step(&p->filter, &fx, &fy, PAD_RATE_HZ);
			pos[0] = fx;
			pos[1] = fy;
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
	x = clamp32(x, INT16_MIN, INT16_MAX);
	y = clamp32(y, INT16_MIN, INT16_MAX);

	/* the stick touch: its sensor (or the application's bit), or the stick deflected */
	if (f->has & OPENSC2_PAD_HAS_STICK_TOUCH) {
		level = opensc2_cap_sample(&p->stick, f->stick_touch,
					   opensc2_stick_touch_on_level(v[OPENSC2_SETTING_STICK_TOUCH_ON]),
					   opensc2_stick_touch_off_level(v[OPENSC2_SETTING_STICK_TOUCH_OFF]));
	} else {
		level = app_stick;
	}
	stick_touched = level || opensc2_approx_distance(stick[0], stick[1]) > STICK_TOUCH_RADIUS;

	/* the grip */
	if (f->has & OPENSC2_PAD_HAS_GRIP) {
		int16_t s34 = v[OPENSC2_SETTING_GRIP_SENSITIVITY];

		grip = opensc2_cap_sample(&p->grip, f->grip, opensc2_grip_on_level(s34),
					  opensc2_grip_off_level(s34, v[OPENSC2_SETTING_GRIP_RELEASE]));
	} else {
		grip = app_grip;
	}

	/* the pressure and the click (a board without a pressure word keeps its click button) */
	if (f->has & OPENSC2_PAD_HAS_PRESSURE) {
		opensc2_pressure_sample(&p->pressure, f->pressure,
					v[side ? OPENSC2_SETTING_RIGHT_CLICK_PRESSURE :
						 OPENSC2_SETTING_LEFT_CLICK_PRESSURE]);
		pressure = p->pressure.last;
		click = p->pressure.click;
		*raw = f->pressure;
		record = true;
	} else {
		click = app_click;
	}

	/* setting 82: a touched stick blanks its pad */
	if (stick_touched && (v[OPENSC2_SETTING_PAD_IGNORE_ON_STICK] & (side ? 2 : 1))) {
		x = 0;
		y = 0;
		touch = false;
	}
	if (!touch) {
		pressure = 0;
		click = false;
	}
	if (v[OPENSC2_SETTING_RAW_PRESSURE] != 0) {
		/* the raw word, also while not touched */
		pressure = (f->has & OPENSC2_PAD_HAS_PRESSURE) ?
				   opensc2_pressure_raw_value(f->pressure) : 0;
	}

	p->out.touched = touch;
	p->out.clicked = click;
	p->out.stick_touched = stick_touched;
	p->out.grip = grip;
	p->out.x = (int16_t)x;
	p->out.y = (int16_t)y;
	p->out.pressure = pressure;
	return record;
}

void osc2_pad_merge(const struct opensc2_pad_result *out, int side, struct opensc2_state *s)
{
	static const uint32_t stick_bit[2] = { OPENSC2_TOUCH_LEFT_STICK, OPENSC2_TOUCH_RIGHT_STICK };
	static const uint32_t grip_bit[2] = { OPENSC2_TOUCH_LEFT_GRIP, OPENSC2_TOUCH_RIGHT_GRIP };
	static const uint32_t touch_bit[2] = { OPENSC2_TOUCH_LEFT_PAD, OPENSC2_TOUCH_RIGHT_PAD };
	static const uint32_t click_bit[2] = { OPENSC2_CLICK_LEFT_PAD, OPENSC2_CLICK_RIGHT_PAD };
	uint32_t bits = stick_bit[side & 1] | grip_bit[side & 1] | touch_bit[side & 1] |
			click_bit[side & 1];
	uint32_t on = (out->stick_touched ? stick_bit[side & 1] : 0) |
		      (out->grip ? grip_bit[side & 1] : 0) | (out->touched ? touch_bit[side & 1] : 0) |
		      (out->clicked ? click_bit[side & 1] : 0);

	s->pad[side & 1] = (struct opensc2_trackpad){
		.x = out->x,
		.y = out->y,
		.pressure = out->pressure,
		.touched = out->touched,
		.clicked = out->clicked,
	};
	s->buttons = (s->buttons & ~bits) | on;
}
