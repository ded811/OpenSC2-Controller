/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the charging rules of <opensc2/charger.h> (lib/opensc2/charger.c): the charge
 * state report 0x43 carries, the charge voltage and the input-voltage search, the give-up and its
 * retry, the factory window's cap. Expected values worked out outside this code.
 */
#include <opensc2/charger.h>

OPENSC2_TEST(charger_plug_charge_done)
{
	struct opensc2_charger c;
	struct opensc2_charger_out o;

	opensc2_charger_init(&c, NULL, 0, &o);
	CHECK_EQ(opensc2_charger_state(&c), OPENSC2_CHARGE_RESET);   /* before the first step */
	CHECK(o.set_charge);
	CHECK_EQ(o.charge_mv, 4120);
	CHECK(!o.set_vin_limit);

	/* on battery: discharging, the charge voltage set again at every step */
	CHECK_EQ(opensc2_charger_step(&c, false, OPENSC2_CHARGER_NOT_CHARGING, 3900, 1000, &o),
		 OPENSC2_CHARGE_DISCHARGING);
	CHECK(o.set_charge && o.charge_mv == 4120);
	CHECK_EQ(opensc2_charger_step(&c, false, OPENSC2_CHARGER_NOT_CHARGING, 3900, 2000, &o), 1);
	CHECK_EQ(opensc2_charger_gauge_state(&c, OPENSC2_CHARGER_NOT_CHARGING),
		 OPENSC2_CHARGE_DISCHARGING);

	/* plugged in: the supply is validated first, the charge voltage raised by the hysteresis */
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3900, 3000, &o),
		 OPENSC2_CHARGE_SRC_VALIDATE);
	CHECK(o.set_charge && o.charge_mv == 4160);
	CHECK(!o.set_vin_limit);   /* the first limit is written 5 s later */
	/* the charger charges: state 2, the voltage stays raised */
	CHECK_EQ(opensc2_charger_gauge_state(&c, OPENSC2_CHARGER_FAST_CHARGE),
		 OPENSC2_CHARGE_CHARGING);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_FAST_CHARGE, 3950, 4000, &o),
		 OPENSC2_CHARGE_CHARGING);
	CHECK(!o.set_charge && !o.set_vin_limit);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_PRECHARGE, 3950, 5000, &o), 2);
	/* done: back to the target, until the power goes */
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_CHARGE_DONE, 4100, 6000, &o),
		 OPENSC2_CHARGE_DONE);
	CHECK(o.set_charge && o.charge_mv == 4120);
	CHECK_EQ(opensc2_charger_gauge_state(&c, OPENSC2_CHARGER_FAST_CHARGE), OPENSC2_CHARGE_DONE);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 4000, 7000, &o), 4);
	CHECK_EQ(opensc2_charger_step(&c, false, OPENSC2_CHARGER_NOT_CHARGING, 4000, 8000, &o), 1);

	/* charging that stops without "done": back to 1, and validating again at the next step */
	(void)opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3900, 9000, &o);
	CHECK_EQ(opensc2_charger_state(&c), OPENSC2_CHARGE_SRC_VALIDATE);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_FAST_CHARGE, 3900, 10000, &o), 2);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3900, 11000, &o), 1);
	CHECK(o.set_charge && o.charge_mv == 4120);
	/* the validation's 5 s timer still runs (it started at 9000) */
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3900, 12000, &o), 1);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3900, 14000, &o), 3);

	/* a battery at the target with input power counts as done */
	opensc2_charger_init(&c, NULL, 0, &o);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 4130, 1000, &o), 1);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 4130, 2000, &o), 4);
}

OPENSC2_TEST(charger_gives_up_and_retries)
{
	struct opensc2_charger c;
	struct opensc2_charger_out o;

	opensc2_charger_init(&c, NULL, 0, &o);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3800, 1000, &o), 1);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3800, 2000, &o), 3);
	/* every 5 s without charging: the limit written, then lowered by 100 mV */
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3800, 6999, &o), 3);
	CHECK(!o.set_vin_limit);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3800, 7000, &o), 3);
	CHECK(o.set_vin_limit && o.vin_limit_mv == 4600);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3800, 12000, &o), 3);
	CHECK(o.set_vin_limit && o.vin_limit_mv == 4500);
	/* the third write reaches 4300 mV: given up, back to 1 with the flag */
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3800, 17000, &o),
		 OPENSC2_CHARGE_DISCHARGING | OPENSC2_CHARGE_GAVE_UP);
	CHECK(o.set_vin_limit && o.vin_limit_mv == 4400);
	CHECK(o.set_charge && o.charge_mv == 4120);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3800, 76999, &o), 0x81);
	/* 60 s later it tries again; the flag stays until it charges */
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3800, 77000, &o), 0x83);
	CHECK(o.set_charge && o.charge_mv == 4160);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_FAST_CHARGE, 3800, 78000, &o), 2);
	/* the power going away clears it too */
	opensc2_charger_init(&c, NULL, 0, &o);
	c.gave_up = true;
	CHECK_EQ(opensc2_charger_step(&c, false, OPENSC2_CHARGER_NOT_CHARGING, 3800, 1000, &o), 1);

	CHECK(opensc2_charger_ship_mode_ok(false));
	CHECK(!opensc2_charger_ship_mode_ok(true));
}

OPENSC2_TEST(charger_window_and_config)
{
	struct opensc2_charger c;
	struct opensc2_charger_out o;
	struct opensc2_charger_config cfg;

	opensc2_charger_init(&c, NULL, 0, &o);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_FAST_CHARGE, 3900, 1000, &o), 1);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_FAST_CHARGE, 3900, 2000, &o), 3);
	/* a factory window's top caps the charge voltage and starts the machine over */
	opensc2_charger_window(&c, 4000);
	CHECK_EQ(opensc2_charger_state(&c), OPENSC2_CHARGE_RESET);
	opensc2_charger_window(&c, 4000);   /* the same value again changes nothing */
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_FAST_CHARGE, 3900, 3000, &o), 1);
	CHECK(o.set_charge && o.charge_mv == 4000);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_FAST_CHARGE, 3900, 4000, &o), 3);
	CHECK(o.set_charge && o.charge_mv == 4040);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_FAST_CHARGE, 3990, 5000, &o), 2);
	(void)opensc2_charger_step(&c, false, OPENSC2_CHARGER_NOT_CHARGING, 4010, 6000, &o);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 4010, 7000, &o), 4);
	/* the window removed: the configured target */
	opensc2_charger_window(&c, 0);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 4010, 8000, &o), 1);
	CHECK(o.set_charge && o.charge_mv == 4120);

	/* a board's own numbers */
	opensc2_charger_defaults(&cfg);
	cfg.target_mv = 4200;
	cfg.vin_start_mv = 4800;
	cfg.vin_min_mv = 4700;
	opensc2_charger_init(&c, &cfg, 0, &o);
	CHECK_EQ(o.charge_mv, 4200);
	(void)opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3900, 1000, &o);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3900, 2000, &o), 3);
	CHECK_EQ(o.charge_mv, 4240);
	CHECK_EQ(opensc2_charger_step(&c, true, OPENSC2_CHARGER_NOT_CHARGING, 3900, 7000, &o), 0x81);
	CHECK_EQ(o.vin_limit_mv, 4800);
}
