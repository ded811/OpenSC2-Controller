/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the command channel's rules as the real controller has them (the protocol
 * documentation), on USB and through the puck's records: the stored request a SET leaves (its
 * bytes over those of earlier requests), a GET's reply (the answer over the request, then the
 * last SET_REPORT), the result codes, the payload read whatever the length says, and the
 * commands whose effect comes after their answer (power requests, the factory reset, 0xA2).
 */
#include <errno.h>
#include <string.h>
#include "internal.h"
#include "stubs.h"

/* A 64-byte SET_REPORT(Feature 1): `01 <opcode> <n> <payload>`, the rest `fill`. */
static int ch_set(uint8_t opcode, const void *payload, uint8_t n, uint8_t fill)
{
	uint8_t frame[64];

	memset(frame, fill, sizeof(frame));
	frame[0] = 1;
	frame[1] = opcode;
	frame[2] = n;
	if (n) {
		memcpy(&frame[3], payload, n);
	}
	return osc2_usb_feature_set(frame, sizeof(frame));
}

/* A GET_REPORT(Feature 1) with the October form's 63-byte replies; the bytes after the reply 0xEE. */
static int ch_get(uint8_t reply[64])
{
	uint8_t op, n;

	memset(reply, 0xEE, 64);
	return osc2_usb_feature_get(reply, 65, 63, &op, &n);
}

OPENSC2_TEST(channel_reply_and_stored_request)
{
	uint8_t reply[64];

	stub_reset();
	/* The first GET after a boot: 0x83, 63 bytes (the October form), zeros after the answer. */
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[1], 0x83);
	CHECK_EQ(reply[2], 30);
	CHECK_EQ(reply[33], 0);
	CHECK_EQ(reply[62], 0);
	CHECK_EQ(reply[63], 0xEE);   /* not sent */

	/* Steam's 0xAE: tag 1 and 20 more bytes. The answer covers the tag and 19 characters, the
	 * twentieth character's place is written 0, and the request's last byte stays after it. */
	{
		uint8_t req[21];

		for (int i = 0; i < 21; i++) {
			req[i] = (uint8_t)i;
		}
		req[0] = 1;
		CHECK_EQ(ch_set(0xAE, req, sizeof(req), 0), 0);
		CHECK_EQ(ch_get(reply), 63);
		CHECK_EQ(reply[2], 20);
		CHECK_EQ(reply[3], 1);
		CHECK(memcmp(&reply[4], "OSC0123456789", 13) == 0);
		CHECK_EQ(reply[17], 0);
		CHECK_EQ(reply[23], 0);    /* the twentieth character's place */
		CHECK_EQ(reply[24], 0);    /* the SET's padding */
	}

	/* The answer over the request: 0x89 with more payload than it reads leaves the rest. */
	CHECK_EQ(ch_set(0x89, (const uint8_t[]){ 45, 0xA1, 0xA2, 0xA3, 0xA4 }, 5, 0x5A), 0);
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[2], 3);
	CHECK_EQ(reply[3], 45);
	CHECK_EQ(opensc2_get_le16(&reply[4]), 50);
	CHECK_EQ(reply[6], 0xA3);   /* the request's bytes after the answer */
	CHECK_EQ(reply[7], 0xA4);
	CHECK_EQ(reply[8], 0x5A);   /* then the rest of the last SET_REPORT */
	CHECK_EQ(reply[62], 0x5A);

	/* A refused SET keeps the stored request, but the reply shows the refused SET's bytes after
	 * the stored request's (the stack's buffer). */
	CHECK_EQ(ch_set(0xC1, NULL, 0, 0x33), -EINVAL);
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[1], 0x89);
	CHECK_EQ(reply[6], 0xA3);
	CHECK_EQ(reply[8], 0x33);

	/* 0xED reads the name in the reply, so a name sent without its NUL runs into the bytes after
	 * the request: the SET's zero padding ends it (the key is read) ... */
	CHECK_EQ(ch_set(0xED, "user/haptic_boot_level", 22, 0), 0);   /* no NUL sent */
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[2], 1);
	CHECK_EQ(reply[3], 2);
	/* ... other padding makes it a name nobody has (no NUL in the 61 bytes: length 0) */
	CHECK_EQ(ch_set(0xED, "user/haptic_boot_level", 22, 'x'), 0);
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[2], 0);
}

OPENSC2_TEST(channel_result_codes)
{
	uint8_t rec[63] = { 0 };
	uint8_t out[63];
	int rc;

	stub_reset();
	osc2.mode = OPENSC2_MODE_WIRELESS;
	stub.link_active[OSC2_LINK_USB] = false;
	stub.link_active[OSC2_LINK_RADIO] = true;
	/* Through the puck: an unknown opcode -EINVAL, a length over 61 -EINVAL. */
	rec[0] = 0xC1;
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, 2), -EINVAL);
	rec[0] = 0x83;
	rec[1] = 62;
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, 63), -EINVAL);
	/* A GET of a command that only acts: -EOPNOTSUPP, {opcode, 0}. */
	rec[0] = 0x8E;
	rec[1] = 0;
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, 2), 0);
	CHECK_EQ(osc2_command_get_rc(OSC2_LINK_RADIO, out, &rc), 2);
	CHECK_EQ(rc, -EOPNOTSUPP);
	CHECK_EQ(out[0], 0x8E);
	CHECK_EQ(out[1], 0);
	/* A record shorter than its length: the rest reads as zeros, as the puck's records are
	 * padded (0x95 with no key bytes is a plain reboot). */
	rec[0] = 0x95;
	rec[1] = 4;
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, 2), 0);
	CHECK_EQ(stub.power, OPENSC2_POWER_REBOOT);
	CHECK_EQ(stub.power_delay_ms, 20);

	/* Settings by name: a name nobody has. */
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO,
				  (const uint8_t *)"\xEE\x06no/key\0", 8), -EINVAL);   /* no value */
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO,
				  (const uint8_t *)"\xEE\x08no/key\0\x01", 10), -EINVAL);
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO,
				  (const uint8_t *)"\xEF\x07no/key\0", 9), -EINVAL);
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO,
				  (const uint8_t *)"\xF0\x07no/key\0", 9), 0);   /* nothing to delete */
	CHECK_EQ(stub.named_deletes, 1);
	/* 0xEF and 0xF0 need the NUL as the last byte; 0xEE a name and a value. */
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO,
				  (const uint8_t *)"\xF0\x08no/key\0\x01", 10), -EINVAL);
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, (const uint8_t *)"\xF0\x00", 2), -EINVAL);
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, (const uint8_t *)"\xEE\x02\0\x01", 4),
		 -EINVAL);
	/* An empty name: 0xED's GET fails. */
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, (const uint8_t *)"\xED\x01\0", 3), 0);
	CHECK_EQ(osc2_command_get_rc(OSC2_LINK_RADIO, out, &rc), 2);
	CHECK_EQ(rc, -EINVAL);
	/* 0xEF of an empty bond: its read function has nothing (-ENODEV); of a key without a read
	 * function: -ENOTSUP. */
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, (const uint8_t *)"\xEF\x0B" "esb/bond_2\0", 13),
		 -ENODEV);
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO,
				  (const uint8_t *)"\xEF\x19" "settings/haptics/enabled\0", 27),
		 -ENOTSUP);
}

/* Another settings handler of the build is reached by name, as on the real controller (the
 * stand-in "app/thing": 2 bytes with a read function; "app/wo": no read function); the library's
 * own "opensc2" subtree never is. */
OPENSC2_TEST(channel_other_handlers)
{
	uint8_t reply[64];

	stub_reset();
	CHECK_EQ(ch_set(0xEE, "app/thing\0\x12\x34", 12, 0), 0);
	CHECK_EQ(stub.named_value[0], 0x12);
	CHECK_EQ(ch_set(0xEE, "app/thing\0\x12", 11, 0), -EINVAL);   /* its size */
	CHECK_EQ(ch_set(0xED, "app/thing", 10, 0), 0);
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[2], 2);
	CHECK_EQ(reply[3], 0x12);
	CHECK_EQ(reply[4], 0x34);
	CHECK_EQ(ch_set(0xEF, "app/thing", 10, 0), 0);
	CHECK_EQ(stub.named_saves, 1);
	CHECK_EQ(stub.named_saved_len, 2);
	CHECK_EQ(ch_set(0xF0, "app/thing", 10, 0), 0);
	CHECK_EQ(stub.named_value[0], 0);
	/* no read function: 0xED length 0, 0xEF -ENOTSUP */
	CHECK_EQ(ch_set(0xEE, "app/wo\0\x01", 8, 0), 0);
	CHECK_EQ(ch_set(0xED, "app/wo", 7, 0), 0);
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[2], 0);
	CHECK_EQ(ch_set(0xEF, "app/wo", 7, 0), -ENOTSUP);
	/* the library's own subtree: no handler */
	CHECK_EQ(ch_set(0xEE, "opensc2/mode\0\x01", 14, 0), -EINVAL);
	CHECK_EQ(ch_set(0xF0, "opensc2/mode", 13, 0), 0);
	CHECK_EQ(stub.named_deletes, 1);   /* app/thing's only */
}

OPENSC2_TEST(channel_user_store_and_bond)
{
	struct opensc2_bond bond = { .puck_id = 0x11223344, .controller_id = 0x55667788 };
	uint8_t raw[OPENSC2_BOND_LEN];
	uint8_t reply[64];

	stub_reset();
	/* 0xDC: type 1 saved, then in use; other types and a failed save acknowledged. */
	CHECK_EQ(ch_set(0xDC, (const uint8_t[]){ 1, 1 }, 2, 0), 0);
	CHECK_EQ(osc2.chime, 1);
	CHECK_EQ(stub.stored_chime, 1);
	CHECK_EQ(ch_set(0xDC, (const uint8_t[]){ 2, 0 }, 2, 0), 0);
	CHECK_EQ(osc2.chime, 1);
	CHECK_EQ(stub.saved_chime, 1);
	stub.storage_error = -EIO;
	CHECK_EQ(ch_set(0xDC, (const uint8_t[]){ 1, 0 }, 2, 0), 0);
	CHECK_EQ(osc2.chime, 1);   /* not saved: the level in use stays */
	stub.storage_error = 0;
	/* the bytes are read whatever the length says */
	{
		uint8_t frame[64] = { 1, 0xDC, 0, 1, 2 };

		CHECK_EQ(osc2_usb_feature_set(frame, sizeof(frame)), 0);
		CHECK_EQ(osc2.chime, 2);
	}

	/* 0xA2: exactly 24 bytes, stored for the next boot; the bond in use stays. */
	opensc2_bond_pack(&bond, raw);
	CHECK_EQ(ch_set(0xA2, raw, 23, 0), -EINVAL);
	CHECK_EQ(stub.bond_writes, 0);
	CHECK_EQ(ch_set(0xA2, raw, sizeof(raw), 0), 0);
	CHECK_EQ(stub.bond_writes, 1);
	CHECK_EQ(stub.stored_bonds[0].puck_id, 0x11223344u);
	CHECK(!opensc2_bond_valid(&osc2.bonds[0]));
	CHECK_EQ(ch_set(0xED, "esb/bond", 9, 0), 0);
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[2], 0);
	/* an ID of 0 deletes the stored one */
	bond.controller_id = 0;
	opensc2_bond_pack(&bond, raw);
	CHECK_EQ(ch_set(0xA2, raw, sizeof(raw), 0), 0);
	CHECK_EQ(stub.stored_bonds[0].puck_id, 0u);
}

OPENSC2_TEST(channel_power_requests)
{
	uint8_t reply[64];

	stub_reset();
	/* Wired: the turn-off requests are acknowledged and ignored. */
	CHECK_EQ(ch_set(0x9F, NULL, 0, 0), 0);
	CHECK_EQ(ch_set(0x95, (const uint8_t[]){ 0x10, 0x97, 0xDF, 0xAB }, 4, 0), 0);
	CHECK_EQ(stub.cmd_power, 2);
	CHECK_EQ(stub.power_requests, 0);
	/* The bootloader, 20 ms later. */
	CHECK_EQ(ch_set(0x90, NULL, 0, 0), 0);
	CHECK_EQ(stub.power, OPENSC2_POWER_BOOTLOADER);
	CHECK_EQ(stub.power_delay_ms, 20);
	/* A factory reset with a wrong key: acknowledged, nothing happens. */
	CHECK_EQ(ch_set(0x86, (const uint8_t[]){ 1, 2, 3, 4 }, 4, 0), 0);
	CHECK_EQ(stub.factory_resets, 0);
	/* A GET of either: nothing to read. */
	CHECK_EQ(ch_get(reply), -EOPNOTSUPP);
	/* 0xA1 with a selector other than 1: 18 zero bytes, the selector's place too. */
	CHECK_EQ(ch_set(0xA1, (const uint8_t[]){ 2, 0x77, 0x77 }, 3, 0x77), 0);
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[2], 18);
	for (int i = 3; i < 21; i++) {
		CHECK_EQ(reply[i], 0);
	}
	CHECK_EQ(reply[21], 0x77);   /* after the answer */
}

/* The board's own setters make the changes the host's commands make. */
OPENSC2_TEST(channel_board_setters)
{
	stub_reset();
	CHECK_EQ(opensc2_setting_set(45, 30), -EACCES);   /* before opensc2_init() */
	osc2.initialised = true;
	CHECK_EQ(opensc2_setting_set(86, 0), -EINVAL);
	CHECK_EQ(opensc2_setting_set(45, 300), 0);
	CHECK_EQ(osc2.settings[45], 100);   /* clamped, as 0x87 */
	CHECK_EQ(osc2.led.brightness, 100);
	CHECK(stub.led_notified > 0);
	CHECK_EQ(opensc2_led_set_color(250, 0, 101, 7), 0);
	CHECK_EQ(osc2.led.red, 250);
	CHECK_EQ(osc2.led.blue, 101);
	CHECK_EQ(opensc2_chime_level_set(1), 0);
	CHECK_EQ(osc2.chime, 1);
	CHECK_EQ(stub.stored_chime, 1);
	stub.storage_error = -EIO;
	CHECK_EQ(opensc2_chime_level_set(0), -EIO);
	CHECK_EQ(osc2.chime, 1);
	stub.storage_error = 0;
	osc2.initialised = false;
}

/* opensc2_identity_set(): the configured identity changed in RAM, answered at once on the
 * command channel, a stored field still winning. */
OPENSC2_TEST(channel_identity_set)
{
	struct opensc2_identity id = { 0 };
	uint8_t reply[64];

	stub_reset();
	osc2.base = osc2.identity;
	id.board_revision = 0x4A;
	CHECK_EQ(opensc2_identity_set(&id), -EACCES);   /* before opensc2_init() */
	osc2.initialised = true;
	CHECK_EQ(opensc2_identity_set(NULL), -EINVAL);
	id.bootloader_build_time = 0x68D2F9F2;
	CHECK_EQ(opensc2_identity_set(&id), 0);
	CHECK_EQ(osc2.identity.board_revision, 0x4A);
	CHECK_EQ(osc2.identity.bootloader_build_time, 0x68D2F9F2);
	CHECK(memcmp(osc2.identity.unit_serial, "OSC0123456789", 13) == 0);   /* empty: kept */
	/* 0x83 answers the new values at once: tag 10 (bootloader), tag 9 (hardware ID). */
	CHECK_EQ(ch_set(0x83, NULL, 0, 0), 0);
	CHECK_EQ(ch_get(reply), 63);
	CHECK_EQ(reply[13], 10);
	CHECK_EQ(opensc2_get_le32(&reply[14]), 0x68D2F9F2);
	CHECK_EQ(reply[23], 9);
	CHECK_EQ(opensc2_get_le32(&reply[24]), 0x4A);
	/* An invalid result changes nothing. */
	memcpy(id.unit_serial, "bad serial", 10);
	CHECK_EQ(opensc2_identity_set(&id), -EINVAL);
	CHECK(memcmp(osc2.identity.unit_serial, "OSC0123456789", 13) == 0);
	/* A stored field wins over the RAM one. */
	memset(&id, 0, sizeof(id));
	id.board_revision = 0x45;
	osc2_id_override_set(&osc2.stored, OSC2_ID_REV, &id);
	id.board_revision = 0x4B;
	CHECK_EQ(opensc2_identity_set(&id), 0);
	CHECK_EQ(osc2.base.board_revision, 0x4B);
	CHECK_EQ(osc2.identity.board_revision, 0x45);
	osc2.initialised = false;
}
