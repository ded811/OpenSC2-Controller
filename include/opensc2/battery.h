/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * battery.h: the battery and the charger, as the real controller's fuel gauge runs them, driven by
 * what the board measures. Two ways to give report 0x43 and vendor command 0xBE their values:
 *
 *  - The library's fuel gauge (CONFIG_OPENSC2_BATTERY_GAUGE, on with CONFIG_OPENSC2_BATTERY). The
 *    application hands opensc2_battery_start() its battery parts (struct opensc2_battery_board):
 *    a way to read the battery voltage, and optionally a charger's controls and a cut-off. From
 *    then on the library runs the real controller's gauge: once a second (the first pass 250 ms
 *    after the start) it reads the charger's status and values, keeps the last 40 battery
 *    readings (40 fresh ones in a row at the start and whenever charging starts or stops, then a
 *    250 ms pause), turns their average into a level on a curve per direction (only rising while
 *    charging, only falling otherwise, 100 once the charge is done), keeps that level over a warm
 *    reboot, and runs the charge state machine whose index report 0x43 carries: 0 starting over,
 *    1 not charging, 3 checking the supply (the charge voltage raised to the target + 40 mV; while
 *    the charger does not charge, the input-voltage limit is lowered every 5 s, 4600, 4500,
 *    4400 mV, then given up for 60 s with the 0x80 flag), 2 charging, 4 done. The charger's values
 *    (status, voltages, currents, temperature) are the application's, set at any time with
 *    opensc2_charger_set_status() and opensc2_charger_set_values() or from the board's
 *    charger_update callback, which the library calls at the start of every pass. Before the
 *    first pass report 0x43 carries what the real controller's first one does: state 0, level
 *    255 (or the level kept over a warm reboot) and zeros.
 *  - Or the application's own values with opensc2_set_battery() (opensc2.h), field for field,
 *    as often as it likes (without the gauge, or to override it until its next pass).
 *
 * Without either, the library reports a full battery whose charge is done (a board without a
 * battery). Every call here may come from any thread at any time.
 */
#ifndef OPENSC2_BATTERY_H_
#define OPENSC2_BATTERY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <opensc2/charger.h>
#include <opensc2/gauge.h>
#include <opensc2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* enum opensc2_charger_status (OFF, PRECHARGE, CHARGING, DONE, UNKNOWN) is <opensc2/charger.h>'s,
 * whose pure helpers state the same charging rules for a board that runs its own gauge. */

/* The charger's measurements, sent as they are in report 0x43 and vendor command 0xBE. What the
 * board does not measure stays 0 (the real controller's values before its first conversion). */
struct opensc2_charger_values {
	uint16_t system_mv;        /* the system rail */
	uint16_t input_mv;         /* the input supply (USB or the dock) */
	uint16_t current_ma;       /* the battery's current, as a magnitude: the charge current while
				    * charging, the draw otherwise (the real controller sends both
				    * positive) */
	uint16_t input_current_ma; /* the current drawn from the input */
	uint16_t temperature;      /* the battery's temperature as the real controller sends it:
				    * thousandths of a degree Celsius, rounded up (25 C = 25000) */
};

/* The board's battery parts. Only read_mv is needed; every callback runs in the gauge's thread
 * (priority CONFIG_OPENSC2_BATTERY_GAUGE_PRIORITY) and may sleep. */
struct opensc2_battery_board {
	/* Read the battery `n` times in a row (n is 1, or up to 40 when the history starts over),
	 * millivolts at the cell, without the stored offset cal/volt_offset (the library adds it).
	 * Return how many readings were taken (a divider switched on once for all of them, as the
	 * real controller does), or a negative errno; a short or failed read keeps the last average. */
	int (*read_mv)(void *user, uint16_t *mv, size_t n);
	/* Optional, at the start of every pass: read the charger and hand its status and values over
	 * (opensc2_charger_set_status(), opensc2_charger_set_values()). A negative errno says the
	 * charger could not be read: that pass keeps the voltage and the level as they were. */
	int (*charger_update)(void *user);
	/* Optional: the charger's charge (termination) voltage, mV: the target (CONFIG_OPENSC2_
	 * CHARGE_TARGET_MV, 4120), the target + CONFIG_OPENSC2_CHARGE_HYSTERESIS_MV during the supply
	 * check and a charge, or the cap of a stored charge window (mte/charge_level). Asked at the
	 * start and at every entry of a state that sets it (on battery once a pass). */
	int (*set_charge_voltage)(void *user, uint16_t mv);
	/* Optional: the charger's input-voltage limit, mV, lowered step by step while a supply does
	 * not charge the battery (4600, 4500, 4400 by default). */
	int (*set_input_voltage_limit)(void *user, uint16_t mv);
	/* Optional: disconnect the battery (a charger's ship mode), called by opensc2_battery_cutoff()
	 * when the board powers off without input power. Usually does not return. */
	int (*battery_cutoff)(void *user);
	/* Optional: the level in percent for an average battery voltage (`charging`: the charger
	 * charges), for a board with its own gauge chip or curve. Without it the library uses the
	 * curves below with the real controller's stretch (opensc2_gauge_percent()). The library
	 * still applies the direction rules and keeps the level. */
	uint8_t (*level)(void *user, uint16_t average_mv, bool charging);
	/* Optional curves (rising millivolts, opensc2/gauge.h); NULL: OpenSC2's single-cell LiPo
	 * curves. The stretch is taken at the charge target (discharging) and at the target plus the
	 * hysteresis (charging), as configured when the gauge starts. */
	const struct opensc2_gauge_point *discharging;
	size_t discharging_len;
	const struct opensc2_gauge_point *charging;
	size_t charging_len;
	/* Optional: input power came or went (opensc2_input_power()). Called once when the gauge
	 * starts and then at every change, looked at every 50 ms, from the system work queue: the
	 * real controller raises its charger's input current limit for any supply and lowers it
	 * again without one, at once. A charger that sets its own limit needs none. */
	int (*input_changed)(void *user, bool present);
	void *user;
};

/* Start the library's fuel gauge with the board's parts (`board` must stay valid). -EINVAL
 * without read_mv, -EALREADY when it runs, -ENOTSUP without CONFIG_OPENSC2_BATTERY_GAUGE. Call it
 * once, after opensc2_init(). */
int opensc2_battery_start(const struct opensc2_battery_board *board);

/* The charger's status and measurements, at any time (the gauge takes them at its next pass;
 * charger_update is the place to set them right before it). Without the gauge they go straight
 * into report 0x43 as fields of the application's battery. -EINVAL for an unknown status. */
int opensc2_charger_set_status(enum opensc2_charger_status status);
int opensc2_charger_set_values(const struct opensc2_charger_values *values);

/* The charge voltage target, mV (3400..4400), as a factory tool sets it: the gauge's state
 * machine starts over with it; 0 goes back to CONFIG_OPENSC2_CHARGE_TARGET_MV. A stored charge
 * window (mte/charge_level) sets its upper end the same way. -ENOTSUP without the gauge. */
int opensc2_battery_set_charge_target(uint16_t mv);

/* What report 0x43 carries now (the gauge's last pass, or the application's battery). */
void opensc2_battery_get(struct opensc2_battery *battery);

/* Cut the battery off, as the real controller does when it powers off: only without input power
 * (on USB power or the dock it logs that it leaves the battery connected and returns -EBUSY),
 * through the board's battery_cutoff. -ENOTSUP without one. The library calls it before
 * sys_poweroff() itself; an application that powers off on its own calls it last. */
int opensc2_battery_cutoff(void);

/* ---- Input power and the capacitive sensors ---------------------------------------------------
 * Input power is USB or the dock (opensc2_power_source() other than OPENSC2_POWER_SOURCE_BATTERY);
 * a build without CONFIG_OPENSC2_BATTERY always has it. The real controller's charger, fuel gauge
 * and trackpad chip driver all follow it.
 *
 * Without input power the real controller boosts its capacitive readings before it uses them
 * (probably because a body that is not tied to the PC's ground couples less): every stick-touch word x1.2, every grip word x1.5 and every trackpad electrode signal
 * x1.25, each multiplied in single precision and cut toward zero, from start-up until a supply
 * is seen and again whenever it goes. The library does the same to the words a board gives with
 * opensc2_set_touch_raw() or opensc2_set_raw() (CONFIG_OPENSC2_TOUCH_UNPOWERED_GAIN, on by default: turn it off for a
 * sensor whose readings do not drop on battery). A board that works out its own trackpad signal
 * from electrodes passes each one through opensc2_cap_now(OPENSC2_CAP_ELECTRODE, ...) first. */
bool opensc2_input_power(void);

enum opensc2_cap_kind {
	OPENSC2_CAP_STICK_TOUCH = 0,   /* x1.2 */
	OPENSC2_CAP_GRIP = 1,          /* x1.5 */
	OPENSC2_CAP_ELECTRODE = 2,     /* x1.25 */
};

/* `value` boosted as the real controller boosts it without input power. Pure. A stick-touch or
 * grip result is a 16-bit word, wrapped as the real controller stores it (only values beyond
 * about +-21800 wrap). */
int32_t opensc2_cap_unpowered(enum opensc2_cap_kind kind, int32_t value);
/* `value` as the library takes it now: opensc2_cap_unpowered() without input power (with
 * CONFIG_OPENSC2_TOUCH_UNPOWERED_GAIN), else as it is. */
int32_t opensc2_cap_now(enum opensc2_cap_kind kind, int32_t value);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_BATTERY_H_ */
