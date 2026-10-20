/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only, in the binary built with CONFIG_OPENSC2_TEST_HOOKS, after hooks_cases.h (whose
 * helpers it uses): the raw injection of the calibration (RAW_INJECT, RAW_OFF, CAL_STATUS), on the
 * frame path as report.c builds it: the raw inputs processed (cal_store.c), then the injection,
 * then the pad rules.
 */
#include <string.h>
#include <opensc2/cal.h>
#include "cal_store.h"

static void cal_build_frame(const struct opensc2_state *app, struct opensc2_state *s,
			    uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	*s = *app;
	osc2_cal_frame(s);
	osc2_test_input(s);
	osc2_cal_rules(s);
	opensc2_encode_state(s, frame_seq++, report);
	osc2_test_frame(report);
}

static uint8_t raw_args[OSC2_TEST_RAW_ARGS];

static int raw_inject(uint8_t flags, uint32_t nonce, uint8_t present, uint16_t lx, uint16_t ly,
		      int16_t lstick_touch, int16_t rgrip)
{
	memset(raw_args, 0, sizeof(raw_args));
	raw_args[0] = flags;
	opensc2_put_le32(&raw_args[1], nonce);
	raw_args[5] = present;
	opensc2_put_le16(&raw_args[6], lx);
	opensc2_put_le16(&raw_args[8], ly);
	opensc2_put_le16(&raw_args[14], 3000);                 /* left trigger */
	opensc2_put_le16(&raw_args[18], (uint16_t)-20);       /* left pressure */
	opensc2_put_le16(&raw_args[22], (uint16_t)lstick_touch);
	opensc2_put_le16(&raw_args[28], (uint16_t)rgrip);
	return usb_op(OSC2_TEST_OP_RAW_INJECT, raw_args, sizeof(raw_args));
}

OPENSC2_TEST(hooks_raw_args)
{
	struct opensc2_raw_inputs raw;
	uint32_t nonce;
	uint8_t flags;

	memset(raw_args, 0, sizeof(raw_args));
	raw_args[0] = 0;
	opensc2_put_le32(&raw_args[1], 0x11223344u);
	raw_args[5] = 0xFF;
	for (int i = 0; i < 12; i++) {
		opensc2_put_le16(&raw_args[6 + 2 * i], (uint16_t)(1000 + i));
	}
	opensc2_put_le16(&raw_args[18], (uint16_t)-5);
	osc2_test_raw_args(raw_args, &raw, &nonce, &flags);
	CHECK_EQ(nonce, 0x11223344u);
	CHECK_EQ(flags, 0);
	CHECK_EQ(raw.present, 0xFF);
	CHECK_EQ(raw.stick[0][0], 1000);   /* LX, LY, RX, RY */
	CHECK_EQ(raw.stick[0][1], 1001);
	CHECK_EQ(raw.stick[1][0], 1002);
	CHECK_EQ(raw.stick[1][1], 1003);
	CHECK_EQ(raw.trigger[0], 1004);
	CHECK_EQ(raw.trigger[1], 1005);
	CHECK_EQ(raw.pressure[0], -5);
	CHECK_EQ(raw.pressure[1], 1007);
	CHECK_EQ(raw.stick_touch[0], 1008);
	CHECK_EQ(raw.stick_touch[1], 1009);
	CHECK_EQ(raw.grip[0], 1010);
	CHECK_EQ(raw.grip[1], 1011);
}

OPENSC2_TEST(hooks_raw_inject)
{
	const uint8_t on9[3] = { 9, 1, 0 };
	struct opensc2_state app, s;
	uint8_t report[OPENSC2_STATE_REPORT_LEN];
	const uint8_t present = OPENSC2_RAW_STICK_L | OPENSC2_RAW_TRIGGER_L |
				OPENSC2_RAW_PRESSURE_L | OPENSC2_RAW_TOUCH_L | OPENSC2_RAW_TOUCH_R;
	int i;

	stub_reset();
	memset(&app, 0, sizeof(app));
	app.stick[OPENSC2_RIGHT].x = 555;   /* the application's own, not raw */
	/* unarmed: nothing, and the ops need arming */
	CHECK_EQ(con_op(OSC2_TEST_OP_CAL_STATUS, NULL, 0), OSC2_TEST_NOT_ARMED);
	CHECK_EQ(arm_on(true, 7, 0, 0), OSC2_TEST_OK);
	/* lizard mode is on after a boot: refused */
	CHECK_EQ(raw_inject(0, 0xCA1B0001u, present, 1000, 3000, 0, 0), OSC2_TEST_DENIED);
	lizard_off_usb();
	CHECK_EQ(usb_op(OSC2_TEST_OP_RAW_INJECT, raw_args, 29), OSC2_TEST_BAD_LEN);
	CHECK_EQ(raw_inject(1, 0xCA1B0001u, present, 1000, 3000, 0, 0), OSC2_TEST_BAD_ARG);
	CHECK_EQ(raw_inject(0, 0xCA1B0001u, present, 1000, 3000, 1100, 1200), OSC2_TEST_OK);
	CHECK_EQ(opensc2_get_le32(RES), 0);   /* frames at the request */

	/* processed as raw inputs: uncalibrated sticks and triggers read 0, the nonce marks it */
	cal_build_frame(&app, &s, report);
	CHECK_EQ(s.imu.timestamp_us, 0xCA1B0001u);
	CHECK_EQ(opensc2_get_le32(&report[30]), 0xCA1B0001u);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, 0);
	CHECK_EQ(s.trigger[OPENSC2_LEFT], 0);
	CHECK_EQ(s.stick[OPENSC2_RIGHT].x, 555);   /* not injected: the application's */
	CHECK(s.buttons & OPENSC2_TOUCH_LEFT_STICK);   /* 1100: the first word fills both */
	CHECK(s.buttons & OPENSC2_TOUCH_RIGHT_GRIP);   /* 1200 */
	CHECK(osc2_test_skip_lizard());                /* kept away from lizard mode */
	/* settings 46 and 62 on the injected counts */
	osc2.settings[OPENSC2_SETTING_RAW_STICKS] = 2;
	osc2.settings[OPENSC2_SETTING_RAW_TRIGGERS] = 1;
	cal_build_frame(&app, &s, report);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, 1000);
	CHECK_EQ(s.stick[OPENSC2_LEFT].y, 3000);
	CHECK_EQ(s.trigger[OPENSC2_LEFT], 1208);   /* 3000 x 1650 / 4096 */
	osc2.settings[OPENSC2_SETTING_RAW_STICKS] = 0;
	osc2.settings[OPENSC2_SETTING_RAW_TRIGGERS] = 0;
	/* an injected state replaces what the raw inputs became */
	CHECK_EQ(inject(true, 0, OPENSC2_BTN_B, 0xA5A50009u), OSC2_TEST_OK);
	cal_build_frame(&app, &s, report);
	CHECK_EQ(s.buttons, OPENSC2_BTN_B);
	CHECK_EQ(s.imu.timestamp_us, 0xA5A50009u);
	CHECK_EQ(usb_op(OSC2_TEST_OP_INJECT_OFF, NULL, 0), OSC2_TEST_OK);

	/* CAL_STATUS: nothing calibrated, the inputs injected and processed, the build's options */
	CHECK_EQ(con_op(OSC2_TEST_OP_CAL_STATUS, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hans_len, 4 + OSC2_TEST_CAL_STATUS_LEN);
	CHECK_EQ(RES[4], 0);          /* usable */
	CHECK_EQ(RES[5], present);    /* raw inputs of the last frame */
	CHECK_EQ(RES[6], present);    /* injected */
	CHECK_EQ(RES[7], 0x03);       /* pad rules, pressure click */
	CHECK_EQ(RES[8], 0);          /* no recording */
	/* the left pressure filter: without a usable calibration it does not follow the raw words
	 * (three frames of -20), it stays at 0 */
	CHECK_EQ((int16_t)opensc2_get_le16(&RES[11]), 0);

	/* RAW_OFF: the application's again */
	CHECK_EQ(usb_op(OSC2_TEST_OP_RAW_OFF, NULL, 0), OSC2_TEST_OK);
	cal_build_frame(&app, &s, report);
	CHECK_EQ(s.imu.timestamp_us, 0);
	CHECK(!(s.buttons & OPENSC2_TOUCH_LEFT_STICK));
	CHECK(!osc2_test_skip_lizard());
	CHECK_EQ(con_op(OSC2_TEST_OP_CAL_STATUS, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(RES[6], 0);

	/* lizard mode coming on stops it in that frame: INJECT_STOP with what bit 2 */
	CHECK_EQ(raw_inject(0, 0xCA1B0002u, present, 1, 2, 0, 0), OSC2_TEST_OK);
	cal_build_frame(&app, &s, report);
	CHECK_EQ(s.imu.timestamp_us, 0xCA1B0002u);
	CHECK_EQ(usb_set(0x87, on9, sizeof(on9)), 0);
	cal_build_frame(&app, &s, report);
	CHECK_EQ(s.imu.timestamp_us, 0);
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_INJECT_STOP, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].d[0], 1);
		CHECK_EQ(ents[i].d[5], 0x04);
	}

	/* the end of arming stops it too */
	lizard_off_usb();
	CHECK_EQ(raw_inject(0, 0xCA1B0003u, present, 1, 2, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(usb_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	cal_build_frame(&app, &s, report);
	CHECK_EQ(s.imu.timestamp_us, 0);
	CHECK_EQ(hk.violations, 0);
	osc2_cal_reset();
}
