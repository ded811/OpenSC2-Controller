/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * charger.c: the charging rules of <opensc2/charger.h>, as the protocol documentation describes
 * the real controller's (not public yet; docs/protocol.md lists the pages). Pure logic.
 */
#include <string.h>
#include <opensc2/charger.h>
#include <opensc2/types.h>

void opensc2_charger_defaults(struct opensc2_charger_config *cfg)
{
	cfg->target_mv = 4120;
	cfg->hysteresis_mv = 40;
	cfg->vin_start_mv = 4600;
	cfg->vin_step_mv = 100;
	cfg->vin_min_mv = 4300;
	cfg->vin_step_ms = 5000;
	cfg->retry_ms = 60000;
}

static bool due(const struct opensc2_charger *c, uint32_t now_ms)
{
	return (int32_t)(now_ms - c->timer_at) >= 0;
}

static void set_charge(struct opensc2_charger *c, bool raised, struct opensc2_charger_out *out)
{
	uint16_t mv = (uint16_t)(c->charge_mv + (raised ? c->cfg.hysteresis_mv : 0));

	out->set_charge = true;
	out->charge_mv = mv;
}

/* Entering a state: what the real controller does on the way in. */
static void enter(struct opensc2_charger *c, uint8_t state, uint32_t now_ms,
		  struct opensc2_charger_out *out)
{
	c->state = state;
	switch (state) {
	case OPENSC2_CHARGE_DISCHARGING:
		set_charge(c, false, out);
		break;
	case OPENSC2_CHARGE_SRC_VALIDATE:
		c->timer_at = now_ms + c->cfg.vin_step_ms;
		c->vin_limit_mv = c->cfg.vin_start_mv;   /* written at the first step, 5 s from now */
		set_charge(c, true, out);
		break;
	case OPENSC2_CHARGE_CHARGING:
		c->gave_up = false;
		break;
	case OPENSC2_CHARGE_DONE:
		c->gave_up = false;
		set_charge(c, false, out);
		break;
	default:
		break;
	}
}

void opensc2_charger_init(struct opensc2_charger *c, const struct opensc2_charger_config *cfg,
			  uint32_t now_ms, struct opensc2_charger_out *out)
{
	memset(c, 0, sizeof(*c));
	memset(out, 0, sizeof(*out));
	if (cfg) {
		c->cfg = *cfg;
	} else {
		opensc2_charger_defaults(&c->cfg);
	}
	c->charge_mv = c->cfg.target_mv;
	c->vin_limit_mv = c->cfg.vin_start_mv;
	c->timer_at = now_ms;
	c->state = OPENSC2_CHARGE_RESET;
	set_charge(c, false, out);
}

void opensc2_charger_window(struct opensc2_charger *c, uint16_t mv)
{
	if (mv == c->window_mv) {
		return;
	}
	c->window_mv = mv;
	c->charge_mv = mv ? mv : c->cfg.target_mv;
	c->state = OPENSC2_CHARGE_RESET;   /* starts over; the next step sets the voltage */
}

uint8_t opensc2_charger_gauge_state(const struct opensc2_charger *c,
				    enum opensc2_charger_status status)
{
	if (c->state == OPENSC2_CHARGE_DONE) {
		return OPENSC2_CHARGE_DONE;
	}
	if (status == OPENSC2_CHARGER_PRECHARGE || status == OPENSC2_CHARGER_CHARGING) {
		return OPENSC2_CHARGE_CHARGING;
	}
	return OPENSC2_CHARGE_DISCHARGING;
}

uint8_t opensc2_charger_step(struct opensc2_charger *c, bool input_power,
			     enum opensc2_charger_status status, uint16_t battery_mv,
			     uint32_t now_ms, struct opensc2_charger_out *out)
{
	bool charging;

	if (status == OPENSC2_CHARGER_UNKNOWN) {
		status = OPENSC2_CHARGER_OFF;   /* no status to go by: these rules take it as off */
	}
	charging = status == OPENSC2_CHARGER_PRECHARGE ||
			status == OPENSC2_CHARGER_CHARGING;

	memset(out, 0, sizeof(*out));
	if (!input_power && c->state != OPENSC2_CHARGE_RESET) {
		c->state = OPENSC2_CHARGE_RESET;   /* the power went: start over */
	}
	switch (c->state) {
	case OPENSC2_CHARGE_RESET:
		enter(c, OPENSC2_CHARGE_DISCHARGING, now_ms, out);
		c->timer_at = now_ms;   /* a supply may be validated at once */
		c->gave_up = false;
		break;
	case OPENSC2_CHARGE_DISCHARGING:
		if (battery_mv >= c->charge_mv) {
			enter(c, OPENSC2_CHARGE_DONE, now_ms, out);
		} else if (input_power && due(c, now_ms)) {
			enter(c, OPENSC2_CHARGE_SRC_VALIDATE, now_ms, out);
		}
		break;
	case OPENSC2_CHARGE_CHARGING:
		if (status == OPENSC2_CHARGER_DONE) {
			enter(c, OPENSC2_CHARGE_DONE, now_ms, out);
		} else if (!charging) {
			enter(c, OPENSC2_CHARGE_DISCHARGING, now_ms, out);
		}
		break;
	case OPENSC2_CHARGE_SRC_VALIDATE:
		if (due(c, now_ms)) {
			if (status == OPENSC2_CHARGER_OFF) {
				/* The supply sags under the charge current: accept a lower voltage. */
				out->set_vin_limit = true;
				out->vin_limit_mv = c->vin_limit_mv;
				c->vin_limit_mv = (uint16_t)(c->vin_limit_mv - c->cfg.vin_step_mv);
			}
			c->timer_at = now_ms + c->cfg.vin_step_ms;
		}
		if (charging) {
			enter(c, OPENSC2_CHARGE_CHARGING, now_ms, out);
		} else if (status == OPENSC2_CHARGER_DONE) {
			enter(c, OPENSC2_CHARGE_DONE, now_ms, out);
		} else if (c->vin_limit_mv <= c->cfg.vin_min_mv) {
			enter(c, OPENSC2_CHARGE_DISCHARGING, now_ms, out);
			c->timer_at = now_ms + c->cfg.retry_ms;
			c->gave_up = true;
		}
		break;
	default:   /* done: until the power goes */
		break;
	}
	return opensc2_charger_state(c);
}

uint8_t opensc2_charger_state(const struct opensc2_charger *c)
{
	return (uint8_t)(c->state | (c->gave_up ? OPENSC2_CHARGE_GAVE_UP : 0));
}
