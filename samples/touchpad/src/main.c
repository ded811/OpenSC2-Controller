/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * A touch module as the right trackpad, wired or wireless. The touch device reports absolute X and
 * Y and either a touch key or a touch strength (Z) through Zephyr's input subsystem; on each
 * complete report the position is scaled to -32767..32767 (Y flipped: up is positive on the
 * controller) and handed to the library. A separate button is the pad's click. In Steam the right
 * pad then works as a trackpad: mouse, camera, or whatever its configuration says.
 * There is no power_request callback: the library does the reboots Steam asks for itself, and it
 * ignores Steam's request to enter a bootloader for a firmware update.
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <opensc2/opensc2.h>

LOG_MODULE_REGISTER(touchpad, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)

static const int32_t width = DT_PROP_BY_IDX(USER, trackpad_size, 0);
static const int32_t height = DT_PROP_BY_IDX(USER, trackpad_size, 1);
static const struct gpio_dt_spec vcc = GPIO_DT_SPEC_GET(USER, vcc_enable_gpios);

/* The module runs from the board's switched VCC, and its driver talks to it while Zephyr starts,
 * before main(). So VCC goes on first: GPIO is ready by then (PRE_KERNEL_1), and the touch driver
 * starts later in POST_KERNEL (CONFIG_INPUT_INIT_PRIORITY, 90). */
static int vcc_on(void)
{
	if (gpio_is_ready_dt(&vcc)) {
		gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);
		k_msleep(100);   /* a generous power-up time for the module */
	}
	return 0;
}
SYS_INIT(vcc_on, POST_KERNEL, 0);

/* ---- The pairing button: the same rules in every sample ------------------------------------- */

static const struct gpio_dt_spec pair_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(opensc2_pair), gpios);
static bool pair_held_at_boot;   /* the press that chose wireless: ignored until it is released */
static bool pair_down;

/* Is the button held while the board starts? gpio-keys has already set the pin up with its
 * pull-up. Read twice, 5 ms apart: a bouncing contact counts as released. */
static bool pair_held(void)
{
	if (!gpio_is_ready_dt(&pair_pin) || gpio_pin_get_dt(&pair_pin) != 1) {
		return false;
	}
	k_msleep(5);
	return gpio_pin_get_dt(&pair_pin) == 1;
}

/* Wired: pressed and released, reboot into wireless mode (on the release, so that the button is
 * up again at the next boot). Wireless: pressed, answer a puck's open pairing window (untested
 * with Steam, which pairs controllers over the cable). */
static void pair_button(bool pressed)
{
	if (pair_held_at_boot) {
		pair_held_at_boot = pressed;   /* released: a normal button from now on */
		return;
	}
	if (opensc2_mode() == OPENSC2_MODE_WIRED) {
		if (pair_down && !pressed) {
			opensc2_reboot_later(OPENSC2_REBOOT_WIRELESS, "pair button", 0);
		}
	} else if (pressed) {
		opensc2_pair_start(0);   /* 0: the default window, 60 s */
	}
	pair_down = pressed;
}

/* ---- The trackpad ---------------------------------------------------------------------------- */

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

/* ---- Buttons: each key's code is the bit to set (the pad click is bit 22) -------------------- */

static void on_button(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->code == OPENSC2_KEY_PAIR) {
		pair_button(evt->value != 0);
	} else if (evt->code < 32) {
		opensc2_set_buttons(BIT(evt->code), evt->value ? BIT(evt->code) : 0);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(opensc2_buttons)), on_button, NULL);

int main(void)
{
	struct opensc2_config config = { .start = true };
	int err;

	if (!device_is_ready(DEVICE_DT_GET(DT_NODELABEL(touch)))) {
		LOG_ERR("The touch module did not start: check its wiring and that it is set for I2C");
	}

	if (pair_held()) {
		pair_held_at_boot = true;
		config.boot_mode = OPENSC2_BOOT_WIRELESS;   /* held while plugging in: wireless */
	}
	err = opensc2_init(&config);
	if (err) {
		/* The console still starts (in wireless mode), so this can be read there. */
		LOG_ERR("opensc2_init failed (%d)", err);
	}
	return 0;
}
