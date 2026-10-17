/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * An RGB LED like the controller's status LED:
 *  - while connected and Steam has its LED override on (controller setting 37), it shows the
 *    colour Steam sets with vendor command 0xC5 (red, green, blue, white in percent), scaled by
 *    the brightness Steam sets (controller setting 45);
 *  - otherwise it shows the link the way the real controller does for the 2.4 GHz link, in its
 *    white-blue: quick double flashes while pairing, a slow pulse while searching, steady when
 *    connected, off when the radio is off.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/input/input.h>
#include <opensc2/opensc2.h>

#define USER DT_PATH(zephyr_user)

static const struct pwm_dt_spec rgb[3] = {
	PWM_DT_SPEC_GET_BY_NAME(USER, red),
	PWM_DT_SPEC_GET_BY_NAME(USER, green),
	PWM_DT_SPEC_GET_BY_NAME(USER, blue),
};

static void set_colour(uint32_t r, uint32_t g, uint32_t b)   /* percent */
{
	const uint32_t pct[3] = { r, g, b };

	for (int i = 0; i < 3; i++) {
		pwm_set_pulse_dt(&rgb[i], rgb[i].period / 100 * MIN(pct[i], 100));
	}
}

static void on_button(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == OPENSC2_KEY_PAIR && evt->value) {
		opensc2_pair_start(0);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(buttons)), on_button, NULL);

int main(void)
{
	static const struct opensc2_config config = { .start = true };
	uint32_t tick = 0;

	for (int i = 0; i < 3; i++) {
		if (!pwm_is_ready_dt(&rgb[i])) {
			printk("PWM not ready\n");
			return 0;
		}
	}
	opensc2_init(&config);

	/* Poll the state every 50 ms; the led callback could do it too, but the status pattern
	 * needs a clock anyway. */
	for (;; tick++) {
		struct opensc2_led led;
		enum opensc2_link_state link = opensc2_link_state();
		bool on;

		opensc2_led_get(&led);
		if (link == OPENSC2_LINK_CONNECTED && led.override) {
			uint32_t w = led.white;

			set_colour(MIN(led.red + w, 100) * led.brightness / 100,
				   MIN(led.green + w, 100) * led.brightness / 100,
				   MIN(led.blue + w, 100) * led.brightness / 100);
		} else {
			switch (link) {
			case OPENSC2_LINK_PAIRING:
				on = tick % 8 == 0 || tick % 8 == 2;
				break;
			case OPENSC2_LINK_LISTENING:
				on = tick % 20 < 4;
				break;
			case OPENSC2_LINK_NO_BOND:
				on = tick % 40 == 0;
				break;
			case OPENSC2_LINK_CONNECTED:
				on = true;
				break;
			default:
				on = false;
				break;
			}
			if (on) {
				set_colour(40 * led.brightness / 100, 40 * led.brightness / 100,
					   50 * led.brightness / 100);
			} else {
				set_colour(0, 0, 0);
			}
		}
		k_msleep(50);
	}
	return 0;
}
