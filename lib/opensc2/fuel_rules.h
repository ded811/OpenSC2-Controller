/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * fuel_rules.h: the decisions of the library's fuel gauge (battery.c, <opensc2/battery.h>), as
 * the protocol documentation describes the real controller's: the charge state machine with its
 * supply check, the level's direction rule and the charger model of a board without a charger
 * status. Pure logic on a caller's struct, with the time passed in; the host tests build it.
 */
#ifndef OPENSC2_FUEL_RULES_H_
#define OPENSC2_FUEL_RULES_H_

#include <stdbool.h>
#include <stdint.h>

/* The states; their index is the charge state report 0x43 carries (enum opensc2_charge_state). */
enum osc2_fuel_state {
	OSC2_FUEL_START = 0,      /* starting over: on to "not charging" at the next run */
	OSC2_FUEL_IDLE = 1,       /* not charging */
	OSC2_FUEL_CHARGING = 2,
	OSC2_FUEL_SUPPLY = 3,     /* checking the supply: the input-voltage limit walks down */
	OSC2_FUEL_DONE = 4,       /* full: level 100 until the input power goes */
};

struct osc2_fuel_cfg {
	uint16_t target_mv;       /* the charge voltage target (4120) */
	uint16_t hysteresis_mv;   /* added during the supply check and a charge (40) */
	uint16_t vin_start_mv;    /* the input-voltage limit the check starts from (4600) */
	uint16_t vin_step_mv;     /* lowered by this (100) ... */
	uint16_t vin_min_mv;      /* ... until it reaches this, then the check gives up (4300) */
	uint32_t step_ms;         /* time per step (5000) */
	uint32_t rest_ms;         /* rest after a give-up (60000) */
};

struct osc2_fuel {
	struct osc2_fuel_cfg cfg;
	uint8_t state;            /* enum osc2_fuel_state */
	bool gave_up;             /* the supply check gave up: 0x80 in the charge state */
	int64_t timer_ms;         /* the supply check's next step, or the end of the rest */
	uint16_t vin_limit_mv;    /* the limit the next step writes */
	uint16_t target_mv;       /* the charge voltage target in effect ... */
	uint16_t target_hi_mv;    /* ... and with the hysteresis */
	uint8_t charger;          /* the last charger status (enum opensc2_charger_status, 0..3) */
	uint16_t voltage_mv;      /* the battery's average, as reported (0 before the first) */
	uint16_t charge_mv;       /* the charge voltage last asked for (0: none yet) */
};

/* The writes a step asks the charger for, in this order; 0: none. */
struct osc2_fuel_out {
	uint16_t vin_limit_mv;
	uint16_t charge_mv;
};

/* Set up in state 0 with the target in effect; asks for the target as the charge voltage. */
void osc2_fuel_init(struct osc2_fuel *f, const struct osc2_fuel_cfg *cfg, struct osc2_fuel_out *out);
/* A new charge voltage target, taken as it is (the window's cap; the public setter checks its
 * range), or the configured one: the machine starts over (state 0). */
void osc2_fuel_set_target(struct osc2_fuel *f, uint16_t mv);
void osc2_fuel_default_target(struct osc2_fuel *f);
/* The machine's part of a pass: without input power it starts over unless the pass began in
 * state 0 (`start_state`), then the state in effect runs once. */
void osc2_fuel_run(struct osc2_fuel *f, int64_t now_ms, bool input, uint8_t start_state,
		   struct osc2_fuel_out *out);

/* The charger charges (precharge or charge). */
bool osc2_fuel_charging(uint8_t charger);
/* Report 0x43's charge state: the state, with 0x80 while the supply check has given up. */
uint8_t osc2_fuel_charge_state(const struct osc2_fuel *f);

/* The level kept after a reading's level `soc`: the first one as it is, then charging only
 * higher, otherwise only lower. */
uint8_t osc2_fuel_keep(bool kept, uint8_t kept_level, uint8_t soc, bool charging);

/* A board without a charger status: off without input, else charging while the average stays
 * under the charge voltage last asked for, then done. */
uint8_t osc2_fuel_model_charger(bool input, uint16_t average_mv, uint16_t charge_mv);

/* A stored charge window (mte/charge_level: the lower, then the upper end, mV): the charge voltage
 * target it sets, its upper end as it is, or -1 for none (no window: both ends 0, or one that is
 * not valid: valid is a lower end of at least 3400 and an upper one of at most 4120, both
 * multiples of 10; so a lower end with an upper end of 0 asks for 0 mV, as on the real one). */
int32_t osc2_fuel_window_cap(uint32_t lo_mv, uint32_t hi_mv);

#endif /* OPENSC2_FUEL_RULES_H_ */
