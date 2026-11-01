/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the library fuel gauge's decisions (lib/opensc2/fuel_rules.c): the charge
 * state machine with its supply check, give-up and rest, the done rules, a new target, the level's
 * direction rule, the charger model of a board without a charger status and the charge window's
 * cap; and the gauge's scale and stretch (gauge.c). Expected values worked out from the protocol
 * documentation's description, not from this code.
 */
#include <opensc2/battery.h>
#include <opensc2/gauge.h>
#include "fuel_rules.h"

static const struct osc2_fuel_cfg fuel_cfg = {
	.target_mv = 4120, .hysteresis_mv = 40, .vin_start_mv = 4600, .vin_step_mv = 100,
	.vin_min_mv = 4300, .step_ms = 5000, .rest_ms = 60000,
};

/* One pass's machine part: the state the pass began in, then the run. */
static void fuel_pass(struct osc2_fuel *f, int64_t now, bool input, uint8_t charger,
		      struct osc2_fuel_out *out)
{
	uint8_t start = f->state;

	f->charger = charger;
	osc2_fuel_run(f, now, input, start, out);
}

OPENSC2_TEST(fuel_start_and_battery)
{
	struct osc2_fuel f;
	struct osc2_fuel_out out;

	osc2_fuel_init(&f, &fuel_cfg, &out);
	CHECK_EQ(out.charge_mv, 4120);              /* the charger starts at the target */
	CHECK_EQ(out.vin_limit_mv, 0);
	CHECK_EQ(osc2_fuel_charge_state(&f), 0);     /* before the first pass */
	CHECK_EQ(f.voltage_mv, 0);

	/* on battery: on to "not charging", the target asked for again at every pass */
	f.voltage_mv = 4200;                         /* even a full battery: never "done" on battery */
	for (int i = 0; i < 3; i++) {
		fuel_pass(&f, 250 + 1000 * i, false, 0, &out);
		CHECK_EQ(osc2_fuel_charge_state(&f), 1);
		CHECK_EQ(out.charge_mv, 4120);
		CHECK_EQ(out.vin_limit_mv, 0);
	}
}

OPENSC2_TEST(fuel_supply_check_gives_up)
{
	struct osc2_fuel f;
	struct osc2_fuel_out out;
	int64_t t = 0;

	osc2_fuel_init(&f, &fuel_cfg, &out);
	f.voltage_mv = 3900;
	fuel_pass(&f, t, true, 0, &out);             /* starting over -> not charging */
	CHECK_EQ(f.state, OSC2_FUEL_IDLE);
	t += 1000;
	fuel_pass(&f, t, true, 0, &out);             /* input power: the supply check at once */
	CHECK_EQ(osc2_fuel_charge_state(&f), 3);
	CHECK_EQ(out.charge_mv, 4160);               /* target + 40 */
	CHECK_EQ(out.vin_limit_mv, 0);
	int64_t entered = t;

	for (t = entered + 1000; t < entered + 5000; t += 1000) {
		fuel_pass(&f, t, true, 0, &out);
		CHECK_EQ(out.vin_limit_mv, 0);           /* nothing before 5 s */
		CHECK_EQ(f.state, OSC2_FUEL_SUPPLY);
	}
	fuel_pass(&f, entered + 5000, true, 0, &out);
	CHECK_EQ(out.vin_limit_mv, 4600);
	CHECK_EQ(out.charge_mv, 0);
	fuel_pass(&f, entered + 10000, true, 0, &out);
	CHECK_EQ(out.vin_limit_mv, 4500);
	CHECK_EQ(f.state, OSC2_FUEL_SUPPLY);
	fuel_pass(&f, entered + 15000, true, 0, &out);
	CHECK_EQ(out.vin_limit_mv, 4400);            /* the third step reaches 4300: given up */
	CHECK_EQ(out.charge_mv, 4120);
	CHECK_EQ(osc2_fuel_charge_state(&f), 0x81);
	/* 60 s of rest with the flag, then the check again (the flag stays until it succeeds) */
	fuel_pass(&f, entered + 74000, true, 0, &out);
	CHECK_EQ(osc2_fuel_charge_state(&f), 0x81);
	fuel_pass(&f, entered + 75000, true, 0, &out);
	CHECK_EQ(osc2_fuel_charge_state(&f), 0x83);
	CHECK_EQ(out.charge_mv, 4160);
	/* the charger charges: charging, the flag gone */
	fuel_pass(&f, entered + 76000, true, 2, &out);
	CHECK_EQ(osc2_fuel_charge_state(&f), 2);
	CHECK_EQ(out.charge_mv, 0);
	/* unplugged: starting over, then "not charging", the flag never comes back */
	fuel_pass(&f, entered + 77000, false, 0, &out);
	CHECK_EQ(osc2_fuel_charge_state(&f), 1);
}

OPENSC2_TEST(fuel_charge_and_done)
{
	struct osc2_fuel f;
	struct osc2_fuel_out out;

	osc2_fuel_init(&f, &fuel_cfg, &out);
	f.voltage_mv = 3900;
	fuel_pass(&f, 0, true, 0, &out);
	fuel_pass(&f, 1000, true, 1, &out);          /* -> the supply check */
	fuel_pass(&f, 2000, true, 1, &out);          /* precharge counts as charging */
	CHECK_EQ(f.state, OSC2_FUEL_CHARGING);
	fuel_pass(&f, 3000, true, 0, &out);          /* stops charging: not charging */
	CHECK_EQ(f.state, OSC2_FUEL_IDLE);
	CHECK_EQ(out.charge_mv, 4120);
	fuel_pass(&f, 4000, true, 0, &out);          /* the check's timer (5 s) still runs */
	CHECK_EQ(f.state, OSC2_FUEL_IDLE);
	fuel_pass(&f, 6000, true, 0, &out);          /* now over: the check again */
	CHECK_EQ(f.state, OSC2_FUEL_SUPPLY);
	fuel_pass(&f, 7000, true, 3, &out);          /* the charger says done */
	CHECK_EQ(osc2_fuel_charge_state(&f), 4);
	CHECK_EQ(out.charge_mv, 4120);
	fuel_pass(&f, 8000, true, 0, &out);          /* done stays, whatever the charger says */
	CHECK_EQ(f.state, OSC2_FUEL_DONE);
	CHECK_EQ(out.charge_mv, 0);
	fuel_pass(&f, 9000, true, 2, &out);
	CHECK_EQ(f.state, OSC2_FUEL_DONE);

	/* a full battery plugged in reads done at once */
	osc2_fuel_init(&f, &fuel_cfg, &out);
	f.voltage_mv = 4120;
	fuel_pass(&f, 0, true, 0, &out);
	fuel_pass(&f, 1000, true, 0, &out);
	CHECK_EQ(f.state, OSC2_FUEL_DONE);
	fuel_pass(&f, 2000, false, 0, &out);         /* until the input goes */
	CHECK_EQ(f.state, OSC2_FUEL_IDLE);

	/* charging -> done */
	osc2_fuel_init(&f, &fuel_cfg, &out);
	f.voltage_mv = 3900;
	fuel_pass(&f, 0, true, 2, &out);
	fuel_pass(&f, 1000, true, 2, &out);
	fuel_pass(&f, 2000, true, 2, &out);
	CHECK_EQ(f.state, OSC2_FUEL_CHARGING);
	fuel_pass(&f, 3000, true, 3, &out);
	CHECK_EQ(f.state, OSC2_FUEL_DONE);
}

OPENSC2_TEST(fuel_target)
{
	struct osc2_fuel f;
	struct osc2_fuel_out out;

	osc2_fuel_init(&f, &fuel_cfg, &out);
	f.voltage_mv = 3900;
	fuel_pass(&f, 0, true, 2, &out);
	fuel_pass(&f, 1000, true, 2, &out);
	fuel_pass(&f, 2000, true, 2, &out);
	osc2_fuel_set_target(&f, 4000);               /* a window's cap: starting over */
	CHECK_EQ(osc2_fuel_charge_state(&f), 0);
	fuel_pass(&f, 3000, true, 2, &out);
	CHECK_EQ(out.charge_mv, 4000);
	fuel_pass(&f, 4000, true, 2, &out);
	CHECK_EQ(out.charge_mv, 4040);                /* the cap + 40 in the check */
	osc2_fuel_default_target(&f);
	CHECK_EQ(f.target_mv, 4120);
	CHECK_EQ(f.target_hi_mv, 4160);
	CHECK_EQ(f.state, OSC2_FUEL_START);
}

OPENSC2_TEST(fuel_level_and_model)
{
	/* the level: the first reading as it is, then only up while charging, only down otherwise */
	CHECK_EQ(osc2_fuel_keep(false, 0, 57, false), 57);
	CHECK_EQ(osc2_fuel_keep(true, 60, 70, true), 70);
	CHECK_EQ(osc2_fuel_keep(true, 60, 50, true), 60);
	CHECK_EQ(osc2_fuel_keep(true, 60, 70, false), 60);
	CHECK_EQ(osc2_fuel_keep(true, 60, 50, false), 50);

	/* no charger status: charging under the charge voltage asked for, then done */
	CHECK_EQ(osc2_fuel_model_charger(false, 3900, 4160), 0);
	CHECK_EQ(osc2_fuel_model_charger(true, 3900, 4160), 2);
	CHECK_EQ(osc2_fuel_model_charger(true, 4159, 4160), 2);
	CHECK_EQ(osc2_fuel_model_charger(true, 4160, 4160), 3);
	CHECK(osc2_fuel_charging(1) && osc2_fuel_charging(2));
	CHECK(!osc2_fuel_charging(0) && !osc2_fuel_charging(3));

	/* the charge window */
	CHECK_EQ(osc2_fuel_window_cap(0, 0), -1);
	CHECK_EQ(osc2_fuel_window_cap(3400, 4100), 4100);
	CHECK_EQ(osc2_fuel_window_cap(3800, 4120), 4120);
	CHECK_EQ(osc2_fuel_window_cap(3399, 4100), -1);
	CHECK_EQ(osc2_fuel_window_cap(3400, 4130), -1);
	CHECK_EQ(osc2_fuel_window_cap(3405, 4100), -1);
	CHECK_EQ(osc2_fuel_window_cap(3400, 4105), -1);
	CHECK_EQ(osc2_fuel_window_cap(0, 4100), -1);
	CHECK_EQ(osc2_fuel_window_cap(3400, 0), 0);     /* valid on the real one too: 0 mV */

	/* the gauge's scale (the curve at the target, whole percent) and stretch */
	CHECK_EQ(opensc2_gauge_scale(opensc2_gauge_lipo_discharging,
				     opensc2_gauge_lipo_discharging_len, 4120), 100);
	CHECK_EQ(opensc2_gauge_scale(opensc2_gauge_lipo_discharging,
				     opensc2_gauge_lipo_discharging_len, 4100), 97);
	CHECK_EQ(opensc2_gauge_percent(opensc2_gauge_lipo_discharging,
				       opensc2_gauge_lipo_discharging_len, 100, 3850), 43);
	CHECK_EQ(opensc2_gauge_percent(opensc2_gauge_lipo_discharging,
				       opensc2_gauge_lipo_discharging_len, 97, 3850), 44);
	CHECK_EQ(opensc2_gauge_stretch(900, 954), 95);  /* s = 95 (95.4 rounded): 90 x 1.05 = 94.5 */
}

/* The boost of the capacitive readings without input power: x1.2 for a stick-touch word, x1.5 for
 * a grip word, x1.25 for an electrode signal, each in single precision and cut toward zero, the
 * words kept in 16 bits. The host build has no CONFIG_OPENSC2_TOUCH_UNPOWERED_GAIN: "now" passes
 * the value on as it is. */
OPENSC2_TEST(cap_unpowered_gain)
{
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_STICK_TOUCH, 500), 600);
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_STICK_TOUCH, 7), 8);       /* 8.4 */
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_STICK_TOUCH, -7), -8);     /* toward zero */
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_STICK_TOUCH, 0), 0);
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_GRIP, 1125), 1687);        /* 1687.5 */
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_GRIP, 900), 1350);
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_GRIP, -3), -4);            /* -4.5 */
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_ELECTRODE, 100), 125);
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_ELECTRODE, 3), 3);         /* 3.75 */
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_ELECTRODE, -3), -3);
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_ELECTRODE, 40000), 50000); /* not a word */
	/* a word past 16 bits wraps, as the real controller stores it */
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_STICK_TOUCH, 30000), 36000 - 65536);
	CHECK_EQ(opensc2_cap_unpowered(OPENSC2_CAP_GRIP, 22000), 33000 - 65536);
	CHECK_EQ(opensc2_cap_unpowered((enum opensc2_cap_kind)7, 123), 123);
	CHECK_EQ(opensc2_cap_now(OPENSC2_CAP_GRIP, 1125), 1125);
}
