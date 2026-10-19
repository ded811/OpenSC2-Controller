/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Buttons, a stick and a trigger from the devicetree, wired or wireless:
 *  - a gpio-keys node (opensc2_buttons) whose zephyr,code values are bits of the controller's
 *    button word, read through Zephyr's input subsystem;
 *  - ADC channels named in zephyr,user ("stick-x", "stick-y", "trigger"), read in main();
 *  - the pairing button, with the same rules in every sample (pair_button() below).
 * The full version (every input, auto-ranging, unwired pins held neutral) is app/src/inputs.c.
 * There is no power_request callback: the library does the reboots Steam asks for itself, and it
 * ignores Steam's request to enter a bootloader for a firmware update.
 */
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <opensc2/opensc2.h>

LOG_MODULE_REGISTER(buttons_sticks, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)

static const struct adc_dt_spec stick_x = ADC_DT_SPEC_GET_BY_NAME(USER, stick_x);
static const struct adc_dt_spec stick_y = ADC_DT_SPEC_GET_BY_NAME(USER, stick_y);
static const struct adc_dt_spec trigger = ADC_DT_SPEC_GET_BY_NAME(USER, trigger);
static const struct gpio_dt_spec vcc = GPIO_DT_SPEC_GET(USER, vcc_enable_gpios);

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

/* ---- Buttons: each key's code is the bit to set ---------------------------------------------- */

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

/* ---- The stick and the trigger --------------------------------------------------------------- */

static int32_t read_adc(const struct adc_dt_spec *spec)
{
	int16_t sample = 0;
	struct adc_sequence seq = { .buffer = &sample, .buffer_size = sizeof(sample) };

	adc_sequence_init_dt(spec, &seq);
	return adc_read_dt(spec, &seq) == 0 ? MAX(sample, 0) : 2048;
}

/* 12-bit reading to -32767..32767 around `centre`: full output 1600 counts away, 100 counts of
 * dead zone in the middle. */
static int16_t to_axis(int32_t raw, int32_t centre)
{
	int32_t d = raw - centre;
	int32_t mag = CLAMP(abs(d) - 100, 0, 1500);

	return (int16_t)((d < 0 ? -mag : mag) * OPENSC2_AXIS_MAX / 1500);
}

int main(void)
{
	struct opensc2_config config = { .start = true };
	int32_t centre_x, centre_y, rest;
	int err;

	gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);   /* power the potentiometers */
	adc_channel_setup_dt(&stick_x);
	adc_channel_setup_dt(&stick_y);
	adc_channel_setup_dt(&trigger);
	k_msleep(20);
	centre_x = read_adc(&stick_x);   /* the stick rests in the middle at power-on */
	centre_y = read_adc(&stick_y);
	rest = read_adc(&trigger);       /* and the trigger at its released end */

	if (pair_held()) {
		pair_held_at_boot = true;
		config.boot_mode = OPENSC2_BOOT_WIRELESS;   /* held while plugging in: wireless */
	}
	err = opensc2_init(&config);
	if (err) {
		/* The console still starts (in wireless mode), so this can be read there. */
		LOG_ERR("opensc2_init failed (%d)", err);
	}

	for (;;) {
		int32_t t = CLAMP(read_adc(&trigger) - rest - 80, 0, 3200);

		opensc2_set_stick(OPENSC2_LEFT, to_axis(read_adc(&stick_x), centre_x),
				  to_axis(read_adc(&stick_y), centre_y));
		opensc2_set_trigger(OPENSC2_RIGHT, (uint16_t)(t * OPENSC2_TRIGGER_MAX / 3200));
		k_msleep(4);
	}
	return 0;
}
