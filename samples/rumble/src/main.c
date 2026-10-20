/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Rumble and haptics on one PWM-driven vibration motor, wired or wireless. The real controller has
 * four actuators that play tones; a motor can only buzz, so:
 *   rumble (report 0x80): duty cycle = the stronger of the two motor speeds. Steam repeats the
 *       report while it wants rumble; the library sends speed 0 when it stops (or the link drops).
 *   clicks, pulses, tones, sweeps and scripts (0x81..0x85): a short buzz; "stop all" stops it.
 *   PCM streams (0x86..0x89): ignored, a motor cannot play samples.
 * The callbacks run in the library's radio thread (wireless) or in the USB stack's thread (wired;
 * lizard mode's own haptic events, its pad ticks and clicks, in the USB report thread), so they
 * only set the PWM and schedule work.
 * There is no power_request callback: the library does the reboots Steam asks for itself, and it
 * ignores Steam's request to enter a bootloader for a firmware update.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <opensc2/opensc2.h>

LOG_MODULE_REGISTER(rumble, LOG_LEVEL_INF);

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

/* ---- The motor ------------------------------------------------------------------------------- */

static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), rumble);
static const struct gpio_dt_spec vcc = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), vcc_enable_gpios);
static uint32_t rumble_level, buzz_level;   /* 0..65535 */

static void apply(void)
{
	uint32_t level = MAX(rumble_level, buzz_level);

	if (pwm_is_ready_dt(&motor)) {
		pwm_set_pulse_dt(&motor, (uint32_t)((uint64_t)motor.period * level / 65535));
	}
}

static void buzz_off(struct k_work *work)
{
	ARG_UNUSED(work);
	buzz_level = 0;
	apply();
}
static K_WORK_DELAYABLE_DEFINE(buzz_work, buzz_off);

/* A buzz of `ms` milliseconds (10 to 1000); 0 stops a buzz at once. */
static void buzz(uint32_t ms)
{
	if (ms == 0) {
		(void)k_work_cancel_delayable(&buzz_work);
		buzz_level = 0;
	} else {
		buzz_level = 52000;   /* about 80 % */
		k_work_reschedule(&buzz_work, K_MSEC(CLAMP(ms, 10, 1000)));
	}
	apply();
}

static void on_rumble(const struct opensc2_rumble *r, void *user_data)
{
	ARG_UNUSED(user_data);
	rumble_level = MAX(r->left_speed, r->right_speed);
	apply();
}

static void on_haptic(const struct opensc2_haptic *h, void *user_data)
{
	ARG_UNUSED(user_data);
	switch (h->kind) {
	case OPENSC2_HAPTIC_COMMAND:
		if (h->command.command == OPENSC2_HAPTIC_CMD_STOP_ALL) {
			buzz(0);
		} else {
			buzz(h->command.command == OPENSC2_HAPTIC_CMD_CLICK_A ? 20 : 35);
		}
		break;
	case OPENSC2_HAPTIC_PULSE:
		buzz((uint32_t)h->pulse.count * (h->pulse.on_us + h->pulse.off_us) / 1000);
		break;
	case OPENSC2_HAPTIC_TONE:
		buzz(h->tone.duration_ms);
		break;
	case OPENSC2_HAPTIC_SWEEP:
		buzz(h->sweep.duration_ms);
		break;
	case OPENSC2_HAPTIC_SCRIPT:
		buzz(120);
		break;
	default:
		break;   /* PCM streams need an actuator that can play samples */
	}
}

int main(void)
{
	static const struct opensc2_callbacks callbacks = {
		.rumble = on_rumble,
		.haptic = on_haptic,
	};
	struct opensc2_config config = { .callbacks = &callbacks, .start = true };
	int err;

	gpio_pin_configure_dt(&vcc, GPIO_OUTPUT_ACTIVE);   /* for a motor or driver on VCC */
	if (pwm_is_ready_dt(&motor)) {
		pwm_set_pulse_dt(&motor, 0);   /* motor off */
	} else {
		LOG_ERR("The PWM output is not ready: no rumble");
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
