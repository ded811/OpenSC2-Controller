/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * fuel_rules.c: the fuel gauge's decisions (fuel_rules.h), following the protocol documentation's
 * description of the real controller's charge state machine. Each state has an entry (when it is
 * entered) and a run (once a pass, the one in effect); an entry may ask for a charge voltage.
 * Also the boost of the capacitive readings without input power (<opensc2/battery.h>).
 */
#include <string.h>
#include <opensc2/battery.h>
#include "fuel_rules.h"

static void enter(struct osc2_fuel *f, uint8_t state, int64_t now_ms, struct osc2_fuel_out *out)
{
	f->state = state;
	switch (state) {
	case OSC2_FUEL_IDLE:
		out->charge_mv = f->target_mv;
		break;
	case OSC2_FUEL_CHARGING:
		f->gave_up = false;
		break;
	case OSC2_FUEL_SUPPLY:
		f->timer_ms = now_ms + f->cfg.step_ms;
		f->vin_limit_mv = f->cfg.vin_start_mv;
		out->charge_mv = f->target_hi_mv;
		break;
	case OSC2_FUEL_DONE:
		f->gave_up = false;
		out->charge_mv = f->target_mv;
		break;
	default:
		break;   /* starting over: nothing on entry */
	}
	if (out->charge_mv) {
		f->charge_mv = out->charge_mv;
	}
}

void osc2_fuel_init(struct osc2_fuel *f, const struct osc2_fuel_cfg *cfg, struct osc2_fuel_out *out)
{
	memset(f, 0, sizeof(*f));
	memset(out, 0, sizeof(*out));
	f->cfg = *cfg;
	f->vin_limit_mv = cfg->vin_start_mv;
	osc2_fuel_default_target(f);
	f->state = OSC2_FUEL_START;
	out->charge_mv = f->target_mv;   /* the charger starts at the target */
	f->charge_mv = f->target_mv;
}

void osc2_fuel_set_target(struct osc2_fuel *f, uint16_t mv)
{
	f->target_mv = mv;
	f->target_hi_mv = (uint16_t)(mv + f->cfg.hysteresis_mv);
	f->state = OSC2_FUEL_START;
}

void osc2_fuel_default_target(struct osc2_fuel *f)
{
	osc2_fuel_set_target(f, f->cfg.target_mv);
}

bool osc2_fuel_charging(uint8_t charger)
{
	return charger == 1 || charger == 2;
}

static bool timer_over(const struct osc2_fuel *f, int64_t now_ms)
{
	return now_ms >= f->timer_ms;
}

void osc2_fuel_run(struct osc2_fuel *f, int64_t now_ms, bool input, uint8_t start_state,
		   struct osc2_fuel_out *out)
{
	memset(out, 0, sizeof(*out));
	if (!input && start_state != OSC2_FUEL_START) {
		f->state = OSC2_FUEL_START;   /* the input power went: start over */
	}
	switch (f->state) {
	case OSC2_FUEL_START:
		enter(f, OSC2_FUEL_IDLE, now_ms, out);
		f->timer_ms = now_ms;         /* a supply check may start at once */
		f->gave_up = false;
		break;
	case OSC2_FUEL_IDLE:
		if (f->voltage_mv >= f->target_mv) {
			enter(f, OSC2_FUEL_DONE, now_ms, out);
		} else if (input && timer_over(f, now_ms)) {
			enter(f, OSC2_FUEL_SUPPLY, now_ms, out);
		}
		break;
	case OSC2_FUEL_CHARGING:
		if (f->charger == 3) {
			enter(f, OSC2_FUEL_DONE, now_ms, out);
		} else if (!osc2_fuel_charging(f->charger)) {
			enter(f, OSC2_FUEL_IDLE, now_ms, out);
		}
		break;
	case OSC2_FUEL_SUPPLY:
		if (timer_over(f, now_ms)) {
			if (f->charger == 0) {
				/* writes the limit in effect, then lowers it for the next step */
				out->vin_limit_mv = f->vin_limit_mv;
				f->vin_limit_mv = f->vin_limit_mv > f->cfg.vin_step_mv ?
					(uint16_t)(f->vin_limit_mv - f->cfg.vin_step_mv) : 0;
			}
			f->timer_ms = now_ms + f->cfg.step_ms;
		}
		if (osc2_fuel_charging(f->charger)) {
			enter(f, OSC2_FUEL_CHARGING, now_ms, out);
		} else if (f->charger == 3) {
			enter(f, OSC2_FUEL_DONE, now_ms, out);
		} else if (f->vin_limit_mv <= f->cfg.vin_min_mv) {
			enter(f, OSC2_FUEL_IDLE, now_ms, out);
			f->timer_ms = now_ms + f->cfg.rest_ms;
			f->gave_up = true;
		}
		break;
	default:
		break;   /* done: stays until the input power goes or the target changes */
	}
}

uint8_t osc2_fuel_charge_state(const struct osc2_fuel *f)
{
	return (uint8_t)(f->state | (f->gave_up ? 0x80 : 0));
}

uint8_t osc2_fuel_keep(bool kept, uint8_t kept_level, uint8_t soc, bool charging)
{
	if (!kept) {
		return soc;
	}
	if (charging) {
		return soc > kept_level ? soc : kept_level;
	}
	return soc < kept_level ? soc : kept_level;
}

uint8_t osc2_fuel_model_charger(bool input, uint16_t average_mv, uint16_t charge_mv)
{
	if (!input) {
		return 0;
	}
	return average_mv < charge_mv ? 2 : 3;
}

int32_t osc2_fuel_window_cap(uint32_t lo_mv, uint32_t hi_mv)
{
	if (lo_mv == 0 && hi_mv == 0) {
		return -1;
	}
	if (lo_mv < 3400 || lo_mv % 10 != 0 || hi_mv > 4120 || hi_mv % 10 != 0) {
		return -1;
	}
	return (int32_t)hi_mv;
}

/* ---- the capacitive readings without input power (<opensc2/battery.h>) ---------------------- */

int32_t opensc2_cap_unpowered(enum opensc2_cap_kind kind, int32_t value)
{
	/* single precision, cut toward zero, as the real controller does; a word is stored back in
	 * 16 bits */
	switch (kind) {
	case OPENSC2_CAP_STICK_TOUCH:
		return (int16_t)(int32_t)((float)(int16_t)value * 1.2f);
	case OPENSC2_CAP_GRIP:
		return (int16_t)(int32_t)((float)(int16_t)value * 1.5f);
	case OPENSC2_CAP_ELECTRODE:
		return (int32_t)((float)value * 1.25f);
	default:
		return value;
	}
}

int32_t opensc2_cap_now(enum opensc2_cap_kind kind, int32_t value)
{
#if defined(CONFIG_OPENSC2_TOUCH_UNPOWERED_GAIN)
	if (!opensc2_input_power()) {
		return opensc2_cap_unpowered(kind, value);
	}
#else
	(void)kind;
#endif
	return value;
}
