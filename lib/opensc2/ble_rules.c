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

enum osc2_ble_adv_form osc2_ble_adv_form(bool bonded, bool fallback)
{
	if (!bonded) {
		return OSC2_BLE_ADV_PAIRING;
	}
	return fallback ? OSC2_BLE_ADV_ALLOW_LISTED : OSC2_BLE_ADV_DIRECTED;
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

	*sr_len = 0;
	if (in->form == OSC2_BLE_ADV_DIRECTED || in->form == OSC2_BLE_ADV_NONE) {
		return 0;   /* directed advertising carries only the two addresses */
	}
	n = put_item(ad, n, OSC2_AD_FLAGS, &flags, 1);
	if (pairing) {
		n = put_item(ad, n, OSC2_AD_APPEARANCE, appearance, sizeof(appearance));
	}
	n = put_item(ad, n, OSC2_AD_TX_POWER, &tx, 1);
	if (pairing) {
		n = put_item(ad, n, OSC2_AD_UUID16_ALL, uuids, sizeof(uuids));
	}
	n = put_item(ad, n, OSC2_AD_MANUFACTURER, company, sizeof(company));

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

/* ---- identities and bonds ------------------------------------------------------------------- */

uint8_t osc2_ble_slot_first_id(uint8_t slot)
{
	return (uint8_t)(1 + 2 * (slot ? 1 : 0));
}

int osc2_ble_id_slot(uint8_t id)
{
	if (id == 0 || id >= OSC2_BLE_ID_COUNT) {
		return -1;
	}
	return (id - 1) / 2;
}

uint8_t osc2_ble_other_id(uint8_t id)
{
	int slot = osc2_ble_id_slot(id);

	if (slot < 0) {
		return 0;
	}
	return id == osc2_ble_slot_first_id((uint8_t)slot) ? (uint8_t)(id + 1) : (uint8_t)(id - 1);
}

uint8_t osc2_ble_pick_id(uint8_t slot, bool first_bonded, bool second_bonded, bool pair_request,
			 bool *reset)
{
	uint8_t first = osc2_ble_slot_first_id(slot);
	uint8_t usual = first_bonded || !second_bonded ? first : (uint8_t)(first + 1);

	*reset = pair_request;
	return pair_request ? osc2_ble_other_id(usual) : usual;
}

bool osc2_ble_new_host(bool bonded_at_start, uint8_t security_level)
{
	return !bonded_at_start && security_level >= 2;
}

enum osc2_ble_after osc2_ble_after_disconnect(uint8_t reason, bool ended_here)
{
	if (ended_here) {
		return OSC2_BLE_AFTER_NOTHING;
	}
	if (reason == OSC2_HCI_REMOTE_USER_TERM || reason == OSC2_HCI_REMOTE_POWER_OFF) {
		return OSC2_BLE_AFTER_WIRELESS_OFF;
	}
	return OSC2_BLE_AFTER_READVERTISE;
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

/* ---- Valve's vendor service ------------------------------------------------------------------- */

void osc2_ble_vendor_uuid(uint8_t xx, uint8_t out[16])
{
	/* 100F6Cxx-1735-4313-B402-38567131E5F3, least significant byte first */
	static const uint8_t base[16] = {
		0xF3, 0xE5, 0x31, 0x71, 0x56, 0x38, 0x02, 0xB4,
		0x13, 0x43, 0x35, 0x17, 0x00, 0x6C, 0x0F, 0x10,
	};

	memcpy(out, base, sizeof(base));
	out[12] = xx;
}

uint8_t osc2_ble_vendor_xx(const struct osc2_hid_report *r)
{
	if (r->type == OSC2_HID_FEATURE) {
		return OSC2_BLE_VENDOR_COMMAND;
	}
	return (uint8_t)(OSC2_BLE_VENDOR_OFFSET + r->id);
}

enum osc2_ble_target osc2_ble_input_target(bool vendor_subscribed, bool hid_subscribed)
{
	if (vendor_subscribed) {
		return OSC2_BLE_TARGET_VENDOR;
	}
	return hid_subscribed ? OSC2_BLE_TARGET_HID : OSC2_BLE_TARGET_NONE;
}

/* ---- what the link is doing ------------------------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(&p[2], (uint16_t)(v >> 16));
}

void osc2_ble_pack_status(const struct osc2_ble_status *st, uint8_t out[OSC2_BLE_STATUS_LEN])
{
	out[0] = (uint8_t)((st->enabled ? 0x01 : 0) | (st->advertising ? 0x02 : 0) |
			   (st->connected ? 0x04 : 0) | (st->active ? 0x08 : 0) |
			   (st->trigger ? 0x10 : 0) | (st->running ? 0x20 : 0) |
			   (st->pair_request ? 0x40 : 0) | (st->wireless_off ? 0x80 : 0));
	out[1] = st->form;
	out[2] = st->slot;
	out[3] = st->id;
	out[4] = st->bonded;
	out[5] = st->identities;
	out[6] = st->security;
	out[7] = st->last_reason;
	out[8] = st->state;
	out[9] = st->reason;
	put16(&out[10], st->mtu);
	put32(&out[12], st->interval_us);
	put32(&out[16], st->window_left_ms);
	put32(&out[20], st->reports);
	put32(&out[24], st->dropped);
}

/* ---- timing ------------------------------------------------------------------------------------- */

uint32_t osc2_ble_interval_us_from_units(uint16_t units)
{
	return (uint32_t)units * 1250u;
}

bool osc2_ble_vendor_flag(int index, bool vendor_subscribed, uint8_t radio_reason)
{
	if (index == OSC2_BLE_STATE_ID - 0x40) {
		return (radio_reason & 1u) != 0;
	}
	return vendor_subscribed;
}

uint8_t osc2_ble_vendor_state_ccc(uint8_t radio_reason, bool on)
{
	return on ? (uint8_t)(radio_reason | 1u) : (uint8_t)(radio_reason & ~1u);
}

size_t osc2_ble_feature_read(const uint8_t value[OSC2_BLE_FEATURE_LEN], uint16_t offset,
			     uint16_t len, const uint8_t *request, uint8_t request_len,
			     const uint8_t *answer, uint8_t out[OSC2_BLE_FEATURE_LEN])
{
	size_t n = offset < OSC2_BLE_FEATURE_LEN ? OSC2_BLE_FEATURE_LEN - offset : 0;

	memset(out, 0, OSC2_BLE_FEATURE_LEN);
	memcpy(out, &value[offset < OSC2_BLE_FEATURE_LEN ? offset : 0], n);
	if (request_len > OSC2_BLE_FEATURE_LEN) {
		request_len = OSC2_BLE_FEATURE_LEN;
	}
	memcpy(out, request, request_len);
	out[1] = answer[1];
	memcpy(&out[2], &answer[2], osc2_ble_get_span(answer[0], answer[1]));
	return n < len ? n : len;
}

uint8_t osc2_ble_get_span(uint8_t opcode, uint8_t n)
{
	uint8_t span = n;

	if (opcode == 0xAE && n == 20) {
		span = 21;   /* the tag, then 20 characters: one byte more than it counts */
	}
	return span > 61 ? 61 : span;
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

void osc2_ble_margin_init(struct osc2_ble_margin *m, uint32_t initial_ticks)
{
	m->peak = 0;
	m->worst = initial_ticks;
	m->frames = 0;
}

uint32_t osc2_ble_margin_initial_ticks(uint32_t ticks_per_s)
{
	return (uint32_t)((uint64_t)OSC2_BLE_MARGIN_INITIAL_32K * ticks_per_s / 32768u);
}

uint32_t osc2_ble_margin(const struct osc2_ble_margin *m)
{
	return m->peak > m->worst ? m->peak : m->worst;
}

void osc2_ble_margin_add(struct osc2_ble_margin *m, uint32_t ticks)
{
	if (m->peak < ticks) {
		m->peak = ticks;
	}
	if (++m->frames == OSC2_BLE_MARGIN_FRAMES) {
		m->worst = m->peak;
		m->peak = 0;
		m->frames = 0;
	}
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
