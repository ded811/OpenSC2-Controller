/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * rumble.c: one vibration motor (or an LRA driver's PWM input) for everything Steam plays on the
 * controller's four haptic actuators, if zephyr,user has a PWM channel named "rumble". A simple
 * mapping, since one motor cannot play tones:
 *   rumble (0x80)          duty = the stronger of the two speeds, until Steam stops repeating it
 *   click (0x82)           a 20 ms (click A) or 35 ms (click B) buzz
 *   pulse (0x81)           buzzes of the pulse's on time, `count` times (at most one second)
 *   tone, sweep (0x83/84)  a buzz for the duration, its strength from the gain
 *   script (0x85)          a 120 ms buzz
 *   PCM stream             ignored
 * Without a "rumble" channel every function here does nothing.
 */
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include "app.h"

#define USER DT_PATH(zephyr_user)

#if DT_PROP_HAS_NAME(USER, pwms, rumble)

static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET_BY_NAME(USER, rumble);
static uint32_t rumble_level;   /* 0..65535, from report 0x80 */
static uint32_t buzz_level;     /* 0..65535, from the haptic effects */
static uint16_t pulses_left, pulse_on_ms, pulse_off_ms;
static bool pulse_phase_on;

static void apply(void)
{
	uint32_t level = MAX(rumble_level, buzz_level);

	if (pwm_is_ready_dt(&motor)) {
		pwm_set_pulse_dt(&motor, (uint32_t)((uint64_t)motor.period * level / 65535));
	}
}

static void buzz_end_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(buzz_work, buzz_end_fn);

static void buzz_end_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (pulses_left) {
		/* Next half of a pulse train. */
		pulse_phase_on = !pulse_phase_on;
		if (!pulse_phase_on) {
			pulses_left--;
		}
		buzz_level = pulse_phase_on ? 65535 : 0;
		apply();
		if (pulses_left) {
			k_work_schedule(&buzz_work, K_MSEC(pulse_phase_on ? pulse_on_ms : pulse_off_ms));
			return;
		}
	}
	buzz_level = 0;
	apply();
}

static void buzz(uint32_t level, uint32_t ms)
{
	pulses_left = 0;
	buzz_level = MIN(level, 65535);
	apply();
	k_work_reschedule(&buzz_work, K_MSEC(MAX(ms, 1)));
}

/* Gain in dB (0 dB = full) to a level. */
static uint32_t level_from_db(int8_t db)
{
	return (uint32_t)(65535.0f * MIN(powf(10.0f, (float)db / 20.0f), 1.0f));
}

void rumble_set(const struct opensc2_rumble *r)
{
	rumble_level = MAX(r->left_speed, r->right_speed);
	apply();
}

void rumble_haptic(const struct opensc2_haptic *h)
{
	switch (h->kind) {
	case OPENSC2_HAPTIC_COMMAND:
		if (h->command.command == OPENSC2_HAPTIC_CMD_STOP_ALL) {
			buzz(0, 1);
		} else {
			buzz(level_from_db(h->command.gain_db),
			     h->command.command == OPENSC2_HAPTIC_CMD_CLICK_A ? 20 : 35);
		}
		break;
	case OPENSC2_HAPTIC_PULSE:
		if (h->pulse.on_us == 0 || h->pulse.count == 0) {
			buzz(0, 1);
			break;
		}
		pulse_on_ms = MAX(h->pulse.on_us / 1000, 5);
		pulse_off_ms = MAX(h->pulse.off_us / 1000, 5);
		pulses_left = MIN(h->pulse.count, 1000 / (pulse_on_ms + pulse_off_ms) + 1);
		pulse_phase_on = true;
		buzz_level = 65535;
		apply();
		k_work_reschedule(&buzz_work, K_MSEC(pulse_on_ms));
		break;
	case OPENSC2_HAPTIC_TONE:
		buzz(level_from_db(h->tone.gain_db), MIN(h->tone.duration_ms, 2000));
		break;
	case OPENSC2_HAPTIC_SWEEP:
		buzz(level_from_db(h->sweep.gain_db), MIN(h->sweep.duration_ms, 2000));
		break;
	case OPENSC2_HAPTIC_SCRIPT:
		buzz(level_from_db(h->script.gain_db), 120);
		break;
	default:
		break;   /* PCM streams need a real actuator driver */
	}
}

int rumble_init(void)
{
	if (!pwm_is_ready_dt(&motor)) {
		return -ENODEV;
	}
	return pwm_set_pulse_dt(&motor, 0);
}

#else /* no "rumble" PWM channel */

void rumble_set(const struct opensc2_rumble *r)
{
	ARG_UNUSED(r);
}

void rumble_haptic(const struct opensc2_haptic *h)
{
	ARG_UNUSED(h);
}

int rumble_init(void)
{
	return 0;
}

#endif
