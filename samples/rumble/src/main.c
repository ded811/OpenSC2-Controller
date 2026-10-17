/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Rumble and haptics on one PWM-driven vibration motor. The real controller has four actuators
 * that play tones; a motor can only buzz, so:
 *   rumble (report 0x80): duty cycle = the stronger of the two motor speeds. Steam repeats the
 *       report while it wants rumble; the library sends speed 0 when it stops (or the link drops).
 *   clicks, pulses, tones, sweeps and scripts (0x81..0x85): a short buzz.
 * The callbacks run in the library's radio thread, so they only set the PWM and schedule work.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/input/input.h>
#include <opensc2/opensc2.h>

static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), rumble);
static uint32_t rumble_level, buzz_level;   /* 0..65535 */

static void apply(void)
{
	uint32_t level = MAX(rumble_level, buzz_level);

	pwm_set_pulse_dt(&motor, (uint32_t)((uint64_t)motor.period * level / 65535));
}

static void buzz_off(struct k_work *work)
{
	ARG_UNUSED(work);
	buzz_level = 0;
	apply();
}
static K_WORK_DELAYABLE_DEFINE(buzz_work, buzz_off);

static void buzz(uint32_t ms)
{
	buzz_level = 52000;   /* about 80 % */
	apply();
	k_work_reschedule(&buzz_work, K_MSEC(CLAMP(ms, 10, 1000)));
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
		if (h->command.command != OPENSC2_HAPTIC_CMD_STOP_ALL) {
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
	static const struct opensc2_callbacks callbacks = {
		.rumble = on_rumble,
		.haptic = on_haptic,
	};
	static const struct opensc2_config config = { .callbacks = &callbacks, .start = true };

	if (!pwm_is_ready_dt(&motor)) {
		printk("PWM not ready\n");
		return 0;
	}
	pwm_set_pulse_dt(&motor, 0);
	return opensc2_init(&config);
}
