/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * gauge.h: battery measurement and state of charge, for input report 0x43, the way the real
 * controller's fuel gauge works, following the protocol documentation (not public yet;
 * docs/protocol.md lists the pages): a divider read by the ADC plus a stored offset
 * (cal/volt_offset), the last 40 readings, a voltage curve per direction, a stretch so that the
 * charge target reads 100 %, and a level that only falls while discharging and only rises while
 * charging. The curves below are OpenSC2's own for a single-cell LiPo; the real controller's
 * tables are Valve's data. Pure functions, no kernel.
 */
#ifndef OPENSC2_GAUGE_H_
#define OPENSC2_GAUGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The battery's millivolts from the ADC pin's: pin x total / bottom + offset, rounded to the
 * nearest. The real controller's divider is 522 kOhm in all with 301 kOhm at the bottom (x 1.734);
 * a cell on the nRF52840's VDDH read as VDDH / 5 is total 5, bottom 1. */
int32_t opensc2_gauge_divider_mv(int32_t pin_mv, uint32_t total_ohm, uint32_t bottom_ohm,
				 int16_t offset_mv);

/* The last readings, as the real controller keeps 40: the latest, their average and their
 * maximum, over the readings since the last reset (at most 40). The real controller takes 40 in a
 * row after a reset (at start-up and whenever charging starts or stops), then one a second:
 * opensc2_gauge_begin() says how many a step wants. */
#define OPENSC2_GAUGE_HISTORY 40

struct opensc2_gauge_history {
	uint16_t mv[OPENSC2_GAUGE_HISTORY];
	uint8_t next;
	uint8_t count;          /* readings since the reset, at most OPENSC2_GAUGE_HISTORY */
};

void opensc2_gauge_history_reset(struct opensc2_gauge_history *h);
void opensc2_gauge_history_add(struct opensc2_gauge_history *h, uint16_t mv);
uint16_t opensc2_gauge_latest(const struct opensc2_gauge_history *h);
uint16_t opensc2_gauge_average(const struct opensc2_gauge_history *h);   /* rounded down */
uint16_t opensc2_gauge_max(const struct opensc2_gauge_history *h);

/* A voltage curve: points in rising millivolts with the charge in tenths of a percent (0..1000);
 * between two points linear, below the first 0, above the last 1000. */
struct opensc2_gauge_point {
	uint16_t mv;
	uint16_t permille;
};

uint16_t opensc2_gauge_curve(const struct opensc2_gauge_point *points, size_t n, uint16_t mv);

/* OpenSC2's curves for a single-cell LiPo, while discharging (3.67 V reads 0 %, the 4120 mV charge
 * target 100 %) and while charging (the voltage reads higher under charge; 4160 mV is 100 %). */
extern const struct opensc2_gauge_point opensc2_gauge_lipo_discharging[];
extern const size_t opensc2_gauge_lipo_discharging_len;
extern const struct opensc2_gauge_point opensc2_gauge_lipo_charging[];
extern const size_t opensc2_gauge_lipo_charging_len;

#define OPENSC2_GAUGE_TARGET_MV           4120   /* the real controller's charge target */
#define OPENSC2_GAUGE_TARGET_CHARGING_MV  4160   /* its target during a charge cycle */

/* The stretch: the curve's charge times (200 - s) / 100, with s the curve's reading at the charge
 * target in percent, rounded to a whole percent and limited to 0..100. */
uint8_t opensc2_gauge_stretch(uint16_t permille, uint16_t target_permille);

/* The level the controller reports. 255 until the first reading (what the real controller's first
 * battery report after a cold start carries). */
#define OPENSC2_GAUGE_NO_LEVEL 255

struct opensc2_gauge {
	struct opensc2_gauge_history history;
	uint8_t level;          /* percent, or OPENSC2_GAUGE_NO_LEVEL */
	uint8_t charge_state;   /* the state of the last step (enum opensc2_charge_state) */
	uint16_t voltage_mv;    /* the voltage to report: the average at the last step outside
				 * "done" (0 before the first) */
};

/* Start from nothing, or from a level kept over a warm reboot (opensc2_battery_kept_level();
 * OPENSC2_GAUGE_NO_LEVEL: none). */
void opensc2_gauge_init(struct opensc2_gauge *g, uint8_t kept_level);

/* A step, the way the real controller's gauge takes its readings. Begin with the charge state
 * (enum opensc2_charge_state); it returns how many fresh readings to take in a row: 40 after a
 * start and whenever charging starts or stops (the readings jump, so the history starts over; the
 * real controller also waits 250 ms first), else 1, and 0 while the charge is done (no reading
 * then: the level reads 100 and the reported voltage holds). Add each reading (battery mV, offset
 * included), then finish: charging (2) uses the charging curve and the level may only rise,
 * anything else the discharging curve and the level may only fall. Returns the level. */
uint8_t opensc2_gauge_begin(struct opensc2_gauge *g, uint8_t charge_state);
void opensc2_gauge_add(struct opensc2_gauge *g, uint16_t battery_mv);
uint8_t opensc2_gauge_finish(struct opensc2_gauge *g);
/* The three with a single reading (the history then grows by one reading a step). */
uint8_t opensc2_gauge_step(struct opensc2_gauge *g, uint16_t battery_mv, uint8_t charge_state);
/* The battery voltage report 0x43 carries, as the real controller's gauge gives it: the average of
 * the readings at the last step outside "done", held while the charge is done. */
uint16_t opensc2_gauge_voltage(const struct opensc2_gauge *g);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_GAUGE_H_ */
