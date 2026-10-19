/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the vendor command channel on USB (commands.c with the stand-ins of stubs.c),
 * request by request as Steam ran its wired session and its pairing with a real controller: the
 * framing of feature report 1, one stored request per link, the answers, the STALLs, the bond
 * written over the cable, the reboot into wireless mode, lizard mode and the settings rules.
 */
#include <errno.h>
#include <string.h>
#include "internal.h"
#include "stubs.h"

static uint8_t usb_frame[64];

/* SET_REPORT(Feature 1): 64 bytes `01 <opcode> <n> <payload>`, zero padded. */
static int usb_set(uint8_t opcode, const void *payload, uint8_t n)
{
	memset(usb_frame, 0, sizeof(usb_frame));
	usb_frame[0] = 1;
	usb_frame[1] = opcode;
	usb_frame[2] = n;
	if (n) {
		memcpy(&usb_frame[3], payload, n);
	}
	return osc2_usb_feature_set(usb_frame, sizeof(usb_frame));
}

/* GET_REPORT(Feature 1) as the July profile answers it (64 bytes). */
static int usb_get(uint8_t reply[64], uint16_t wlength)
{
	uint8_t opcode, n;

	memset(reply, 0xEE, 64);
	return osc2_usb_feature_get(reply, wlength, 64, &opcode, &n);
}

static const char key_bond[] = "esb/bond";
static const char key_transport[] = "user/wireless_transport";

OPENSC2_TEST(usb_cmd_identity)
{
	const uint8_t tag1 = 1;
	uint8_t reply[64];

	stub_reset();
	/* No SET yet: a GET answers the attributes. */
	CHECK_EQ(usb_get(reply, 65), 64);
	CHECK_EQ(reply[0], 0x01);
	CHECK_EQ(reply[1], 0x83);
	CHECK_EQ(reply[2], 30);

	/* 0xAE tag 1: the unit serial, the same as the USB serial string. */
	CHECK_EQ(usb_set(0xAE, &tag1, 1), 0);
	CHECK_EQ(usb_get(reply, 65), 64);
	CHECK_EQ(reply[1], 0xAE);
	CHECK_EQ(reply[2], 20);
	CHECK_EQ(reply[3], 1);
	CHECK(memcmp(&reply[4], "OSC0123456789", 13) == 0);
	CHECK_EQ(reply[17], 0);
	CHECK_EQ(reply[63], 0);   /* zeros after the answer */

	/* 0x83: six {tag, u32} in the real controller's order, attribute 11 = 4000. */
	CHECK_EQ(usb_set(0x83, NULL, 0), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[2], 30);
	CHECK_EQ(reply[3], 1);
	CHECK_EQ(opensc2_get_le32(&reply[4]), 0x1302);
	CHECK_EQ(reply[8], 2);
	CHECK_EQ(reply[13], 10);
	CHECK_EQ(opensc2_get_le32(&reply[14]), 0x68D2F92Eu);
	CHECK_EQ(reply[18], 4);
	CHECK_EQ(opensc2_get_le32(&reply[19]), 0x6A628345u);
	CHECK_EQ(reply[23], 9);
	CHECK_EQ(opensc2_get_le32(&reply[24]), 0x48);
	CHECK_EQ(reply[28], 11);
	CHECK_EQ(opensc2_get_le32(&reply[29]), 4000);
	/* GETs do not consume the request. */
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], 0x83);

	/* Steam's second 0xAE carries left-overs (n = 21): only the tag counts. */
	{
		uint8_t junk[21] = { 1, 0x83, 0x1E, 0x01, 0x02, 0x13 };

		CHECK_EQ(usb_set(0xAE, junk, sizeof(junk)), 0);
		CHECK_EQ(usb_get(reply, 64), 64);
		CHECK_EQ(reply[2], 20);
		CHECK(memcmp(&reply[4], "OSC0123456789", 13) == 0);
	}
}

OPENSC2_TEST(usb_cmd_stalls)
{
	const uint8_t tag1 = 1;
	const uint8_t audio[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x09, 0x05, 0xFF, 0xFF,
				    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	const uint8_t led100[3] = { 45, 100, 0 };
	uint8_t reply[64];

	stub_reset();
	CHECK_EQ(usb_set(0xAE, &tag1, 1), 0);
	/* An unknown opcode (0xC1, SDL's audio mapping) is stalled; the stored request stays. */
	CHECK(usb_set(0xC1, audio, sizeof(audio)) < 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], 0xAE);
	/* A set-only command is stored, but a GET of it stalls. */
	CHECK_EQ(usb_set(0x87, led100, sizeof(led100)), 0);
	CHECK(usb_get(reply, 64) < 0);
	/* Framing: exactly 64 bytes, report ID 1 or 2, n at most 61. */
	memset(usb_frame, 0, sizeof(usb_frame));
	usb_frame[0] = 1;
	usb_frame[1] = 0x83;
	CHECK_EQ(osc2_usb_feature_set(usb_frame, 63), -EMSGSIZE);
	CHECK_EQ(osc2_usb_feature_set(usb_frame, 64), 0);
	usb_frame[2] = 62;
	CHECK_EQ(osc2_usb_feature_set(usb_frame, 64), -E2BIG);
	usb_frame[2] = 0;
	usb_frame[0] = 3;
	CHECK_EQ(osc2_usb_feature_set(usb_frame, 64), -EBADMSG);
	/* The October form answers 63 bytes; a short wLength cuts the reply. */
	{
		uint8_t opcode, n;

		CHECK_EQ(osc2_usb_feature_get(reply, 64, 63, &opcode, &n), 63);
		CHECK_EQ(opcode, 0x83);
		CHECK_EQ(n, 30);
		CHECK_EQ(osc2_usb_feature_get(reply, 8, 64, &opcode, &n), 8);
	}
}

OPENSC2_TEST(usb_cmd_calibration_and_chime)
{
	const uint8_t persist[2] = { 0x01, 0x20 };
	const uint8_t bad_side[2] = { 0x02, 0x01 };
	const uint8_t chime[2] = { 0x01, 0x02 };

	stub_reset();
	/* Steam sends 0xE2 01 20 in every session: acknowledged for side 0 or 1. */
	CHECK_EQ(usb_set(0xE2, persist, sizeof(persist)), 0);
	CHECK(usb_set(0xE2, bad_side, sizeof(bad_side)) < 0);
	CHECK_EQ(usb_set(0xDC, chime, sizeof(chime)), 0);
	CHECK_EQ(osc2.chime, 2);
	CHECK_EQ(stub.saved_chime, 1);
}

OPENSC2_TEST(usb_cmd_pairing)
{
	uint8_t key_and_bond[9 + OPENSC2_BOND_LEN];
	struct opensc2_bond bond = { .puck_id = 0x01020304, .controller_id = 0x0A0B0C0D };
	uint8_t reply[64];

	stub_reset();
	memcpy(bond.puck_serial, "PUCK000000001", 13);

	/* Steam reads the stored transport (2: the puck, bond 1) ... */
	CHECK_EQ(usb_set(0xED, key_transport, sizeof(key_transport)), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], 0xED);
	CHECK_EQ(reply[2], 1);
	CHECK_EQ(reply[3], 2);
	/* ... and the bond: none yet, so length 0. */
	CHECK_EQ(usb_set(0xED, key_bond, sizeof(key_bond)), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[2], 0);
	/* Saving an empty bond is refused. */
	CHECK(usb_set(0xEF, key_bond, sizeof(key_bond)) < 0);

	/* The new bond into RAM, saved, read back, saved again. */
	memcpy(key_and_bond, key_bond, sizeof(key_bond));
	opensc2_bond_pack(&bond, &key_and_bond[9]);
	CHECK_EQ(usb_set(0xEE, key_and_bond, sizeof(key_and_bond)), 0);
	CHECK(stub.radio_events & OSC2_EV_BOND);
	CHECK_EQ(stub.saved_bonds, 0);   /* RAM only */
	CHECK_EQ(usb_set(0xEF, key_bond, sizeof(key_bond)), 0);
	CHECK_EQ(stub.saved_bonds, 1);
	CHECK_EQ(usb_set(0xED, key_bond, sizeof(key_bond)), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[2], OPENSC2_BOND_LEN);
	CHECK(memcmp(&reply[3], &key_and_bond[9], OPENSC2_BOND_LEN) == 0);
	CHECK_EQ(usb_set(0xEF, key_bond, sizeof(key_bond)), 0);

	/* A bond of another length is refused and changes nothing. */
	CHECK(usb_set(0xEE, key_and_bond, sizeof(key_and_bond) - 1) < 0);
	CHECK_EQ(osc2.bonds[0].puck_id, 0x01020304u);
	/* 0xEF needs the key's NUL as the last byte. */
	{
		uint8_t longer[10];

		memcpy(longer, key_bond, sizeof(key_bond));
		longer[9] = 0x55;
		CHECK(usb_set(0xEF, longer, sizeof(longer)) < 0);
	}

	/* Steam's reboot into wireless mode: acknowledged, acted on 20 ms later. */
	{
		const uint8_t warm[4] = { 0x52, 0xAF, 0x27, 0xA4 };

		CHECK_EQ(usb_set(0x95, warm, sizeof(warm)), 0);
		CHECK_EQ(stub.power_requests, 1);
		CHECK_EQ(stub.power, OPENSC2_POWER_REBOOT_KEEP_LINK);
		CHECK_EQ(stub.power_delay_ms, 20);
	}
}

/* Steam reads the bonds through the puck after a wireless connection: a SET record 0xED, then a GET
 * record whose byte Valve's puck leaves unset. The answer must be for the SET's opcode, or the puck
 * drops it ("Failed to read triton info"). */
OPENSC2_TEST(radio_cmd_get_answers_last_set)
{
	uint8_t key_and_bond[9 + OPENSC2_BOND_LEN];
	uint8_t rec[2 + sizeof(key_bond)] = { 0xED, sizeof(key_bond) };
	struct opensc2_bond bond = { .puck_id = 0x01020304, .controller_id = 0x0A0B0C0D };
	uint8_t answer[63];

	stub_reset();
	memcpy(bond.puck_serial, "PUCK000000001", 13);
	/* No SET yet: the attributes, as on USB. */
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, answer), 2 + 30);
	CHECK_EQ(answer[0], 0x83);
	CHECK_EQ(answer[1], 30);

	memcpy(key_and_bond, key_bond, sizeof(key_bond));
	opensc2_bond_pack(&bond, &key_and_bond[9]);
	CHECK_EQ(usb_set(0xEE, key_and_bond, sizeof(key_and_bond)), 0);
	memcpy(&rec[2], key_bond, sizeof(key_bond));
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, sizeof(rec)), 0);
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, answer), 2 + OPENSC2_BOND_LEN);
	CHECK_EQ(answer[0], 0xED);
	CHECK_EQ(answer[1], OPENSC2_BOND_LEN);
	CHECK(memcmp(&answer[2], &key_and_bond[9], OPENSC2_BOND_LEN) == 0);
	/* Asked again (a retried GET), the same answer. */
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, answer), 2 + OPENSC2_BOND_LEN);
	CHECK_EQ(answer[0], 0xED);
}

OPENSC2_TEST(usb_cmd_links_and_power)
{
	const uint8_t sel9 = 9;
	const uint8_t turn_off[4] = { 0x10, 0x97, 0xDF, 0xAB };
	uint8_t reply[64], answer[63];

	stub_reset();
	/* The radio link's request does not change what a USB GET answers. */
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, (const uint8_t[]){ 0x89, 1, 9 }, 3), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], 0x83);
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, answer), 2 + 3);
	CHECK_EQ(answer[2], 9);
	CHECK_EQ(usb_set(0x89, &sel9, 1), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[2], 3);
	CHECK_EQ(reply[3], 9);
	CHECK_EQ(opensc2_get_le16(&reply[4]), 1);   /* lizard mode on */

	/* Turn-off is acknowledged and ignored while wired, scheduled while wireless. */
	CHECK_EQ(usb_set(0x9F, NULL, 0), 0);
	CHECK_EQ(usb_set(0x95, turn_off, sizeof(turn_off)), 0);
	CHECK_EQ(stub.power_requests, 0);
	osc2.mode = OPENSC2_MODE_WIRELESS;
	CHECK_EQ(usb_set(0x9F, NULL, 0), 0);
	CHECK_EQ(stub.power, OPENSC2_POWER_TURN_OFF);
	/* A plain reboot and the bootloader key. */
	osc2.mode = OPENSC2_MODE_WIRED;
	CHECK_EQ(usb_set(0x95, (const uint8_t[]){ 1, 2, 3, 4 }, 4), 0);
	CHECK_EQ(stub.power, OPENSC2_POWER_REBOOT);
	CHECK_EQ(usb_set(0x95, (const uint8_t[]){ 0xC0, 0xBA, 0xAA, 0xEC }, 4), 0);
	CHECK_EQ(stub.power, OPENSC2_POWER_BOOTLOADER);

	/* 0xF2 selector 2: uptime and the last disconnect reason as a u32 (9 bytes). */
	stub.now_ms = 123456;
	osc2.last_disconnect_reason = 1;
	CHECK_EQ(usb_set(0xF2, (const uint8_t[]){ 2 }, 1), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[2], 9);
	CHECK_EQ(opensc2_get_le32(&reply[4]), 123456);
	CHECK_EQ(opensc2_get_le32(&reply[8]), 1);
}

OPENSC2_TEST(usb_cmd_lizard)
{
	const uint8_t off9[3] = { 9, 0, 0 }, on9[3] = { 9, 1, 0 };
	const uint8_t wd_off[3] = { 71, 0, 0 }, wd_on[3] = { 71, 1, 0 };

	stub_reset();
	/* The first 0x81: lizard mode off with its release reports, the watchdog re-armed. */
	CHECK_EQ(usb_set(0x81, NULL, 0), 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LIZARD_MODE], 0);
	CHECK(strcmp(stub.lizard, "L") == 0);
	CHECK_EQ(stub.watchdog_armed, 1);
	/* Again: no reports (switches only), re-armed again. */
	CHECK_EQ(usb_set(0x81, NULL, 0), 0);
	CHECK(strcmp(stub.lizard, "LD") == 0);
	CHECK_EQ(stub.watchdog_armed, 2);
	/* Every write of setting 9 = 0 re-arms; 9 = 1 turns it on without a re-arm. */
	CHECK_EQ(usb_set(0x87, off9, 3), 0);
	CHECK_EQ(stub.watchdog_armed, 3);
	CHECK_EQ(usb_set(0x87, on9, 3), 0);
	CHECK(strcmp(stub.lizard, "LDE") == 0);
	CHECK_EQ(stub.watchdog_armed, 3);
	/* 0x85 raises the switches only. */
	CHECK_EQ(usb_set(0x85, NULL, 0), 0);
	CHECK(strcmp(stub.lizard, "LDEU") == 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LIZARD_MODE], 1);
	/* 0x8E: defaults (lizard mode back on), the IMU mode kept. */
	CHECK_EQ(usb_set(0x81, NULL, 0), 0);
	CHECK_EQ(usb_set(0x87, (const uint8_t[]){ 48, 0x18, 0 }, 3), 0);
	CHECK_EQ(usb_set(0x8E, NULL, 0), 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LIZARD_MODE], 1);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_IMU_MODE], 0x18);
	CHECK(strcmp(stub.lizard, "LDEULE") == 0);
	/* Setting 71: to 0 cancels, an unchanged write does nothing, back to 1 arms. */
	CHECK_EQ(usb_set(0x87, wd_off, 3), 0);
	CHECK_EQ(stub.watchdog_cancelled, 1);
	CHECK_EQ(usb_set(0x87, wd_off, 3), 0);
	CHECK_EQ(stub.watchdog_cancelled, 1);
	stub.watchdog_armed = 0;
	CHECK_EQ(usb_set(0x87, off9, 3), 0);   /* re-arming is ignored while 71 = 0 */
	CHECK_EQ(stub.watchdog_armed, 0);
	CHECK_EQ(usb_set(0x87, wd_on, 3), 0);
	CHECK_EQ(stub.watchdog_armed, 1);
	/* No active link: 0x81 is refused. */
	stub.link_active[OSC2_LINK_USB] = false;
	CHECK(usb_set(0x81, NULL, 0) < 0);
}

OPENSC2_TEST(usb_cmd_settings)
{
	/* Steam's settings block A, and values clamped to their ranges. */
	const uint8_t block_a[15] = { 48, 0, 0, 7, 7, 0, 8, 7, 0, 49, 2, 0, 82, 3, 0 };
	const uint8_t clicks[15] = { 24, 0, 0, 52, 0xFF, 0xFF, 53, 0xFF, 0xFF, 52, 0xFF, 0xFF,
				     53, 0xFF, 0xFF };
	const uint8_t clamped[9] = { 9, 5, 0, 68, 10, 0, 45, 200, 0 };

	stub_reset();
	CHECK_EQ(usb_set(0x87, block_a, sizeof(block_a)), 0);
	CHECK_EQ(osc2.settings[7], 7);
	CHECK_EQ(osc2.settings[49], 2);
	CHECK_EQ(usb_set(0x87, clicks, sizeof(clicks)), 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LEFT_CLICK_PRESSURE], -1);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_RIGHT_CLICK_PRESSURE], -1);
	CHECK_EQ(usb_set(0x87, clamped, sizeof(clamped)), 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LIZARD_MODE], 1);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_TRIGGER_THRESHOLD], 40);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LED_BRIGHTNESS], 100);
	CHECK_EQ(osc2.led.brightness, 100);
	CHECK(stub.led_notified > 0);
	/* Triplets only. */
	CHECK(usb_set(0x87, clamped, 4) < 0);
}
