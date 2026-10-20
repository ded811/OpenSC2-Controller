/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * An RGB LED like the controller's status LED, wired or wireless, with the same patterns and
 * colours as app/ (app/src/status.c): the library's system state machine selects the real
 * controller's LED mode (opensc2_led_status(), on by default), and the library's pattern player
 * (<opensc2/led.h>) turns it into a colour every 10 ms with the real controller's patterns and
 * fades: white with a little blue for the puck (a slow pulse while looking for it, steady once
 * connected), green while wired, amber on USB power without a PC or with wireless off, and Steam's
 * own colour (vendor command 0xC5) while Steam's override is on (controller setting 37). Each
 * channel is scaled by the unit's trim (cal/rgbw_*, opensc2_cal_led_trim()) and by the brightness
 * Steam sets (settings 45 and 38); the white channel is mixed into red, green and blue.
 * There is no power_request callback: the library does the reboots Steam asks for itself, and it
 * ignores Steam's request to enter a bootloader for a firmware update.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <opensc2/opensc2.h>
#include <opensc2/cal.h>
#include <opensc2/led.h>

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
static struct opensc2_led_player player;

static void set_duty(const struct pwm_dt_spec *ch, uint32_t duty)   /* thousandths */
{
	duty = MIN(duty, OPENSC2_LED_FULL_SCALE);
	(void)pwm_set_pulse_dt(ch, (uint32_t)((uint64_t)ch->period * duty / OPENSC2_LED_FULL_SCALE));
}

/* The LED's mode. With the system state machine (the default) the library's; without it a plain
 * one from the mode, the link and USB, as app/src/status.c does. */
static void get_status(struct opensc2_led_status *st)
{
	opensc2_led_status(st);
#if !defined(CONFIG_OPENSC2_SYSTEM)
	if (opensc2_mode() == OPENSC2_MODE_WIRED) {
		st->mode = opensc2_usb_state() == OPENSC2_USB_CONFIGURED ? OPENSC2_LED_MODE_WIRED :
			   OPENSC2_LED_MODE_AMBER;
	} else {
		st->mode = OPENSC2_LED_MODE_PUCK;
		st->connected = opensc2_link_state() == OPENSC2_LINK_CONNECTED;
		st->pairing = opensc2_link_state() == OPENSC2_LINK_PAIRING;
		st->external_power = true;
	}
#endif
}

/* One step of the player, every OPENSC2_LED_STEP_MS: the colour in thousandths of full duty
 * (R, G, B, W), then each channel's duty with its trim and Steam's brightness. */
static void led_step(void)
{
	struct opensc2_led_status st;
	uint16_t c[4];
	uint32_t white;

	get_status(&st);
	opensc2_led_player_step(&player, &st, k_uptime_get_32(), c);
	if (!led_ready) {
		return;
	}
	white = opensc2_led_duty(c[3], opensc2_cal_led_trim(3), st.brightness);
	for (int i = 0; i < 3; i++) {
		set_duty(&rgb[i], opensc2_led_duty(c[i], opensc2_cal_led_trim((uint8_t)i),
						   st.brightness) + white);
	}
}

int main(void)
{
	struct opensc2_config config = { .start = true };
	int err;

	gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);   /* a common anode is on VCC */
	led_ready = pwm_is_ready_dt(&rgb[0]) && pwm_is_ready_dt(&rgb[1]) &&
		    pwm_is_ready_dt(&rgb[2]);
	if (!led_ready) {
		LOG_ERR("The PWM outputs are not ready: no LED");
	}
	opensc2_led_player_init(&player);

	if (pair_held()) {
		pair_held_at_boot = true;
		config.boot_mode = OPENSC2_BOOT_WIRELESS;   /* held while plugging in: wireless */
	}
	err = opensc2_init(&config);
	if (err) {
		/* The console still starts (in wireless mode), so this can be read there. */
		LOG_ERR("opensc2_init failed (%d)", err);
	}

	/* The player needs a clock: step it every 10 ms. The library's led_status callback tells of
	 * a new mode too, but the patterns move between calls anyway. */
	for (;;) {
		led_step();
		k_msleep(OPENSC2_LED_STEP_MS);
	}
	return 0;
}
