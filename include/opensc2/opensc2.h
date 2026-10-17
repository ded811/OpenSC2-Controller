/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * OpenSC2: make an nRF52 board act as a Steam Controller 2 towards an unmodified Steam Controller
 * 2 puck, over the puck's 2.4 GHz link (Nordic Enhanced ShockBurst).
 *
 * The library owns the radio protocol: pairing, connecting, reconnecting, keep-alive, channel
 * changes, the protocol version, the identity packet and the vendor commands Steam needs. The
 * application owns the hardware: it fills in the input state and reacts to the outputs. There are
 * no pins in the library.
 *
 *     static const struct opensc2_callbacks cb = { .link_changed = on_link };
 *     static const struct opensc2_config cfg = { .callbacks = &cb, .start = true };
 *
 *     opensc2_init(&cfg);
 *     opensc2_pair_start(0);                          // while Steam's puck is pairing
 *     opensc2_set_buttons(OPENSC2_BTN_A, OPENSC2_BTN_A);  // A pressed
 *
 * Every field, its range and where it shows up in Steam: docs/api.md.
 */
#ifndef OPENSC2_OPENSC2_H_
#define OPENSC2_OPENSC2_H_

#include <opensc2/types.h>
#include <opensc2/codec.h>
#include <opensc2/settings.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Link ------------------------------------------------------------------------------------ */

enum opensc2_link_state {
	OPENSC2_LINK_OFF,         /* radio off (before opensc2_start, after opensc2_stop or a turn-off) */
	OPENSC2_LINK_NO_BOND,     /* radio on, but the selected bond slot is empty: pair first */
	OPENSC2_LINK_PAIRING,     /* answering a puck's pairing beacon */
	OPENSC2_LINK_LISTENING,   /* waiting for the bonded puck's connection request */
	OPENSC2_LINK_CONNECTED,   /* reports are flowing */
};

/* Why the state changed. 0..4 are the controller's disconnect reasons (vendor command 0xF2, 2). */
enum opensc2_reason {
	OPENSC2_REASON_NONE = 0,
	OPENSC2_REASON_LINK_LOST = 1,       /* 500 ms without a packet from the puck */
	OPENSC2_REASON_DISABLED = 2,        /* opensc2_stop() or a local turn-off */
	OPENSC2_REASON_PUCK_TURN_OFF = 3,   /* the puck sent "turn off" (the PC went to sleep) */
	OPENSC2_REASON_BAD_VERSION = 4,     /* the puck's protocol version is not one we know */
	OPENSC2_REASON_PAIRED = 16,         /* pairing succeeded; the bond is stored */
	OPENSC2_REASON_PAIRING_FAILED,      /* the pairing window closed without a puck */
	OPENSC2_REASON_LISTEN_TIMEOUT,      /* CONFIG_OPENSC2_LISTEN_TIMEOUT_S passed without a puck */
	OPENSC2_REASON_BOND_CHANGED,        /* a bond or the slot selection changed */
};

struct opensc2_link_info {
	enum opensc2_link_state state;
	enum opensc2_reason reason;   /* of the last change */
	uint8_t slot;                 /* bond slot in use, 0 or 1 */
	uint8_t channel;              /* current radio channel (2400 + n MHz) */
	uint8_t channels[4];          /* channel list: current, then alternates (0xFF unused) */
	uint8_t protocol;             /* protocol version agreed with the puck */
	int8_t rssi_dbm;              /* of the last packet */
	uint32_t packets;             /* packets received in this connection */
	uint32_t reports;             /* state reports queued in this connection */
	uint32_t dropped;             /* state reports dropped (acknowledgement queue full) */
	uint32_t connected_ms;        /* time since the link came up, 0 when not connected */
};

/* ---- Callbacks --------------------------------------------------------------------------------
 * Every callback runs in the library's radio thread. Return quickly (well under a millisecond):
 * the link only lives while acknowledgements keep up with the puck. Hand anything slow to your own
 * thread or work queue. Any callback may be NULL.
 */

/* What a vendor command callback is asked. A SET runs a command; a GET answers the command the last
 * SET named (the puck always sends SET, then GET). */
struct opensc2_vendor_cmd {
	uint8_t opcode;
	uint8_t len;
	const uint8_t *payload;   /* `len` bytes; for a GET, the payload of the SET before it */
	bool is_get;
};

/* Power requests. Vendor commands are acted on about 20 ms later so their answer still goes out. */
enum opensc2_power_request {
	OPENSC2_POWER_TURN_OFF_PUCK,     /* the puck sent "turn off": the PC is going to sleep */
	OPENSC2_POWER_TURN_OFF,          /* vendor command 0x9F, or 0x95 with 0xABDF9710 */
	OPENSC2_POWER_REBOOT,            /* 0x95 with any other value */
	OPENSC2_POWER_REBOOT_KEEP_LINK,  /* 0x95 with 0xA427AF52: come back with the link up */
	OPENSC2_POWER_BOOTLOADER,        /* 0x90, or 0x95 with 0xECAABAC0 (Steam's firmware update) */
	OPENSC2_POWER_FACTORY_RESET,     /* 0x86 with 0xEFC1A917: bonds are erased; reboot now */
};

struct opensc2_callbacks {
	/* The link changed state. */
	void (*link_changed)(const struct opensc2_link_info *info, void *user_data);

	/* Called right before each state report is built (about every 3 ms while connected), so
	 * inputs can be sampled at the last moment. Change `state` in place; it is kept. */
	void (*sample)(struct opensc2_state *state, void *user_data);

	/* Output report 0x80, decoded. The library sends speeds of 0 with `timeout` set when the host
	 * stopped repeating it (CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS) or the link drops. */
	void (*rumble)(const struct opensc2_rumble *rumble, void *user_data);

	/* Output reports 0x81..0x89, decoded (pulse, command, tone, sweep, script, PCM). */
	void (*haptic)(const struct opensc2_haptic *haptic, void *user_data);

	/* Any output report, raw, before decoding (ID first). */
	void (*output_report)(const uint8_t *report, size_t len, void *user_data);

	/* The status LED colour or its settings changed (vendor command 0xC5, settings 37 and 45). */
	void (*led)(const struct opensc2_led *led, void *user_data);

	/* Steam wrote a controller setting (vendor command 0x87), or restored all of them (0x8E:
	 * number OPENSC2_SETTINGS_ALL). Lizard mode is setting 9, the IMU mode 48. */
	void (*setting_changed)(uint8_t number, int16_t value, void *user_data);

	/* The puck says the PC is asleep (it keeps the link). The real controller plays a short cue. */
	void (*host_asleep)(void *user_data);

	/* Turn off, reboot, bootloader, factory reset. See enum opensc2_power_request. */
	void (*power_request)(enum opensc2_power_request request, void *user_data);

	/* Vendor commands the library does not answer itself (calibration, ...). For a SET, return 0
	 * or a negative errno. For a GET, write up to OPENSC2_ANSWER_MAX (61) bytes to `answer`, set
	 * `*answer_len` and return 0. Return -ENOTSUP for commands you do not know. */
	int (*vendor_command)(const struct opensc2_vendor_cmd *cmd, uint8_t *answer,
			      uint8_t *answer_len, void *user_data);
};

/* ---- Initialisation -------------------------------------------------------------------------- */

#define OPENSC2_SLOT_STORED 0xFF

struct opensc2_config {
	/* Who the controller says it is. NULL, or any empty / zero field: the Kconfig default
	 * (CONFIG_OPENSC2_UNIT_SERIAL and friends). An identity stored on the board with
	 * opensc2_identity_store() or the shell overrides both. */
	const struct opensc2_identity *identity;
	const struct opensc2_callbacks *callbacks;
	void *user_data;
	/* Bond slot to use, 0 or 1; OPENSC2_SLOT_STORED: the one stored last (default 0). */
	uint8_t bond_slot;
	/* Start the radio at once (otherwise call opensc2_start()). */
	bool start;
};

/* Initialise the library: load bonds and the stored identity from Zephyr settings, start the radio
 * thread. Call once. Returns 0 or a negative errno (-EINVAL for an invalid identity). */
int opensc2_init(const struct opensc2_config *config);

/* Radio on: listen for the bonded puck (OPENSC2_LINK_LISTENING), or wait in OPENSC2_LINK_NO_BOND
 * when the selected slot is empty. */
int opensc2_start(void);

/* Radio off. Like the real controller, nothing is sent: the puck notices the silence. */
int opensc2_stop(void);

/* Pair with a puck whose pairing window is open (Steam opens it). The new bond replaces the one in
 * the selected slot once pairing succeeds. `seconds` 0: CONFIG_OPENSC2_PAIRING_TIMEOUT_S. Starts
 * the radio if it was off. */
int opensc2_pair_start(uint16_t seconds);
int opensc2_pair_cancel(void);

enum opensc2_link_state opensc2_link_state(void);
int opensc2_link_info(struct opensc2_link_info *info);

/* ---- Inputs ------------------------------------------------------------------------------------
 * All setters are thread safe and cheap: they change the state the next report is built from.
 * Unset fields stay at zero, which reads as neutral (sticks centred, triggers released).
 */

int opensc2_set_state(const struct opensc2_state *state);
void opensc2_get_state(struct opensc2_state *state);

/* Change the bits in `mask` to `value` (other bits stay). */
int opensc2_set_buttons(uint32_t mask, uint32_t value);
int opensc2_set_stick(enum opensc2_side side, int16_t x, int16_t y);
/* 0..32767. Unless CONFIG_OPENSC2_TRIGGER_CLICK_AUTO is off, the trigger's digital bit follows
 * controller setting 68 (90 % by default) with 5 % hysteresis; with it off, set the bit
 * yourself. */
int opensc2_set_trigger(enum opensc2_side side, uint16_t value);
int opensc2_set_trackpad(enum opensc2_side side, const struct opensc2_trackpad *pad);
int opensc2_set_imu(const struct opensc2_imu *imu);

/* Battery state, sent as input report 0x43 every CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS and
 * answered to vendor command 0xBE. Until it is set: 100 %, "charging done". */
int opensc2_set_battery(const struct opensc2_battery *battery);

/* Queue any other input report (ID first, at most 64 bytes, the length the puck expects for that
 * ID). It rides with the next state reports. -ENOMEM when the queue is full, -ENOTCONN offline. */
int opensc2_send_report(const uint8_t *report, size_t len);

/* Haptic stream flow control, input report 0x44 (channel: left grip 0, right grip 1, left pad 3,
 * right pad 4; flags as documented in docs/api.md). */
int opensc2_send_haptic_status(uint8_t channel_id, uint32_t flags);

/* ---- Bonds ----------------------------------------------------------------------------------- */

int opensc2_bond_get(uint8_t slot, struct opensc2_bond *bond);
/* Store a bond (both IDs zero: delete it). Takes effect when the radio next listens. */
int opensc2_bond_set(uint8_t slot, const struct opensc2_bond *bond);
/* Use bond slot 0 or 1 from now on (stored). Restarts listening. */
int opensc2_select_slot(uint8_t slot);
uint8_t opensc2_selected_slot(void);

/* ---- Identity -------------------------------------------------------------------------------- */

/* The identity in use. */
void opensc2_identity_get(struct opensc2_identity *identity);
/* Store an identity on the board (Zephyr settings). Empty / zero fields are not stored and keep
 * their configured value. Validated first (-EINVAL). Takes effect at once for new connections. */
int opensc2_identity_store(const struct opensc2_identity *identity);
/* Forget the stored identity: back to the opensc2_init() / Kconfig values. */
int opensc2_identity_clear(void);

/* ---- Settings and other state Steam sets ----------------------------------------------------- */

int16_t opensc2_setting_get(uint8_t number);
bool opensc2_lizard_mode(void);
void opensc2_led_get(struct opensc2_led *led);
/* The power-on chime level Steam stores with vendor command 0xDC: 1 quiet, 2 normal (default),
 * anything else silent. */
uint8_t opensc2_chime_level(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_OPENSC2_H_ */
