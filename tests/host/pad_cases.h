/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the trackpads frame by frame (<opensc2/trackpad.h>, pad.c and pad_rules.c), as
 * an application with a trackpad sensor feeds them and the state reports carry them: the position
 * path (scale, filter, rotation, stabiliser, Y up), the stick touch with its deflection rule, the
 * grip, the pressure and its click from cal/prs_*, setting 82, setting 60, the start-overs, the
 * factory pressure recording and the hand-over between frames and finished values. Uses the helpers
 * of cal_store_cases.h.
 */
#include <opensc2/trackpad.h>
#include "pad_rules.h"

static const uint8_t pad_prs_bytes[OPENSC2_CAL_PRESSURE_LEN] = {
	/* format 1, A 100, B 2100, gain 2000 */
	0x01, 0x00, 0x64, 0x00, 0x34, 0x08, 0xD0, 0x07,
};

static struct opensc2_pad_frame pad_at(bool touched, int32_t x, int32_t y)
{
	return (struct opensc2_pad_frame){ .touched = touched, .x = x, .y = y };
}

OPENSC2_TEST(pad_frames_position)
{
	struct opensc2_state s;
	struct opensc2_pad_frame f = pad_at(true, 750000, 250000);
	struct opensc2_pad_result r;

	stub_reset();
	CHECK_EQ(opensc2_pad_frame((enum opensc2_side)2, &f), -EINVAL);
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, NULL), -EINVAL);
	CHECK_EQ(osc2_pad_frame_sides(), 0);
	/* the filter (setting 65) off: the pad scale doubled, Y up */
	osc2.settings[OPENSC2_SETTING_PAD_FILTER] = 0;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(osc2_pad_frame_sides(), 2);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(r.frames);
	CHECK(r.touched);
	CHECK_EQ(r.x, 2 * opensc2_pad_scale(750000));
	CHECK_EQ(r.y, -2 * opensc2_pad_scale(250000));
	CHECK_EQ(r.x, 16382);
	CHECK_EQ(r.pressure, 0);   /* no pressure word */
	/* into the next state report, over the application's values for that side */
	cal_frame_state(&s);
	CHECK(s.pad[OPENSC2_RIGHT].touched);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].x, 16382);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].y, 16382);
	CHECK(s.buttons & OPENSC2_TOUCH_RIGHT_PAD);
	CHECK_EQ(s.buttons & OPENSC2_TOUCH_LEFT_PAD, 0);
	/* lifted: 0, 0 and no touch */
	f = pad_at(false, 750000, 250000);
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	cal_frame_state(&s);
	CHECK(!s.pad[OPENSC2_RIGHT].touched);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].x, 0);
	CHECK_EQ(s.buttons & OPENSC2_TOUCH_RIGHT_PAD, 0);
	/* released: the side is the application's again */
	CHECK_EQ(opensc2_pad_frame_release(OPENSC2_RIGHT), 0);
	CHECK_EQ(osc2_pad_frame_sides(), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(!r.frames);
}

OPENSC2_TEST(pad_frames_filter_rotation_stabiliser)
{
	struct opensc2_pad_frame f;
	struct opensc2_pad_result r;
	struct opensc2_pad_filter ref;
	int16_t fx, fy;

	stub_reset();
	/* setting 65 (on by default): the speed-adaptive filter on the pad scale, from the touch */
	opensc2_pad_filter_reset(&ref);
	for (int i = 0; i < 6; i++) {
		int32_t x = 500000 + 60000 * i;

		f = pad_at(true, x, 500000);
		CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
		fx = opensc2_pad_scale(x);
		fy = opensc2_pad_scale(500000);
		opensc2_pad_filter_step(&ref, &fx, &fy, 250.0f);
		CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
		CHECK_EQ(r.x, 2 * fx);
		CHECK_EQ(r.y, -2 * fy);
	}
	CHECK(r.x < 2 * opensc2_pad_scale(800000));   /* it lags a moving thumb */
	/* a frame without a touch starts the filter over */
	f = pad_at(false, 0, 0);
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	f = pad_at(true, 900000, 500000);
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	opensc2_pad_filter_reset(&ref);
	fx = opensc2_pad_scale(900000);
	fy = 0;
	opensc2_pad_filter_step(&ref, &fx, &fy, 250.0f);
	CHECK_EQ(r.x, 2 * fx);

	/* the rotation (2 and 24): the left pad by minus the angle, on the pad scale */
	osc2.settings[OPENSC2_SETTING_PAD_FILTER] = 0;
	osc2.settings[OPENSC2_SETTING_PAD_ROTATION_ANGLE] = 90;
	osc2.settings[OPENSC2_SETTING_PAD_ROTATION] = 1;
	f = pad_at(true, 750000, 500000);   /* 8191 to the right on the pad scale */
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	CHECK_EQ(r.x, 0);
	CHECK_EQ(r.y, -2 * (8191 * 8191 / 8192));   /* turned down the pad (Y up in the report) */
	osc2.settings[OPENSC2_SETTING_PAD_ROTATION] = 0;

	/* the stabiliser (80 = 2): held at the touch-down point within 4369 */
	osc2.settings[OPENSC2_SETTING_PAD_STABILIZER] = 2;
	f = pad_at(false, 0, 0);
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	static const int32_t xs[] = { 500000, 530000, 560000, 570000 };
	static const int16_t want[] = { 0, 0, 0, 1148 };   /* 656 moved + an eighth of 3930 + 1 */

	for (size_t i = 0; i < ARRAY_SIZE(xs); i++) {
		f = pad_at(true, xs[i], 500000);
		CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
		CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
		CHECK_EQ(r.x, want[i]);
	}
}

OPENSC2_TEST(pad_frames_stick_touch_and_grip)
{
	struct opensc2_state s;
	struct opensc2_pad_frame f = pad_at(true, 500000, 500000);
	struct opensc2_pad_result r;

	stub_reset();
	f.has = OPENSC2_PAD_HAS_STICK_TOUCH | OPENSC2_PAD_HAS_GRIP;
	/* stick touch above setting 30 (500), held while the mean is at least 31 (400) */
	f.stick_touch = 600;
	f.grip = 1200;   /* above 1125 */
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	CHECK(r.stick_touched);
	CHECK(r.grip);
	/* setting 82 (default 3): the touched stick blanks this pad, the bits stay */
	CHECK(!r.touched);
	CHECK_EQ(r.x, 0);
	cal_frame_state(&s);
	CHECK(s.buttons & OPENSC2_TOUCH_LEFT_STICK);
	CHECK(s.buttons & OPENSC2_TOUCH_LEFT_GRIP);
	CHECK_EQ(s.buttons & OPENSC2_TOUCH_LEFT_PAD, 0);
	f.stick_touch = 200;   /* mean 400: held */
	f.grip = 800;          /* mean 1000: held (off below 900) */
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	CHECK(r.stick_touched);
	CHECK(r.grip);
	f.stick_touch = 100;   /* mean 150: off */
	f.grip = 0;            /* mean 400: off */
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	CHECK(!r.stick_touched);
	CHECK(!r.grip);
	CHECK(r.touched);   /* the pad is back */

	/* the deflection rule: the last report's stick beyond 12000 */
	opensc2_set_stick_raw(OPENSC2_LEFT, 0, 0);   /* raw, uncalibrated: reads 0, 0 */
	cal_frame_state(&s);
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	CHECK(!r.stick_touched);
	CHECK_EQ(opensc2_cal_raw_release(OPENSC2_RAW_STICK_L), 0);
	osc2.state.stick[OPENSC2_LEFT].x = 20000;   /* the application's finished stick */
	memset(&s, 0, sizeof(s));
	s.stick[OPENSC2_LEFT].x = 20000;
	osc2_cal_frame(&s);
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	CHECK(r.stick_touched);
	CHECK(!r.touched);   /* and blanks the pad */
	/* not with raw sticks (setting 46): they are no positions */
	osc2.settings[OPENSC2_SETTING_RAW_STICKS] = 2;
	osc2_cal_frame(&s);
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	CHECK(!r.stick_touched);
	osc2.settings[OPENSC2_SETTING_RAW_STICKS] = 0;

	/* without the words, the application's bits stand in (a capacitive key, for example) */
	memset(&s, 0, sizeof(s));
	osc2_cal_frame(&s);
	f = pad_at(true, 500000, 500000);
	osc2.state.buttons = OPENSC2_TOUCH_RIGHT_STICK | OPENSC2_TOUCH_RIGHT_GRIP;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(r.stick_touched);
	CHECK(r.grip);
	CHECK(!r.touched);   /* 82: blanked by the key */
	osc2.state.buttons = 0;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(!r.stick_touched);
	CHECK(r.touched);
}

OPENSC2_TEST(pad_frames_pressure_and_click)
{
	struct opensc2_state s;
	struct opensc2_pad_frame f = pad_at(true, 500000, 500000);
	struct opensc2_pad_result r;
	struct opensc2_pressure_proc ref = { 0 };
	struct opensc2_cal_pressure c;
	uint8_t out[64];

	stub_reset();
	f.has = OPENSC2_PAD_HAS_PRESSURE;
	f.pressure = 2100;
	/* uncalibrated: pressure 0, no click */
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK_EQ(r.pressure, 0);
	CHECK(!r.clicked);
	/* calibrated (cal/prs_r): the filter, the stage and the click of setting 53 */
	CHECK_EQ(cal_named(0xEE, "cal/prs_r", pad_prs_bytes, 8), 0);
	opensc2_cal_pressure_unpack(pad_prs_bytes, &c);
	opensc2_pressure_setup(&ref, &c);
	for (int i = 0; i < 5; i++) {
		CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
		opensc2_pressure_sample(&ref, 2100, 40);
		CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
		CHECK_EQ(r.pressure, ref.last);
		CHECK_EQ(r.clicked, ref.click);
	}
	CHECK(r.clicked);
	cal_frame_state(&s);
	CHECK(s.buttons & OPENSC2_CLICK_RIGHT_PAD);
	CHECK(s.pad[OPENSC2_RIGHT].clicked);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].pressure, ref.last);
	/* lifted: no pressure, no click (the filter goes on) */
	f.touched = false;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK_EQ(r.pressure, 0);
	CHECK(!r.clicked);
	/* setting 60: the raw word, also untouched */
	osc2.settings[OPENSC2_SETTING_RAW_PRESSURE] = 1;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK_EQ(r.pressure, 2100);
	osc2.settings[OPENSC2_SETTING_RAW_PRESSURE] = 0;
	/* a change of setting 53 starts the pads over: the click clears and is worked out again */
	f.touched = true;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(r.clicked);
	osc2.settings[OPENSC2_SETTING_RIGHT_CLICK_PRESSURE] = 100;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(!r.clicked);   /* 108 % is not reached at B */

	/* the factory pressure recording takes the frames' words */
	CHECK_EQ(cal_set(0xC3, (const uint8_t[]){ 1 }, 1), 0);
	CHECK_EQ(cal_set(0xC3, (const uint8_t[]){ 2 }, 1), 0);
	f.pressure = 2600;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	f.pressure = 2400;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(cal_set(0xC3, (const uint8_t[]){ 3 }, 1), 0);
	osc2_cal_flush();
	CHECK_EQ(cal_read("cal/prs_r", out), 8);
	opensc2_cal_pressure_unpack(out, &c);
	CHECK_EQ(c.b, 2600);
	CHECK_EQ(cal_read("cal/prs_l", out), 0);   /* no left pad on this board */
	/* the new key starts the pad over at its next frame */
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(!r.clicked);

	/* a board without a pressure word keeps its click button (only while touched) */
	f = pad_at(true, 500000, 500000);
	osc2.state.buttons = OPENSC2_CLICK_LEFT_PAD;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	CHECK(r.clicked);
	f.touched = false;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_LEFT, &r), 0);
	CHECK(!r.clicked);
	osc2.state.buttons = 0;
}

OPENSC2_TEST(pad_frames_raw_sensor)
{
	static const float valve_right[4] = { 0.11f, 0.88f, 0.05f, 0.91f };
	struct opensc2_pad_frame f = { .has = OPENSC2_PAD_HAS_LEVEL | OPENSC2_PAD_HAS_CENTROID };
	struct opensc2_pad_result r;
	struct opensc2_pad_touch ref;
	int32_t x, y;

	stub_reset();
	osc2.settings[OPENSC2_SETTING_PAD_FILTER] = 0;
	/* the level decides the touch (72 on 1300, 73 held 1000), not `touched` */
	f.touched = true;
	f.level = 1200;
	f.x = 900000;
	f.y = 500000;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(!r.touched);
	f.level = 2000;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(r.touched);
	/* no stretch by default: the edge push only (0.9 pushed to 0.9175) */
	x = 900000;
	y = 500000;
	opensc2_pad_centroid(&x, &y, false, NULL);
	CHECK_EQ(x, 917499);
	CHECK_EQ(r.x, 2 * opensc2_pad_scale(x));
	CHECK_EQ(r.y, 0);
	/* a stretch of the board's own, while setting 66 is on (the default) */
	CHECK_EQ(opensc2_pad_set_stretch(OPENSC2_RIGHT, valve_right), 0);
	CHECK_EQ(opensc2_pad_set_stretch(OPENSC2_RIGHT, (const float[4]){ 0, 0, 0, 1 }), -EINVAL);
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	x = 900000;
	y = 500000;
	opensc2_pad_centroid(&x, &y, true, valve_right);
	CHECK_EQ(r.x, 2 * opensc2_pad_scale(x));
	CHECK(r.x > 2 * opensc2_pad_scale(917499));
	/* setting 66 = 0: the edge push only */
	osc2.settings[OPENSC2_SETTING_PAD_CLIP] = 0;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK_EQ(r.x, 2 * opensc2_pad_scale(917499));
	/* near lift-off the damping holds the position back, as the helper does it */
	osc2.settings[OPENSC2_SETTING_PAD_CLIP] = 1;
	CHECK_EQ(opensc2_pad_set_stretch(OPENSC2_RIGHT, NULL), 0);
	f.level = 600;   /* lifted */
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	opensc2_pad_touch_reset(&ref);
	static const int32_t levels[] = { 2000, 2000, 1500, 1100 };
	static const int32_t xs[] = { 600000, 610000, 700000, 800000 };

	for (size_t i = 0; i < ARRAY_SIZE(levels); i++) {
		f.level = levels[i];
		f.x = xs[i];
		f.y = 500000;
		f.has = OPENSC2_PAD_HAS_LEVEL;   /* positions as they are */
		CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
		x = xs[i];
		y = 500000;
		CHECK(opensc2_pad_touch_step(&ref, levels[i], 1300, 1000, &x, &y));
		CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
		CHECK_EQ(r.x, 2 * opensc2_pad_scale(x));
	}
	CHECK(x < 800000);   /* damped */
}

OPENSC2_TEST(pad_frames_raw_sensor_past_edge)
{
	struct opensc2_pad_frame f = { .has = OPENSC2_PAD_HAS_LEVEL | OPENSC2_PAD_HAS_CENTROID };
	struct opensc2_pad_result r;
	float d, pushed, t, damp, expect;

	stub_reset();
	osc2.settings[OPENSC2_SETTING_PAD_FILTER] = 0;
	/* touch-down at 0.98: the edge push takes it past the edge (1.0983), reported as 1 */
	f.level = 2000;
	f.x = 980000;
	f.y = 500000;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(r.touched);
	CHECK_EQ(r.x, 2 * opensc2_pad_scale(1000000));
	/* near lift-off the damping starts from the pushed position, not from the edge: the real
	 * controller's driver limits the position only when it reports it */
	f.level = 1100;   /* running average 1887: damping between 1000 and 1687 */
	f.x = 600000;
	CHECK_EQ(opensc2_pad_frame(OPENSC2_RIGHT, &f), 0);
	CHECK_EQ(opensc2_pad_result(OPENSC2_RIGHT, &r), 0);
	CHECK(r.touched);
	d = 0.98f - 0.5f;
	pushed = 0.98f + (d - 0.35f) * 7.0f * (d - 0.35f);
	t = (1100.0f - 1000.0f) / (1687.0f - 1000.0f);
	damp = fmaf(t, 1.0f - 0.2f, 0.2f);
	expect = fmaf(0.6f - pushed, damp, pushed);
	CHECK(expect > 0.9f);   /* from the edge it would be about 0.873 */
	CHECK_EQ(r.x, 2 * opensc2_pad_scale((int32_t)(expect * 1000000.0f)));
}

OPENSC2_TEST(pad_frames_live_bits)
{
	struct opensc2_state s;
	struct opensc2_pad_frame f = pad_at(true, 500000, 500000);

	stub_reset();
	/* a pad that reports only movements: the click switch and a capacitive key change between
	 * two of its frames, and the next report has them */
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	memset(&s, 0, sizeof(s));
	s.buttons = OPENSC2_CLICK_LEFT_PAD;
	osc2_cal_frame(&s);
	CHECK(s.pad[OPENSC2_LEFT].clicked);
	CHECK(s.buttons & OPENSC2_CLICK_LEFT_PAD);
	memset(&s, 0, sizeof(s));
	s.buttons = OPENSC2_TOUCH_LEFT_STICK;   /* the key: 82 blanks the pad at once */
	osc2_cal_frame(&s);
	CHECK(!s.pad[OPENSC2_LEFT].touched);
	CHECK(s.buttons & OPENSC2_TOUCH_LEFT_STICK);
	memset(&s, 0, sizeof(s));
	osc2_cal_frame(&s);
	CHECK(s.pad[OPENSC2_LEFT].touched);
	CHECK(!s.pad[OPENSC2_LEFT].clicked);
}

OPENSC2_TEST(pad_frames_report_rules)
{
	struct osc2_input_rules rules;
	struct opensc2_state s;
	struct opensc2_pad_frame f = pad_at(true, 500000, 500000);
	int16_t settings[OPENSC2_SETTINGS_COUNT];

	stub_reset();
	for (int i = 0; i < OPENSC2_SETTINGS_COUNT; i++) {
		settings[i] = opensc2_setting_default((uint8_t)i);
	}
	CHECK_EQ(opensc2_pad_frame(OPENSC2_LEFT, &f), 0);
	cal_frame_state(&s);
	/* the report's own rules leave a side fed by frames alone: the stick deflected in this
	 * report does not blank the pad (the frames decide, with the stick they last saw) */
	s.stick[OPENSC2_LEFT].x = 30000;
	osc2_input_rules_reset(&rules);
	rules.pad_sides = osc2_pad_frame_sides();
	osc2_input_rules_apply(&rules, OSC2_RULES_DEFAULT, settings, &s);
	CHECK(s.pad[OPENSC2_LEFT].touched);
	CHECK_EQ(s.buttons & OPENSC2_TOUCH_LEFT_STICK, 0);
	/* the other side still gets them */
	s.stick[OPENSC2_RIGHT].x = 30000;
	s.pad[OPENSC2_RIGHT].touched = true;
	s.buttons |= OPENSC2_TOUCH_RIGHT_PAD;
	rules.pad_sides = osc2_pad_frame_sides();
	osc2_input_rules_apply(&rules, OSC2_RULES_DEFAULT, settings, &s);
	CHECK(s.buttons & OPENSC2_TOUCH_RIGHT_STICK);
	CHECK(!s.pad[OPENSC2_RIGHT].touched);
}
