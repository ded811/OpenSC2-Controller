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

/* The three ways the controller advertises. */
enum osc2_ble_adv_form {
	OSC2_BLE_ADV_PAIRING = 0,     /* every 100 to 150 ms, open to any host, with the
				       * pairing-mode items */
	OSC2_BLE_ADV_ALLOW_LISTED,    /* every 100 to 150 ms, only the bonded host may connect or
				       * ask for the scan response */
	OSC2_BLE_ADV_DIRECTED,        /* high duty cycle to the bonded host, 1.28 s at most; no
				       * advertising data */
	OSC2_BLE_ADV_NONE = 0xFF,     /* not advertising (the status, ring entries) */
};

/* The form for the identity in use: pairing mode while it has no bond; with one, directed
 * advertising to that host first, and allow-listed advertising once that try is over
 * (`fallback`: the directed advertising ended without a connection, or a connection failed while
 * advertising). A pairing request picks an identity without a bond (osc2_ble_pick_id()), so it
 * always ends in pairing mode. */
enum osc2_ble_adv_form osc2_ble_adv_form(bool bonded, bool fallback);

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
 * response's. Pairing mode gives 20 bytes, allow-listed 10, directed none (its packets carry only
 * the two addresses). */
size_t osc2_ble_adv_data(const struct osc2_ble_adv_in *in, uint8_t ad[OSC2_BLE_AD_MAX],
			 uint8_t sr[OSC2_BLE_AD_MAX], size_t *sr_len);

/* Walk AD structures {length, type, data} in `buf`: the next one at `*pos` into `type`, `data`
 * and `len`. Returns false at the end or for a malformed one. */
bool osc2_ble_ad_next(const uint8_t *buf, size_t buf_len, size_t *pos, uint8_t *type,
		      const uint8_t **data, uint8_t *len);

/* ---- identities and bonds ------------------------------------------------------------------- */

/* Five identities. Identity 0, the stack's default, never advertises; slot 1 (transport value 0)
 * has identities 1 and 2, slot 2 (transport value 1) identities 3 and 4. Each identity has its own
 * static random address and keeps at most the bond of one host; each slot keeps at most one. */
#define OSC2_BLE_ID_COUNT   5

/* The first identity of `slot` (0 or 1): 1 or 3. */
uint8_t osc2_ble_slot_first_id(uint8_t slot);
/* The slot of identity `id` (0 or 1); -1 for identity 0 and anything above 4. */
int osc2_ble_id_slot(uint8_t id);
/* The other identity of the same slot (1 and 2, 3 and 4); 0 for an identity in no slot. */
uint8_t osc2_ble_other_id(uint8_t id);

/* The identity a start of `slot` advertises on, from which of the slot's two identities have a
 * bond. Without a pairing request: the first if it has a bond or neither has one, otherwise the
 * second. With one: the slot's other identity, which `*reset` says to reset first (a fresh random
 * address and no keys), so the bond the slot holds stays until a new host has paired. */
uint8_t osc2_ble_pick_id(uint8_t slot, bool first_bonded, bool second_bonded, bool pair_request,
			 bool *reset);

/* The bond rule, checked when the link reaches security level 2 (once per connection): a host
 * that encrypts a link on an identity which had no bond when its advertising started is the slot's
 * new host, whether it bonded or not, as on the real controller. Then the bonds on the slot's
 * other identity are deleted (one host per slot), the moment the real controller plays its
 * "paired" cue. */
bool osc2_ble_new_host(bool bonded_at_start, uint8_t security_level);

/* ---- the advertising window and disconnects ---------------------------------------------- */

/* Every start of advertising, whatever its form, arms a 60 s window: if no host has connected
 * when it ends, advertising stops and wireless goes off. A failed connection starts advertising
 * again, with a fresh window. */
#define OSC2_BLE_ADV_WINDOW_MS   60000u
/* After a lost link the same identity advertises again this much later. */
#define OSC2_BLE_READVERTISE_MS  500u

/* HCI disconnect reasons the rules name. */
#define OSC2_HCI_CONN_TIMEOUT        0x08   /* supervision timeout */
#define OSC2_HCI_REMOTE_USER_TERM    0x13   /* the host ended the link */
#define OSC2_HCI_REMOTE_POWER_OFF    0x15   /* the host is powering off */
#define OSC2_HCI_LOCAL_TERM          0x16   /* this side ended it */
#define OSC2_HCI_ADV_TIMEOUT         0x3C   /* directed advertising ended unanswered */

/* What follows a link that ended with HCI reason `reason`. */
enum osc2_ble_after {
	OSC2_BLE_AFTER_READVERTISE = 0,    /* the link was lost (a supervision timeout, anything but
					    * the two below): the same identity advertises again
					    * 500 ms later */
	OSC2_BLE_AFTER_WIRELESS_OFF = 1,   /* the host ended the link (0x13) or powered off (0x15):
					    * "wireless off", as when the window runs out */
	OSC2_BLE_AFTER_NOTHING = 2,        /* the controller ended it itself, to turn wireless off
					    * or to switch: advertising stays off */
};
/* `ended_here`: this side ended the link (then `reason` does not matter). */
enum osc2_ble_after osc2_ble_after_disconnect(uint8_t reason, bool ended_here);

/* Why an advertising round ended (ring entries). */
enum osc2_ble_adv_end {
	OSC2_BLE_ADV_END_CONNECTED = 0,   /* a host connected */
	OSC2_BLE_ADV_END_FAILED = 1,      /* a connection failed: 0x3C when directed advertising ended
					   * unanswered */
	OSC2_BLE_ADV_END_WINDOW = 2,      /* the window ran out */
	OSC2_BLE_ADV_END_STOPPED = 3,     /* stopped: Bluetooth off, a switch, a new round */
};

/* What turned wireless off (ring entries). */
enum osc2_ble_off_cause {
	OSC2_BLE_OFF_WINDOW = 0,          /* the advertising window ran out */
	OSC2_BLE_OFF_HOST = 1,            /* the host ended the link or powered off */
};

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

/* A read of the feature report's value at `offset`, `len` bytes at most, as the real controller
 * answers it: the stored value from `offset` (what the host last wrote on this connection, zero at
 * its start), with the stored request {opcode, length, payload} (`request_len` bytes) written over
 * its start, and the GET's answer over that ({opcode, n, data}: `answer[1]` = n, then the bytes the
 * GET writes from `answer[2]`, osc2_ble_get_span(); `answer` holds 63 bytes). So the bytes after
 * the answer are left-overs of the request and of the last write, and a read at any offset gets
 * the answer at its start: the value cannot be read in parts. Into `out` (64 bytes); returns the
 * bytes read, 0 for an offset at or past the end (the caller refuses one past it). */
size_t osc2_ble_feature_read(const uint8_t value[OSC2_BLE_FEATURE_LEN], uint16_t offset,
			     uint16_t len, const uint8_t *request, uint8_t request_len,
			     const uint8_t *answer, uint8_t out[OSC2_BLE_FEATURE_LEN]);
/* How many bytes after its length byte a GET of `opcode` that answered `n` bytes writes, as on the
 * real controller: `n`, except the string attribute's (0xAE), which writes its tag and 20
 * characters padded with zeros while it counts 20 bytes, so the byte after its answer is the
 * string's padding, not a left-over. At most 61. */
uint8_t osc2_ble_get_span(uint8_t opcode, uint8_t n);

/* ---- Valve's vendor service ------------------------------------------------------------------- */

/* A second GATT service mirrors the HID service's reports as plain characteristics, for hosts that
 * talk GATT themselves (SDL's Android and Apple back ends). Its 128-bit UUIDs share one base,
 * 100F6Cxx-1735-4313-B402-38567131E5F3: xx 0x32 the service, 0x34 the command channel (feature
 * report 1), 0x35 + the report ID for the inputs 0x40 to 0x45 (0x75 to 0x7A, read and notify) and
 * the outputs 0x80 to 0x89 (0xB5 to 0xBE, read and write). 41 attributes. */
#define OSC2_BLE_VENDOR_SERVICE   0x32
#define OSC2_BLE_VENDOR_COMMAND   0x34
#define OSC2_BLE_VENDOR_OFFSET    0x35
#define OSC2_BLE_VENDOR_ATTRS     41

/* The UUID with `xx` as its 16 bytes go on the air, least significant first:
 * F3 E5 31 71 56 38 02 B4 13 43 35 17 xx 6C 0F 10. */
void osc2_ble_vendor_uuid(uint8_t xx, uint8_t out[16]);
/* `xx` of the vendor characteristic of report `r` (an entry of osc2_ble_reports). */
uint8_t osc2_ble_vendor_xx(const struct osc2_hid_report *r);

/* Where an input report goes: on the vendor characteristic if the host enabled its notifications
 * there, otherwise on the HID service's if enabled there, otherwise nowhere (it is dropped). */
enum osc2_ble_target {
	OSC2_BLE_TARGET_NONE = 0,
	OSC2_BLE_TARGET_VENDOR,
	OSC2_BLE_TARGET_HID,
};
enum osc2_ble_target osc2_ble_input_target(bool vendor_subscribed, bool hid_subscribed);

/* The real controller's quirk in its flags of the vendor service's subscriptions, which OpenSC2
 * copies: the flag of input `index` 5 (report 0x45, the state) is bit 0 of the puck link's last
 * disconnect reason (what vendor command 0xF2 selector 2 reports), not the host's subscription.
 * The flag the vendor service has for input `index` of osc2_ble_reports, from the host's
 * subscription there and that reason. */
bool osc2_ble_vendor_flag(int index, bool vendor_subscribed, uint8_t radio_reason);
/* That reason after the host turned the vendor 0x45's notifications on or off (and when the stack
 * turns them off at a disconnect): bit 0 set or cleared. */
uint8_t osc2_ble_vendor_state_ccc(uint8_t radio_reason, bool on);

/* ---- what the link is doing ------------------------------------------------------------------ */

/* For the shell and the test channel. */
struct osc2_ble_status {
	bool enabled;          /* the stack is on */
	bool advertising;
	bool open;             /* advertising in pairing mode (any host) */
	bool connected;
	bool active;           /* past the gate: reports flow */
	bool trigger;          /* paced by the connection-event trigger (else by a timer) */
	bool running;          /* Bluetooth is the running wireless transport */
	bool pair_request;     /* the next start opens the slot for a new host */
	bool wireless_off;     /* the window or the host turned wireless off (until a start) */
	uint8_t form;          /* enum osc2_ble_adv_form of the advertising running, else NONE */
	uint8_t slot;          /* 0 or 1 */
	uint8_t id;            /* the identity in use */
	uint8_t bonded;        /* one bit per identity that has a bond (bit 1 for identity 1, ...) */
	uint8_t identities;    /* identities that exist (5 once Bluetooth has started) */
	uint8_t security;      /* the link's security level */
	uint8_t last_reason;   /* the HCI reason of the last disconnect (0: none yet) */
	uint8_t state;         /* enum opensc2_link_state */
	uint8_t reason;        /* enum opensc2_reason of the last change */
	uint16_t mtu;
	uint32_t interval_us;
	uint32_t window_left_ms;   /* left of the advertising window, 0 when none runs */
	uint32_t reports, dropped;
	char name[32];
	char address[32];      /* the identity's own address, as text */
};

/* The 28 bytes the test channel's BLE_STATUS answers (docs/test-protocol.md): flags (bit 0
 * enabled, 1 advertising, 2 connected, 3 active, 4 trigger, 5 running, 6 pairing request, 7
 * wireless off), form, slot, identity, bonded, identities, security, last reason, state, reason,
 * MTU u16, interval u32, window left u32, reports u32, dropped u32. No name, no address. */
#define OSC2_BLE_STATUS_LEN 28
void osc2_ble_pack_status(const struct osc2_ble_status *st, uint8_t out[OSC2_BLE_STATUS_LEN]);

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

/* The sampling margin: the report thread wakes this long before the slot, so the state report is
 * built and sent by then. As on the real controller it is the longest time a frame took (sampling,
 * the report, lizard mode) over the current run of 65 frames and the run before it, and starts at
 * 81 ticks of a 32,768 Hz clock (about 2.5 ms) until the first run is complete. Times in ticks of
 * the caller's clock. */
#define OSC2_BLE_MARGIN_FRAMES       65
#define OSC2_BLE_MARGIN_INITIAL_US   2472   /* 81 / 32768 s */
struct osc2_ble_margin {
	uint32_t peak;      /* the longest of the current run */
	uint32_t worst;     /* the longest of the last complete run (the initial value before one) */
	uint8_t frames;     /* frames in the current run */
};
void osc2_ble_margin_init(struct osc2_ble_margin *m, uint32_t initial_ticks);
/* How long before the slot to wake. */
uint32_t osc2_ble_margin(const struct osc2_ble_margin *m);
/* One frame took `ticks`. */
void osc2_ble_margin_add(struct osc2_ble_margin *m, uint32_t ticks);

/* ---- device information --------------------------------------------------------------------- */

/* A build time as the firmware and software revision strings show it: 8 lowercase hex digits. */
void osc2_ble_hex32(uint32_t value, char out[9]);

#endif /* OPENSC2_BLE_RULES_H_ */
