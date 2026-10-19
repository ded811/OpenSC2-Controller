/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The smallest OpenSC2 application: a Steam Controller 2 with one button (A) and a pairing button,
 * both read by one loop in main().
 *
 * Plugged into a PC it comes up wired, and Steam can add it over the cable: Steam pairs it with
 * its puck and reboots it into wireless mode, and it then connects through the puck. The pairing
 * button:
 *  - held while plugging in: this boot is wireless (that press is ignored until it is released);
 *  - pressed and released while wired: reboot into wireless mode (on the release, so that the
 *    button is up again at the next boot);
 *  - pressed while wireless: answer a puck whose pairing window is open over the air (untested
 *    with Steam, which pairs controllers over the cable).
 * There is no power_request callback: the library does the reboots Steam asks for itself, and it
 * ignores Steam's request to enter a bootloader for a firmware update.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <opensc2/opensc2.h>

LOG_MODULE_REGISTER(minimal, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)

/* A button read every 5 ms by the loop below. Its state changes only when two reads in a row
 * agree: a contact bounces for a few milliseconds when it closes or opens. */
struct button {
	struct gpio_dt_spec pin;
	bool last_read;
	bool down;
};

static struct button button_a = { .pin = GPIO_DT_SPEC_GET(USER, a_gpios) };
static struct button button_pair = { .pin = GPIO_DT_SPEC_GET(USER, pair_gpios) };

static bool button_poll(struct button *b)
{
	bool now = gpio_pin_get_dt(&b->pin) == 1;

	if (now == b->last_read) {
		b->down = now;
	}
	b->last_read = now;
	return b->down;
}

int main(void)
{
	struct opensc2_config config = { .start = true };
	bool pair_held_at_boot;
	int err;

	gpio_pin_configure_dt(&button_a.pin, GPIO_INPUT);
	gpio_pin_configure_dt(&button_pair.pin, GPIO_INPUT);
	k_msleep(1);   /* let the pull-ups settle */

	/* Held while plugging in, read twice 5 ms apart: wireless this boot. */
	button_poll(&button_pair);
	k_msleep(5);
	pair_held_at_boot = button_poll(&button_pair);
	if (pair_held_at_boot) {
		config.boot_mode = OPENSC2_BOOT_WIRELESS;
	}

	err = opensc2_init(&config);
	if (err) {
		/* The console still starts (in wireless mode), so this can be read there. */
		LOG_ERR("opensc2_init failed (%d)", err);
	}

	for (;;) {
		bool pair_was_down = button_pair.down;
		bool pair_down = button_poll(&button_pair);

		if (pair_held_at_boot) {
			pair_held_at_boot = pair_down;   /* released: a normal button from now on */
		} else if (opensc2_mode() == OPENSC2_MODE_WIRED) {
			if (pair_was_down && !pair_down) {
				opensc2_reboot_later(OPENSC2_REBOOT_WIRELESS, "pair button", 0);
			}
		} else if (pair_down && !pair_was_down) {
			opensc2_pair_start(0);   /* 0: the default window, 60 s */
		}

		opensc2_set_buttons(OPENSC2_BTN_A, button_poll(&button_a) ? OPENSC2_BTN_A : 0);
		k_msleep(5);
	}
	return 0;
}
