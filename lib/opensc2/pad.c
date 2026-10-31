/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * pad.c: the trackpads frame by frame (<opensc2/trackpad.h>, CONFIG_OPENSC2_CAL), as the real
 * controller processes its trackpad chip's frames, following the protocol documentation (not
 * public yet; docs/protocol.md lists the pages). Each frame is worked out when it arrives, in the
 * caller's thread, under one lock; the results wait for the next state report, which takes them
 * (osc2_pad_merge()) in place of the application's finished values for that side. The arithmetic is
 * the helpers' (cal.c, sensor_helpers.c: the real controller's single precision); the touch
 * stabiliser here is the frame path's own, which keeps its state across a stretch of setting 80 not
 * being 2 and starts over only without a touch, as the real controller's does.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <opensc2/opensc2.h>
#include <opensc2/cal.h>
#include <opensc2/sensor_helpers.h>
#include <opensc2/trackpad.h>
#include "internal.h"
#include "cal_store.h"
#include "pad_rules.h"

#if defined(CONFIG_OPENSC2_CAL)

static const uint32_t stick_bit[2] = { OPENSC2_TOUCH_LEFT_STICK, OPENSC2_TOUCH_RIGHT_STICK };
static const uint32_t grip_bit[2] = { OPENSC2_TOUCH_LEFT_GRIP, OPENSC2_TOUCH_RIGHT_GRIP };
static const uint32_t click_bit[2] = { OPENSC2_CLICK_LEFT_PAD, OPENSC2_CLICK_RIGHT_PAD };

static struct k_spinlock pad_lock;
static struct osc2_pad_proc pads[2];
static struct osc2_pad_watch watch;
static int16_t stick_cache[2][2];   /* the sticks the last state report had (0 in raw mode) */

static uint32_t app_buttons(void)
{
	k_spinlock_key_t lk = k_spin_lock(&osc2.lock);
	uint32_t b = osc2.state.buttons;

	k_spin_unlock(&osc2.lock, lk);
	return b;
}

static void settings_now(struct osc2_pad_settings *s)
{
	for (int i = 0; i < OPENSC2_SETTINGS_COUNT && i < (int)ARRAY_SIZE(s->v); i++) {
		s->v[i] = opensc2_setting_get((uint8_t)i);
	}
}

int opensc2_pad_frame(enum opensc2_side side, const struct opensc2_pad_frame *frame)
{
	struct osc2_pad_settings set;
	struct opensc2_cal_pressure cal[2];
	uint32_t loads[2];
	uint32_t app;
	k_spinlock_key_t lk;
	int16_t raw_pressure = 0;
	bool record;

	if (side > OPENSC2_RIGHT || !frame) {
		return -EINVAL;
	}
	settings_now(&set);
	for (int i = 0; i < 2; i++) {
		loads[i] = osc2_cal_pressure_key(i, &cal[i]);
	}
	app = app_buttons();

	lk = k_spin_lock(&pad_lock);
	osc2_pad_watch_step(&watch, pads, &set, loads, cal);
	pads[side].frames = true;
	record = osc2_pad_step(&pads[side], side, frame, &set, stick_cache[side],
			       (app & stick_bit[side]) != 0, (app & grip_bit[side]) != 0,
			       (app & click_bit[side]) != 0, &raw_pressure);
	k_spin_unlock(&pad_lock, lk);

	if (record) {
		osc2_cal_rec_pressure_frame(side, raw_pressure);   /* the factory recording, 0xC3 */
	}
	return 0;
}

int opensc2_pad_frame_release(enum opensc2_side side)
{
	k_spinlock_key_t lk;

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	lk = k_spin_lock(&pad_lock);
	pads[side].frames = false;
	memset(&pads[side].out, 0, sizeof(pads[side].out));
	k_spin_unlock(&pad_lock, lk);
	return 0;
}

int opensc2_pad_result(enum opensc2_side side, struct opensc2_pad_result *result)
{
	k_spinlock_key_t lk;

	if (side > OPENSC2_RIGHT || !result) {
		return -EINVAL;
	}
	lk = k_spin_lock(&pad_lock);
	*result = pads[side].out;
	result->frames = pads[side].frames;
	k_spin_unlock(&pad_lock, lk);
	return 0;
}

uint8_t osc2_pad_frame_sides(void)
{
	k_spinlock_key_t lk = k_spin_lock(&pad_lock);
	uint8_t sides = (pads[0].frames ? 1u : 0u) | (pads[1].frames ? 2u : 0u);

	k_spin_unlock(&pad_lock, lk);
	return sides;
}

void osc2_pad_report(struct opensc2_state *s, bool raw_sticks)
{
	k_spinlock_key_t lk = k_spin_lock(&pad_lock);

	for (int side = 0; side < 2; side++) {
		/* the deflection rule's sticks: the calibrated ones, none in raw mode (setting 46) */
		stick_cache[side][0] = raw_sticks ? 0 : s->stick[side].x;
		stick_cache[side][1] = raw_sticks ? 0 : s->stick[side].y;
		if (pads[side].frames) {
			osc2_pad_merge(&pads[side].out, side, s);
		}
	}
	k_spin_unlock(&pad_lock, lk);
}

bool osc2_pad_status(int side, int16_t *filtered, bool *click)
{
	k_spinlock_key_t lk = k_spin_lock(&pad_lock);
	bool frames = pads[side & 1].frames;

	*filtered = pads[side & 1].pressure.filtered;
	*click = pads[side & 1].pressure.click;
	k_spin_unlock(&pad_lock, lk);
	return frames;
}

void osc2_pad_reset(void)
{
	k_spinlock_key_t lk = k_spin_lock(&pad_lock);

	memset(pads, 0, sizeof(pads));
	memset(&watch, 0, sizeof(watch));
	memset(stick_cache, 0, sizeof(stick_cache));
	k_spin_unlock(&pad_lock, lk);
}

#else /* !CONFIG_OPENSC2_CAL */

int opensc2_pad_frame(enum opensc2_side side, const struct opensc2_pad_frame *frame)
{
	ARG_UNUSED(side);
	ARG_UNUSED(frame);
	return -ENOTSUP;
}

int opensc2_pad_frame_release(enum opensc2_side side)
{
	ARG_UNUSED(side);
	return -ENOTSUP;
}

int opensc2_pad_result(enum opensc2_side side, struct opensc2_pad_result *result)
{
	ARG_UNUSED(side);
	if (result) {
		memset(result, 0, sizeof(*result));
	}
	return -ENOTSUP;
}

#endif /* CONFIG_OPENSC2_CAL */
