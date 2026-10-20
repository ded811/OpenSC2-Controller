/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * ble_rules.h: the parts of OpenSC2's Bluetooth LE link that are pure rules, so the host tests
 * build them as the firmware does: the device name, the advertising data and scan response, the
 * HID service's report table and the report map it is checked against, the link-up gate, the
 * state report's cut to 0x45, the checks of a written value, the report pacing and the text the
 * Device Information Service shows. No kernel, no Bluetooth headers.
 *
 * The values follow the protocol documentation's Bluetooth page (not public yet; docs/protocol.md
 * lists the pages).
 */
#ifndef OPENSC2_BLE_RULES_H_
#define OPENSC2_BLE_RULES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the name ---------------------------------------------------------------------------------- */

/* A legacy advertising PDU or scan response holds 31 bytes; a name item takes 2 of them. */
#define OSC2_BLE_AD_MAX        31
#define OSC2_BLE_NAME_MAX      29

/* The device name: `prefix` and the unit serial ("Steam Ctrl (BT) " and 13 characters: 29), cut
 * to `size` - 1 characters. Returns its length. */
size_t osc2_ble_name(const char *prefix, const char *serial, char *out, size_t size);

/* ---- advertising ------------------------------------------------------------------------------- */

/* AD types. */
#define OSC2_AD_FLAGS          0x01
#define OSC2_AD_UUID16_ALL     0x03
#define OSC2_AD_NAME_SHORT     0x08
#define OSC2_AD_NAME_COMPLETE  0x09
#define OSC2_AD_TX_POWER       0x0A
#define OSC2_AD_APPEARANCE     0x19
#define OSC2_AD_MANUFACTURER   0xFF

/* Flags: BR/EDR not supported, and LE General Discoverable in pairing mode only. */
#define OSC2_AD_FLAG_GENERAL   0x02
#define OSC2_AD_FLAG_NO_BREDR  0x04

#define OSC2_BLE_UUID_HID      0x1812
#define OSC2_BLE_UUID_BATTERY  0x180F
#define OSC2_BLE_APPEARANCE    0x03C4   /* gamepad */

/* The ways the controller advertises. The real controller first tries high-duty directed
 * advertising to its bonded host; that third form is not built yet. */
enum osc2_ble_adv_form {
	OSC2_BLE_ADV_PAIRING = 0,     /* open to any host, with the pairing-mode items */
	OSC2_BLE_ADV_ALLOW_LISTED,    /* only the bonded host may connect or scan */
};

/* The form for an identity: open while it has no bond or a pairing was asked for; otherwise only
 * its bonded host. */
enum osc2_ble_adv_form osc2_ble_adv_form(bool bonded, bool pairing_request);

struct osc2_ble_adv_in {
	enum osc2_ble_adv_form form;
	int8_t tx_power_dbm;      /* the advertiser's power, as the radio controller reports it */
	uint16_t appearance;
	uint16_t company_id;      /* manufacturer data with no payload */
	const char *name;         /* the scan response's name in pairing mode */
};

/* The advertising data in `ad` (flags, appearance, TX power, the HID and battery service UUIDs,
 * manufacturer data, in that order; appearance and UUIDs only in pairing mode) and the scan
 * response in `sr` (the name in pairing mode, complete if it fits in 29 characters, else cut and
 * marked short; empty otherwise). Returns the advertising data's length; `*sr_len` the scan
 * response's. Pairing mode gives 20 bytes, allow-listed 10. */
size_t osc2_ble_adv_data(const struct osc2_ble_adv_in *in, uint8_t ad[OSC2_BLE_AD_MAX],
			 uint8_t sr[OSC2_BLE_AD_MAX], size_t *sr_len);

/* Walk AD structures {length, type, data} in `buf`: the next one at `*pos` into `type`, `data`
 * and `len`. Returns false at the end or for a malformed one. */
bool osc2_ble_ad_next(const uint8_t *buf, size_t buf_len, size_t *pos, uint8_t *type,
		      const uint8_t **data, uint8_t *len);

/* ---- the HID service's reports ------------------------------------------------------------------ */

#define OSC2_HID_INPUT    1
#define OSC2_HID_OUTPUT   2
#define OSC2_HID_FEATURE  3

struct osc2_hid_report {
	uint8_t id;
	uint8_t type;     /* OSC2_HID_INPUT, _OUTPUT, _FEATURE (the Report Reference's type) */
	uint8_t size;     /* bytes after the ID, as the report map declares */
};

/* The reports that have a characteristic, in the service's order: inputs 0x40 to 0x45, outputs
 * 0x80 to 0x89, feature 0x01. The map also declares inputs 0x79 and 0x7B and feature 0x02, which
 * have none. */
#define OSC2_BLE_INPUTS     6
#define OSC2_BLE_OUTPUTS    10
#define OSC2_BLE_REPORTS    (OSC2_BLE_INPUTS + OSC2_BLE_OUTPUTS + 1)
extern const struct osc2_hid_report osc2_ble_reports[OSC2_BLE_REPORTS];

/* The index of report `id` of `type` in osc2_ble_reports, or -1. */
int osc2_ble_report_index(uint8_t id, uint8_t type);

/* The reports a HID report map declares, in the order they first appear: their ID, type and size
 * in bytes (the bits of every main item, padding included, rounded up). Returns how many, at most
 * `max`; -1 for a map this parser cannot read (a long item, a size over 255 bytes). */
int osc2_hid_map_reports(const uint8_t *map, size_t len, struct osc2_hid_report *out, int max);

/* ---- the link ------------------------------------------------------------------------------------ */

/* The October gate: reports flow once the link is encrypted (security level 2 or more) and its
 * ATT MTU is 64 or more (a 45-byte state report in one notification), in either order. */
#define OSC2_BLE_GATE_MTU    64
bool osc2_ble_gate(uint8_t security_level, uint16_t mtu);

/* The state report 0x42 (54 bytes with its ID) as Bluetooth sends it: report 0x45, the first 45
 * bytes after the ID (the quaternion is cut). Returns the value's length, 0 when `report` is not a
 * whole 0x42. */
#define OSC2_BLE_STATE_ID    0x45
#define OSC2_BLE_STATE_LEN   45
size_t osc2_ble_state_value(const uint8_t *report, size_t len, const uint8_t **value);

/* An input report on its way to a notification: the characteristic it goes to (an index of
 * osc2_ble_reports) and the value, exactly the declared size (0x42 cut to 0x45; a shorter one
 * zero padded into `pad`, a longer one cut). -1 for an ID with no input characteristic. */
int osc2_ble_input_value(const uint8_t *report, size_t len, uint8_t pad[64],
			 const uint8_t **value, uint8_t *value_len);

/* A write of `len` bytes at `offset` into a value of `size` bytes: 0, or the ATT error a longer
 * one gets (0x07, invalid offset). */
#define OSC2_ATT_ERR_INVALID_PDU      0x04
#define OSC2_ATT_ERR_INVALID_OFFSET   0x07
uint8_t osc2_ble_write_check(uint16_t offset, uint16_t len, uint16_t size);

/* The feature report's value: 64 bytes, one more than the map declares. A command is its first 63
 * bytes {opcode, length, payload}. */
#define OSC2_BLE_FEATURE_LEN 64

/* ---- timing --------------------------------------------------------------------------------- */

/* Connection intervals: 1.25 ms units to µs; the real controller asks for 7.5 ms and calls a
 * longer one suboptimal. */
#define OSC2_BLE_INTERVAL_WANTED_US 7500
uint32_t osc2_ble_interval_us_from_units(uint16_t units);
bool osc2_ble_interval_suboptimal(uint32_t interval_us);

/* Report pacing: the next state report is built one connection interval after the start of the
 * connection event that was just signalled, less `lead_us`, so it is ready for the next event.
 * In ticks of a clock with `ticks_per_s` ticks a second (rounded down). */
int64_t osc2_ble_slot_ticks(int64_t event_ticks, uint32_t interval_us, uint32_t lead_us,
			    uint32_t ticks_per_s);

/* ---- device information --------------------------------------------------------------------- */

/* A build time as the firmware and software revision strings show it: 8 lowercase hex digits. */
void osc2_ble_hex32(uint32_t value, char out[9]);

#endif /* OPENSC2_BLE_RULES_H_ */
