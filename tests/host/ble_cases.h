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

	/* The form: open without a bond or when a pairing is asked for. */
	CHECK_EQ(osc2_ble_adv_form(false, false), OSC2_BLE_ADV_PAIRING);
	CHECK_EQ(osc2_ble_adv_form(true, false), OSC2_BLE_ADV_ALLOW_LISTED);
	CHECK_EQ(osc2_ble_adv_form(true, true), OSC2_BLE_ADV_PAIRING);
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
