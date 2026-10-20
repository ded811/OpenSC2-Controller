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
	opensc2_gauge_history_add(&h, 4000);   /* the first reading fills all 40 */
	CHECK_EQ(opensc2_gauge_latest(&h), 4000);
	CHECK_EQ(opensc2_gauge_average(&h), 4000);
	CHECK_EQ(opensc2_gauge_max(&h), 4000);
	opensc2_gauge_history_add(&h, 3900);
	CHECK_EQ(opensc2_gauge_latest(&h), 3900);
	CHECK_EQ(opensc2_gauge_average(&h), 3997);
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
}

OPENSC2_TEST(gauge_level)
{
	struct opensc2_gauge g;

	opensc2_gauge_init(&g, OPENSC2_GAUGE_NO_LEVEL);
	CHECK_EQ(g.level, OPENSC2_GAUGE_NO_LEVEL);
	CHECK_EQ(opensc2_gauge_step(&g, 4000, OPENSC2_CHARGE_DISCHARGING), 81);
	CHECK_EQ(opensc2_gauge_step(&g, 4100, OPENSC2_CHARGE_DISCHARGING), 81);   /* never rises */
	CHECK_EQ(opensc2_gauge_step(&g, 3800, OPENSC2_CHARGE_DISCHARGING), 80);
	/* charging: the history starts over, the level only rises */
	CHECK_EQ(opensc2_gauge_step(&g, 4000, OPENSC2_CHARGE_CHARGING), 80);
	CHECK_EQ(opensc2_gauge_average(&g.history), 4000);
	CHECK_EQ(opensc2_gauge_step(&g, 4150, OPENSC2_CHARGE_CHARGING), 80);
	/* done reads 100 */
	CHECK_EQ(opensc2_gauge_step(&g, 4200, OPENSC2_CHARGE_DONE), 100);
	CHECK_EQ(opensc2_gauge_step(&g, 3900, OPENSC2_CHARGE_DISCHARGING), 100);

	/* a level kept over a warm reboot: the first reading cannot raise it */
	opensc2_gauge_init(&g, 55);
	CHECK_EQ(opensc2_gauge_step(&g, 4000, OPENSC2_CHARGE_DISCHARGING), 55);
	opensc2_gauge_init(&g, 200);
	CHECK_EQ(g.level, OPENSC2_GAUGE_NO_LEVEL);
}
