/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * input_rules.c: the state report's rules after the injection point (input_rules.h), written from
 * the real controller's behaviour as the protocol documentation (not public yet; docs/protocol.md
 * lists the pages) describes it:
 *   - stick touch: a stick counts as touched while its position lies further than 12000 from the
 *     centre, measured with the distance approximation below (about 12200 along an axis, 12340 on
 *     a diagonal); a radius, no hysteresis, and never with raw sticks (setting 46), whose values
 *     are not positions;
 *   - pad click: on above the threshold plus 8 points, off below it minus 8; the click follows
 *     the pressure also while the pad is not touched, but only a touched pad reports it; a change
 *     of the threshold clears it;
 *   - setting 82: with the side's bit set and that side's stick touched (by its sensor or by the
 *     fallback), the pad reports no touch, no click, position 0, 0 and no pressure (raw pressure,
 *     setting 60 = 1, still passes); the stick-touch and grip bits stay;
 *   - raw triggers (setting 62 not 0): both digital bits are 0.
 * Pure logic.
 */
#include <opensc2/codec.h>
#include "input_rules.h"
#include "report_rules.h"

static const uint32_t stick_bit[2] = { OPENSC2_TOUCH_LEFT_STICK, OPENSC2_TOUCH_RIGHT_STICK };
static const uint32_t touch_bit[2] = { OPENSC2_TOUCH_LEFT_PAD, OPENSC2_TOUCH_RIGHT_PAD };
static const uint32_t click_bit[2] = { OPENSC2_CLICK_LEFT_PAD, OPENSC2_CLICK_RIGHT_PAD };
static const uint32_t trigger_bit[2] = { OPENSC2_CLICK_LEFT_TRIGGER, OPENSC2_CLICK_RIGHT_TRIGGER };

#define CLICK_UNSET INT16_MIN   /* no threshold seen yet */

void osc2_input_rules_reset(struct osc2_input_rules *r)
{
	for (int side = 0; side < 2; side++) {
		r->trigger_bit[side] = false;
		r->pad_click[side] = false;
		r->click_setting[side] = CLICK_UNSET;
		r->decided_bit[side] = false;
	}
	r->level68 = CLICK_UNSET;
	r->decided = 0;
}

void osc2_input_rules_decide(struct osc2_input_rules *r, int side, bool bit)
{
	if (side == 0 || side == 1) {
		r->decided |= (uint8_t)(1u << side);
		r->decided_bit[side] = bit;
	}
}

void osc2_input_rules_triggers_set_up(struct osc2_input_rules *r)
{
	r->trigger_bit[0] = false;
	r->trigger_bit[1] = false;
}

int32_t osc2_approx_len(int32_t dx, int32_t dy)
{
	uint32_t a = (uint32_t)(dx < 0 ? -dx : dx);
	uint32_t b = (uint32_t)(dy < 0 ? -dy : dy);
	uint32_t big = a > b ? a : b;
	uint32_t small = a > b ? b : a;
	uint32_t sum = 1007u * big + 441u * small;

	if (big < 16u * small) {
		sum -= 40u * big;
	}
	return (int32_t)((sum + 512u) / 1024u);
}

bool osc2_pad_click(uint16_t pressure, int16_t setting, bool previous)
{
	float t = (setting >= 0 ? (float)setting : 100.0f) / 100.0f;
	float h = 8.0f / 100.0f;
	float v = (float)(pressure > OPENSC2_TRIGGER_MAX ? OPENSC2_TRIGGER_MAX : pressure) /
		  (float)OPENSC2_TRIGGER_MAX;

	if (previous) {
		return !(t - h > v);
	}
	return t + h < v;
}

/* An eighth of the gap, rounded down (toward minus infinity), plus one unit toward the thumb. */
static int32_t close_step(int32_t gap)
{
	if (gap > 0) {
		return gap / 8 + 1;
	}
	if (gap < 0) {
		return -((-gap + 7) / 8) - 1;
	}
	return 0;
}

void osc2_stabiliser_reset(struct osc2_stabiliser *st)
{
	st->on = false;
	st->holding = false;
	st->ax = st->ay = st->ox = st->oy = st->px = st->py = 0;
}

void osc2_stabiliser_step(struct osc2_stabiliser *st, int32_t radius, bool touched, int32_t *x,
			  int32_t *y)
{
	int32_t tx = *x, ty = *y, dx, dy;

	if (!touched) {
		osc2_stabiliser_reset(st);
		return;
	}
	if (!st->on) {
		/* The touch-down point is the output, and the point the hold is measured from. */
		st->on = true;
		st->holding = true;
		st->ax = st->ox = st->px = tx;
		st->ay = st->oy = st->py = ty;
		return;
	}
	if (st->holding) {
		if (osc2_approx_len(tx - st->ax, ty - st->ay) <= radius) {
			st->px = tx;
			st->py = ty;
			*x = st->ox;
			*y = st->oy;
			return;
		}
		st->holding = false;   /* gone further: followed for the rest of the touch */
	}
	dx = tx - st->px;
	dy = ty - st->py;
	st->ox += dx;
	st->oy += dy;
	if (dx || dy) {
		st->ox += close_step(tx - st->ox);
		st->oy += close_step(ty - st->oy);
	}
	st->px = tx;
	st->py = ty;
	*x = st->ox;
	*y = st->oy;
}

static void set_bit(uint32_t *buttons, uint32_t bit, bool on)
{
	*buttons = on ? (*buttons | bit) : (*buttons & ~bit);
}

void osc2_input_rules_apply(struct osc2_input_rules *r, uint32_t rules,
			    const int16_t settings[OPENSC2_SETTINGS_COUNT], struct opensc2_state *s)
{
	for (int side = 0; side < 2; side++) {
		struct opensc2_trackpad *pad = &s->pad[side];

		/* The stick-touch fallback reads positions: not raw stick output. */
		if ((rules & OSC2_RULE_STICK_FALLBACK) &&
		    settings[OPENSC2_SETTING_RAW_STICKS] == 0 &&
		    osc2_approx_len(s->stick[side].x, s->stick[side].y) > OSC2_STICK_TOUCH_RADIUS) {
			s->buttons |= stick_bit[side];
		}

		/* The click by pressure replaces the application's click. */
		if (rules & OSC2_RULE_PAD_CLICK) {
			int16_t setting = settings[side ? OPENSC2_SETTING_RIGHT_CLICK_PRESSURE :
							  OPENSC2_SETTING_LEFT_CLICK_PRESSURE];
			bool touched = pad->touched || (s->buttons & touch_bit[side]);

			if (setting != r->click_setting[side]) {
				r->pad_click[side] = false;   /* a new threshold clears the click */
				r->click_setting[side] = setting;
			}
			r->pad_click[side] = osc2_pad_click(pad->pressure, setting, r->pad_click[side]);
			pad->clicked = r->pad_click[side] && touched;
			set_bit(&s->buttons, click_bit[side], pad->clicked);
		} else {
			r->pad_click[side] = false;
			r->click_setting[side] = CLICK_UNSET;
		}

		/* Setting 82: no pad while the thumb rests on the stick on its side. */
		if ((rules & OSC2_RULE_PAD_BLANK) &&
		    (settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK] & (side ? 2 : 1)) &&
		    (s->buttons & stick_bit[side])) {
			pad->touched = false;
			pad->clicked = false;
			pad->x = 0;
			pad->y = 0;
			if (settings[OPENSC2_SETTING_RAW_PRESSURE] != 1) {
				pad->pressure = 0;
			}
			s->buttons &= ~(touch_bit[side] | click_bit[side]);
		}
	}

	/* The triggers' digital bits; none with raw trigger output. A new setting 68 sets the
	 * triggers up again, which clears their bits. */
	if (settings[OPENSC2_SETTING_TRIGGER_THRESHOLD] != r->level68) {
		r->level68 = settings[OPENSC2_SETTING_TRIGGER_THRESHOLD];
		osc2_input_rules_triggers_set_up(r);
	}
	if ((rules & OSC2_RULE_RAW_TRIGGERS) && settings[OPENSC2_SETTING_RAW_TRIGGERS] != 0) {
		for (int side = 0; side < 2; side++) {
			r->trigger_bit[side] = false;
			set_bit(&s->buttons, trigger_bit[side], false);
		}
	} else if (rules & OSC2_RULE_TRIGGER_BITS) {
		for (int side = 0; side < 2; side++) {
			if (r->decided & (1u << side)) {
				r->trigger_bit[side] = r->decided_bit[side];   /* the calibration's */
			} else {
				r->trigger_bit[side] = osc2_trigger_click(
					s->trigger[side], settings[OPENSC2_SETTING_TRIGGER_THRESHOLD],
					r->trigger_bit[side]);
			}
			set_bit(&s->buttons, trigger_bit[side], r->trigger_bit[side]);
		}
	}
	r->decided = 0;
}
