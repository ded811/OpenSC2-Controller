/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Test cases for the wire formats (codec.c), lizard mode, the settings defaults and the fusion
 * helper. Written once, run two ways: as a Zephyr ztest suite (tests/codec, for native_sim or
 * QEMU) and as a plain C program on a PC (tests/host). The including file defines
 * OPENSC2_TEST(name), CHECK(cond) and CHECK_EQ(a, b).
 *
 * The expected byte offsets are the published layouts (docs/protocol.md), written out by hand.
 */
#include <math.h>
#include <string.h>
#include <opensc2/codec.h>
#include <opensc2/settings.h>
#include <opensc2/fusion.h>
#include "lizard.h"

static struct opensc2_identity test_identity(void)
{
	struct opensc2_identity id;

	memset(&id, 0, sizeof(id));
	memcpy(id.unit_serial, "OSC2DEV000001", 13);
	memcpy(id.board_serial, "OSC2PCB000001", 13);
	memcpy(id.git_revision, "opensc2-0.1", 11);
	id.product_id = 0x1302;
	id.board_revision = 0x4B;
	id.build_time = 0x6AD00000;
	id.bootloader_build_time = 0x11223344;
	return id;
}

/* ---- report 0x42 ---------------------------------------------------------------------------- */

OPENSC2_TEST(state_layout)
{
	struct opensc2_state s;
	uint8_t r[OPENSC2_STATE_REPORT_LEN];

	memset(&s, 0, sizeof(s));
	s.buttons = OPENSC2_BTN_A | OPENSC2_BTN_STEAM | OPENSC2_BTN_L;
	s.trigger[OPENSC2_LEFT] = 1000;
	s.trigger[OPENSC2_RIGHT] = 40000;            /* clamps to 32767 */
	s.stick[OPENSC2_LEFT].x = -32768;            /* clamps to -32767 */
	s.stick[OPENSC2_LEFT].y = 0x1234;
	s.stick[OPENSC2_RIGHT].x = -2;
	s.stick[OPENSC2_RIGHT].y = 32767;
	s.pad[OPENSC2_LEFT].touched = true;
	s.pad[OPENSC2_LEFT].x = 100;
	s.pad[OPENSC2_LEFT].y = -100;
	s.pad[OPENSC2_LEFT].pressure = 500;
	s.pad[OPENSC2_RIGHT].x = 7;                  /* not touched: reported as 0, 0 */
	s.pad[OPENSC2_RIGHT].y = 7;
	s.pad[OPENSC2_RIGHT].clicked = true;
	s.imu.timestamp_us = 0xA1B2C3D4;
	s.imu.accel[0] = 1;
	s.imu.accel[2] = 16384;
	s.imu.gyro[1] = -1;
	s.imu.quat[0] = 32767;
	s.imu.quat[3] = 0x0102;

	CHECK_EQ(opensc2_encode_state(&s, 0x7F, r), 54);
	CHECK_EQ(r[0], 0x42);
	CHECK_EQ(r[1], 0x7F);
	/* buttons: A | STEAM | L | left pad touch | right pad click */
	CHECK_EQ(opensc2_get_le32(&r[2]), 0x00090001u | 0x02000000u | 0x00400000u);
	CHECK_EQ(opensc2_get_le16(&r[6]), 1000);
	CHECK_EQ(opensc2_get_le16(&r[8]), 32767);
	CHECK_EQ((int16_t)opensc2_get_le16(&r[10]), -32767);
	CHECK_EQ(opensc2_get_le16(&r[12]), 0x1234);
	CHECK_EQ((int16_t)opensc2_get_le16(&r[14]), -2);
	CHECK_EQ(opensc2_get_le16(&r[16]), 32767);
	CHECK_EQ((int16_t)opensc2_get_le16(&r[18]), 100);
	CHECK_EQ((int16_t)opensc2_get_le16(&r[20]), -100);
	CHECK_EQ(opensc2_get_le16(&r[22]), 500);
	CHECK_EQ(opensc2_get_le16(&r[24]), 0);
	CHECK_EQ(opensc2_get_le16(&r[26]), 0);
	CHECK_EQ(opensc2_get_le32(&r[30]), 0xA1B2C3D4u);
	CHECK_EQ(opensc2_get_le16(&r[34]), 1);
	CHECK_EQ(opensc2_get_le16(&r[38]), 16384);
	CHECK_EQ((int16_t)opensc2_get_le16(&r[42]), -1);
	CHECK_EQ(opensc2_get_le16(&r[46]), 32767);     /* quaternion W */
	CHECK_EQ(opensc2_get_le16(&r[52]), 0x0102);    /* quaternion Z, the last two bytes */
}

OPENSC2_TEST(state_roundtrip)
{
	struct opensc2_state s, back;
	uint8_t r[OPENSC2_STATE_REPORT_LEN];
	uint8_t seq = 0;

	memset(&s, 0, sizeof(s));
	s.buttons = OPENSC2_BTN_DPAD_UP | OPENSC2_TOUCH_RIGHT_PAD;
	s.trigger[OPENSC2_RIGHT] = 32767;
	s.stick[OPENSC2_RIGHT].y = -5000;
	s.pad[OPENSC2_RIGHT].touched = true;
	s.pad[OPENSC2_RIGHT].x = -32767;
	s.imu.gyro[2] = 1234;
	opensc2_encode_state(&s, 9, r);
	opensc2_decode_state(r, &back, &seq);
	CHECK_EQ(seq, 9);
	CHECK_EQ(back.buttons, s.buttons);
	CHECK_EQ(back.trigger[OPENSC2_RIGHT], 32767);
	CHECK_EQ(back.stick[OPENSC2_RIGHT].y, -5000);
	CHECK(back.pad[OPENSC2_RIGHT].touched);
	CHECK_EQ(back.pad[OPENSC2_RIGHT].x, -32767);
	CHECK_EQ(back.imu.gyro[2], 1234);
	CHECK_EQ(opensc2_input_report_size(0x42) + 1, OPENSC2_STATE_REPORT_LEN);
}

/* ---- battery, haptic status ----------------------------------------------------------------- */

OPENSC2_TEST(battery_reports)
{
	struct opensc2_battery b;
	uint8_t r[OPENSC2_BATTERY_REPORT_LEN], d[OPENSC2_BATTERY_DATA_LEN];

	memset(&b, 0, sizeof(b));
	b.charge_state = OPENSC2_CHARGE_CHARGING;
	b.level = 150;   /* clamps to 100 */
	b.battery_mv = 3900;
	b.system_mv = 3300;
	b.input_mv = 5000;
	b.current_ma = -250;
	b.input_current_ma = 400;
	b.temperature = 0x0BCD;
	b.gauge_flags = 0x5A;

	CHECK_EQ(opensc2_encode_battery(&b, r), 15);
	CHECK_EQ(r[0], 0x43);
	CHECK_EQ(r[1], 2);
	CHECK_EQ(r[2], 100);
	CHECK_EQ(opensc2_get_le16(&r[3]), 3900);
	CHECK_EQ(opensc2_get_le16(&r[5]), 3300);
	CHECK_EQ(opensc2_get_le16(&r[7]), 5000);
	CHECK_EQ((int16_t)opensc2_get_le16(&r[9]), -250);
	CHECK_EQ(opensc2_get_le16(&r[11]), 400);
	CHECK_EQ(opensc2_get_le16(&r[13]), 0x0BCD);
	CHECK_EQ(opensc2_input_report_size(0x43) + 1, OPENSC2_BATTERY_REPORT_LEN);

	CHECK_EQ(opensc2_encode_battery_data(&b, d), 15);
	CHECK_EQ(opensc2_get_le16(&d[0]), 0x0BCD);   /* temperature first */
	CHECK_EQ(opensc2_get_le16(&d[2]), 3900);
	CHECK_EQ(d[12], 100);
	CHECK_EQ(d[13], 0x5A);
	CHECK_EQ(d[14], 2);
}

OPENSC2_TEST(haptic_status)
{
	uint8_t r[OPENSC2_HAPTIC_STATUS_LEN];

	CHECK_EQ(opensc2_encode_haptic_status(3, 0x08, r), 6);
	CHECK_EQ(r[0], 0x44);
	CHECK_EQ(r[1], 3);
	CHECK_EQ(opensc2_get_le32(&r[2]), 0x08);
	CHECK_EQ(opensc2_input_report_size(0x44) + 1, OPENSC2_HAPTIC_STATUS_LEN);
}

/* ---- identity ------------------------------------------------------------------------------- */

OPENSC2_TEST(identity_record)
{
	struct opensc2_identity id = test_identity();
	uint8_t rec[OPENSC2_IDENTITY_RECORD_LEN];

	CHECK_EQ(opensc2_encode_identity(&id, rec), 57);
	CHECK_EQ(opensc2_get_le32(&rec[0]), 0x1302);
	CHECK_EQ(opensc2_get_le32(&rec[4]), 0x6AD00000u);
	CHECK_EQ(opensc2_get_le32(&rec[8]), 0x11223344u);
	CHECK_EQ(opensc2_get_le32(&rec[12]), 0x4B);
	CHECK(memcmp(&rec[16], "OSC2DEV000001", 14) == 0);   /* with its NUL */
	CHECK(memcmp(&rec[30], "OSC2PCB000001", 14) == 0);
	CHECK(memcmp(&rec[44], "opensc2-0.1\0\0", 13) == 0);
}

OPENSC2_TEST(identity_checks)
{
	struct opensc2_identity id = test_identity();

	CHECK(opensc2_serial_valid("ABC0123456789"));
	CHECK(!opensc2_serial_valid("abc0123456789"));   /* lower case */
	CHECK(!opensc2_serial_valid("ABC012345678"));    /* 12 characters */
	CHECK(!opensc2_serial_valid("ABC01234567890"));  /* 14 characters */
	CHECK(!opensc2_serial_valid("ABC-123456789"));
	CHECK(opensc2_git_revision_valid("970218aed150"));
	CHECK(!opensc2_git_revision_valid(""));
	CHECK(!opensc2_git_revision_valid("1234567890123"));
	CHECK(!opensc2_git_revision_valid("has space"));
	CHECK_EQ(opensc2_identity_check(&id), 0);
	id.unit_serial[3] = 'x';
	id.product_id = 0;
	CHECK_EQ(opensc2_identity_check(&id), OPENSC2_ID_BAD_UNIT_SERIAL | OPENSC2_ID_BAD_PRODUCT_ID);
}

OPENSC2_TEST(attributes)
{
	struct opensc2_identity id = test_identity();
	uint8_t a[30], s[20];

	CHECK_EQ(opensc2_encode_attributes(&id, 4000, a), 30);
	CHECK_EQ(a[0], 1);
	CHECK_EQ(opensc2_get_le32(&a[1]), 0x1302);
	CHECK_EQ(a[5], 2);
	CHECK_EQ(opensc2_get_le32(&a[6]), 0);
	CHECK_EQ(a[10], 10);
	CHECK_EQ(opensc2_get_le32(&a[11]), 0x11223344u);
	CHECK_EQ(a[15], 4);
	CHECK_EQ(opensc2_get_le32(&a[16]), 0x6AD00000u);
	CHECK_EQ(a[20], 9);
	CHECK_EQ(opensc2_get_le32(&a[21]), 0x4B);
	CHECK_EQ(a[25], 11);
	CHECK_EQ(opensc2_get_le32(&a[26]), 4000);

	CHECK_EQ(opensc2_encode_string_attribute(&id, 1, s), 20);
	CHECK_EQ(s[0], 1);
	CHECK(memcmp(&s[1], "OSC2DEV000001", 14) == 0);
	CHECK_EQ(s[19], 0);
	opensc2_encode_string_attribute(&id, 4, s);
	CHECK(memcmp(&s[1], "OSC2PCB000001", 13) == 0);
	opensc2_encode_string_attribute(&id, 0, s);
	CHECK(memcmp(&s[1], "OSC2PCB000001", 13) == 0);
	opensc2_encode_string_attribute(&id, 3, s);
	CHECK(memcmp(&s[1], "opensc2-0.1", 12) == 0);
	opensc2_encode_string_attribute(&id, 7, s);
	CHECK(memcmp(&s[1], "NA\0", 3) == 0);
}

/* ---- bonds and packets ------------------------------------------------------------------------ */

OPENSC2_TEST(bonds)
{
	struct opensc2_bond b, back;
	uint8_t raw[OPENSC2_BOND_LEN];

	memset(&b, 0, sizeof(b));
	b.puck_id = 0x01020304;
	b.controller_id = 0xA0B0C0D0;
	memcpy(b.puck_serial, "PUCKSERIAL000", 13);
	opensc2_bond_pack(&b, raw);
	CHECK_EQ(raw[0], 0x04);
	CHECK_EQ(raw[7], 0xA0);
	CHECK(memcmp(&raw[8], "PUCKSERIAL000\0\0\0", 16) == 0);
	opensc2_bond_unpack(raw, &back);
	CHECK(memcmp(&b, &back, sizeof(b)) == 0);
	CHECK(opensc2_bond_valid(&b));
	b.controller_id = 0;
	CHECK(!opensc2_bond_valid(&b));
}

OPENSC2_TEST(connect_request)
{
	const uint8_t pkt[18] = { 0xE1, 0x04, 0x03, 0x02, 0x01, 0xD0, 0xC0, 0xB0, 0xA0,
				  40, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x31 };
	struct opensc2_connect_request req;

	CHECK(opensc2_parse_connect_request(pkt, sizeof(pkt), &req));
	CHECK_EQ(req.puck_id, 0x01020304u);
	CHECK_EQ(req.controller_id, 0xA0B0C0D0u);
	CHECK_EQ(req.channel, 40);
	CHECK(memcmp(req.address, "\x11\x22\x33\x44", 4) == 0);
	CHECK_EQ(req.prefix, 0x31);
	CHECK(!opensc2_parse_connect_request(pkt, 17, &req));
	{
		uint8_t bad[18];

		memcpy(bad, pkt, sizeof(bad));
		bad[9] = 101;
		CHECK(!opensc2_parse_connect_request(bad, sizeof(bad), &req));
		bad[9] = 40;
		bad[0] = 0xE2;
		CHECK(!opensc2_parse_connect_request(bad, sizeof(bad), &req));
	}
}

OPENSC2_TEST(protocol_version)
{
	const uint8_t v00[3] = { 0xE7, 0, 0 }, v01[3] = { 0xE7, 0, 1 }, v11[3] = { 0xE7, 1, 1 };
	const uint8_t v10[3] = { 0xE7, 1, 0 }, v21[3] = { 0xE7, 2, 1 };

	CHECK_EQ(opensc2_protocol_version(v00, 3), 0);
	CHECK_EQ(opensc2_protocol_version(v01, 3), 1);
	CHECK_EQ(opensc2_protocol_version(v11, 3), 1);
	CHECK_EQ(opensc2_protocol_version(v10, 3), -1);
	CHECK_EQ(opensc2_protocol_version(v21, 3), -1);
	CHECK_EQ(opensc2_protocol_version(v01, 1), 0);
}

OPENSC2_TEST(channel_change)
{
	const uint8_t three[4] = { 0xE4, 36, 2, 80 };
	const uint8_t five[6] = { 0xE4, 10, 20, 30, 40, 50 };
	const uint8_t bad[2] = { 0xE4, 120 };
	uint8_t ch[4];

	CHECK_EQ(opensc2_parse_channel_change(three, sizeof(three), ch), 3);
	CHECK_EQ(ch[0], 36);
	CHECK_EQ(ch[2], 80);
	CHECK_EQ(ch[3], 0xFF);
	CHECK_EQ(opensc2_parse_channel_change(five, sizeof(five), ch), 4);
	CHECK_EQ(ch[3], 40);
	CHECK_EQ(opensc2_parse_channel_change(bad, sizeof(bad), ch), 0);
	CHECK_EQ(opensc2_parse_channel_change(three, 1, ch), 0);
}

OPENSC2_TEST(pairing_packets)
{
	uint8_t pkt[OPENSC2_PAIRING_PKT_LEN];
	uint8_t offer[21] = { 0xE0, 0x78, 0x56, 0x34, 0x12 };
	uint32_t puck_id;
	char serial[17];

	CHECK_EQ(opensc2_build_pairing(OPENSC2_PKT_PAIRING_REQUEST, 0x93A7A291, "OSC2DEV000001",
				       pkt), 21);
	CHECK_EQ(pkt[0], 0xF0);
	CHECK_EQ(opensc2_get_le32(&pkt[1]), 0x93A7A291u);
	CHECK(memcmp(&pkt[5], "OSC2DEV000001\0\0\0", 16) == 0);

	memcpy(&offer[5], "PUCK0000000001", 14);
	CHECK(opensc2_parse_pairing_offer(offer, sizeof(offer), &puck_id, serial));
	CHECK_EQ(puck_id, 0x12345678u);
	CHECK(strcmp(serial, "PUCK0000000001") == 0);
	offer[0] = 0xE2;
	CHECK(!opensc2_parse_pairing_offer(offer, sizeof(offer), &puck_id, serial));

	CHECK(!opensc2_address_byte_ok(0x00));
	CHECK(!opensc2_address_byte_ok(0xAA));
	CHECK(!opensc2_address_byte_ok(0x55));
	CHECK(opensc2_address_byte_ok(0x93));
}

OPENSC2_TEST(records)
{
	/* keep-alive: SET {0x87, 3, 9, 0, 0}, GET {0x87}, output {0x82, 4, 1, 0xF4} */
	const uint8_t pkt[] = { 0xE3, 5, 1, 0x87, 3, 9, 0, 0, 1, 3, 0x87, 4, 5, 0x82, 4, 1, 0xF4 };
	struct opensc2_record rec;
	size_t pos = 1;

	CHECK_EQ(opensc2_record_next(pkt, sizeof(pkt), &pos, &rec), 1);
	CHECK_EQ(rec.type, 1);
	CHECK_EQ(rec.len, 5);
	CHECK_EQ(rec.data[0], 0x87);
	CHECK_EQ(opensc2_record_next(pkt, sizeof(pkt), &pos, &rec), 1);
	CHECK_EQ(rec.type, 3);
	CHECK_EQ(rec.data[0], 0x87);
	CHECK_EQ(opensc2_record_next(pkt, sizeof(pkt), &pos, &rec), 1);
	CHECK_EQ(rec.type, 5);
	CHECK_EQ(rec.len, 4);
	CHECK_EQ(opensc2_record_next(pkt, sizeof(pkt), &pos, &rec), 0);
	pos = 1;
	CHECK_EQ(opensc2_record_next(pkt, 5, &pos, &rec), -1);   /* record runs past the end */
}

OPENSC2_TEST(payload_builder)
{
	struct opensc2_payload p;
	uint8_t report[OPENSC2_STATE_REPORT_LEN] = { 0x42 };
	uint8_t answer[63] = { 0x83, 30 };

	opensc2_payload_start(&p, OPENSC2_PKT_APP_DATA);
	CHECK_EQ(opensc2_payload_add_record(&p, OPENSC2_REC_GET_ANSWER, answer, 63), 0);
	CHECK_EQ(opensc2_payload_add_record(&p, OPENSC2_REC_INPUT, report, sizeof(report)), 0);
	CHECK_EQ(p.len, 1 + 65 + 56);
	CHECK_EQ(p.data[1], 63);
	CHECK_EQ(p.data[2], 4);
	CHECK_EQ(p.data[66], 54);
	CHECK_EQ(p.data[67], 6);
	CHECK_EQ(p.data[68], 0x42);
	/* 122 used: a 6-byte battery record would pass 127. */
	CHECK_EQ(opensc2_payload_add_record(&p, OPENSC2_REC_INPUT, report, 4), -1);
	CHECK_EQ(opensc2_payload_add_record(&p, OPENSC2_REC_INPUT, report, 3), 0);
	CHECK_EQ(p.len, 127);
}

/* ---- output reports ----------------------------------------------------------------------------- */

OPENSC2_TEST(output_rumble)
{
	const uint8_t r[10] = { 0x80, 0, 0x10, 0x00, 0x34, 0x12, 0xFE, 0x78, 0x56, 0x02 };
	struct opensc2_output o[2];

	CHECK_EQ(opensc2_decode_output(r, sizeof(r), o), 1);
	CHECK_EQ(o[0].type, OPENSC2_OUTPUT_RUMBLE);
	CHECK_EQ(o[0].rumble.intensity, 0x0010);
	CHECK_EQ(o[0].rumble.left_speed, 0x1234);
	CHECK_EQ(o[0].rumble.left_gain_db, -2);
	CHECK_EQ(o[0].rumble.right_speed, 0x5678);
	CHECK_EQ(o[0].rumble.right_gain_db, 2);
	CHECK_EQ(opensc2_decode_output(r, 9, o), -1);
}

OPENSC2_TEST(output_haptics)
{
	const uint8_t pulse[8] = { 0x81, 0, 0x10, 0x27, 0x20, 0x4E, 3, 0 };
	const uint8_t cmd[4] = { 0x82, 5, 1, 0xFA };
	const uint8_t tone[10] = { 0x83, 3, 0xF4, 0xC8, 0x00, 0x2C, 0x01, 10, 0, 50 };
	const uint8_t sweep[9] = { 0x84, 6, 0xFD, 0x2C, 0x01, 0x64, 0x00, 0xE8, 0x03 };
	const uint8_t script[4] = { 0x85, 2, 9, 0 };
	struct opensc2_output o[2];

	CHECK_EQ(opensc2_decode_output(pulse, sizeof(pulse), o), 1);
	CHECK_EQ(o[0].haptic.kind, OPENSC2_HAPTIC_PULSE);
	CHECK_EQ(o[0].haptic.channels, OPENSC2_HAPTIC_RIGHT_PAD);   /* 0x81 swaps the pads */
	CHECK_EQ(o[0].haptic.pulse.on_us, 10000);
	CHECK_EQ(o[0].haptic.pulse.off_us, 20000);
	CHECK_EQ(o[0].haptic.pulse.count, 3);

	CHECK_EQ(opensc2_decode_output(cmd, sizeof(cmd), o), 1);
	CHECK_EQ(o[0].haptic.kind, OPENSC2_HAPTIC_COMMAND);
	CHECK_EQ(o[0].haptic.channels, OPENSC2_HAPTIC_LEFT_GRIP | OPENSC2_HAPTIC_RIGHT_GRIP);
	CHECK_EQ(o[0].haptic.command.command, OPENSC2_HAPTIC_CMD_CLICK_A);
	CHECK_EQ(o[0].haptic.command.gain_db, -6);

	CHECK_EQ(opensc2_decode_output(tone, sizeof(tone), o), 1);
	CHECK_EQ(o[0].haptic.kind, OPENSC2_HAPTIC_TONE);
	CHECK_EQ(o[0].haptic.channels, OPENSC2_HAPTIC_LEFT_GRIP);
	CHECK_EQ(o[0].haptic.tone.gain_db, -12);
	CHECK_EQ(o[0].haptic.tone.freq_hz, 200);
	CHECK_EQ(o[0].haptic.tone.duration_ms, 300);
	CHECK_EQ(o[0].haptic.tone.lfo_hz, 10);
	CHECK_EQ(o[0].haptic.tone.lfo_depth, 50);

	CHECK_EQ(opensc2_decode_output(sweep, sizeof(sweep), o), 1);
	CHECK_EQ(o[0].haptic.kind, OPENSC2_HAPTIC_SWEEP);
	CHECK_EQ(o[0].haptic.channels, OPENSC2_HAPTIC_LEFT_PAD | OPENSC2_HAPTIC_RIGHT_PAD);
	CHECK(o[0].haptic.sweep.antiphase);
	CHECK_EQ(o[0].haptic.sweep.duration_ms, 300);
	CHECK_EQ(o[0].haptic.sweep.start_hz, 100);
	CHECK_EQ(o[0].haptic.sweep.end_hz, 1000);

	CHECK_EQ(opensc2_decode_output(script, sizeof(script), o), 1);
	CHECK_EQ(o[0].haptic.kind, OPENSC2_HAPTIC_SCRIPT);
	CHECK_EQ(o[0].haptic.script.id, 9);
	CHECK_EQ(o[0].haptic.channels, OPENSC2_HAPTIC_LEFT_PAD | OPENSC2_HAPTIC_RIGHT_PAD);
}

OPENSC2_TEST(output_pcm)
{
	const uint8_t control[4] = { 0x86, 2, 3, 6 };   /* set format 6 = signed 8-bit at 2 kHz, left pad */
	uint8_t data87[8] = { 0x87, 4, 1, 2, 3, 4, 5, 6 };
	uint8_t dual[64] = { 0x88, 3 };
	const uint8_t data89[6] = { 0x89, 2, 0x80, 7, 8, 9 };
	const uint8_t nothing[2] = { 0x8A, 0 };
	struct opensc2_output o[2];

	CHECK_EQ(opensc2_decode_output(control, sizeof(control), o), 1);
	CHECK_EQ(o[0].haptic.kind, OPENSC2_HAPTIC_PCM_CONTROL);
	CHECK_EQ(o[0].haptic.channels, OPENSC2_HAPTIC_LEFT_PAD);
	CHECK_EQ(o[0].haptic.pcm_control.operation, OPENSC2_PCM_SET_FORMAT);
	CHECK_EQ(o[0].haptic.pcm_control.encoding, OPENSC2_PCM_S8);
	CHECK_EQ(o[0].haptic.pcm_control.rate_hz, 2000);

	CHECK_EQ(opensc2_decode_output(data87, sizeof(data87), o), 1);
	CHECK_EQ(o[0].haptic.channels, OPENSC2_HAPTIC_RIGHT_GRIP);   /* 0x87 target 4 */
	CHECK_EQ(o[0].haptic.pcm.len, 6);
	CHECK_EQ(o[0].haptic.pcm.data[0], 1);
	data87[1] = 1;
	opensc2_decode_output(data87, sizeof(data87), o);
	CHECK_EQ(o[0].haptic.channels, 0);                           /* 0x87 target 1: nothing */

	dual[2] = 11;
	dual[33] = 22;
	CHECK_EQ(opensc2_decode_output(dual, sizeof(dual), o), 2);
	CHECK_EQ(o[0].haptic.channels, OPENSC2_HAPTIC_LEFT_GRIP);
	CHECK_EQ(o[0].haptic.pcm.data[0], 11);
	CHECK_EQ(o[1].haptic.channels, OPENSC2_HAPTIC_RIGHT_GRIP);
	CHECK_EQ(o[1].haptic.pcm.data[0], 22);
	CHECK_EQ(o[1].haptic.pcm.len, 3);
	CHECK_EQ(opensc2_decode_output(dual, 20, o), -1);

	CHECK_EQ(opensc2_decode_output(data89, sizeof(data89), o), 1);
	CHECK_EQ(o[0].haptic.channels, OPENSC2_HAPTIC_LEFT_GRIP | OPENSC2_HAPTIC_RIGHT_GRIP);
	CHECK_EQ(o[0].haptic.pcm.len, 2);
	CHECK_EQ(o[0].haptic.pcm.data[1], 8);

	CHECK_EQ(opensc2_decode_output(nothing, sizeof(nothing), o), 0);
}

/* ---- lizard mode, settings, fusion ----------------------------------------------------------- */

OPENSC2_TEST(lizard)
{
	struct opensc2_lizard lz;
	struct opensc2_state s;
	uint8_t kb[9], mouse[6];

	opensc2_lizard_reset(&lz);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(opensc2_lizard_step(&lz, &s, kb, mouse), 0);

	s.buttons = OPENSC2_BTN_A | OPENSC2_BTN_DPAD_LEFT;
	CHECK_EQ(opensc2_lizard_step(&lz, &s, kb, mouse), OPENSC2_LIZARD_KEYBOARD);
	CHECK_EQ(kb[0], 0x41);
	CHECK_EQ(kb[3], 0x28);   /* Enter */
	CHECK_EQ(kb[4], 0x50);   /* left arrow */
	CHECK_EQ(opensc2_lizard_step(&lz, &s, kb, mouse), 0);   /* unchanged: nothing new */

	s.buttons = OPENSC2_CLICK_RIGHT_TRIGGER;
	s.pad[OPENSC2_RIGHT].touched = true;
	s.pad[OPENSC2_RIGHT].x = 0;
	CHECK_EQ(opensc2_lizard_step(&lz, &s, kb, mouse),
		 OPENSC2_LIZARD_KEYBOARD | OPENSC2_LIZARD_MOUSE);
	CHECK_EQ(kb[3], 0);
	CHECK_EQ(mouse[0], 0x40);
	CHECK_EQ(mouse[1], 1);
	s.pad[OPENSC2_RIGHT].x = 640;    /* 10 pointer steps to the right */
	s.pad[OPENSC2_RIGHT].y = 640;    /* finger up: pointer up */
	CHECK_EQ(opensc2_lizard_step(&lz, &s, kb, mouse), OPENSC2_LIZARD_MOUSE);
	CHECK_EQ((int8_t)mouse[2], 10);
	CHECK_EQ((int8_t)mouse[3], -10);
}

OPENSC2_TEST(settings_defaults)
{
	CHECK_EQ(opensc2_setting_default(OPENSC2_SETTING_LIZARD_MODE), 1);
	CHECK_EQ(opensc2_setting_default(OPENSC2_SETTING_TRIGGER_THRESHOLD), 90);
	CHECK_EQ(opensc2_setting_default(OPENSC2_SETTING_RUMBLE_GAIN_DB), -10);
	CHECK_EQ(opensc2_setting_default(OPENSC2_SETTING_LED_BRIGHTNESS), 50);
	CHECK_EQ(opensc2_setting_default(OPENSC2_SETTING_IMU_MODE), 0);
	CHECK_EQ(opensc2_setting_default(1), 0);
}

OPENSC2_TEST(fusion)
{
	struct opensc2_fusion f;
	const float level[3] = { 0.0f, 0.0f, 1.0f };
	const float still[3] = { 0.0f, 0.0f, 0.0f };
	const float yaw[3] = { 0.0f, 0.0f, 90.0f };
	int16_t q[4];

	opensc2_fusion_init(&f);
	for (int i = 0; i < 100; i++) {
		opensc2_fusion_update(&f, level, still, 0.004f);
	}
	opensc2_fusion_quat(&f, q);
	CHECK(q[0] > 32700);
	CHECK(q[1] > -50 && q[1] < 50);

	/* One second at 90 deg/s around Z: a quarter turn, w = x = cos(45 deg) * 32767. */
	for (int i = 0; i < 250; i++) {
		opensc2_fusion_update(&f, level, yaw, 0.004f);
	}
	opensc2_fusion_quat(&f, q);
	CHECK(q[0] > 22800 && q[0] < 23600);
	CHECK(q[3] > 22800 && q[3] < 23600);

	CHECK_EQ(opensc2_accel_from_g(1.0f), 16384);
	CHECK_EQ(opensc2_accel_from_g(-3.0f), -32767);
	CHECK_EQ(opensc2_gyro_from_dps(2000.0f), 32767);
}
