/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * charger.h: the real controller's charging rules, for a board whose charger the application
 * drives, following the protocol documentation (not public yet; docs/protocol.md lists the pages).
 * The real controller's charger chip is told a charge voltage and an input-voltage limit; these
 * helpers say when to set which, and give the charge state that input report 0x43 carries
 * (enum opensc2_charge_state, plus OPENSC2_CHARGE_GAVE_UP). Pure functions, no kernel: the
 * application reads its charger, calls opensc2_charger_step() once a second and writes what the
 * step asks for to its own chip. A charger without those settings can still use the charge state
 * (ignore the writes).
 *
 * The rules, once a second:
 *  - No input power: the machine starts over (state 1, "discharging", after a step; 0 before the
 *    first step) and the charge voltage is the target.
 *  - Input power: first the supply is validated (state 3): the charge voltage goes to the target
 *    plus the hysteresis and, every 5 s while the charger does not charge, the input-voltage limit
 *    is written and then lowered by 100 mV (4600, 4500, 4400 mV). Charging (state 2) or done
 *    (state 4) ends it; reaching 4300 mV gives up: back to state 1 with OPENSC2_CHARGE_GAVE_UP set
 *    (report 0x43 carries it; the LED and the system state machine then count the level as 100),
 *    and the next try 60 s later.
 *  - Charging ends at "done" (state 4: the charge voltage back to the target) or, when the charger
 *    stops charging, back at state 1 and from there validating again.
 *  - A battery at or above the target with input power counts as done too.
 *  - "Done" lasts until the input power goes away.
 * The input current limit is not decided here: the real controller raises it whenever its charger
 * detects any supply type, and lowers it without one. Its ship mode (the battery cut off at a
 * power-off) is entered only without input power: see opensc2_charger_ship_mode_ok().
 */
#ifndef OPENSC2_CHARGER_H_
#define OPENSC2_CHARGER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the charger says it does: the real controller's charger reports the first four. Shared
 * with the library's fuel gauge (<opensc2/battery.h>), which runs these same rules itself. */
enum opensc2_charger_status {
	OPENSC2_CHARGER_OFF = 0,          /* not charging (no input power, or the input does not do) */
	OPENSC2_CHARGER_PRECHARGE = 1,    /* charging a deeply discharged cell gently */
	OPENSC2_CHARGER_CHARGING = 2,     /* charging */
	OPENSC2_CHARGER_DONE = 3,         /* the charge is done */
	/* The board cannot tell (no charger status): the library's fuel gauge then uses its own model
	 * of a charger, charging while there is input power and the battery's average stays under the
	 * charge voltage the gauge last asked for, done from there on, off without input power. The
	 * gauge's default; the pure rules below take it as "off". */
	OPENSC2_CHARGER_UNKNOWN = 0xFF,
};

/* The rules' numbers; opensc2_charger_defaults() gives the real controller's. */
struct opensc2_charger_config {
	uint16_t target_mv;        /* the charge voltage: 4120 mV */
	uint16_t hysteresis_mv;    /* added while a supply is validated and while charging: 40 mV */
	uint16_t vin_start_mv;     /* the input-voltage limit's first value: 4600 mV */
	uint16_t vin_step_mv;      /* lowered by this at every step: 100 mV */
	uint16_t vin_min_mv;       /* at or below this the search gives up: 4300 mV */
	uint32_t vin_step_ms;      /* a step every 5000 ms */
	uint32_t retry_ms;         /* the next try after giving up: 60000 ms */
};

void opensc2_charger_defaults(struct opensc2_charger_config *cfg);

struct opensc2_charger {
	struct opensc2_charger_config cfg;
	uint8_t state;           /* enum opensc2_charge_state */
	bool gave_up;
	uint16_t charge_mv;      /* the charge voltage now: the target, or the factory window's top */
	uint16_t window_mv;      /* the window last applied (0: none) */
	uint16_t vin_limit_mv;   /* the input-voltage limit the next step writes */
	uint32_t timer_at;
};

/* What a call asks the application to write to its charger. */
struct opensc2_charger_out {
	bool set_charge;
	uint16_t charge_mv;       /* the charge (regulation) voltage */
	bool set_vin_limit;
	uint16_t vin_limit_mv;    /* the input-voltage limit */
};

/* Start (state 0); `cfg` NULL takes the defaults. Asks for the charge voltage = the target. */
void opensc2_charger_init(struct opensc2_charger *c, const struct opensc2_charger_config *cfg,
			  uint32_t now_ms, struct opensc2_charger_out *out);

/* The charge voltage the machine works with: `mv`, or 0 for the configured target. The real
 * controller caps it at the top of a stored factory charge window (opensc2_charge_target_mv()):
 * pass that value at every step, a change starts the machine over (state 0; the next step sets
 * the voltage). The gauge's level is not rescaled by it, on the real controller either. */
void opensc2_charger_window(struct opensc2_charger *c, uint16_t mv);

/* The charge state for opensc2_gauge_begin(), taken before this second's step as the real
 * controller's gauge does: "done" while the machine is done, else charging while the charger
 * says it charges, else discharging. */
uint8_t opensc2_charger_gauge_state(const struct opensc2_charger *c,
				    enum opensc2_charger_status status);

/* Once a second, after the gauge's step: input power present (any supply type the charger
 * detected), the charger's status, and the averaged battery voltage (opensc2_gauge_voltage()).
 * Returns the charge state for opensc2_battery.charge_state (OPENSC2_CHARGE_GAVE_UP added while
 * set). */
uint8_t opensc2_charger_step(struct opensc2_charger *c, bool input_power,
			     enum opensc2_charger_status status, uint16_t battery_mv,
			     uint32_t now_ms, struct opensc2_charger_out *out);

/* The charge state as the last step left it (report 0x43's byte). */
uint8_t opensc2_charger_state(const struct opensc2_charger *c);

/* May the board cut its battery off (a charger's ship mode) at a power-off? Only without a supply
 * connected, as on the real controller. */
static inline bool opensc2_charger_ship_mode_ok(bool input_power)
{
	return !input_power;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_CHARGER_H_ */
