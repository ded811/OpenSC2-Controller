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
	/* The real controller keeps values above 100 and reads them back cut to whole hundreds (the
	 * library clamps at 0xC5, proto_cmd_led_readback below, so these never reach its answers). */
	CHECK_EQ(osc2_led_readback(101), 100);
	CHECK_EQ(osc2_led_readback(199), 100);
	CHECK_EQ(osc2_led_readback(200), 200);
	CHECK_EQ(osc2_led_readback(255), 200);
}

/* The 0x44 replies of one output report, decoded as the library decodes it: their channel IDs,
 * in order, as a number (12 = left grip then right pad... read digit by digit, +1 each: 0 none). */
static uint32_t stop_ids(const uint8_t *report, size_t len)
{
	struct opensc2_output out[2];
	uint32_t code = 0;
	int n = opensc2_decode_output(report, len, out);

	for (int i = 0; i < n; i++) {
		uint8_t ids[4];
		uint8_t k = osc2_stop_status(&out[i], ids);

		for (uint8_t j = 0; j < k; j++) {
			code = code * 10 + ids[j] + 1;
		}
	}
	return code;
}

OPENSC2_TEST(proto_stop_status)
{
	/* IDs + 1, digit by digit: left grip 1, right grip 2, left pad 4, right pad 5. */
	static const struct {
		uint8_t report[10];
		uint8_t len;
		uint32_t want;
	} cases[] = {
		/* Steam's zero pulses: target 0 is the right pad for 0x81, target 1 the left */
		{ { 0x81, 0, 0, 0, 0, 0, 0, 0 }, 8, 5 },
		{ { 0x81, 1, 0, 0, 0, 0, 0, 0 }, 8, 4 },
		/* only the exact values swap: 0x80 is the left pad, 0x81 the right one (44 03 02 and
		 * 44 04 02) */
		{ { 0x81, 0x80, 0, 0, 0, 0, 0, 0 }, 8, 4 },
		{ { 0x81, 0x81, 0, 0, 0, 0, 0, 0 }, 8, 5 },
		/* on time 0 is a stop whatever else is there; two channels: the left first */
		{ { 0x81, 2, 0, 0, 0xFF, 0xFF, 3, 0 }, 8, 45 },
		{ { 0x81, 7, 0, 0, 0, 0, 0, 0 }, 8, 12 },
		{ { 0x81, 8, 0, 0, 0, 0, 0, 0 }, 8, 0 },      /* a target that selects nothing */
		/* a pulse train, a click: no stop */
		{ { 0x81, 2, 0xE8, 3, 0xE8, 3, 2, 0 }, 8, 0 },
		{ { 0x81, 2, 0xE8, 3, 0, 0, 2, 0 }, 8, 0 },
		/* 0x82: command 0 stops, the clicks do not */
		{ { 0x82, 6, 0, 0 }, 4, 45 },
		{ { 0x82, 3, 0, 0xF6 }, 4, 1 },
		{ { 0x82, 2, 1, 0 }, 4, 0 },
		{ { 0x82, 2, 2, 0 }, 4, 0 },
		/* 0x80: a zero speed stops that grip, the right one first */
		{ { 0x80, 0, 0xE8, 3, 0x10, 0x27, 0, 0, 0, 0 }, 10, 2 },
		{ { 0x80, 0, 0xE8, 3, 0, 0, 0, 0x10, 0x27, 0 }, 10, 1 },
		{ { 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 10, 21 },
		{ { 0x80, 0, 0xE8, 3, 0x10, 0x27, 0, 0x20, 0x4E, 0 }, 10, 0 },
		/* 0x86: the stream stop, the right channel first, in 0x86's own numbering */
		{ { 0x86, 1, 5, 0 }, 4, 54 },
		{ { 0x86, 1, 2, 0 }, 4, 21 },
		{ { 0x86, 1, 0x40, 0 }, 4, 2 },
		{ { 0x86, 1, 0x80, 0 }, 4, 21 },
		{ { 0x86, 1, 3, 0 }, 4, 4 },
		{ { 0x86, 2, 5, 1 }, 4, 0 },
		{ { 0x86, 0, 5, 0 }, 4, 0 },
		/* nothing else: a tone, a sweep, a script, PCM data */
		{ { 0x83, 2, 0, 0, 0, 0, 0, 0, 0, 0 }, 10, 0 },
		{ { 0x84, 2, 0, 0, 0, 0, 0, 0, 0 }, 9, 0 },
		{ { 0x85, 2, 0, 0 }, 4, 0 },
		{ { 0x87, 2, 0, 0 }, 4, 0 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		uint32_t got = stop_ids(cases[i].report, cases[i].len);

		if (got != cases[i].want) {
			printf("  case %u: report 0x%02x target 0x%02x\n", (unsigned int)i,
			       cases[i].report[0], cases[i].report[1]);
		}
		CHECK_EQ(got, cases[i].want);
	}
	/* The IDs are 0x44's (opensc2_send_haptic_status()). */
	CHECK_EQ(OSC2_STATUS_LEFT_GRIP, 0);
	CHECK_EQ(OSC2_STATUS_RIGHT_GRIP, 1);
	CHECK_EQ(OSC2_STATUS_LEFT_PAD, 3);
	CHECK_EQ(OSC2_STATUS_RIGHT_PAD, 4);
	CHECK_EQ(OSC2_STATUS_STOPPED, 0x02);
}

#if defined(CONFIG_OPENSC2_TEST_HOOKS)
#include "test_core.h"

OPENSC2_TEST(proto_stop_status_entry)
{
	uint8_t e[OSC2_TEST_ENTRY_MAX], d[5] = { OSC2_STATUS_RIGHT_PAD, 0x02, 0, 0, 0 };
	char text[160];
	uint8_t n = osc2_test_entry(e, 1500, OSC2_TEST_KIND_STOP_STATUS, OSC2_TEST_LINK_USB, d, 5);

	CHECK_EQ(n, OSC2_TEST_ENTRY_HEADER + 5);
	osc2_test_fmt_entry(text, sizeof(text), 9, e, n);
	CHECK(strcmp(text, "9 1.500 usb STOP_STATUS channel=4 flags=0x2") == 0);
}
#endif

OPENSC2_TEST(proto_reset_flags)
{
	/* Zephyr's RESET_* bits: pin 0, software 1, brown-out 2, power-on 3, watchdog 4, debug 5,
	 * security 6, low-power wake 7, lock-up 8, ... flash 16. */
	CHECK_EQ(osc2_reset_flags(0), 0);
	CHECK_EQ(osc2_reset_flags(1u << 3), OSC2_RESET_POWER_ON);
	CHECK_EQ(osc2_reset_flags(1u << 2), OSC2_RESET_BROWNOUT);
	CHECK_EQ(osc2_reset_flags(1u << 0), OSC2_RESET_PIN);
	CHECK_EQ(osc2_reset_flags(1u << 4), OSC2_RESET_WATCHDOG);
	CHECK_EQ(osc2_reset_flags(1u << 1), OSC2_RESET_SOFTWARE);
	for (int bit = 5; bit <= 16; bit++) {
		CHECK_EQ(osc2_reset_flags(1u << bit), OSC2_RESET_OTHER);
	}
	/* They add up: a pin reset, then two reboots, then a watchdog, before a power-on. */
	CHECK_EQ(osc2_reset_flags((1u << 0) | (1u << 1) | (1u << 4)), 0x70);
	CHECK_EQ(osc2_reset_flags(0x1FFFF), 0x7B);
	CHECK_EQ(OSC2_RESET_POWER_ON | OSC2_RESET_BROWNOUT | OSC2_RESET_OTHER | OSC2_RESET_PIN |
		 OSC2_RESET_WATCHDOG | OSC2_RESET_SOFTWARE, 0x7B);
}

OPENSC2_TEST(proto_cmd_reset_info)
{
	uint8_t reply[64];

	stub_reset();
	/* Selector 1: 1, the flags byte, the chip's 8-byte ID, 8 zero bytes: 18 in all. (No chip on
	 * a PC: flags and ID read 0 here.) */
	CHECK_EQ(proto_usb(0xA1, (const uint8_t[]){ 1 }, 1, reply), 18);
	CHECK_EQ(reply[3], 1);
	CHECK(memcmp(&reply[4 + 9], (const uint8_t[8]){ 0 }, 8) == 0);
	CHECK_EQ(reply[3 + 18], 0);
	/* Any other selector: 0 and 17 zero bytes. */
	for (int sel = 0; sel < 256; sel += 51) {
		const uint8_t s = (uint8_t)sel;

		if (s == 1) {
			continue;
		}
		CHECK_EQ(proto_usb(0xA1, &s, 1, reply), 18);
		CHECK(memcmp(&reply[3], (const uint8_t[18]){ 0 }, 18) == 0);
	}
	/* No payload: selector 0. */
	CHECK_EQ(proto_usb(0xA1, NULL, 0, reply), 18);
	CHECK_EQ(reply[3], 0);
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
	/* A number above 85 is no setting: it reads 0, an answer and not a STALL, as on the real
	 * controller. */
	for (int number = 86; number < 256; number += 13) {
		const uint8_t n = (uint8_t)number;

		CHECK_EQ(proto_usb(0x89, &n, 1, reply), 3);
		CHECK_EQ(reply[3], n);
		CHECK_EQ(opensc2_get_le16(&reply[4]), 0);
	}
	{
		const uint8_t top = 0xFF;

		CHECK_EQ(proto_usb(0x89, &top, 1, reply), 3);
		CHECK(reply[3] == 0xFF && reply[4] == 0 && reply[5] == 0);
	}
}

/* 0x87 limits every setting to its documented range, as the real controller does (the documentation
 * gives all 86; 48, 84 and 85 go to the IMU unclamped); 0x89 reads the stored value back. */
OPENSC2_TEST(proto_cmd_settings_ranges)
{
	static const struct {
		uint8_t number;
		int16_t min, max;
	} some[] = {
		{ 2, 0, 360 }, { 8, 0, 1 }, { 24, 0, 30 }, { 27, -24, 12 }, { 46, 0, 2 },
		{ 52, -1, 100 }, { 60, 0, 1 }, { 62, 0, 2 }, { 75, 0, 1 }, { 76, -24, 6 },
		{ 80, 0, 2 }, { 82, 0, 3 }, { 28, 0, 4096 }, { 72, 0, 16000 }, { 25, 3, 99 },
	};
	static const int16_t probes[] = { -32768, -25, -2, -1, 0, 1, 2, 3, 4, 7, 9, 11, 13, 31,
					  100, 101, 181, 200, 256, 301, 361, 401, 501, 801, 1001,
					  1201, 2001, 4097, 16001, 16385, 20001, 25001, 32767 };
	uint8_t reply[64];

	stub_reset();
	/* every number, values around every range: what 0x87 stores is the clamped value */
	for (int n = 0; n < OPENSC2_SETTINGS_COUNT; n++) {
		const uint8_t number = (uint8_t)n;

		if (number == OPENSC2_SETTING_LIZARD_MODE || number == OPENSC2_SETTING_STEAM_WATCHDOG) {
			continue;   /* they switch lizard mode and the watchdog: their own tests */
		}
		for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
			uint8_t w[3] = { number };
			int16_t want = opensc2_setting_clamp(number, probes[i]);

			opensc2_put_le16(&w[1], (uint16_t)probes[i]);
			CHECK_EQ(proto_usb(0x87, w, sizeof(w), reply), -1);   /* set-only */
			CHECK_EQ(osc2.settings[number], want);
			CHECK_EQ(proto_usb(0x89, &number, 1, reply), 3);
			CHECK_EQ((int16_t)opensc2_get_le16(&reply[4]),
				 n == 48 || n == 84 || n == 85 ? opensc2_setting_default(number) : want);
		}
	}
	/* the documented ranges, by hand, for the settings the library acts on */
	for (size_t i = 0; i < ARRAY_SIZE(some); i++) {
		CHECK_EQ(opensc2_setting_clamp(some[i].number, (int16_t)(some[i].min - 1)),
			 some[i].min);
		CHECK_EQ(opensc2_setting_clamp(some[i].number, (int16_t)(some[i].max + 1)),
			 some[i].max);
		CHECK_EQ(opensc2_setting_clamp(some[i].number, some[i].max), some[i].max);
	}
	/* Steam's {8, 7} is stored as 1; the IMU's three keep any value for the application */
	CHECK_EQ(opensc2_setting_clamp(8, 7), 1);
	CHECK_EQ(opensc2_setting_clamp(48, -32768), -32768);
	CHECK_EQ(opensc2_setting_clamp(84, 7), 7);
	CHECK_EQ(opensc2_setting_clamp(85, 32767), 32767);
	/* 28 and 29 start above their range and read back so, until a write limits them */
	stub_reset();
	{
		const uint8_t n28 = 28, n29 = 29;

		CHECK_EQ(proto_usb(0x89, &n28, 1, reply), 3);
		CHECK_EQ((int16_t)opensc2_get_le16(&reply[4]), 16500);
		CHECK_EQ(proto_usb(0x89, &n29, 1, reply), 3);
		CHECK_EQ((int16_t)opensc2_get_le16(&reply[4]), 15000);
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
