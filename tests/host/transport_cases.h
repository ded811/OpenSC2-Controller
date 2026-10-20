/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the link plumbing and the wireless transport. The transport rules
 * (transport_rules.c) on their own; then commands.c with the stand-ins of stubs.c: the values 0 to
 * 3 of user/wireless_transport (0xED, 0xEE, 0xEF, 0xF0), attribute 11 of 0x83 per link, the
 * Bluetooth and dock links' stored requests, 0x81 and 0x85 per link, and the strict GET the
 * Bluetooth link uses. Included by run_tests.c after commands_cases.h (its helpers).
 */
#include "transport_rules.h"

/* A SET {opcode, n, payload} on any link, as a record of 63 bytes (the form USB and Bluetooth
 * hand over). */
static int32_t link_set(enum osc2_link link, uint8_t opcode, const void *payload, uint8_t n)
{
	uint8_t rec[63] = { opcode, n };

	if (n) {
		memcpy(&rec[2], payload, n);
	}
	return osc2_command_set(link, rec, sizeof(rec));
}

/* 0x83's attribute 11 (the last of the six {tag, u32} pairs) on `link`. */
static uint32_t attr11(enum osc2_link link)
{
	uint8_t out[63];

	CHECK_EQ(link_set(link, 0x83, NULL, 0), 0);
	CHECK_EQ(osc2_command_get_strict(link, out), 63);
	CHECK_EQ(out[0], 0x83);
	CHECK_EQ(out[1], 30);
	CHECK_EQ(out[2 + 25], 11);   /* the sixth tag */
	return opensc2_get_le32(&out[2 + 26]);
}

OPENSC2_TEST(transport_rules)
{
	char word[8];

	/* What each value is, and what each build can run. */
	CHECK(osc2_transport_is_ble(0) && osc2_transport_is_ble(1));
	CHECK(!osc2_transport_is_ble(2) && !osc2_transport_is_ble(3) && !osc2_transport_is_ble(4));
	CHECK(osc2_transport_is_puck(2) && osc2_transport_is_puck(3) && !osc2_transport_is_puck(1));
	CHECK(!osc2_transport_usable(0, false) && osc2_transport_usable(0, true));
	CHECK(osc2_transport_usable(3, false) && !osc2_transport_usable(4, true));
	CHECK_EQ(osc2_transport_puck_slot(2), 0);
	CHECK_EQ(osc2_transport_puck_slot(3), 1);
	CHECK_EQ(osc2_transport_puck_slot(1), -1);
	CHECK_EQ(osc2_transport_ble_slot(1), 1);
	CHECK_EQ(osc2_transport_ble_slot(2), -1);
	/* A host's 0xEE: 0 and 1 only with Bluetooth; anything above 3 changes nothing. */
	CHECK_EQ(osc2_transport_key_value(0, false), -1);
	CHECK_EQ(osc2_transport_key_value(1, true), 1);
	CHECK_EQ(osc2_transport_key_value(3, false), 3);
	CHECK_EQ(osc2_transport_key_value(4, true), -1);
	CHECK_EQ(osc2_transport_key_value(0xFF, true), -1);
	/* The boot choice: the stored one if usable, else an older firmware's slot, else 2. */
	CHECK_EQ(osc2_transport_at_boot(OSC2_TRANSPORT_NONE, 0, false), 2);
	CHECK_EQ(osc2_transport_at_boot(OSC2_TRANSPORT_NONE, 1, false), 3);
	CHECK_EQ(osc2_transport_at_boot(OSC2_TRANSPORT_NONE, -1, true), 2);
	CHECK_EQ(osc2_transport_at_boot(1, 0, true), 1);
	CHECK_EQ(osc2_transport_at_boot(1, 1, false), 3);   /* a Bluetooth choice without Bluetooth */
	CHECK_EQ(osc2_transport_at_boot(3, 0, false), 3);
	CHECK_EQ(osc2_transport_at_boot(7, -1, true), 2);   /* a stored value above 3: the default */
	/* October's rule: saved once the link is up, unless it is the stored one already. */
	CHECK(osc2_transport_save_due(2, OSC2_TRANSPORT_NONE));
	CHECK(!osc2_transport_save_due(2, 2));
	CHECK(osc2_transport_save_due(0, 2));
	CHECK(osc2_transport_save_due(3, 1));    /* back from Bluetooth slot 2 to the second puck */
	CHECK(!osc2_transport_save_due(OSC2_TRANSPORT_NONE, 2));
	CHECK(!osc2_transport_save_due(4, 2));   /* no such transport */
	/* Names and the shell's words. */
	CHECK(strcmp(osc2_transport_name(0), "Bluetooth 1") == 0);
	CHECK(strcmp(osc2_transport_name(3), "puck 2") == 0);
	CHECK(strcmp(osc2_transport_name(9), "?") == 0);
	CHECK(strcmp(osc2_transport_word(4), "?") == 0);
	for (uint8_t t = 0; t <= 3; t++) {
		strcpy(word, osc2_transport_word(t));
		CHECK_EQ(osc2_transport_from_word(word), t);
	}
	CHECK_EQ(osc2_transport_from_word("bluetooth"), -1);
}

/* The key in a build without Bluetooth (the host tests' build): 2 and 3 select the puck's slot in
 * RAM, 0 and 1 are taken and change nothing (as before), 0xEF stores, 0xF0 deletes. */
OPENSC2_TEST(transport_key_without_ble)
{
	uint8_t reply[64];
	uint8_t named[40];
	size_t key_len = sizeof(key_transport);

	stub_reset();
	CHECK_EQ(usb_set(0xED, key_transport, (uint8_t)key_len), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[2], 1);
	CHECK_EQ(reply[3], 2);   /* the default: the puck with bond 1 */

	memcpy(named, key_transport, key_len);
	named[key_len] = 3;
	CHECK_EQ(usb_set(0xEE, named, (uint8_t)(key_len + 1)), 0);
	CHECK_EQ(osc2.transport, 3);
	CHECK_EQ(osc2.slot, 1);
	CHECK(stub.radio_events & OSC2_EV_BOND);
	CHECK_EQ(stub.saved_transport, 0);   /* RAM only */
	named[key_len] = 1;   /* Bluetooth: no Bluetooth in this build */
	CHECK_EQ(usb_set(0xEE, named, (uint8_t)(key_len + 1)), 0);
	CHECK_EQ(osc2.transport, 3);
	named[key_len] = 7;   /* above 3 */
	CHECK_EQ(usb_set(0xEE, named, (uint8_t)(key_len + 1)), 0);
	CHECK_EQ(osc2.transport, 3);
	CHECK(usb_set(0xEE, named, (uint8_t)key_len) < 0);   /* no value byte */
	CHECK_EQ(usb_set(0xED, key_transport, (uint8_t)key_len), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[3], 3);

	/* 0xEF stores the choice in RAM; 0xF0 deletes it and goes back to the default. */
	CHECK_EQ(usb_set(0xEF, key_transport, (uint8_t)key_len), 0);
	CHECK_EQ(stub.saved_transport, 1);
	CHECK_EQ(stub.stored_transport, 3);
	CHECK_EQ(usb_set(0xF0, key_transport, (uint8_t)key_len), 0);
	CHECK_EQ(stub.deleted_transport, 1);
	CHECK_EQ(osc2.transport, 2);
	CHECK_EQ(osc2.slot, 0);
	CHECK_EQ(stub.stored_transport, OSC2_TRANSPORT_NONE);

	/* The factory reset forgets the choice too. */
	osc2.transport = 3;
	osc2.slot = 1;
	CHECK_EQ(usb_set(0x86, (const uint8_t[]){ 0x17, 0xA9, 0xC1, 0xEF }, 4), 0);
	CHECK_EQ(osc2.transport, 2);
	CHECK_EQ(osc2.slot, 0);
}

/* Attribute 11 follows the link (CMD-3): 4000 on USB and the radio, the connection interval on
 * Bluetooth once it is active (0 before), 64000 on the dock. */
OPENSC2_TEST(transport_attribute_11)
{
	stub_reset();
	CHECK_EQ(attr11(OSC2_LINK_USB), 4000);
	CHECK_EQ(attr11(OSC2_LINK_RADIO), 4000);
	CHECK_EQ(attr11(OSC2_LINK_DOCK), 64000);
	stub.ble_interval_us = 7500;
	CHECK_EQ(attr11(OSC2_LINK_BLE), 0);   /* not past its gate */
	stub.link_active[OSC2_LINK_BLE] = true;
	CHECK_EQ(attr11(OSC2_LINK_BLE), 7500);
	stub.ble_interval_us = 15000;
	CHECK_EQ(attr11(OSC2_LINK_BLE), 15000);
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	/* The test link reports its mode's real link: USB while wired, else the running one. */
	CHECK_EQ(attr11(OSC2_LINK_TEST), 4000);
	osc2.mode = OPENSC2_MODE_WIRELESS;
	stub.ble_running = true;
	CHECK_EQ(attr11(OSC2_LINK_TEST), 15000);
	stub.link_active[OSC2_LINK_BLE] = false;   /* connected, not past its gate yet */
	CHECK_EQ(attr11(OSC2_LINK_TEST), 0);
	stub.link_active[OSC2_LINK_BLE] = true;
	stub.ble_running = false;
	CHECK_EQ(attr11(OSC2_LINK_TEST), 4000);
#endif
}

/* The Bluetooth and dock links keep stored requests of their own; 0x81 and 0x85 act only from an
 * active link (Bluetooth past its gate), never from the dock; the strict GET refuses a command
 * without an answer. */
OPENSC2_TEST(transport_links)
{
	uint8_t out[63];

	stub_reset();
	osc2.mode = OPENSC2_MODE_WIRELESS;
	stub.link_active[OSC2_LINK_USB] = false;
	/* Separate stored requests: a SET on Bluetooth leaves the radio's and the dock's alone. */
	CHECK_EQ(link_set(OSC2_LINK_BLE, 0xAE, (const uint8_t[]){ 3 }, 1), 0);
	CHECK_EQ(osc2_command_get_strict(OSC2_LINK_BLE, out), 63);
	CHECK_EQ(out[0], 0xAE);
	CHECK_EQ(out[1], 20);
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, out), 32);
	CHECK_EQ(out[0], 0x83);
	CHECK_EQ(osc2_command_get_strict(OSC2_LINK_DOCK, out), 63);
	CHECK_EQ(out[0], 0x83);
	/* A command without an answer: the strict GET fails (an ATT error on Bluetooth). */
	CHECK_EQ(link_set(OSC2_LINK_BLE, 0x8E, NULL, 0), 0);
	CHECK(osc2_command_get_strict(OSC2_LINK_BLE, out) < 0);
	/* 0x81: refused before the gate, taken after it; never from the dock. */
	CHECK(link_set(OSC2_LINK_BLE, 0x81, NULL, 0) < 0);
	CHECK(link_set(OSC2_LINK_BLE, 0x85, NULL, 0) < 0);
	stub.link_active[OSC2_LINK_BLE] = true;
	CHECK_EQ(link_set(OSC2_LINK_BLE, 0x81, NULL, 0), 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LIZARD_MODE], 0);
	stub.link_active[OSC2_LINK_DOCK] = true;   /* even so, the dock never counts */
	CHECK(link_set(OSC2_LINK_DOCK, 0x81, NULL, 0) < 0);
	CHECK(link_set(OSC2_LINK_DOCK, 0x85, NULL, 0) < 0);
	/* Every other command works on the dock link, 0x87 included. */
	CHECK_EQ(link_set(OSC2_LINK_DOCK, 0x87, (const uint8_t[]){ 45, 30, 0 }, 3), 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LED_BRIGHTNESS], 30);
}
