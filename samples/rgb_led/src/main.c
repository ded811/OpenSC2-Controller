/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * An RGB LED like the controller's status LED, wired or wireless, with the same patterns and
 * colours as app/ (app/src/status.c):
 *  - with a host (wireless: connected through the puck; wired: set up by the PC) and Steam's LED
 *    override on (controller setting 37), the colour Steam sets with vendor command 0xC5 (red,
 *    green, blue, white in percent), scaled by the brightness Steam sets (controller setting 45);
 *  - otherwise the state: wireless in the radio's white-blue (quick double flashes while pairing,
 *    a slow blink while looking for the puck, a short blink every 2 s with no pairing, steady
 *    when connected, off with the radio off); wired, amber with a fast blink while no PC has set
 *    the board up, green with a short dark blink every 2 s once one has.
 * There is no power_request callback: the library does the reboots Steam asks for itself, and it
 * ignores Steam's request to enter a bootloader for a firmware update.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <opensc2/opensc2.h>

LOG_MODULE_REGISTER(rgb_led, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)

static const struct pwm_dt_spec rgb[3] = {
	PWM_DT_SPEC_GET_BY_NAME(USER, red),
	PWM_DT_SPEC_GET_BY_NAME(USER, green),
	PWM_DT_SPEC_GET_BY_NAME(USER, blue),
};
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

static void on_button(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == OPENSC2_KEY_PAIR) {
		pair_button(evt->value != 0);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(opensc2_buttons)), on_button, NULL);

/* ---- The LED --------------------------------------------------------------------------------- */

static bool led_ready;

static void set_colour(uint32_t r, uint32_t g, uint32_t b)   /* percent */
{
	const uint32_t pct[3] = { r, g, b };

	if (!led_ready) {
		return;
	}
	for (int i = 0; i < 3; i++) {
		pwm_set_pulse_dt(&rgb[i], rgb[i].period / 100 * MIN(pct[i], 100));
	}
}

/* The patterns, in ticks of 50 ms. */
static bool radio_pattern(enum opensc2_link_state link, uint32_t tick)
{
	switch (link) {
	case OPENSC2_LINK_PAIRING:
		return tick % 8 == 0 || tick % 8 == 2;   /* two quick flashes every 400 ms */
	case OPENSC2_LINK_LISTENING:
		return tick % 20 < 4;                    /* 200 ms every second */
	case OPENSC2_LINK_NO_BOND:
		return tick % 40 == 0;                   /* 50 ms every 2 s */
	case OPENSC2_LINK_CONNECTED:
		return true;
	default:
		return false;                            /* the radio is off */
	}
}

static bool usb_pattern(enum opensc2_usb_state usb, uint32_t tick)
{
	switch (usb) {
	case OPENSC2_USB_NO_HOST:
		return tick % 4 < 2;                     /* 100 ms on, 100 ms off */
	case OPENSC2_USB_CONFIGURED:
		return tick % 40 >= 2;                   /* on, 100 ms dark every 2 s */
	default:
		return false;                            /* the first 0.5 s on the bus */
	}
}

int main(void)
{
	struct opensc2_config config = { .start = true };
	uint32_t tick = 0;
	int err;

	gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);   /* a common anode is on VCC */
	led_ready = pwm_is_ready_dt(&rgb[0]) && pwm_is_ready_dt(&rgb[1]) &&
		    pwm_is_ready_dt(&rgb[2]);
	if (!led_ready) {
		LOG_ERR("The PWM outputs are not ready: no LED");
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

	/* Poll the state every 50 ms; the led callback could report Steam's colour too, but the
	 * patterns need a clock anyway. */
	for (;; tick++) {
		bool wired = opensc2_mode() == OPENSC2_MODE_WIRED;
		enum opensc2_usb_state usb = opensc2_usb_state();
		enum opensc2_link_state link = opensc2_link_state();
		bool host = wired ? usb == OPENSC2_USB_CONFIGURED : link == OPENSC2_LINK_CONNECTED;
		struct opensc2_led led;
		uint32_t level;

		opensc2_led_get(&led);
		level = led.brightness;   /* controller setting 45, 0..100 (50 by default) */
		if (host && led.override) {
			/* Steam's colour; the white channel is mixed into all three. */
			set_colour(MIN(led.red + led.white, 100) * level / 100,
				   MIN(led.green + led.white, 100) * level / 100,
				   MIN(led.blue + led.white, 100) * level / 100);
		} else if (!(wired ? usb_pattern(usb, tick) : radio_pattern(link, tick))) {
			set_colour(0, 0, 0);
		} else if (wired && host) {
			set_colour(0, 50 * level / 100, 0);                     /* green: on a PC */
		} else if (wired) {
			set_colour(40 * level / 100, 40 * level / 100, 0);      /* amber: no PC yet */
		} else {
			set_colour(80 * level / 100, 80 * level / 100, level);  /* the radio's white-blue */
		}
		k_msleep(50);
	}
	return 0;
}
