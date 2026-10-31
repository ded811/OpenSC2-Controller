/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * trackpad.c: touch devices of Zephyr's input subsystem as the controller's trackpads. Choose
 * them in the overlay:
 *     chosen { opensc2,right-trackpad = &touch; opensc2,left-trackpad = &touch2; };
 * and give their coordinate range in zephyr,user (default 1024 x 1024):
 *     right-trackpad-size = <1024 1024>;
 *     left-trackpad-size = <1024 1024>;
 * The device reports absolute X and Y (INPUT_ABS_X/Y) and either a touch key (INPUT_BTN_TOUCH,
 * touch screens such as the FT5336, GT911 or CST816S) or a touch strength (INPUT_ABS_Z, the Cirque
 * Pinnacle in absolute mode). Touch devices count Y from the top, as the real controller's pads do
 * (the report turns it up); set *-trackpad-keep-y for a device that counts from the bottom. A pad
 * click is a button with code OPENSC2_KEY_LEFT/RIGHT_PAD_CLICK in opensc2_buttons.
 *
 * Every report of the device (each sync) is a trackpad frame for the library
 * (<opensc2/trackpad.h>), which processes it as the real controller processes its pads, at the
 * device's own rate: the pad scale, the position filter of setting 65 (on by default), the rotation
 * of settings 2 and 24, the stabiliser of setting 80, the stick-touch and grip bits, setting 82.
 * The pad's pressure is the real controller's without a pressure calibration: 0, and no click by
 * pressure, so a board with a click switch keeps its click.
 *
 * CONFIG_APP_PAD_PROCESSING (off by default, untested) gives, for a device with a strength, that
 * strength times trackpad-strength-scale in zephyr,user (40 by default: tune it to your part) as
 * the frame's signal level, from which the library takes the touch as the real controller's
 * trackpad driver does (settings 72 and 73, the damping near lift-off), and as the pad's raw
 * pressure word, so a pressure calibration (vendor command 0xC3 or "opensc2 cal pressure") gives
 * the pad its pressure and its click by pressure.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <opensc2/trackpad.h>
#include "app.h"

#define USER DT_PATH(zephyr_user)

/* Entry `idx` of zephyr,user's `prop`, or 1024 when the overlay does not set it. */
#define SIZE_OR_DEFAULT(prop, idx)                                                        \
	COND_CODE_1(DT_NODE_HAS_PROP(USER, prop), (DT_PROP_BY_IDX(USER, prop, idx)), (1024))

#if DT_HAS_CHOSEN(opensc2_right_trackpad) || DT_HAS_CHOSEN(opensc2_left_trackpad)

struct pad_tracker {
	enum opensc2_side side;
	int32_t width, height;
	bool keep_y;
	int32_t x, y, z;
	bool touch_key_seen, touch_key;
};

/* A device coordinate as millionths of the pad (an absolute event's value, as the real
 * controller's trackpad chip reports it). */
static int32_t millionths(int32_t v, int32_t size)
{
	return (int32_t)((int64_t)v * 1000000 / MAX(size, 1));
}

#if defined(CONFIG_APP_PAD_PROCESSING)
#define STRENGTH_SCALE DT_PROP_OR(USER, trackpad_strength_scale, 40)
#endif

static void pad_event(struct pad_tracker *t, const struct input_event *evt)
{
	struct opensc2_pad_frame f = { 0 };

	if (evt->type == INPUT_EV_ABS) {
		if (evt->code == INPUT_ABS_X) {
			t->x = evt->value;
		} else if (evt->code == INPUT_ABS_Y) {
			t->y = evt->value;
		} else if (evt->code == INPUT_ABS_Z) {
			t->z = evt->value;
		}
	} else if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH) {
		t->touch_key_seen = true;
		t->touch_key = evt->value != 0;
	}
	if (!evt->sync) {
		return;
	}

	f.x = millionths(t->x, t->width);
	f.y = millionths(t->y, t->height);
	if (t->keep_y) {
		f.y = 1000000 - f.y;   /* counted from the bottom: from the top, as the pads count */
	}
#if defined(CONFIG_APP_PAD_PROCESSING)
	if (!t->touch_key_seen) {
		int32_t level = t->z * STRENGTH_SCALE;

		/* the touch from the level (the library's, with the damping near lift-off), and the
		 * level as the pad's raw pressure word */
		f.has = OPENSC2_PAD_HAS_LEVEL | OPENSC2_PAD_HAS_PRESSURE;
		f.level = level;
		f.pressure = (int16_t)CLAMP(level, INT16_MIN, INT16_MAX);
	} else {
		f.touched = t->touch_key;
	}
#else
	f.touched = t->touch_key_seen ? t->touch_key : (t->z > 0 || t->x || t->y);
#endif
	(void)opensc2_pad_frame(t->side, &f);
}

#if DT_HAS_CHOSEN(opensc2_right_trackpad)
static struct pad_tracker right_pad = {
	.side = OPENSC2_RIGHT,
	.width = SIZE_OR_DEFAULT(right_trackpad_size, 0),
	.height = SIZE_OR_DEFAULT(right_trackpad_size, 1),
	.keep_y = DT_PROP_OR(USER, right_trackpad_keep_y, 0),
};

static void on_right_pad(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	pad_event(&right_pad, evt);
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(opensc2_right_trackpad)), on_right_pad, NULL);
#endif

#if DT_HAS_CHOSEN(opensc2_left_trackpad)
static struct pad_tracker left_pad = {
	.side = OPENSC2_LEFT,
	.width = SIZE_OR_DEFAULT(left_trackpad_size, 0),
	.height = SIZE_OR_DEFAULT(left_trackpad_size, 1),
	.keep_y = DT_PROP_OR(USER, left_trackpad_keep_y, 0),
};

static void on_left_pad(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	pad_event(&left_pad, evt);
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(opensc2_left_trackpad)), on_left_pad, NULL);
#endif

#endif /* any trackpad chosen */

int trackpad_init(void)
{
	/* Nothing to do: the input callbacks above are registered at build time. */
	return 0;
}
