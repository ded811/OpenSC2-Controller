/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the pure rules of the Bluetooth LE link (ble_rules.c): the name, the advertising
 * data and scan response byte for byte, the HID service's report table against the report map the
 * USB device uses too, the link-up gate, the cut to 0x45, the checks of written values, the
 * pacing and the revision strings. Included by run_tests.c.
 */
#include "ble_rules.h"

static const char test_serial[] = "OSC0123456789";

OPENSC2_TEST(ble_name)
{
	char name[32];

	CHECK_EQ(osc2_ble_name("Steam Ctrl (BT) ", test_serial, name, sizeof(name)), 29);
	CHECK(strcmp(name, "Steam Ctrl (BT) OSC0123456789") == 0);
	/* Cut to the buffer. */
	CHECK_EQ(osc2_ble_name("Steam Ctrl (BT) ", test_serial, name, 8), 7);
	CHECK(strcmp(name, "Steam C") == 0);
	CHECK_EQ(osc2_ble_name("", "", name, sizeof(name)), 0);
}

OPENSC2_TEST(ble_advertising_data)
{
	static const uint8_t pairing_ad[20] = {
		0x02, 0x01, 0x06,                     /* flags: general, no BR/EDR */
		0x03, 0x19, 0xC4, 0x03,               /* appearance: gamepad */
		0x02, 0x0A, 0x06,                     /* TX power: +6 dBm */
		0x05, 0x03, 0x12, 0x18, 0x0F, 0x18,   /* HID, battery */
		0x03, 0xFF, 0x5D, 0x05,               /* company 0x055D, no payload */
	};
	static const uint8_t allow_ad[10] = {
		0x02, 0x01, 0x04, 0x02, 0x0A, 0x06, 0x03, 0xFF, 0x5D, 0x05,
	};
	char name[32];
	struct osc2_ble_adv_in in = {
		.form = OSC2_BLE_ADV_PAIRING,
		.tx_power_dbm = 6,
		.appearance = OSC2_BLE_APPEARANCE,
		.company_id = 0x055D,
		.name = name,
	};
	uint8_t ad[OSC2_BLE_AD_MAX], sr[OSC2_BLE_AD_MAX];
	size_t sr_len = 99;
	size_t pos = 0;
	uint8_t type, len;
	const uint8_t *data;

	osc2_ble_name("Steam Ctrl (BT) ", test_serial, name, sizeof(name));
	CHECK_EQ(osc2_ble_adv_data(&in, ad, sr, &sr_len), 20);
	CHECK(memcmp(ad, pairing_ad, sizeof(pairing_ad)) == 0);
	/* The scan response is exactly full: 1E 09 and the 29 characters. */
	CHECK_EQ(sr_len, 31);
	CHECK_EQ(sr[0], 0x1E);
	CHECK_EQ(sr[1], OSC2_AD_NAME_COMPLETE);
	CHECK(memcmp(&sr[2], name, 29) == 0);
	/* Walking the items. */
	CHECK(osc2_ble_ad_next(ad, 20, &pos, &type, &data, &len));
	CHECK_EQ(type, OSC2_AD_FLAGS);
	CHECK_EQ(len, 1);
	CHECK_EQ(data[0], 0x06);
	for (int i = 0; i < 4; i++) {
		CHECK(osc2_ble_ad_next(ad, 20, &pos, &type, &data, &len));
	}
	CHECK_EQ(type, OSC2_AD_MANUFACTURER);
	CHECK(!osc2_ble_ad_next(ad, 20, &pos, &type, &data, &len));
	CHECK(!osc2_ble_ad_next((const uint8_t[]){ 0x05, 0x09, 'a' }, 3, &(size_t){ 0 }, &type,
				&data, &len));   /* runs past the end */

	/* Allow-listed: no appearance, no UUIDs, flags 04, an empty scan response. */
	in.form = OSC2_BLE_ADV_ALLOW_LISTED;
	CHECK_EQ(osc2_ble_adv_data(&in, ad, sr, &sr_len), 10);
	CHECK(memcmp(ad, allow_ad, sizeof(allow_ad)) == 0);
	CHECK_EQ(sr_len, 0);

	/* Another TX power and company ID; a name too long for the scan response is cut and marked
	 * short. */
	in.form = OSC2_BLE_ADV_PAIRING;
	in.tx_power_dbm = -4;
	in.company_id = 0xFFFF;
	in.name = "A longer device name prefix 0123456789";
	CHECK_EQ(osc2_ble_adv_data(&in, ad, sr, &sr_len), 20);
	CHECK_EQ(ad[9], 0xFC);
	CHECK_EQ(ad[18], 0xFF);
	CHECK_EQ(ad[19], 0xFF);
	CHECK_EQ(sr_len, 31);
	CHECK_EQ(sr[1], OSC2_AD_NAME_SHORT);
	CHECK(memcmp(&sr[2], in.name, 29) == 0);

	/* Directed advertising carries no data and no scan response. */
	in.form = OSC2_BLE_ADV_DIRECTED;
	sr_len = 99;
	CHECK_EQ(osc2_ble_adv_data(&in, ad, sr, &sr_len), 0);
	CHECK_EQ(sr_len, 0);

	/* The form: pairing mode without a bond; with one, directed first, allow-listed after the
	 * directed try (or a failed connection). */
	CHECK_EQ(osc2_ble_adv_form(false, false), OSC2_BLE_ADV_PAIRING);
	CHECK_EQ(osc2_ble_adv_form(false, true), OSC2_BLE_ADV_PAIRING);
	CHECK_EQ(osc2_ble_adv_form(true, false), OSC2_BLE_ADV_DIRECTED);
	CHECK_EQ(osc2_ble_adv_form(true, true), OSC2_BLE_ADV_ALLOW_LISTED);
}

/* Five identities, two per slot; which one a start advertises on; the bond rule. */
OPENSC2_TEST(ble_identities_and_bonds)
{
	/* What the slot gets without a pairing request: the first identity if it has a bond or
	 * neither has one, otherwise the second; with one, the other one, reset first. */
	static const struct {
		uint8_t slot;
		bool first, second, pair;
		uint8_t id;
		bool reset;
	} pick[] = {
		{ 0, false, false, false, 1, false },   /* a slot never paired: identity 1, open */
		{ 0, true, false, false, 1, false },
		{ 0, false, true, false, 2, false },
		{ 0, true, true, false, 1, false },     /* both (after an interrupted rule): the first */
		{ 0, false, false, true, 2, true },     /* a pairing request: the other one, reset */
		{ 0, true, false, true, 2, true },      /* the bond on 1 stays until a host pairs on 2 */
		{ 0, false, true, true, 1, true },
		{ 1, false, false, false, 3, false },
		{ 1, true, false, false, 3, false },
		{ 1, false, true, false, 4, false },
		{ 1, true, false, true, 4, true },
		{ 1, false, true, true, 3, true },
	};

	CHECK_EQ(osc2_ble_slot_first_id(0), 1);
	CHECK_EQ(osc2_ble_slot_first_id(1), 3);
	CHECK_EQ(osc2_ble_id_slot(0), -1);         /* identity 0 never advertises */
	CHECK_EQ(osc2_ble_id_slot(1), 0);
	CHECK_EQ(osc2_ble_id_slot(2), 0);
	CHECK_EQ(osc2_ble_id_slot(3), 1);
	CHECK_EQ(osc2_ble_id_slot(4), 1);
	CHECK_EQ(osc2_ble_id_slot(5), -1);
	CHECK_EQ(osc2_ble_other_id(1), 2);
	CHECK_EQ(osc2_ble_other_id(2), 1);
	CHECK_EQ(osc2_ble_other_id(3), 4);
	CHECK_EQ(osc2_ble_other_id(4), 3);
	CHECK_EQ(osc2_ble_other_id(0), 0);
	for (size_t i = 0; i < sizeof(pick) / sizeof(pick[0]); i++) {
		bool reset = !pick[i].reset;

		CHECK_EQ(osc2_ble_pick_id(pick[i].slot, pick[i].first, pick[i].second, pick[i].pair,
					  &reset), pick[i].id);
		CHECK_EQ(reset, pick[i].reset);
	}

	/* A new host: bonded on an identity without a bond at the start of its advertising, once
	 * encrypted. A host that was bonded already, or did not bond, is none. */
	CHECK(osc2_ble_new_host(false, true, 2));
	CHECK(osc2_ble_new_host(false, true, 4));
	CHECK(!osc2_ble_new_host(false, true, 1));
	CHECK(!osc2_ble_new_host(true, true, 2));
	CHECK(!osc2_ble_new_host(false, false, 2));
}

/* What follows a disconnect, and the timings of advertising. */
OPENSC2_TEST(ble_disconnect_rules)
{
	CHECK_EQ(osc2_ble_after_disconnect(OSC2_HCI_REMOTE_USER_TERM, false),
		 OSC2_BLE_AFTER_WIRELESS_OFF);
	CHECK_EQ(osc2_ble_after_disconnect(OSC2_HCI_REMOTE_POWER_OFF, false),
		 OSC2_BLE_AFTER_WIRELESS_OFF);
	CHECK_EQ(osc2_ble_after_disconnect(OSC2_HCI_CONN_TIMEOUT, false), OSC2_BLE_AFTER_READVERTISE);
	CHECK_EQ(osc2_ble_after_disconnect(0x14, false), OSC2_BLE_AFTER_READVERTISE);
	CHECK_EQ(osc2_ble_after_disconnect(0x3E, false), OSC2_BLE_AFTER_READVERTISE);
	CHECK_EQ(osc2_ble_after_disconnect(OSC2_HCI_LOCAL_TERM, false), OSC2_BLE_AFTER_READVERTISE);
	/* The controller's own ends, to turn wireless off or to switch, lead to nothing. */
	CHECK_EQ(osc2_ble_after_disconnect(OSC2_HCI_REMOTE_USER_TERM, true), OSC2_BLE_AFTER_NOTHING);
	CHECK_EQ(osc2_ble_after_disconnect(OSC2_HCI_CONN_TIMEOUT, true), OSC2_BLE_AFTER_NOTHING);
	CHECK_EQ(OSC2_BLE_ADV_WINDOW_MS, 60000);
	CHECK_EQ(OSC2_BLE_READVERTISE_MS, 500);
}

/* Valve's vendor service: its UUIDs on the air and the characteristic of every report; where a
 * report goes. */
OPENSC2_TEST(ble_vendor_service)
{
	static const uint8_t service[16] = {
		0xF3, 0xE5, 0x31, 0x71, 0x56, 0x38, 0x02, 0xB4,
		0x13, 0x43, 0x35, 0x17, 0x32, 0x6C, 0x0F, 0x10,
	};
	uint8_t uuid[16];

	osc2_ble_vendor_uuid(OSC2_BLE_VENDOR_SERVICE, uuid);
	CHECK(memcmp(uuid, service, sizeof(uuid)) == 0);
	osc2_ble_vendor_uuid(0x7A, uuid);
	CHECK_EQ(uuid[12], 0x7A);
	CHECK(memcmp(uuid, service, 12) == 0 && memcmp(&uuid[13], &service[13], 3) == 0);
	/* The command channel 0x34; inputs 0x75..0x7A; outputs 0xB5..0xBE. */
	CHECK_EQ(osc2_ble_vendor_xx(&osc2_ble_reports[16]), 0x34);
	for (int i = 0; i < OSC2_BLE_INPUTS; i++) {
		CHECK_EQ(osc2_ble_vendor_xx(&osc2_ble_reports[i]), 0x75 + i);
	}
	for (int j = 0; j < OSC2_BLE_OUTPUTS; j++) {
		CHECK_EQ(osc2_ble_vendor_xx(&osc2_ble_reports[OSC2_BLE_INPUTS + j]), 0xB5 + j);
	}
	/* SDL's state report characteristic is 0x45's: 100F6C7A. */
	CHECK_EQ(osc2_ble_vendor_xx(&osc2_ble_reports[osc2_ble_report_index(0x45, OSC2_HID_INPUT)]),
		 0x7A);
	/* The service, the command channel's two, three per input, two per output. */
	CHECK_EQ(OSC2_BLE_VENDOR_ATTRS, 1 + 2 + 3 * OSC2_BLE_INPUTS + 2 * OSC2_BLE_OUTPUTS);
	/* The vendor characteristic first, then the HID one, else dropped. */
	CHECK_EQ(osc2_ble_input_target(true, true), OSC2_BLE_TARGET_VENDOR);
	CHECK_EQ(osc2_ble_input_target(true, false), OSC2_BLE_TARGET_VENDOR);
	CHECK_EQ(osc2_ble_input_target(false, true), OSC2_BLE_TARGET_HID);
	CHECK_EQ(osc2_ble_input_target(false, false), OSC2_BLE_TARGET_NONE);
}

/* The test channel's BLE_STATUS answer, byte for byte. */
OPENSC2_TEST(ble_status_pack)
{
	struct osc2_ble_status st;
	uint8_t out[OSC2_BLE_STATUS_LEN];

	memset(&st, 0, sizeof(st));
	st.enabled = true;
	st.connected = true;
	st.active = true;
	st.running = true;
	st.wireless_off = false;
	st.form = OSC2_BLE_ADV_NONE;
	st.slot = 1;
	st.id = 3;
	st.bonded = 0x08;
	st.identities = 5;
	st.security = 2;
	st.last_reason = 0x08;
	st.state = 4;
	st.reason = 16;
	st.mtu = 67;
	st.interval_us = 7500;
	st.window_left_ms = 0x01020304;
	st.reports = 1000;
	st.dropped = 3;
	strcpy(st.name, "not in the answer");
	strcpy(st.address, "not in the answer");
	memset(out, 0xEE, sizeof(out));
	osc2_ble_pack_status(&st, out);
	CHECK_EQ(out[0], 0x01 | 0x04 | 0x08 | 0x20);
	CHECK_EQ(out[1], 0xFF);
	CHECK_EQ(out[2], 1);
	CHECK_EQ(out[3], 3);
	CHECK_EQ(out[4], 0x08);
	CHECK_EQ(out[5], 5);
	CHECK_EQ(out[6], 2);
	CHECK_EQ(out[7], 0x08);
	CHECK_EQ(out[8], 4);
	CHECK_EQ(out[9], 16);
	CHECK_EQ(opensc2_get_le16(&out[10]), 67);
	CHECK_EQ(opensc2_get_le32(&out[12]), 7500);
	CHECK_EQ(opensc2_get_le32(&out[16]), 0x01020304);
	CHECK_EQ(opensc2_get_le32(&out[20]), 1000);
	CHECK_EQ(opensc2_get_le32(&out[24]), 3);
	st.advertising = true;
	st.trigger = true;
	st.pair_request = true;
	st.wireless_off = true;
	osc2_ble_pack_status(&st, out);
	CHECK_EQ(out[0], 0xFF);
}

/* The service's report table is the report map's (the USB report descriptor, byte for byte): every
 * characteristic's ID, type and size as declared, and only 0x79, 0x7B and feature 0x02 without
 * one. */
OPENSC2_TEST(ble_report_table_matches_the_map)
{
	struct osc2_hid_report map[32];
	int n = osc2_hid_map_reports(opensc2_hid_report_desc, OPENSC2_HID_REPORT_DESC_LEN, map, 32);
	int without = 0;

	CHECK_EQ(n, OSC2_BLE_REPORTS + 3);
	for (int i = 0; i < OSC2_BLE_REPORTS; i++) {
		bool found = false;

		for (int j = 0; j < n; j++) {
			if (map[j].id == osc2_ble_reports[i].id &&
			    map[j].type == osc2_ble_reports[i].type) {
				CHECK_EQ(map[j].size, osc2_ble_reports[i].size);
				found = true;
			}
		}
		CHECK(found);
	}
	for (int j = 0; j < n; j++) {
		if (osc2_ble_report_index(map[j].id, map[j].type) < 0) {
			CHECK((map[j].id == 0x79 && map[j].type == OSC2_HID_INPUT && map[j].size == 1) ||
			      (map[j].id == 0x7B && map[j].type == OSC2_HID_INPUT &&
			       map[j].size == 12) ||
			      (map[j].id == 0x02 && map[j].type == OSC2_HID_FEATURE &&
			       map[j].size == 63));
			without++;
		}
	}
	CHECK_EQ(without, 3);
	/* The mouse and keyboard are 5 and 8 bytes, padding included. */
	CHECK_EQ(map[0].id, 0x40);
	CHECK_EQ(map[0].size, 5);
	CHECK_EQ(map[1].id, 0x41);
	CHECK_EQ(map[1].size, 8);
	/* Indices, and what has no characteristic. */
	CHECK_EQ(osc2_ble_report_index(0x40, OSC2_HID_INPUT), 0);
	CHECK_EQ(osc2_ble_report_index(0x45, OSC2_HID_INPUT), 5);
	CHECK_EQ(osc2_ble_report_index(0x80, OSC2_HID_OUTPUT), 6);
	CHECK_EQ(osc2_ble_report_index(0x89, OSC2_HID_OUTPUT), 15);
	CHECK_EQ(osc2_ble_report_index(0x01, OSC2_HID_FEATURE), 16);
	CHECK_EQ(osc2_ble_report_index(0x8A, OSC2_HID_OUTPUT), -1);
	CHECK_EQ(osc2_ble_report_index(0x02, OSC2_HID_FEATURE), -1);
	/* A map this parser cannot read. */
	CHECK_EQ(osc2_hid_map_reports((const uint8_t[]){ 0xFE, 0x01, 0x00 }, 3, map, 32), -1);
	CHECK_EQ(osc2_hid_map_reports((const uint8_t[]){ 0x85 }, 1, map, 32), -1);
}

OPENSC2_TEST(ble_gate_and_values)
{
	uint8_t state[54], pad[64];
	const uint8_t *value;
	uint8_t value_len;

	/* Encrypted and an ATT MTU of 64 or more, in either order. */
	CHECK(!osc2_ble_gate(1, 247));
	CHECK(!osc2_ble_gate(2, 63));
	CHECK(!osc2_ble_gate(2, 23));
	CHECK(osc2_ble_gate(2, 64));
	CHECK(osc2_ble_gate(4, 67));

	/* 0x42 goes out as 0x45: its first 45 bytes after the ID. */
	for (int i = 0; i < 54; i++) {
		state[i] = (uint8_t)i;
	}
	state[0] = 0x42;
	CHECK_EQ(osc2_ble_state_value(state, 54, &value), 45);
	CHECK(value == &state[1]);
	CHECK_EQ(osc2_ble_state_value(state, 53, &value), 0);
	CHECK_EQ(osc2_ble_input_value(state, 54, pad, &value, &value_len), 5);
	CHECK_EQ(value_len, 45);
	CHECK(value == &state[1]);
	CHECK_EQ(value[44], 45);
	/* Other inputs at their size: a short one zero padded, an unknown one refused. */
	CHECK_EQ(osc2_ble_input_value((const uint8_t[]){ 0x40, 1, 2, 3, 4, 5 }, 6, pad, &value,
				      &value_len), 0);
	CHECK_EQ(value_len, 5);
	CHECK_EQ(value[4], 5);
	CHECK_EQ(osc2_ble_input_value((const uint8_t[]){ 0x43, 9, 9 }, 3, pad, &value, &value_len),
		 3);
	CHECK_EQ(value_len, 14);
	CHECK(value == pad);
	CHECK_EQ(value[1], 9);
	CHECK_EQ(value[2], 0);
	CHECK_EQ(osc2_ble_input_value((const uint8_t[]){ 0x79, 2 }, 2, pad, &value, &value_len), -1);
	CHECK_EQ(osc2_ble_input_value(state, 0, pad, &value, &value_len), -1);

	/* Writes: up to the value's size, from any offset; past it, invalid offset. */
	CHECK_EQ(osc2_ble_write_check(0, 64, OSC2_BLE_FEATURE_LEN), 0);
	CHECK_EQ(osc2_ble_write_check(0, 62, OSC2_BLE_FEATURE_LEN), 0);
	CHECK_EQ(osc2_ble_write_check(1, 64, OSC2_BLE_FEATURE_LEN), OSC2_ATT_ERR_INVALID_OFFSET);
	CHECK_EQ(osc2_ble_write_check(0, 65, OSC2_BLE_FEATURE_LEN), OSC2_ATT_ERR_INVALID_OFFSET);
	CHECK_EQ(osc2_ble_write_check(8, 1, 9), 0);
	CHECK_EQ(osc2_ble_write_check(9, 1, 9), OSC2_ATT_ERR_INVALID_OFFSET);
}

OPENSC2_TEST(ble_timing_and_strings)
{
	char hex[9];

	CHECK_EQ(osc2_ble_interval_us_from_units(6), 7500);
	CHECK_EQ(osc2_ble_interval_us_from_units(12), 15000);
	CHECK(!osc2_ble_interval_suboptimal(7500));
	CHECK(osc2_ble_interval_suboptimal(8750));
	/* One interval after the event, less the lead: 7 ms at 32,768 Hz is 229 ticks. */
	CHECK_EQ(osc2_ble_slot_ticks(1000, 7500, 500, 32768), 1229);
	CHECK_EQ(osc2_ble_slot_ticks(1000, 400, 500, 32768), 1000);   /* never before the event */
	CHECK_EQ(osc2_ble_slot_ticks(0, 15000, 500, 1000000), 14500);
	osc2_ble_hex32(0x6AC686B3u, hex);
	CHECK(strcmp(hex, "6ac686b3") == 0);
	osc2_ble_hex32(0x0000000Au, hex);
	CHECK(strcmp(hex, "0000000a") == 0);
}
