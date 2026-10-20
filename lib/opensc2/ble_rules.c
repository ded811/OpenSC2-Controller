/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * ble_rules.c: the pure rules of the Bluetooth LE link (ble_rules.h).
 */
#include <string.h>
#include "ble_rules.h"

/* ---- the name ------------------------------------------------------------------------------- */

size_t osc2_ble_name(const char *prefix, const char *serial, char *out, size_t size)
{
	size_t n = 0;

	if (size == 0) {
		return 0;
	}
	for (const char *p = prefix; *p && n + 1 < size; p++) {
		out[n++] = *p;
	}
	for (const char *p = serial; *p && n + 1 < size; p++) {
		out[n++] = *p;
	}
	out[n] = '\0';
	return n;
}

/* ---- advertising ------------------------------------------------------------------------------ */

enum osc2_ble_adv_form osc2_ble_adv_form(bool bonded, bool pairing_request)
{
	return bonded && !pairing_request ? OSC2_BLE_ADV_ALLOW_LISTED : OSC2_BLE_ADV_PAIRING;
}

/* Append {len + 1, type, data}; returns the new length (unchanged when it does not fit). */
static size_t put_item(uint8_t *buf, size_t at, uint8_t type, const uint8_t *data, size_t len)
{
	if (at + 2 + len > OSC2_BLE_AD_MAX) {
		return at;
	}
	buf[at] = (uint8_t)(len + 1);
	buf[at + 1] = type;
	memcpy(&buf[at + 2], data, len);
	return at + 2 + len;
}

size_t osc2_ble_adv_data(const struct osc2_ble_adv_in *in, uint8_t ad[OSC2_BLE_AD_MAX],
			 uint8_t sr[OSC2_BLE_AD_MAX], size_t *sr_len)
{
	bool pairing = in->form == OSC2_BLE_ADV_PAIRING;
	uint8_t flags = OSC2_AD_FLAG_NO_BREDR | (pairing ? OSC2_AD_FLAG_GENERAL : 0);
	uint8_t appearance[2] = { (uint8_t)in->appearance, (uint8_t)(in->appearance >> 8) };
	uint8_t tx = (uint8_t)in->tx_power_dbm;
	uint8_t uuids[4] = { (uint8_t)OSC2_BLE_UUID_HID, (uint8_t)(OSC2_BLE_UUID_HID >> 8),
			     (uint8_t)OSC2_BLE_UUID_BATTERY, (uint8_t)(OSC2_BLE_UUID_BATTERY >> 8) };
	uint8_t company[2] = { (uint8_t)in->company_id, (uint8_t)(in->company_id >> 8) };
	size_t n = 0;

	n = put_item(ad, n, OSC2_AD_FLAGS, &flags, 1);
	if (pairing) {
		n = put_item(ad, n, OSC2_AD_APPEARANCE, appearance, sizeof(appearance));
	}
	n = put_item(ad, n, OSC2_AD_TX_POWER, &tx, 1);
	if (pairing) {
		n = put_item(ad, n, OSC2_AD_UUID16_ALL, uuids, sizeof(uuids));
	}
	n = put_item(ad, n, OSC2_AD_MANUFACTURER, company, sizeof(company));

	*sr_len = 0;
	if (pairing && in->name && in->name[0]) {
		size_t len = strlen(in->name);

		*sr_len = put_item(sr, 0,
				   len > OSC2_BLE_NAME_MAX ? OSC2_AD_NAME_SHORT : OSC2_AD_NAME_COMPLETE,
				   (const uint8_t *)in->name,
				   len > OSC2_BLE_NAME_MAX ? OSC2_BLE_NAME_MAX : len);
	}
	return n;
}

bool osc2_ble_ad_next(const uint8_t *buf, size_t buf_len, size_t *pos, uint8_t *type,
		      const uint8_t **data, uint8_t *len)
{
	size_t at = *pos;

	if (at >= buf_len || buf[at] == 0 || at + 1 + buf[at] > buf_len) {
		return false;
	}
	*type = buf[at + 1];
	*data = &buf[at + 2];
	*len = (uint8_t)(buf[at] - 1);
	*pos = at + 1 + buf[at];
	return true;
}

/* ---- the HID service's reports ------------------------------------------------------------------ */

const struct osc2_hid_report osc2_ble_reports[OSC2_BLE_REPORTS] = {
	{ 0x40, OSC2_HID_INPUT, 5 },    /* lizard mode's mouse */
	{ 0x41, OSC2_HID_INPUT, 8 },    /* lizard mode's keyboard */
	{ 0x42, OSC2_HID_INPUT, 53 },   /* the state, never notified: it goes out as 0x45 */
	{ 0x43, OSC2_HID_INPUT, 14 },   /* battery */
	{ 0x44, OSC2_HID_INPUT, 5 },    /* haptic stream status */
	{ 0x45, OSC2_HID_INPUT, 45 },   /* the state without the quaternion */
	{ 0x80, OSC2_HID_OUTPUT, 9 },   /* rumble */
	{ 0x81, OSC2_HID_OUTPUT, 7 },   /* pulse */
	{ 0x82, OSC2_HID_OUTPUT, 3 },   /* command */
	{ 0x83, OSC2_HID_OUTPUT, 9 },   /* tone */
	{ 0x84, OSC2_HID_OUTPUT, 8 },   /* sweep */
	{ 0x85, OSC2_HID_OUTPUT, 3 },   /* script */
	{ 0x86, OSC2_HID_OUTPUT, 3 },   /* PCM stream control */
	{ 0x87, OSC2_HID_OUTPUT, 63 },  /* PCM data */
	{ 0x88, OSC2_HID_OUTPUT, 63 },  /* PCM data for both grips */
	{ 0x89, OSC2_HID_OUTPUT, 63 },  /* PCM data with a length */
	{ 0x01, OSC2_HID_FEATURE, 63 }, /* the vendor command channel */
};

int osc2_ble_report_index(uint8_t id, uint8_t type)
{
	for (int i = 0; i < OSC2_BLE_REPORTS; i++) {
		if (osc2_ble_reports[i].id == id && osc2_ble_reports[i].type == type) {
			return i;
		}
	}
	return -1;
}

/* Add `bits` to report `id` of `type` in out[0..*n). */
static int add_bits(struct osc2_hid_report *out, uint32_t bits_out[], int *n, int max, uint8_t id,
		    uint8_t type, uint32_t bits)
{
	for (int i = 0; i < *n; i++) {
		if (out[i].id == id && out[i].type == type) {
			bits_out[i] += bits;
			return 0;
		}
	}
	if (*n >= max) {
		return -1;
	}
	out[*n].id = id;
	out[*n].type = type;
	bits_out[*n] = bits;
	(*n)++;
	return 0;
}

int osc2_hid_map_reports(const uint8_t *map, size_t len, struct osc2_hid_report *out, int max)
{
	uint32_t bits[32] = { 0 };
	uint32_t report_size = 0, report_count = 0;
	uint8_t id = 0;
	int n = 0;
	size_t at = 0;

	if (max > (int)(sizeof(bits) / sizeof(bits[0]))) {
		max = (int)(sizeof(bits) / sizeof(bits[0]));
	}
	while (at < len) {
		uint8_t prefix = map[at];
		uint8_t size = prefix & 0x03;
		uint32_t data = 0;

		if (prefix == 0xFE) {
			return -1;   /* a long item */
		}
		size = size == 3 ? 4 : size;
		if (at + 1 + size > len) {
			return -1;
		}
		for (uint8_t i = 0; i < size; i++) {
			data |= (uint32_t)map[at + 1 + i] << (8 * i);
		}
		switch (prefix & 0xFC) {
		case 0x84:   /* Report ID */
			id = (uint8_t)data;
			break;
		case 0x74:   /* Report Size */
			report_size = data;
			break;
		case 0x94:   /* Report Count */
			report_count = data;
			break;
		case 0x80:   /* Input */
		case 0x90:   /* Output */
		case 0xB0: { /* Feature */
			uint8_t type = (prefix & 0xFC) == 0x80 ? 1 : (prefix & 0xFC) == 0x90 ? 2 : 3;

			if (add_bits(out, bits, &n, max, id, type, report_size * report_count)) {
				return -1;
			}
			break;
		}
		default:
			break;
		}
		at += 1 + size;
	}
	for (int i = 0; i < n; i++) {
		uint32_t bytes = (bits[i] + 7) / 8;

		if (bytes > 255) {
			return -1;
		}
		out[i].size = (uint8_t)bytes;
	}
	return n;
}

/* ---- the link -------------------------------------------------------------------------------- */

bool osc2_ble_gate(uint8_t security_level, uint16_t mtu)
{
	return security_level >= 2 && mtu >= OSC2_BLE_GATE_MTU;
}

size_t osc2_ble_state_value(const uint8_t *report, size_t len, const uint8_t **value)
{
	if (len != 54 || report[0] != 0x42) {
		return 0;
	}
	*value = &report[1];
	return OSC2_BLE_STATE_LEN;
}

int osc2_ble_input_value(const uint8_t *report, size_t len, uint8_t pad[64],
			 const uint8_t **value, uint8_t *value_len)
{
	uint8_t id;
	int index;
	size_t have;

	if (len < 1) {
		return -1;
	}
	id = report[0] == 0x42 ? OSC2_BLE_STATE_ID : report[0];
	index = osc2_ble_report_index(id, OSC2_HID_INPUT);
	if (index < 0) {
		return -1;
	}
	*value_len = osc2_ble_reports[index].size;
	have = len - 1;
	if (have >= *value_len) {
		*value = &report[1];   /* a longer one (0x42 among them) is cut */
	} else {
		memset(pad, 0, 64);
		memcpy(pad, &report[1], have);
		*value = pad;
	}
	return index;
}

uint8_t osc2_ble_write_check(uint16_t offset, uint16_t len, uint16_t size)
{
	return (uint32_t)offset + len > size ? OSC2_ATT_ERR_INVALID_OFFSET : 0;
}

/* ---- timing ------------------------------------------------------------------------------------- */

uint32_t osc2_ble_interval_us_from_units(uint16_t units)
{
	return (uint32_t)units * 1250u;
}

bool osc2_ble_interval_suboptimal(uint32_t interval_us)
{
	return interval_us > OSC2_BLE_INTERVAL_WANTED_US;
}

int64_t osc2_ble_slot_ticks(int64_t event_ticks, uint32_t interval_us, uint32_t lead_us,
			    uint32_t ticks_per_s)
{
	uint32_t us = interval_us > lead_us ? interval_us - lead_us : 0;

	return event_ticks + (int64_t)((uint64_t)us * ticks_per_s / 1000000u);
}

/* ---- device information ---------------------------------------------------------------------- */

void osc2_ble_hex32(uint32_t value, char out[9])
{
	static const char digits[] = "0123456789abcdef";

	for (int i = 7; i >= 0; i--) {
		out[i] = digits[value & 0xF];
		value >>= 4;
	}
	out[8] = '\0';
}
