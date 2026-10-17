/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The smallest OpenSC2 application: a Steam Controller 2 with one button (A). Press the pairing
 * button while Steam has the puck's pairing window open; after that the board reconnects by itself.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <opensc2/opensc2.h>

static const struct gpio_dt_spec button_a = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), a_gpios);
static const struct gpio_dt_spec button_pair = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pair_gpios);

int main(void)
{
	static const struct opensc2_config config = { .start = true };
	bool pair_was_down = false;

	gpio_pin_configure_dt(&button_a, GPIO_INPUT);
	gpio_pin_configure_dt(&button_pair, GPIO_INPUT);
	opensc2_init(&config);

	for (;;) {
		bool pair_down = gpio_pin_get_dt(&button_pair) == 1;

		if (pair_down && !pair_was_down) {
			opensc2_pair_start(0);
		}
		pair_was_down = pair_down;
		opensc2_set_buttons(OPENSC2_BTN_A, gpio_pin_get_dt(&button_a) == 1 ? OPENSC2_BTN_A : 0);
		k_msleep(5);
	}
	return 0;
}
