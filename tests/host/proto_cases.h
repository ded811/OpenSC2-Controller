/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the rules of the vendor commands the protocol pages describe answer by answer
 * (proto_rules.c), and the command channel giving those answers (commands.c with the stand-ins of
 * stubs.c): 0x89 for the IMU's settings, 0xE9's read-back of the LED colour.
 */
#include <string.h>
#include "internal.h"
#include "proto_rules.h"
#include "stubs.h"

/* SET_REPORT(Feature 1) then GET_REPORT(Feature 1), the July form (64 bytes): the answer's bytes
 * from reply[3] on; returns n, or -1 when either stage stalled. */
static int proto_usb(uint8_t opcode, const void *payload, uint8_t n, uint8_t reply[64])
{
	uint8_t frame[64] = { 1, opcode, n };
	uint8_t op, got;

	if (n) {
		memcpy(&frame[3], payload, n);
	}
	if (osc2_usb_feature_set(frame, sizeof(frame)) != 0) {
		return -1;
	}
	memset(reply, 0xEE, 64);
	if (osc2_usb_feature_get(reply, 64, 64, &op, &got) != 64) {
		return -1;
	}
	return got;
}

OPENSC2_TEST(proto_setting_answer)
{
	/* The IMU's three settings answer their defaults, whatever is kept. */
	CHECK_EQ(osc2_setting_answer(OPENSC2_SETTING_IMU_MODE, 0x18), 0);
	CHECK_EQ(osc2_setting_answer(OPENSC2_SETTING_IMU_BIAS, 0), 1);
	CHECK_EQ(osc2_setting_answer(OPENSC2_SETTING_IMU_THRESHOLD, 50), 100);
	/* Every other one its value. */
	CHECK_EQ(osc2_setting_answer(OPENSC2_SETTING_LIZARD_MODE, 0), 0);
	CHECK_EQ(osc2_setting_answer(OPENSC2_SETTING_LED_BRIGHTNESS, 100), 100);
	CHECK_EQ(osc2_setting_answer(47, -5), -5);
	CHECK_EQ(osc2_setting_answer(49, 2), 2);
	CHECK_EQ(osc2_setting_answer(83, 7), 7);
	CHECK_EQ(osc2_setting_answer(86, 9), 9);
}

OPENSC2_TEST(proto_led_readback)
{
	CHECK_EQ(osc2_led_readback(100), 100);
	CHECK_EQ(osc2_led_readback(0), 0);
	CHECK_EQ(osc2_led_readback(1), 0);
	CHECK_EQ(osc2_led_readback(50), 0);
	CHECK_EQ(osc2_led_readback(99), 0);
	CHECK_EQ(osc2_led_readback(101), 0);   /* (0xC5 clamps to 100 before it is kept) */
	CHECK_EQ(osc2_led_readback(255), 0);
}

OPENSC2_TEST(proto_cmd_imu_settings)
{
	const uint8_t writes[9] = { 48, 0x18, 0, 84, 0, 0, 85, 50, 0 };
	uint8_t reply[64];

	stub_reset();
	CHECK_EQ(proto_usb(0x87, writes, sizeof(writes), reply), -1);   /* set-only: the GET stalls */
	/* The application sees what Steam wrote ... */
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_IMU_MODE], 0x18);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_IMU_BIAS], 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_IMU_THRESHOLD], 50);
	/* ... and 0x89 answers the defaults, as the real controller's IMU keeps the values. */
	for (int i = 0; i < 3; i++) {
		static const uint8_t numbers[3] = { 48, 84, 85 };
		static const int16_t defaults[3] = { 0, 1, 100 };

		CHECK_EQ(proto_usb(0x89, &numbers[i], 1, reply), 3);
		CHECK_EQ(reply[3], numbers[i]);
		CHECK_EQ((int16_t)opensc2_get_le16(&reply[4]), defaults[i]);
	}
	/* A neighbour answers what was written. */
	{
		const uint8_t w49[3] = { 49, 2, 0 }, n49 = 49;

		CHECK_EQ(proto_usb(0x87, w49, sizeof(w49), reply), -1);
		CHECK_EQ(proto_usb(0x89, &n49, 1, reply), 3);
		CHECK_EQ(opensc2_get_le16(&reply[4]), 2);
	}
}

OPENSC2_TEST(proto_cmd_led_readback)
{
	const uint8_t colour[4] = { 100, 0, 50, 10 }, full[4] = { 100, 100, 100, 100 };
	const uint8_t over[4] = { 0xFF, 99, 101, 1 };
	uint8_t reply[64];

	stub_reset();
	CHECK_EQ(proto_usb(0xC5, colour, sizeof(colour), reply), -1);   /* set-only */
	/* The colour is kept as sent (the led callback and opensc2_led_get() see it) ... */
	CHECK_EQ(osc2.led.red, 100);
	CHECK_EQ(osc2.led.blue, 50);
	CHECK_EQ(osc2.led.white, 10);
	/* ... and read back as the real controller reads it: 100 stays, anything else is 0. */
	CHECK_EQ(proto_usb(0xE9, NULL, 0, reply), 4);
	CHECK(memcmp(&reply[3], (const uint8_t[]){ 100, 0, 0, 0 }, 4) == 0);
	CHECK_EQ(proto_usb(0xC5, full, sizeof(full), reply), -1);
	CHECK_EQ(proto_usb(0xE9, NULL, 0, reply), 4);
	CHECK(memcmp(&reply[3], full, 4) == 0);
	/* 0xC5 clamps to 100 first: 255 becomes 100 and reads back as 100. */
	CHECK_EQ(proto_usb(0xC5, over, sizeof(over), reply), -1);
	CHECK_EQ(osc2.led.red, 100);
	CHECK_EQ(osc2.led.blue, 100);
	CHECK_EQ(proto_usb(0xE9, NULL, 0, reply), 4);
	CHECK(memcmp(&reply[3], (const uint8_t[]){ 100, 0, 100, 0 }, 4) == 0);
}
