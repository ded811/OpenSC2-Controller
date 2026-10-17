/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * A touch module as the right trackpad. The touch device reports absolute X and Y and either a
 * touch key or a touch strength (Z) through Zephyr's input subsystem; on each complete event the
 * position is scaled to -32767..32767 (Y flipped: up is positive on the controller) and handed to
 * the library. A separate button is the pad's click. In Steam the right pad then works as a
 * trackpad: mouse, camera, or whatever its configuration says.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <opensc2/opensc2.h>

#define USER DT_PATH(zephyr_user)

static const int32_t width = DT_PROP_BY_IDX(USER, trackpad_size, 0);
static const int32_t height = DT_PROP_BY_IDX(USER, trackpad_size, 1);
static const struct gpio_dt_spec vcc = GPIO_DT_SPEC_GET(USER, vcc_enable_gpios);

static int16_t scale(int32_t v, int32_t size)
{
	return (int16_t)CLAMP(v * (2 * OPENSC2_AXIS_MAX) / size - OPENSC2_AXIS_MAX,
			      -OPENSC2_AXIS_MAX, OPENSC2_AXIS_MAX);
}

static void on_touch(struct input_event *evt, void *user_data)
{
	static int32_t x, y, z;
	static bool key_seen, key_down;
	struct opensc2_trackpad pad = { 0 };

	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_X) {
		x = evt->value;
	} else if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_Y) {
		y = evt->value;
	} else if (evt->type == INPUT_EV_ABS && evt->code == INPUT_ABS_Z) {
		z = evt->value;   /* touch strength, 0 when nothing touches */
	} else if (evt->type == INPUT_EV_KEY && evt->code == INPUT_BTN_TOUCH) {
		key_seen = true;
		key_down = evt->value;
	}
	if (!evt->sync) {
		return;   /* wait for the whole report */
	}
	pad.touched = key_seen ? key_down : z > 0;
	if (pad.touched) {
		pad.x = scale(x, width);
		pad.y = (int16_t)-scale(y, height);
		pad.pressure = z ? (uint16_t)MIN(z * 520, OPENSC2_TRIGGER_MAX) : 16384;
	}
	opensc2_set_trackpad(OPENSC2_RIGHT, &pad);
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(touch)), on_touch, NULL);

static void on_button(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->code == OPENSC2_KEY_PAIR) {
		if (evt->value) {
			opensc2_pair_start(0);
		}
	} else if (evt->code < 32) {
		opensc2_set_buttons(BIT(evt->code), evt->value ? BIT(evt->code) : 0);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(buttons)), on_button, NULL);

int main(void)
{
	static const struct opensc2_config config = { .start = true };

	gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);
	return opensc2_init(&config);
}
