/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the battery helpers of <opensc2/gauge.h> (lib/opensc2/gauge.c): the divider and
 * offset, the 40-reading history, the curves, the stretch, and the level that only falls while
 * discharging and only rises while charging. Expected values worked out outside this code.
 */
#include <opensc2/gauge.h>

OPENSC2_TEST(gauge_divider_history)
{
	struct opensc2_gauge_history h;

	/* the real controller's divider: 522 kOhm, 301 at the bottom */
	CHECK_EQ(opensc2_gauge_divider_mv(2000, 522000, 301000, 0), 3468);
	CHECK_EQ(opensc2_gauge_divider_mv(2000, 522000, 301000, -15), 3453);   /* cal/volt_offset */
	CHECK_EQ(opensc2_gauge_divider_mv(800, 5, 1, 0), 4000);                 /* VDDH / 5 */
	CHECK_EQ(opensc2_gauge_divider_mv(800, 5, 0, 7), 807);

	opensc2_gauge_history_reset(&h);
	CHECK_EQ(opensc2_gauge_latest(&h), 0);
	CHECK_EQ(opensc2_gauge_average(&h), 0);
	opensc2_gauge_history_add(&h, 4000);   /* the readings since the reset count */
	CHECK_EQ(opensc2_gauge_latest(&h), 4000);
	CHECK_EQ(opensc2_gauge_average(&h), 4000);
	CHECK_EQ(opensc2_gauge_max(&h), 4000);
	opensc2_gauge_history_add(&h, 3900);
	CHECK_EQ(opensc2_gauge_latest(&h), 3900);
	CHECK_EQ(opensc2_gauge_average(&h), 3950);
	CHECK_EQ(opensc2_gauge_max(&h), 4000);
	for (int i = 0; i < OPENSC2_GAUGE_HISTORY; i++) {
		opensc2_gauge_history_add(&h, 3800);
	}
	CHECK_EQ(opensc2_gauge_average(&h), 3800);
	CHECK_EQ(opensc2_gauge_max(&h), 3800);
}

OPENSC2_TEST(gauge_curves)
{
	const struct opensc2_gauge_point *d = opensc2_gauge_lipo_discharging;
	size_t dn = opensc2_gauge_lipo_discharging_len;
	const struct opensc2_gauge_point *c = opensc2_gauge_lipo_charging;
	size_t cn = opensc2_gauge_lipo_charging_len;

	CHECK_EQ(opensc2_gauge_curve(d, dn, 3600), 0);
	CHECK_EQ(opensc2_gauge_curve(d, dn, 3670), 0);     /* 3.67 V reads 0 % */
	CHECK_EQ(opensc2_gauge_curve(d, dn, 3735), 100);   /* linear between points */
	CHECK_EQ(opensc2_gauge_curve(d, dn, 3850), 430);
	CHECK_EQ(opensc2_gauge_curve(d, dn, 4000), 805);
	CHECK_EQ(opensc2_gauge_curve(d, dn, OPENSC2_GAUGE_TARGET_MV), 1000);
	CHECK_EQ(opensc2_gauge_curve(d, dn, 4200), 1000);
	CHECK_EQ(opensc2_gauge_curve(c, cn, 4000), 480);
	CHECK_EQ(opensc2_gauge_curve(c, cn, 4003), 488);
	CHECK_EQ(opensc2_gauge_curve(c, cn, OPENSC2_GAUGE_TARGET_CHARGING_MV), 1000);
	/* points must rise: every curve does */
	for (size_t i = 1; i < dn; i++) {
		CHECK(d[i].mv > d[i - 1].mv && d[i].permille >= d[i - 1].permille);
	}
	for (size_t i = 1; i < cn; i++) {
		CHECK(c[i].mv > c[i - 1].mv && c[i].permille >= c[i - 1].permille);
	}

	/* the stretch: x (200 - s) / 100 with s the reading at the target, rounded, 0..100 */
	CHECK_EQ(opensc2_gauge_stretch(1000, 1000), 100);
	CHECK_EQ(opensc2_gauge_stretch(500, 1000), 50);
	CHECK_EQ(opensc2_gauge_stretch(950, 950), 100);   /* 95 % at the target reads 100 */
	CHECK_EQ(opensc2_gauge_stretch(0, 500), 0);
	CHECK_EQ(opensc2_gauge_stretch(1000, 900), 100);  /* limited */
	/* s is the target's reading in whole percent: 97.4 % counts as 97 (x 1.03), 97.5 % as 98 */
	CHECK_EQ(opensc2_gauge_stretch(500, 974), 52);
	CHECK_EQ(opensc2_gauge_stretch(500, 975), 51);
	CHECK_EQ(opensc2_gauge_stretch(500, 1100), 50);   /* s at most 100 */
}

OPENSC2_TEST(gauge_level)
{
	struct opensc2_gauge g;

	/* As the real controller's gauge: 40 readings in a row after a start, then one a step. */
	opensc2_gauge_init(&g, OPENSC2_GAUGE_NO_LEVEL);
	CHECK_EQ(g.level, OPENSC2_GAUGE_NO_LEVEL);
	CHECK_EQ(opensc2_gauge_voltage(&g), 0);
	CHECK_EQ(opensc2_gauge_begin(&g, OPENSC2_CHARGE_DISCHARGING), 40);
	for (int i = 0; i < 40; i++) {
		opensc2_gauge_add(&g, 4000);
	}
	CHECK_EQ(opensc2_gauge_finish(&g), 81);
	CHECK_EQ(opensc2_gauge_voltage(&g), 4000);
	CHECK_EQ(opensc2_gauge_begin(&g, OPENSC2_CHARGE_DISCHARGING), 1);
	opensc2_gauge_add(&g, 4400);
	CHECK_EQ(opensc2_gauge_finish(&g), 81);                                    /* never rises */
	CHECK_EQ(opensc2_gauge_voltage(&g), 4010);                                 /* the average */
	CHECK_EQ(opensc2_gauge_step(&g, 3600, OPENSC2_CHARGE_DISCHARGING), 81);  /* 4000 average */
	/* charging: the history starts over (40 again), the level only rises */
	CHECK_EQ(opensc2_gauge_begin(&g, OPENSC2_CHARGE_CHARGING), 40);
	for (int i = 0; i < 40; i++) {
		opensc2_gauge_add(&g, 3900);
	}
	CHECK_EQ(opensc2_gauge_finish(&g), 81);
	CHECK_EQ(opensc2_gauge_average(&g.history), 3900);
	CHECK_EQ(opensc2_gauge_step(&g, 4150, OPENSC2_CHARGE_CHARGING), 81);
	/* done reads 100, takes no reading, and the reported voltage holds */
	CHECK_EQ(opensc2_gauge_begin(&g, OPENSC2_CHARGE_DONE), 0);
	CHECK_EQ(opensc2_gauge_finish(&g), 100);
	CHECK_EQ(opensc2_gauge_step(&g, 4200, OPENSC2_CHARGE_DONE), 100);
	CHECK_EQ(opensc2_gauge_voltage(&g), 3906);
	/* charging ended at "done", which started the history over: on battery again, 40 fresh
	 * readings; the level falls from 100 to what they read */
	CHECK_EQ(opensc2_gauge_begin(&g, OPENSC2_CHARGE_DISCHARGING), 40);
	opensc2_gauge_add(&g, 3900);
	CHECK_EQ(opensc2_gauge_finish(&g), 59);
	/* a single reading a step still works (the history grows by one a step) */
	opensc2_gauge_init(&g, OPENSC2_GAUGE_NO_LEVEL);
	CHECK_EQ(opensc2_gauge_step(&g, 4000, OPENSC2_CHARGE_DISCHARGING), 81);

	/* a level kept over a warm reboot: the first reading cannot raise it */
	opensc2_gauge_init(&g, 55);
	CHECK_EQ(opensc2_gauge_step(&g, 4000, OPENSC2_CHARGE_DISCHARGING), 55);
	opensc2_gauge_init(&g, 200);
	CHECK_EQ(g.level, OPENSC2_GAUGE_NO_LEVEL);
}

OPENSC2_TEST(gauge_charger_tops_up)
{
	struct opensc2_gauge g;

	/* Done without the charger charging again: the readings from before it stay, on battery
	 * one fresh reading a step. */
	opensc2_gauge_init(&g, OPENSC2_GAUGE_NO_LEVEL);
	CHECK_EQ(opensc2_gauge_begin_charger(&g, OPENSC2_CHARGE_DISCHARGING, false), 40);
	for (int i = 0; i < 40; i++) {
		opensc2_gauge_add(&g, 4100);
	}
	(void)opensc2_gauge_finish(&g);
	CHECK_EQ(opensc2_gauge_begin_charger(&g, OPENSC2_CHARGE_DONE, false), 0);
	CHECK_EQ(opensc2_gauge_finish(&g), 100);
	CHECK_EQ(opensc2_gauge_begin_charger(&g, OPENSC2_CHARGE_DISCHARGING, false), 1);
	/* The charger tops the battery up while the state stays "done": the history starts over
	 * (no reading yet), so the first step on battery takes 40 fresh ones. */
	opensc2_gauge_init(&g, OPENSC2_GAUGE_NO_LEVEL);
	CHECK_EQ(opensc2_gauge_begin_charger(&g, OPENSC2_CHARGE_DISCHARGING, false), 40);
	for (int i = 0; i < 40; i++) {
		opensc2_gauge_add(&g, 4100);
	}
	(void)opensc2_gauge_finish(&g);
	CHECK_EQ(opensc2_gauge_begin_charger(&g, OPENSC2_CHARGE_DONE, true), 0);
	CHECK_EQ(g.history.count, 0);
	CHECK_EQ(opensc2_gauge_finish(&g), 100);
	CHECK_EQ(opensc2_gauge_voltage(&g), 4100);   /* held */
	CHECK_EQ(opensc2_gauge_begin_charger(&g, OPENSC2_CHARGE_DONE, false), 0);
	CHECK_EQ(opensc2_gauge_begin_charger(&g, OPENSC2_CHARGE_DISCHARGING, false), 40);
	/* opensc2_gauge_begin() takes "charging" from the state */
	CHECK_EQ(opensc2_gauge_begin(&g, OPENSC2_CHARGE_CHARGING), 40);
	CHECK(g.charger_charging);
}

OPENSC2_TEST(gauge_own_curves)
{
	/* a board's own discharging curve: 3500 mV 0 %, 4000 mV 100 % (target 4000) */
	static const struct opensc2_gauge_point flat[] = { { 3500, 0 }, { 4000, 1000 } };
	static const struct opensc2_gauge_curves own = {
		.discharging = flat,
		.discharging_len = 2,
		.target_mv = 4000,
	};
	struct opensc2_gauge g;

	opensc2_gauge_init(&g, OPENSC2_GAUGE_NO_LEVEL);
	opensc2_gauge_set_curves(&g, &own);
	CHECK_EQ(opensc2_gauge_step(&g, 3750, OPENSC2_CHARGE_DISCHARGING), 50);
	/* charging keeps the built-in curve and target (none given) */
	opensc2_gauge_init(&g, OPENSC2_GAUGE_NO_LEVEL);
	opensc2_gauge_set_curves(&g, &own);
	CHECK_EQ(opensc2_gauge_step(&g, 4000, OPENSC2_CHARGE_CHARGING), 48);
	/* back to the built-in curves */
	opensc2_gauge_init(&g, OPENSC2_GAUGE_NO_LEVEL);
	opensc2_gauge_set_curves(&g, &own);
	opensc2_gauge_set_curves(&g, NULL);
	CHECK_EQ(opensc2_gauge_step(&g, 4000, OPENSC2_CHARGE_DISCHARGING), 81);
	/* init forgets them */
	opensc2_gauge_set_curves(&g, &own);
	opensc2_gauge_init(&g, OPENSC2_GAUGE_NO_LEVEL);
	CHECK(g.curves == NULL);
}
