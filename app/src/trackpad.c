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
 * Pinnacle in absolute mode), which also becomes the pad's pressure. Touch devices count Y from the
 * top; the controller's pads count up as positive, so Y is flipped (set *-trackpad-keep-y to keep
 * it). A pad click is a button with code OPENSC2_KEY_LEFT/RIGHT_PAD_CLICK in opensc2_buttons.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
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

static int16_t scale(int32_t v, int32_t size)
{
	int32_t out = v * (2 * OPENSC2_AXIS_MAX) / MAX(size, 1) - OPENSC2_AXIS_MAX;

	return (int16_t)CLAMP(out, -OPENSC2_AXIS_MAX, OPENSC2_AXIS_MAX);
}

static void pad_event(struct pad_tracker *t, const struct input_event *evt)
{
	struct opensc2_trackpad pad = { 0 };

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

	pad.touched = t->touch_key_seen ? t->touch_key : (t->z > 0 || t->x || t->y);
	if (pad.touched) {
		pad.x = scale(t->x, t->width);
		pad.y = scale(t->y, t->height);
		if (!t->keep_y) {
			pad.y = (int16_t)-pad.y;
		}
		/* A touch strength (0..63 on the Pinnacle) as pressure; a plain touch as half. */
		pad.pressure = t->z > 0 ? (uint16_t)MIN(t->z * 520, OPENSC2_TRIGGER_MAX) : 16384;
	}
	opensc2_set_trackpad(t->side, &pad);
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
