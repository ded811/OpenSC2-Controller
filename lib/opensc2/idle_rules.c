/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * idle_rules.c: the idle monitor (idle_rules.h). Pure logic.
 */
#include "idle_rules.h"

#define TOUCH_BITS (OPENSC2_TOUCH_RIGHT_PAD | OPENSC2_TOUCH_LEFT_PAD | OPENSC2_TOUCH_RIGHT_STICK | \
		    OPENSC2_TOUCH_LEFT_STICK)

bool osc2_idle_enabled(int16_t timeout_s)
{
	return timeout_s > 0 && timeout_s <= OSC2_IDLE_MAX_S;
}

void osc2_idle_start(struct osc2_idle *m, int16_t timeout_s, uint32_t now_ms)
{
	m->history = 0xFFFF;
	m->active = false;
	m->timeout_s = timeout_s;
	/* Rounded down: 56.25 s at the default 900 s. */
	m->period_ms = osc2_idle_enabled(timeout_s) ?
		       (uint32_t)timeout_s * 1000u / OSC2_IDLE_PERIODS : 0;
	m->period_end = now_ms + m->period_ms;
}

void osc2_idle_setting(struct osc2_idle *m, int16_t timeout_s, uint32_t now_ms)
{
	if (timeout_s != m->timeout_s) {
		osc2_idle_start(m, timeout_s, now_ms);
	}
}

void osc2_idle_activity(struct osc2_idle *m)
{
	m->active = true;
}

bool osc2_idle_poll(struct osc2_idle *m, uint32_t now_ms)
{
	bool idle = false;

	if (m->period_ms == 0) {
		return false;
	}
	while ((int32_t)(now_ms - m->period_end) >= 0) {
		m->history = (uint16_t)((m->history << 1) | (m->active ? 1u : 0u));
		m->active = false;
		m->period_end += m->period_ms;
		if (m->history == 0) {
			idle = true;
		}
	}
	return idle;
}

int32_t osc2_idle_next(const struct osc2_idle *m, uint32_t now_ms)
{
	int32_t left;

	if (m->period_ms == 0) {
		return -1;
	}
	left = (int32_t)(m->period_end - now_ms);
	return left < 0 ? 0 : left;
}

bool osc2_idle_recent(const struct osc2_idle *m)
{
	return m->period_ms == 0 || m->history != 0 || m->active;
}

bool osc2_idle_buttons_active(uint32_t before, uint32_t after)
{
	uint32_t b = before & OPENSC2_BUTTONS_MASK, a = after & OPENSC2_BUTTONS_MASK;

	return (a != b && a != 0) || (after & TOUCH_BITS) != 0;
}

bool osc2_idle_state_active(const struct opensc2_state *s)
{
	for (int side = 0; side < 2; side++) {
		if (s->stick[side].x > OSC2_IDLE_STICK_THRESHOLD ||
		    s->stick[side].x < -OSC2_IDLE_STICK_THRESHOLD ||
		    s->stick[side].y > OSC2_IDLE_STICK_THRESHOLD ||
		    s->stick[side].y < -OSC2_IDLE_STICK_THRESHOLD || s->trigger[side] > 0 ||
		    s->pad[side].touched) {
			return true;
		}
	}
	return (s->buttons & TOUCH_BITS) != 0;
}
