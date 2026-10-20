/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the library's calibration (cal_store.c with the stand-ins of cal_host.c) through
 * the vendor command channel on USB, as a host would use it: the cal/ keys by name, the factory
 * reset keeping them, 0xFE erasing them, 0xE2 saving one side, the recordings 0xD8, 0xC0 and 0xC3
 * fed by raw inputs, the raw inputs' processing with settings 30, 31, 34, 35, 46, 52, 53, 60 and
 * 62, and the pad rules (settings 2, 24 and 80). These commands are never sent by the hardware test
 * suite (its safety rule S5), so they are tested here only.
 */
#include <errno.h>
#include <string.h>
#include <opensc2/cal.h>
#include "internal.h"
#include "cal_store.h"
#include "cal_host.h"
#include "stubs.h"

static uint8_t cal_frame_buf[64];

static int cal_set(uint8_t opcode, const void *payload, uint8_t n)
{
	memset(cal_frame_buf, 0, sizeof(cal_frame_buf));
	cal_frame_buf[0] = 1;
	cal_frame_buf[1] = opcode;
	cal_frame_buf[2] = n;
	if (n) {
		memcpy(&cal_frame_buf[3], payload, n);
	}
	return osc2_usb_feature_set(cal_frame_buf, sizeof(cal_frame_buf));
}

/* GET_REPORT after a SET of 0xED with `name`: the answer's length, its bytes into `out`. */
static int cal_read(const char *name, uint8_t *out)
{
	uint8_t reply[64], opcode, n;
	int rc = cal_set(0xED, name, (uint8_t)(strlen(name) + 1));

	if (rc) {
		return rc;
	}
	rc = osc2_usb_feature_get(reply, 64, 64, &opcode, &n);
	if (rc < 0) {
		return rc;
	}
	memcpy(out, &reply[3], reply[2]);
	return reply[2];
}

/* 0xEE / 0xEF / 0xF0 with `name` and a value. */
static int cal_named(uint8_t opcode, const char *name, const void *value, uint8_t len)
{
	uint8_t p[61];
	size_t k = strlen(name) + 1;

	memcpy(p, name, k);
	if (len) {
		memcpy(&p[k], value, len);
	}
	return cal_set(opcode, p, (uint8_t)(k + len));
}

static const uint8_t cal_joy_bytes[OPENSC2_CAL_STICK_LEN] = {
	/* format 2; X 300, 3000, rest 1600..1700; Y 200, 3100, rest 1550..1650 (mV) */
	0x02, 0x00, 0x2C, 0x01, 0xB8, 0x0B, 0x40, 0x06, 0xA4, 0x06,
	0xC8, 0x00, 0x1C, 0x0C, 0x0E, 0x06, 0x72, 0x06,
};

static void cal_frame_state(struct opensc2_state *s)
{
	memset(s, 0, sizeof(*s));
	osc2_cal_frame(s);
	osc2_cal_rules(s);
}

OPENSC2_TEST(cal_store_by_name)
{
	uint8_t out[64], value[18], len;
	uint32_t gen;

	stub_reset();
	/* a new board: no stick calibration, the LED trims at their defaults */
	CHECK_EQ(cal_read("cal/joy_l", out), 0);
	CHECK_EQ(cal_read("cal/rgbw_r", out), 1);
	CHECK_EQ(out[0], 90);
	CHECK_EQ(cal_read("cal/rgbw_g", out), 1);
	CHECK_EQ(out[0], 35);
	CHECK_EQ(cal_read("cal/rgbw_b", out), 1);
	CHECK_EQ(out[0], 120);
	CHECK_EQ(cal_read("cal/rgbw_w", out), 1);
	CHECK_EQ(out[0], 80);
	CHECK_EQ(cal_read("cal/volt_offset", out), 2);
	CHECK_EQ(cal_read("cal/touch_r", out), 0);   /* its format word is 0: empty */
	CHECK_EQ(cal_read("cal/sensors/gyroscope/bias", out), 0);   /* never readable */
	CHECK(!opensc2_cal_stick_ok(OPENSC2_LEFT));
	/* a touch key reads back while its format word is not 0 */
	CHECK_EQ(cal_named(0xEE, "cal/touch_r", (const uint8_t[10]){ 1, 0, 7 }, 10), 0);
	CHECK_EQ(cal_read("cal/touch_r", out), 10);
	CHECK_EQ(out[0], 1);
	CHECK_EQ(out[2], 7);
	CHECK_EQ(cal_named(0xEE, "cal/touch_r", (const uint8_t[10]){ 0, 0, 7 }, 10), 0);
	CHECK_EQ(cal_read("cal/touch_r", out), 0);

	/* 0xEE: RAM only, the exact size */
	gen = opensc2_cal_generation();
	CHECK(cal_named(0xEE, "cal/joy_l", cal_joy_bytes, 17) < 0);
	CHECK(cal_named(0xEE, "cal/joy_l", cal_joy_bytes, 1) < 0);
	CHECK_EQ(cal_named(0xEE, "cal/joy_l", cal_joy_bytes, 18), 0);
	CHECK(opensc2_cal_generation() != gen);
	CHECK_EQ(cal_read("cal/joy_l", out), 18);
	CHECK(memcmp(out, cal_joy_bytes, 18) == 0);
	CHECK(opensc2_cal_stick_ok(OPENSC2_LEFT));
	CHECK(!opensc2_cal_stick_ok(OPENSC2_RIGHT));
	CHECK_EQ(cal_host.writes, 0);   /* nothing written yet */

	/* 0xEF saves it at once; the NUL must be the last byte */
	CHECK(cal_named(0xEF, "cal/joy_l", "x", 1) < 0);
	CHECK_EQ(cal_named(0xEF, "cal/joy_l", NULL, 0), 0);
	CHECK_EQ(cal_host.writes, 1);
	CHECK_EQ(cal_host_flash_get("cal/joy_l", value, &len), 0);
	CHECK_EQ(len, 18);
	CHECK(memcmp(value, cal_joy_bytes, 18) == 0);
	/* a key without a value cannot be saved */
	CHECK(cal_named(0xEF, "cal/joy_r", NULL, 0) < 0);

	/* the stored key comes back at the next boot */
	cal_host_boot();
	CHECK_EQ(cal_read("cal/joy_l", out), 18);
	CHECK(memcmp(out, cal_joy_bytes, 18) == 0);

	/* 0xF0 deletes it and empties the RAM value at once */
	CHECK_EQ(cal_named(0xF0, "cal/joy_l", NULL, 0), 0);
	CHECK_EQ(cal_read("cal/joy_l", out), 0);
	CHECK(cal_host_flash_get("cal/joy_l", value, &len) < 0);
	CHECK(!opensc2_cal_stick_ok(OPENSC2_LEFT));

	/* the LED trims: any value is used (0 dark, above 100 brighter); 0xED shows them only while
	 * they lie within 1..199 */
	CHECK_EQ(cal_named(0xEE, "cal/rgbw_r", (const uint8_t[]){ 150 }, 1), 0);
	CHECK_EQ(opensc2_cal_led_trim(0), 150);
	CHECK_EQ(cal_read("cal/rgbw_r", out), 1);
	CHECK_EQ(cal_named(0xEE, "cal/rgbw_r", (const uint8_t[]){ 0 }, 1), 0);
	CHECK_EQ(cal_read("cal/rgbw_r", out), 0);
	CHECK_EQ(opensc2_cal_led_trim(0), 0);      /* the red channel dark */
	CHECK_EQ(cal_named(0xEE, "cal/rgbw_b", (const uint8_t[]){ 200 }, 1), 0);
	CHECK_EQ(cal_read("cal/rgbw_b", out), 0);
	CHECK_EQ(opensc2_cal_led_trim(2), 200);
	CHECK_EQ(cal_named(0xF0, "cal/rgbw_w", NULL, 0), 0);   /* RAM 0 at once: dark */
	CHECK_EQ(cal_read("cal/rgbw_w", out), 0);
	CHECK_EQ(opensc2_cal_led_trim(3), 0);
	CHECK_EQ(opensc2_cal_led_trim(4), 0);
	/* the next boot, with no trim stored: the defaults again */
	cal_host_boot();
	CHECK_EQ(opensc2_cal_led_trim(0), 90);
	CHECK_EQ(opensc2_cal_led_trim(2), 120);
	CHECK_EQ(opensc2_cal_led_trim(3), 80);

	/* the battery offset */
	CHECK_EQ(cal_named(0xEE, "cal/volt_offset", (const uint8_t[]){ 0xF1, 0xFF }, 2), 0);
	CHECK_EQ(opensc2_cal_volt_offset(), -15);
	CHECK_EQ(cal_read("cal/volt_offset", out), 2);
	CHECK_EQ(out[0], 0xF1);

	/* the gyro bias: set in RAM, never read back, cannot be saved, can be deleted */
	float bias[3] = { 0.01f, -0.02f, 0.5f }, back[3];
	uint8_t raw[12];

	memcpy(raw, bias, sizeof(raw));
	CHECK_EQ(opensc2_cal_gyro_bias(back), -ENOENT);
	CHECK_EQ(cal_named(0xEE, "cal/sensors/gyroscope/bias", raw, 12), 0);
	CHECK_EQ(cal_read("cal/sensors/gyroscope/bias", out), 0);
	CHECK(cal_named(0xEF, "cal/sensors/gyroscope/bias", NULL, 0) < 0);
	CHECK_EQ(opensc2_cal_gyro_bias(back), 0);
	CHECK(back[2] == 0.5f);
	raw[0] = 0x00, raw[1] = 0x00, raw[2] = 0xC0, raw[3] = 0x7F;   /* NaN */
	CHECK_EQ(cal_named(0xEE, "cal/sensors/gyroscope/bias", raw, 12), 0);
	CHECK_EQ(opensc2_cal_gyro_bias(back), -ENOENT);
	CHECK_EQ(cal_named(0xF0, "cal/sensors/gyroscope/bias", NULL, 0), 0);

	/* an unknown cal/ name is the application's: refused by the blank canvas */
	CHECK_EQ(cal_read("cal/foo", out), 0);
	CHECK(cal_named(0xEE, "cal/foo", "x", 1) < 0);
	CHECK(cal_named(0xF0, "cal/foo", NULL, 0) < 0);
}

/* 0xEF saves what 0xED would read back, as the real controller saves what the key's read function
 * gives: never an empty touch key, a trim outside 1..199 or the gyro bias. */
OPENSC2_TEST(cal_store_save_refusals)
{
	uint8_t value[18], len;
	int writes;

	stub_reset();
	/* a default trim saves */
	CHECK_EQ(cal_named(0xEF, "cal/rgbw_g", NULL, 0), 0);
	CHECK_EQ(cal_host_flash_get("cal/rgbw_g", value, &len), 0);
	CHECK(len == 1 && value[0] == 35);
	/* trims of 0 and 200: refused, nothing written; 199 saves */
	writes = cal_host.writes;
	CHECK_EQ(cal_named(0xEE, "cal/rgbw_r", (const uint8_t[]){ 0 }, 1), 0);
	CHECK(cal_named(0xEF, "cal/rgbw_r", NULL, 0) < 0);
	CHECK_EQ(cal_named(0xEE, "cal/rgbw_r", (const uint8_t[]){ 200 }, 1), 0);
	CHECK(cal_named(0xEF, "cal/rgbw_r", NULL, 0) < 0);
	CHECK_EQ(cal_host.writes, writes);
	CHECK(cal_host_flash_get("cal/rgbw_r", value, &len) < 0);
	CHECK_EQ(cal_named(0xEE, "cal/rgbw_r", (const uint8_t[]){ 199 }, 1), 0);
	CHECK_EQ(cal_named(0xEF, "cal/rgbw_r", NULL, 0), 0);
	CHECK_EQ(cal_host_flash_get("cal/rgbw_r", value, &len), 0);
	CHECK_EQ(value[0], 199);
	/* a touch key only with a format word */
	writes = cal_host.writes;
	CHECK(cal_named(0xEF, "cal/touch_l", NULL, 0) < 0);
	CHECK_EQ(cal_host.writes, writes);
	CHECK(cal_host_flash_get("cal/touch_l", value, &len) < 0);
	CHECK_EQ(cal_named(0xEE, "cal/touch_l", (const uint8_t[10]){ 2, 0, 9 }, 10), 0);
	CHECK_EQ(cal_named(0xEF, "cal/touch_l", NULL, 0), 0);
	CHECK_EQ(cal_host_flash_get("cal/touch_l", value, &len), 0);
	CHECK(len == 10 && value[0] == 2 && value[2] == 9);
	/* the battery offset, which 0xED always reads, saves even as 0 */
	CHECK_EQ(cal_named(0xEF, "cal/volt_offset", NULL, 0), 0);
	CHECK_EQ(cal_host_flash_get("cal/volt_offset", value, &len), 0);
	CHECK_EQ(len, 2);
}

OPENSC2_TEST(cal_store_erase_and_reset)
{
	static const uint8_t factory[4] = { 0x17, 0xA9, 0xC1, 0xEF };
	static const uint8_t erase[4] = { 0x85, 0x48, 0x67, 0xC6 };
	uint8_t out[64], value[18], len;

	stub_reset();
	CHECK_EQ(cal_named(0xEE, "cal/joy_r", cal_joy_bytes, 18), 0);
	CHECK_EQ(cal_named(0xEF, "cal/joy_r", NULL, 0), 0);
	/* a factory reset keeps the calibration */
	CHECK_EQ(cal_set(0x86, factory, 4), 0);
	CHECK_EQ(stub.power, OPENSC2_POWER_FACTORY_RESET);
	cal_host_boot();
	CHECK_EQ(cal_read("cal/joy_r", out), 18);

	/* 0xFE: only with its key */
	CHECK(cal_set(0xFE, (const uint8_t[]){ 0x85, 0x48, 0x67, 0xC7 }, 4) < 0);
	CHECK(cal_set(0xFE, NULL, 0) < 0);
	CHECK_EQ(cal_host.deferred, 0);
	CHECK_EQ(cal_set(0xFE, erase, 4), 0);
	CHECK_EQ(cal_host.deferred, 1);
	/* the RAM copy stays in use until the next boot */
	CHECK_EQ(cal_read("cal/joy_r", out), 18);
	CHECK(opensc2_cal_stick_ok(OPENSC2_RIGHT));
	osc2_cal_flush();
	CHECK(cal_host_flash_get("cal/joy_r", value, &len) < 0);
	CHECK_EQ(cal_read("cal/joy_r", out), 18);
	cal_host_boot();
	CHECK_EQ(cal_read("cal/joy_r", out), 0);
	CHECK(!opensc2_cal_stick_ok(OPENSC2_RIGHT));

	/* the key is read whatever the length says (the bytes after it, here behind n = 0) */
	memset(cal_frame_buf, 0, sizeof(cal_frame_buf));
	cal_frame_buf[0] = 1;
	cal_frame_buf[1] = 0xFE;
	memcpy(&cal_frame_buf[3], erase, 4);
	CHECK_EQ(osc2_usb_feature_set(cal_frame_buf, sizeof(cal_frame_buf)), 0);
	/* a GET of 0xFE has nothing to answer */
	uint8_t reply[64], opcode, n;

	CHECK(osc2_usb_feature_get(reply, 64, 64, &opcode, &n) < 0);
}

OPENSC2_TEST(cal_store_erase_order)
{
	static const uint8_t erase[4] = { 0x85, 0x48, 0x67, 0xC6 };
	uint8_t value[18], len;

	stub_reset();
	CHECK_EQ(cal_named(0xEE, "cal/joy_r", cal_joy_bytes, 18), 0);
	CHECK_EQ(cal_named(0xEF, "cal/joy_r", NULL, 0), 0);
	/* a save asked for before the erase does not outlive it (both wait for the work queue) */
	CHECK_EQ(cal_set(0xE2, (const uint8_t[]){ 1, 2 }, 2), 0);
	CHECK_EQ(cal_set(0xFE, erase, 4), 0);
	osc2_cal_flush();
	CHECK(cal_host_flash_get("cal/joy_r", value, &len) < 0);
	/* one asked for after it is written after it, from the RAM copy the erase left in use */
	CHECK_EQ(cal_set(0xFE, erase, 4), 0);
	CHECK_EQ(cal_set(0xE2, (const uint8_t[]){ 1, 2 }, 2), 0);
	osc2_cal_flush();
	CHECK_EQ(cal_host_flash_get("cal/joy_r", value, &len), 0);
	CHECK_EQ(len, 18);
	/* a recording finished before the erase is not saved either */
	CHECK_EQ(cal_set(0xC3, (const uint8_t[]){ 1 }, 1), 0);
	opensc2_set_pressure_raw(OPENSC2_LEFT, 900);
	cal_frame_state(&(struct opensc2_state){ 0 });
	CHECK_EQ(cal_set(0xC3, (const uint8_t[]){ 3 }, 1), 0);
	CHECK_EQ(cal_set(0xFE, erase, 4), 0);
	osc2_cal_flush();
	CHECK(cal_host_flash_get("cal/prs_l", value, &len) < 0);
}

OPENSC2_TEST(cal_store_pad_start_over)
{
	static const uint8_t prs[8] = { 1, 0, 100, 0, 0x34, 0x08, 0xD0, 0x07 };   /* 100, 2100, 2000 */
	struct opensc2_state s;

	/* the stabiliser starts over at a change of setting 34 too: the next sample is the new touch
	 * point instead of being held */
	stub_reset();
	osc2.settings[OPENSC2_SETTING_PAD_STABILIZER] = 2;
	static const int16_t xs[] = { 1000, 3000, 3000 };
	static const int16_t want[] = { 1000, 1000, 3000 };

	for (size_t i = 0; i < ARRAY_SIZE(xs); i++) {
		if (i == 2) {
			osc2.settings[OPENSC2_SETTING_GRIP_SENSITIVITY] = 200;
		}
		memset(&s, 0, sizeof(s));
		s.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = xs[i], .y = 1000, .touched = true };
		osc2_cal_frame(&s);
		osc2_cal_rules(&s);
		CHECK_EQ(s.pad[OPENSC2_LEFT].x, want[i]);
	}

	/* a load of a pad's pressure key starts that pad over (its stick-touch mean), not the other */
	stub_reset();
	opensc2_set_touch_raw(OPENSC2_LEFT, 1100, 0);
	opensc2_set_touch_raw(OPENSC2_RIGHT, 1100, 0);
	cal_frame_state(&s);
	CHECK(s.buttons & OPENSC2_TOUCH_LEFT_STICK);
	CHECK(s.buttons & OPENSC2_TOUCH_RIGHT_STICK);
	opensc2_set_touch_raw(OPENSC2_LEFT, 300, 0);
	opensc2_set_touch_raw(OPENSC2_RIGHT, 300, 0);
	CHECK_EQ(cal_named(0xEE, "cal/prs_l", prs, 8), 0);
	cal_frame_state(&s);
	CHECK(!(s.buttons & OPENSC2_TOUCH_LEFT_STICK));   /* the first word fills both: 300 */
	CHECK(s.buttons & OPENSC2_TOUCH_RIGHT_STICK);     /* the mean of 1100 and 300: held */

	/* a stick key does not touch the pads: a click held inside the hysteresis band stays */
	stub_reset();
	CHECK_EQ(cal_named(0xEE, "cal/prs_r", prs, 8), 0);
	opensc2_set_pressure_raw(OPENSC2_RIGHT, 2100);
	for (int i = 0; i < 3; i++) {   /* the filter reaches 1213: 56 %, on */
		memset(&s, 0, sizeof(s));
		s.pad[OPENSC2_RIGHT].touched = true;
		osc2_cal_frame(&s);
	}
	CHECK(s.pad[OPENSC2_RIGHT].clicked);
	opensc2_set_pressure_raw(OPENSC2_RIGHT, 909);   /* the filter settles at 909: 39 % */
	for (int i = 0; i < 20; i++) {
		memset(&s, 0, sizeof(s));
		s.pad[OPENSC2_RIGHT].touched = true;
		osc2_cal_frame(&s);
	}
	CHECK(s.pad[OPENSC2_RIGHT].clicked);
	CHECK_EQ(cal_named(0xEE, "cal/joy_l", cal_joy_bytes, 18), 0);
	memset(&s, 0, sizeof(s));
	s.pad[OPENSC2_RIGHT].touched = true;
	osc2_cal_frame(&s);
	CHECK(s.pad[OPENSC2_RIGHT].clicked);
	/* its own key does: the click starts over and stays off inside the band */
	CHECK_EQ(cal_named(0xEE, "cal/prs_r", prs, 8), 0);
	memset(&s, 0, sizeof(s));
	s.pad[OPENSC2_RIGHT].touched = true;
	osc2_cal_frame(&s);
	CHECK(!s.pad[OPENSC2_RIGHT].clicked);
}

OPENSC2_TEST(cal_store_persist)
{
	static const uint8_t trg[OPENSC2_CAL_TRIGGER_LEN] = { 2, 0, 0xDC, 0x05, 0xC8, 0x00, 0 };
	uint8_t value[18], len;

	stub_reset();
	CHECK(cal_set(0xE2, (const uint8_t[]){ 2, 7 }, 2) < 0);   /* side above 1 */
	CHECK_EQ(cal_set(0xE2, (const uint8_t[]){ 0, 7 }, 2), 0);  /* nothing in RAM: nothing */
	osc2_cal_flush();
	CHECK_EQ(cal_host.writes, 0);
	CHECK_EQ(cal_named(0xEE, "cal/trg_r", trg, sizeof(trg)), 0);
	CHECK_EQ(cal_named(0xEE, "cal/joy_r", cal_joy_bytes, 18), 0);
	/* Steam's 0xE2 01 20: mask bit 5 only, nothing saved */
	CHECK_EQ(cal_set(0xE2, (const uint8_t[]){ 1, 0x20 }, 2), 0);
	osc2_cal_flush();
	CHECK_EQ(cal_host.writes, 0);
	/* the right side's trigger only */
	CHECK_EQ(cal_set(0xE2, (const uint8_t[]){ 1, 1 }, 2), 0);
	CHECK_EQ(cal_host.deferred, 2);
	CHECK_EQ(cal_host.writes, 0);   /* from the work queue */
	osc2_cal_flush();
	CHECK_EQ(cal_host.writes, 1);
	CHECK_EQ(cal_host_flash_get("cal/trg_r", value, &len), 0);
	CHECK_EQ(len, 7);
	CHECK(cal_host_flash_get("cal/joy_r", value, &len) < 0);
	/* then the stick */
	CHECK_EQ(cal_set(0xE2, (const uint8_t[]){ 1, 2 }, 2), 0);
	osc2_cal_flush();
	CHECK_EQ(cal_host_flash_get("cal/joy_r", value, &len), 0);
	/* n = 0 reads the padding: side 0, mask 0 */
	CHECK_EQ(cal_set(0xE2, NULL, 0), 0);
	/* a write that fails is only logged */
	cal_host.fail_writes = true;
	CHECK_EQ(cal_set(0xE2, (const uint8_t[]){ 1, 3 }, 2), 0);
	osc2_cal_flush();
	cal_host.fail_writes = false;
	/* the length is not checked: behind n = 0 the next two bytes are the side and the mask */
	CHECK_EQ(cal_named(0xEE, "cal/joy_l", cal_joy_bytes, 18), 0);
	memset(cal_frame_buf, 0, sizeof(cal_frame_buf));
	cal_frame_buf[0] = 1;
	cal_frame_buf[1] = 0xE2;
	cal_frame_buf[3] = 0;   /* side 0 */
	cal_frame_buf[4] = 2;   /* the stick */
	CHECK_EQ(osc2_usb_feature_set(cal_frame_buf, sizeof(cal_frame_buf)), 0);
	osc2_cal_flush();
	CHECK_EQ(cal_host_flash_get("cal/joy_l", value, &len), 0);
	cal_frame_buf[3] = 2;   /* a side above 1 there is refused */
	CHECK(osc2_usb_feature_set(cal_frame_buf, sizeof(cal_frame_buf)) < 0);
}

OPENSC2_TEST(cal_store_boot_load)
{
	uint8_t out[64];

	/* what a boot finds in storage: a key of another length is refused (the built-in value
	 * stays), the others are used from RAM */
	stub_reset();
	CHECK_EQ(osc2_cal_flash_write("cal/joy_l", cal_joy_bytes, 17), 0);
	CHECK_EQ(osc2_cal_flash_write("cal/joy_r", cal_joy_bytes, 18), 0);
	CHECK_EQ(osc2_cal_flash_write("cal/rgbw_g", (const uint8_t[]){ 150 }, 1), 0);
	CHECK_EQ(osc2_cal_flash_write("cal/volt_offset", (const uint8_t[]){ 0x0A, 0x00 }, 2), 0);
	cal_host_boot();
	CHECK_EQ(cal_read("cal/joy_l", out), 0);
	CHECK(!opensc2_cal_stick_ok(OPENSC2_LEFT));
	CHECK_EQ(cal_read("cal/joy_r", out), 18);
	CHECK(opensc2_cal_stick_ok(OPENSC2_RIGHT));
	CHECK_EQ(opensc2_cal_led_trim(1), 150);
	CHECK_EQ(opensc2_cal_led_trim(0), 90);   /* not stored: the default */
	CHECK_EQ(opensc2_cal_volt_offset(), 10);
	/* the glue's names are the keys' own without "cal/" */
	CHECK_EQ(osc2_cal_load("joy_l", cal_joy_bytes, 19), -EINVAL);
	CHECK_EQ(osc2_cal_load("joy", cal_joy_bytes, 18), -ENOENT);
	CHECK_EQ(osc2_cal_load("joy_l", cal_joy_bytes, 18), 0);
	CHECK(opensc2_cal_stick_ok(OPENSC2_LEFT));
}

/* The wire form of a state, to compare two states field by field (not their padding). */
static void cal_wire(const struct opensc2_state *s, uint8_t out[OPENSC2_STATE_REPORT_LEN])
{
	memset(out, 0, OPENSC2_STATE_REPORT_LEN);
	opensc2_encode_state(s, 0, out);
}

OPENSC2_TEST(cal_store_finished_values)
{
	/* Steam's settings in its wired session (24, 52, 53, 46, 34, 35, 84, 85, 82, 48, 45) */
	static const uint8_t steam[] = {
		24, 0, 0, 52, 0xFF, 0xFF, 53, 0xFF, 0xFF, 46, 0, 0, 34, 100, 0, 35, 80, 0,
		84, 1, 0, 85, 50, 0, 82, 3, 0, 48, 0, 0, 45, 100, 0,
	};
	struct opensc2_state app, s;
	uint8_t want[OPENSC2_STATE_REPORT_LEN], got[OPENSC2_STATE_REPORT_LEN];

	/* An application that gives finished values only: the calibration's processing and the pad
	 * rules leave every field as it was, with the defaults and with Steam's settings. */
	stub_reset();
	memset(&app, 0, sizeof(app));
	app.buttons = 0x3FFFFFFFu;
	app.trigger[OPENSC2_LEFT] = 12345;
	app.trigger[OPENSC2_RIGHT] = 32767;
	app.stick[OPENSC2_LEFT] = (struct opensc2_stick){ .x = -32767, .y = 32767 };
	app.stick[OPENSC2_RIGHT] = (struct opensc2_stick){ .x = 1001, .y = -1 };
	app.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = 1001, .y = -3001, .pressure = 20000,
							   .touched = true, .clicked = true };
	app.pad[OPENSC2_RIGHT] = (struct opensc2_trackpad){ .x = -32767, .y = 32767,
							    .pressure = 32767, .touched = true };
	app.imu = (struct opensc2_imu){ .timestamp_us = 0x12345678u, .accel = { 1, -2, 16384 },
					.gyro = { -3, 4, -5 }, .quat = { 32767, 1, 2, 3 } };
	cal_wire(&app, want);
	for (int round = 0; round < 2; round++) {
		if (round == 1) {
			CHECK_EQ(cal_set(0x87, steam, sizeof(steam)), 0);
		}
		for (int i = 0; i < 3; i++) {
			s = app;
			osc2_cal_frame(&s);
			osc2_cal_rules(&s);
			cal_wire(&s, got);
			CHECK(memcmp(got, want, sizeof(got)) == 0);
		}
	}
}

OPENSC2_TEST(cal_store_recordings)
{
	struct opensc2_state s;
	uint8_t out[64];
	struct opensc2_cal_stick st;
	struct opensc2_cal_trigger tr;
	struct opensc2_cal_pressure pr;

	stub_reset();
	/* sticks: rest, circles, save at the next frame */
	CHECK(!opensc2_cal_recording(OPENSC2_CAL_REC_STICKS));
	CHECK_EQ(cal_set(0xD8, (const uint8_t[]){ 0x55, 1 }, 2), 0);   /* payload[0] is not read */
	CHECK(opensc2_cal_recording(OPENSC2_CAL_REC_STICKS));   /* what an application asks */
	CHECK(!opensc2_cal_recording(OPENSC2_CAL_REC_TRIGGERS));
	CHECK(!opensc2_cal_recording((enum opensc2_cal_rec_kind)3));
	opensc2_set_stick_raw(OPENSC2_LEFT, 1986, 1924);
	opensc2_set_stick_raw(OPENSC2_RIGHT, 2000, 2000);
	cal_frame_state(&s);
	opensc2_set_stick_raw(OPENSC2_LEFT, 2110, 2047);
	cal_frame_state(&s);
	CHECK_EQ(cal_set(0xD8, (const uint8_t[]){ 0, 2 }, 2), 0);
	opensc2_set_stick_raw(OPENSC2_LEFT, 372, 248);
	cal_frame_state(&s);
	opensc2_set_stick_raw(OPENSC2_LEFT, 3723, 3847);
	cal_frame_state(&s);
	CHECK_EQ(cal_read("cal/joy_l", out), 0);   /* not yet */
	CHECK_EQ(cal_set(0xD8, (const uint8_t[]){ 0, 3 }, 2), 0);
	CHECK(opensc2_cal_recording(OPENSC2_CAL_REC_STICKS));   /* until the next frame saves */
	cal_frame_state(&s);
	CHECK(!opensc2_cal_recording(OPENSC2_CAL_REC_STICKS));
	CHECK_EQ(cal_read("cal/joy_l", out), 18);
	opensc2_cal_stick_unpack(out, &st);
	CHECK_EQ(st.format, 2);
	CHECK_EQ(st.x.rest_low, opensc2_cal_counts_to_mv(1986, 3300));
	CHECK_EQ(st.x.rest_high, opensc2_cal_counts_to_mv(2110, 3300));
	CHECK_EQ(st.x.min, opensc2_cal_counts_to_mv(372, 3300));
	CHECK_EQ(st.y.max, opensc2_cal_counts_to_mv(3847, 3300));
	CHECK_EQ(cal_read("cal/joy_r", out), 18);   /* the right side got samples too */
	/* applied from the next frame: the centre (2047 counts) reads 0, the far end full */
	opensc2_set_stick_raw(OPENSC2_LEFT, 2047, 3847);
	cal_frame_state(&s);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, 0);
	CHECK_EQ(s.stick[OPENSC2_LEFT].y, 32767);
	/* saved from the work queue */
	CHECK_EQ(cal_host.writes, 0);
	osc2_cal_flush();
	CHECK_EQ(cal_host.writes, 2);

	/* triggers: the left rises when pulled, the right falls */
	CHECK_EQ(cal_set(0xC0, (const uint8_t[]){ 1 }, 1), 0);
	opensc2_set_trigger_raw(OPENSC2_LEFT, 500);
	opensc2_set_trigger_raw(OPENSC2_RIGHT, 3700);
	cal_frame_state(&s);
	CHECK_EQ(cal_set(0xC0, (const uint8_t[]){ 2 }, 1), 0);
	opensc2_set_trigger_raw(OPENSC2_LEFT, 3700);
	opensc2_set_trigger_raw(OPENSC2_RIGHT, 500);
	cal_frame_state(&s);
	CHECK_EQ(cal_set(0xC0, (const uint8_t[]){ 3 }, 1), 0);
	cal_frame_state(&s);
	CHECK_EQ(cal_read("cal/trg_l", out), 7);
	opensc2_cal_trigger_unpack(out, &tr);
	CHECK_EQ(tr.direction, 0);
	CHECK_EQ(tr.a_mv, opensc2_cal_counts_to_mv(3700, 1650));
	CHECK_EQ(tr.b_mv, opensc2_cal_counts_to_mv(500, 1650));
	CHECK_EQ(cal_read("cal/trg_r", out), 7);
	opensc2_cal_trigger_unpack(out, &tr);
	CHECK_EQ(tr.direction, 1);
	/* the right trigger pulled reads full, released 0 */
	cal_frame_state(&s);
	CHECK_EQ(s.trigger[OPENSC2_RIGHT], 32767);
	opensc2_set_trigger_raw(OPENSC2_RIGHT, 3700);
	cal_frame_state(&s);
	CHECK_EQ(s.trigger[OPENSC2_RIGHT], 0);

	/* pressure: 1 start, 2 the firm press, 3 saved at once */
	CHECK_EQ(cal_set(0xC3, (const uint8_t[]){ 1 }, 1), 0);
	opensc2_set_pressure_raw(OPENSC2_LEFT, 100);
	cal_frame_state(&s);
	CHECK_EQ(cal_set(0xC3, (const uint8_t[]){ 2 }, 1), 0);
	opensc2_set_pressure_raw(OPENSC2_LEFT, 2100);
	cal_frame_state(&s);
	opensc2_set_pressure_raw(OPENSC2_LEFT, 1500);
	cal_frame_state(&s);
	CHECK_EQ(cal_set(0xC3, (const uint8_t[]){ 3 }, 1), 0);
	CHECK_EQ(cal_read("cal/prs_l", out), 8);
	opensc2_cal_pressure_unpack(out, &pr);
	CHECK_EQ(pr.format, 1);
	CHECK_EQ(pr.a, 0);
	CHECK_EQ(pr.b, 2100);
	CHECK_EQ(pr.gain, 2000);
	CHECK_EQ(cal_read("cal/prs_r", out), 0);   /* no samples on the right: kept as it was */
	CHECK(opensc2_cal_pressure_ok(OPENSC2_LEFT));

	/* a step 3 without a step 1 does nothing; neither does a step above 3 */
	CHECK_EQ(cal_set(0xD8, (const uint8_t[]){ 0, 3 }, 2), 0);
	CHECK_EQ(cal_set(0xD8, (const uint8_t[]){ 0, 9 }, 2), 0);
	cal_frame_state(&s);
	osc2_cal_flush();
	CHECK_EQ(cal_host.writes, 5);   /* the two sticks, two triggers and the left pad only */
}

OPENSC2_TEST(cal_store_raw_inputs)
{
	static const uint8_t prs[8] = { 1, 0, 100, 0, 0x34, 0x08, 0xD0, 0x07 };   /* 100, 2100, 2000 */
	struct opensc2_state s;

	stub_reset();
	/* uncalibrated: a raw stick and trigger read 0 */
	opensc2_set_stick_raw(OPENSC2_LEFT, 4000, 100);
	opensc2_set_trigger_raw(OPENSC2_LEFT, 4000);
	cal_frame_state(&s);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, 0);
	CHECK_EQ(s.stick[OPENSC2_LEFT].y, 0);
	CHECK_EQ(s.trigger[OPENSC2_LEFT], 0);
	/* setting 46: 1 millivolts, 2 the counts, both without calibration */
	osc2.settings[OPENSC2_SETTING_RAW_STICKS] = 1;
	cal_frame_state(&s);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, 3222);
	CHECK_EQ(s.stick[OPENSC2_LEFT].y, 80);
	osc2.settings[OPENSC2_SETTING_RAW_STICKS] = 2;
	cal_frame_state(&s);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, 4000);
	osc2.settings[OPENSC2_SETTING_RAW_STICKS] = 0;
	/* setting 62 likewise for the triggers */
	osc2.settings[OPENSC2_SETTING_RAW_TRIGGERS] = 1;
	cal_frame_state(&s);
	CHECK_EQ(s.trigger[OPENSC2_LEFT], 1611);
	osc2.settings[OPENSC2_SETTING_RAW_TRIGGERS] = 2;
	cal_frame_state(&s);
	CHECK_EQ(s.trigger[OPENSC2_LEFT], 4000);
	osc2.settings[OPENSC2_SETTING_RAW_TRIGGERS] = 0;
	/* calibrated, with a stored key */
	CHECK_EQ(cal_named(0xEE, "cal/joy_l", cal_joy_bytes, 18), 0);
	opensc2_set_stick_raw(OPENSC2_LEFT, 2049, 3000);
	cal_frame_state(&s);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, 21);
	CHECK_EQ(s.stick[OPENSC2_LEFT].y, 19846);
	/* released: the finished value is the application's again (here 0, a cleared state) */
	opensc2_cal_raw_release(OPENSC2_RAW_STICK_L);
	memset(&s, 0, sizeof(s));
	s.stick[OPENSC2_LEFT].x = 1234;
	osc2_cal_frame(&s);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, 1234);

	/* pressure: 0 untouched, the calibrated value and the click while touched */
	CHECK_EQ(cal_named(0xEE, "cal/prs_r", prs, 8), 0);
	opensc2_set_pressure_raw(OPENSC2_RIGHT, 2100);
	cal_frame_state(&s);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].pressure, 0);   /* not touched */
	for (int i = 0; i < 3; i++) {
		memset(&s, 0, sizeof(s));
		s.pad[OPENSC2_RIGHT].touched = true;
		osc2_cal_frame(&s);
	}
	CHECK_EQ(s.pad[OPENSC2_RIGHT].pressure, 22463);   /* the filter's fourth step: 1434 */
	CHECK(s.pad[OPENSC2_RIGHT].clicked);
	CHECK(s.buttons & OPENSC2_CLICK_RIGHT_PAD);
	/* an untouched pad does not click, though the click state follows the pressure */
	memset(&s, 0, sizeof(s));
	s.buttons = OPENSC2_CLICK_RIGHT_PAD;   /* even a click bit from the application */
	osc2_cal_frame(&s);
	CHECK(!(s.buttons & OPENSC2_CLICK_RIGHT_PAD));
	CHECK_EQ(s.pad[OPENSC2_RIGHT].pressure, 0);
	/* setting 60 = 1: the raw word, also untouched */
	osc2.settings[OPENSC2_SETTING_RAW_PRESSURE] = 1;
	opensc2_set_pressure_raw(OPENSC2_RIGHT, -40);
	cal_frame_state(&s);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].pressure, 0);
	opensc2_set_pressure_raw(OPENSC2_RIGHT, 777);
	cal_frame_state(&s);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].pressure, 777);
	osc2.settings[OPENSC2_SETTING_RAW_PRESSURE] = 0;
	/* no usable calibration on the left: never a pressure or a click */
	opensc2_set_pressure_raw(OPENSC2_LEFT, 30000);
	for (int i = 0; i < 6; i++) {
		memset(&s, 0, sizeof(s));
		s.pad[OPENSC2_LEFT].touched = true;
		osc2_cal_frame(&s);
	}
	CHECK_EQ(s.pad[OPENSC2_LEFT].pressure, 0);
	CHECK(!s.pad[OPENSC2_LEFT].clicked);
}

OPENSC2_TEST(cal_store_capacitive)
{
	struct opensc2_state s;

	stub_reset();
	/* stick touch above 500 (the mean of two words), held while at least 400 */
	opensc2_set_touch_raw(OPENSC2_LEFT, 600, 0);
	cal_frame_state(&s);
	CHECK(s.buttons & OPENSC2_TOUCH_LEFT_STICK);
	CHECK(!(s.buttons & OPENSC2_TOUCH_LEFT_GRIP));
	opensc2_set_touch_raw(OPENSC2_LEFT, 300, 0);
	cal_frame_state(&s);   /* mean 450: held */
	CHECK(s.buttons & OPENSC2_TOUCH_LEFT_STICK);
	cal_frame_state(&s);   /* mean 300: off */
	CHECK(!(s.buttons & OPENSC2_TOUCH_LEFT_STICK));
	/* grip: on above 1125, held while at least 900 (settings 34 = 100, 35 = 80) */
	opensc2_set_touch_raw(OPENSC2_RIGHT, 0, 1200);
	cal_frame_state(&s);
	CHECK(s.buttons & OPENSC2_TOUCH_RIGHT_GRIP);
	opensc2_set_touch_raw(OPENSC2_RIGHT, 0, 950);
	cal_frame_state(&s);
	cal_frame_state(&s);
	CHECK(s.buttons & OPENSC2_TOUCH_RIGHT_GRIP);
	/* a change of setting 34 starts the pads over: 950 is now below the new on level */
	osc2.settings[OPENSC2_SETTING_GRIP_SENSITIVITY] = 200;   /* on 2025, off 1620 */
	cal_frame_state(&s);
	CHECK(!(s.buttons & OPENSC2_TOUCH_RIGHT_GRIP));
	opensc2_set_touch_raw(OPENSC2_RIGHT, 0, 2100);
	cal_frame_state(&s);   /* mean 1525 */
	CHECK(!(s.buttons & OPENSC2_TOUCH_RIGHT_GRIP));
	cal_frame_state(&s);   /* mean 2100 */
	CHECK(s.buttons & OPENSC2_TOUCH_RIGHT_GRIP);
	/* the application's bits for an input that is not raw stay */
	opensc2_cal_raw_release(OPENSC2_RAW_TOUCH_L | OPENSC2_RAW_TOUCH_R);
	memset(&s, 0, sizeof(s));
	s.buttons = OPENSC2_TOUCH_LEFT_GRIP;
	osc2_cal_frame(&s);
	CHECK(s.buttons & OPENSC2_TOUCH_LEFT_GRIP);
}

OPENSC2_TEST(cal_store_pad_rules)
{
	struct opensc2_state s;

	stub_reset();
	/* nothing changes with the defaults (2 = 0, 80 = 1) */
	memset(&s, 0, sizeof(s));
	s.pad[OPENSC2_RIGHT] = (struct opensc2_trackpad){ .x = 1001, .y = -3001, .touched = true };
	osc2_cal_rules(&s);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].x, 1001);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].y, -3001);
	/* the rotation: settings 2 and 24 both non-zero; Y up in the report (the rotation works on
	 * the value before the negation, at half scale) */
	osc2.settings[OPENSC2_SETTING_PAD_ROTATION_ANGLE] = 90;
	osc2.settings[OPENSC2_SETTING_PAD_ROTATION] = 1;
	s.pad[OPENSC2_RIGHT] = (struct opensc2_trackpad){ .x = 2000, .y = 0, .touched = true };
	s.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = 2000, .y = 0, .touched = true };
	osc2_cal_rules(&s);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].x, 0);
	CHECK_EQ(s.pad[OPENSC2_RIGHT].y, 2000);    /* the right pad turns counter-clockwise */
	CHECK_EQ(s.pad[OPENSC2_LEFT].x, 0);
	CHECK_EQ(s.pad[OPENSC2_LEFT].y, -2000);    /* the left one clockwise */
	osc2.settings[OPENSC2_SETTING_PAD_ROTATION] = 0;
	/* the stabiliser with 80 = 2: a new touch is held until the thumb moves past 4369 */
	osc2.settings[OPENSC2_SETTING_PAD_STABILIZER] = 2;
	static const int16_t xs[] = { 1000, 3000, 5400, 5500 };
	static const int16_t want[] = { 1000, 1000, 1000, 1651 };

	for (size_t i = 0; i < ARRAY_SIZE(xs); i++) {
		memset(&s, 0, sizeof(s));
		s.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = xs[i], .y = 1000, .touched = true };
		osc2_cal_rules(&s);
		CHECK_EQ(s.pad[OPENSC2_LEFT].x, want[i]);
		CHECK_EQ(s.pad[OPENSC2_LEFT].y, 1000);
	}
	/* a frame without a touch starts it over */
	memset(&s, 0, sizeof(s));
	osc2_cal_rules(&s);
	s.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = 9000, .y = 1000, .touched = true };
	osc2_cal_rules(&s);
	CHECK_EQ(s.pad[OPENSC2_LEFT].x, 9000);
	s.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = 12000, .y = 1000, .touched = true };
	osc2_cal_rules(&s);
	CHECK_EQ(s.pad[OPENSC2_LEFT].x, 9000);    /* held: 3000 from the touch point */
	/* so does setting 80 leaving 2 in the middle of a touch: the pad's own position meanwhile,
	 * and the first sample with 80 = 2 again is the new touch point */
	osc2.settings[OPENSC2_SETTING_PAD_STABILIZER] = 1;
	s.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = 12001, .y = 1000, .touched = true };
	osc2_cal_rules(&s);
	CHECK_EQ(s.pad[OPENSC2_LEFT].x, 12001);   /* untouched by the rules, even an odd value */
	osc2.settings[OPENSC2_SETTING_PAD_STABILIZER] = 2;
	s.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = 12500, .y = 1000, .touched = true };
	osc2_cal_rules(&s);
	CHECK_EQ(s.pad[OPENSC2_LEFT].x, 12500);
	s.pad[OPENSC2_LEFT] = (struct opensc2_trackpad){ .x = 13000, .y = 1000, .touched = true };
	osc2_cal_rules(&s);
	CHECK_EQ(s.pad[OPENSC2_LEFT].x, 12500);   /* held at the new touch point */
	osc2.settings[OPENSC2_SETTING_PAD_STABILIZER] = 1;
}
