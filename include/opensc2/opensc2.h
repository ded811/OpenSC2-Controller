/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * OpenSC2: make an nRF52 board act as a Steam Controller 2 towards an unmodified Steam Controller
 * 2 puck, over the puck's 2.4 GHz link (Nordic Enhanced ShockBurst), and, on chips with USB, as a
 * wired Steam Controller 2 on its USB cable.
 *
 * The library owns the protocols: pairing, connecting, reconnecting, keep-alive, channel changes,
 * the protocol version, the identity packet, the USB device and the vendor commands Steam needs.
 * The application owns the hardware: it fills in the input state and reacts to the outputs. There
 * are no pins in the library.
 *
 *     static const struct opensc2_callbacks cb = { .link_changed = on_link };
 *     static const struct opensc2_config cfg = { .callbacks = &cb, .start = true };
 *
 *     opensc2_init(&cfg);
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
	OPENSC2_REASON_LISTEN_TIMEOUT,      /* CONFIG_OPENSC2_LISTEN_TIMEOUT_S passed without a puck;
					     * Bluetooth: the advertising window ended without a host */
	OPENSC2_REASON_BOND_CHANGED,        /* a bond or the slot selection changed */
	OPENSC2_REASON_HOST_DISCONNECTED,   /* Bluetooth: the host ended the link (HCI 0x13) or
					     * powered off (0x15): wireless off */
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
	/* The transport this describes (enum opensc2_transport): the puck's radio, or Bluetooth
	 * (CONFIG_OPENSC2_BLE), where `slot` is the Bluetooth slot and the radio fields are 0. */
	uint8_t transport;
};

/* ---- Modes ------------------------------------------------------------------------------------
 * Each boot is either wireless (the radio link to the puck) or wired (a USB Steam Controller 2,
 * radio off), decided once in opensc2_init() and kept until the next reboot, like the real
 * controller: a boot with USB power is wired, unless the warm-restart mark is set (Steam's "reboot
 * into wireless mode" after it paired the controller over the cable), the stored policy says
 * wireless, or the application asks for wireless (for example while a button is held).
 */

enum opensc2_mode {
	OPENSC2_MODE_WIRELESS = 0,   /* the puck's 2.4 GHz link; on USB power the cable only powers */
	OPENSC2_MODE_WIRED = 1,      /* the USB device 28DE:1302; the radio stays off */
};

/* Why this boot is in its mode. */
enum opensc2_mode_reason {
	OPENSC2_MODE_NO_USB_POWER,   /* no USB power at boot */
	OPENSC2_MODE_USB_POWER,      /* USB power and no warm-restart mark (policy auto) */
	OPENSC2_MODE_WARM_RESTART,   /* the warm-restart mark: opensc2_reboot(OPENSC2_REBOOT_WIRELESS) */
	OPENSC2_MODE_WIRED_RESTART,  /* opensc2_reboot(OPENSC2_REBOOT_WIRED) */
	OPENSC2_MODE_POLICY,         /* the policy is "wireless" */
	OPENSC2_MODE_APP,            /* opensc2_config.boot_mode */
	OPENSC2_MODE_NO_USB,         /* built without CONFIG_OPENSC2_USB */
	OPENSC2_MODE_USB_FAILED,     /* this boot was wired, but the USB device could not be set up:
				      * wireless instead (the USB session log has the error) */
};

/* The rule for boots on USB power, stored on the board (opensc2_policy_set()). */
enum opensc2_policy {
	OPENSC2_POLICY_AUTO = 0,     /* the real controller's rule: USB power at boot means wired */
	OPENSC2_POLICY_WIRELESS = 1, /* never wired (a board that only talks to the puck) */
};

/* What the application wants for this boot (opensc2_config.boot_mode). */
enum opensc2_boot_mode {
	OPENSC2_BOOT_DEFAULT = 0,    /* the policy and the warm-restart mark decide */
	OPENSC2_BOOT_WIRELESS,       /* wireless this boot (for example: a button held at power-on) */
	OPENSC2_BOOT_WIRED,          /* wired this boot, whatever the policy */
};

/* How opensc2_reboot() comes back. */
enum opensc2_reboot {
	OPENSC2_REBOOT_PLAIN,        /* by the boot rule: on USB power with policy auto, wired */
	OPENSC2_REBOOT_WIRELESS,     /* warm: wireless, also on USB power */
	OPENSC2_REBOOT_WIRED,        /* wired if USB power is there, whatever the policy (once) */
};

/* ---- USB (wired mode) ------------------------------------------------------------------------ */

enum opensc2_usb_state {
	OPENSC2_USB_OFF,             /* not on the bus: wireless mode, on the way to a reboot, or USB
				      * power gone (the cable pulled) */
	OPENSC2_USB_ATTACHED,        /* on the bus, not configured: the first 0.5 s, or after the host
				      * deconfigured the device (a bus reset) until it configures it
				      * again */
	OPENSC2_USB_NO_HOST,         /* 0.5 s without a configuration (a charger, or no driver) */
	OPENSC2_USB_CONFIGURED,      /* a host configured the device: state reports flow */
};

struct opensc2_usb_info {
	enum opensc2_usb_state state;
	bool active;                 /* configured right now (a bus reset deconfigures) */
	bool suspended;              /* the bus is suspended: reports are dropped */
	uint32_t reports;            /* state reports sent since the configuration */
	uint32_t dropped;            /* state reports not sent (no free buffer, suspended) */
	uint32_t requests;           /* feature reports (vendor commands) received */
	uint32_t stalls;             /* requests refused with a STALL */
	uint32_t outputs;            /* output reports received */
	uint32_t configured_ms;      /* time since the configuration, 0 when not configured */
};

/* ---- Callbacks --------------------------------------------------------------------------------
 * Callbacks run in the library's radio thread (wireless) or in its USB report thread or the USB
 * stack's thread (wired), except where noted. Return quickly (well under a millisecond): the link
 * only lives while acknowledgements keep up with the puck. Hand anything slow to your own thread
 * or work queue. Any callback may be NULL.
 */

/* What a vendor command callback is asked. A SET runs a command; a GET answers the command the last
 * SET named. */
struct opensc2_vendor_cmd {
	uint8_t opcode;
	uint8_t len;
	const uint8_t *payload;   /* `len` bytes; for a GET, the payload of the SET before it */
	bool is_get;
};

/* Power requests, in the system work queue. Vendor commands are acted on about 20 ms later so their
 * answer still goes out. Without a power_request callback the library does the suggested action
 * itself (the reboots); with one, the application decides. */
enum opensc2_power_request {
	OPENSC2_POWER_TURN_OFF_PUCK,     /* the puck sent "turn off": the PC is going to sleep */
	OPENSC2_POWER_TURN_OFF,          /* vendor command 0x9F, or 0x95 with 0xABDF9710 (wireless only) */
	OPENSC2_POWER_REBOOT,            /* 0x95 with any other value: opensc2_reboot(PLAIN) */
	OPENSC2_POWER_REBOOT_KEEP_LINK,  /* 0x95 with 0xA427AF52, Steam's "reboot into wireless mode",
					  * or STEAM pressed while no USB host configured the wired
					  * device, or (a charger test build only) no host within
					  * CONFIG_OPENSC2_NO_HOST_WIRELESS_MS: opensc2_reboot(WIRELESS) */
	OPENSC2_POWER_BOOTLOADER,        /* 0x90, or 0x95 with 0xECAABAC0 (Steam's firmware update) */
	OPENSC2_POWER_FACTORY_RESET,     /* 0x86 with 0xEFC1A917: bonds are erased; reboot now */
	OPENSC2_POWER_USB_PLUGGED,       /* USB power appeared while wireless (policy auto) with the
					  * link down or STEAM held: the real controller reboots to
					  * come back wired, opensc2_reboot(PLAIN) */
	OPENSC2_POWER_USB_UNPLUGGED,     /* USB power gone for 0.2 s while wired (a board without a
					  * battery has lost its power anyway): the real controller
					  * shuts down. Power off, or opensc2_reboot(PLAIN), which
					  * comes back wireless without USB power (the default) */
	OPENSC2_POWER_WIRELESS_OFF,      /* Bluetooth gave up: its 60 s advertising window ended
					  * without a host, or the host ended the link (HCI 0x13) or
					  * powered off (0x15). The library has already stopped the
					  * transport (the link state is OFF and says why); the real
					  * controller turns off on battery and stays on with wireless
					  * off on USB power. Default: nothing more */
};

struct opensc2_callbacks {
	/* The radio link changed state. */
	void (*link_changed)(const struct opensc2_link_info *info, void *user_data);

	/* Called right before each state report is built (about every 3 ms through the puck, every
	 * 4 ms on USB), so inputs can be sampled at the last moment. Change `state` in place; it is
	 * kept. */
	void (*sample)(struct opensc2_state *state, void *user_data);

	/* Output report 0x80, decoded. The library sends speeds of 0 with `timeout` set when the host
	 * stopped repeating it (CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS) or the link drops (then from the
	 * system work queue). */
	void (*rumble)(const struct opensc2_rumble *rumble, void *user_data);

	/* Output reports 0x81..0x89, decoded (pulse, command, tone, sweep, script, PCM). */
	void (*haptic)(const struct opensc2_haptic *haptic, void *user_data);

	/* Any output report, raw, before decoding (ID first). */
	void (*output_report)(const uint8_t *report, size_t len, void *user_data);

	/* The status LED colour or its settings changed (vendor command 0xC5, settings 37 and 45). */
	void (*led)(const struct opensc2_led *led, void *user_data);

	/* A controller setting changed value (vendor command 0x87), or all of them went back to their
	 * defaults (0x8E, the Steam watchdog: number OPENSC2_SETTINGS_ALL). Lizard mode is setting 9,
	 * the IMU mode 48. */
	void (*setting_changed)(uint8_t number, int16_t value, void *user_data);

	/* The puck says the PC is asleep (it keeps the link). The real controller plays a short cue. */
	void (*host_asleep)(void *user_data);

	/* Turn off, reboot, bootloader, factory reset, USB power. See enum opensc2_power_request. */
	void (*power_request)(enum opensc2_power_request request, void *user_data);

	/* Vendor commands the library does not answer itself (calibration, ...). For a SET, return 0
	 * or a negative errno. For a GET, write up to OPENSC2_ANSWER_MAX (61) bytes to `answer`, set
	 * `*answer_len` and return 0. Return -ENOTSUP for commands you do not know. While wired it
	 * runs in the USB stack's thread, inside the host's control request: only the return value
	 * counts (the library clears errno before it answers). */
	int (*vendor_command)(const struct opensc2_vendor_cmd *cmd, uint8_t *answer,
			      uint8_t *answer_len, void *user_data);

	/* Wired mode: the USB device changed state (attached, no host, configured, suspended). In the
	 * system work queue. */
	void (*usb_changed)(const struct opensc2_usb_info *info, void *user_data);
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
	/* Start the radio at once in wireless mode (otherwise call opensc2_start()). */
	bool start;
	/* This boot's mode, if the application wants to choose it (enum opensc2_boot_mode). */
	enum opensc2_boot_mode boot_mode;
};

/* Initialise the library: load bonds, the stored identity and the mode policy from Zephyr
 * settings, decide the mode of this boot, then start the USB device (wired) or the radio thread
 * (wireless). A wired boot whose USB device cannot be set up goes on wireless
 * (OPENSC2_MODE_USB_FAILED). Call once. Returns 0 or a negative errno (-EINVAL for an invalid
 * identity); after an error the debug console (CONFIG_OPENSC2_USB_CONSOLE) still starts, so the
 * log can be read. */
int opensc2_init(const struct opensc2_config *config);

/* Radio on: listen for the bonded puck (OPENSC2_LINK_LISTENING), or wait in OPENSC2_LINK_NO_BOND
 * when the selected slot is empty. -EBUSY in wired mode. */
int opensc2_start(void);

/* Radio off. Like the real controller, nothing is sent: the puck notices the silence. */
int opensc2_stop(void);

/* Pair with a puck whose pairing window is open over the air. The new bond replaces the one in the
 * selected slot once pairing succeeds. `seconds` 0: CONFIG_OPENSC2_PAIRING_TIMEOUT_S. Starts the
 * radio if it was off. -EBUSY in wired mode (Steam pairs a wired controller itself). */
int opensc2_pair_start(uint16_t seconds);
int opensc2_pair_cancel(void);

enum opensc2_link_state opensc2_link_state(void);
int opensc2_link_info(struct opensc2_link_info *info);

/* ---- Modes ----------------------------------------------------------------------------------- */

/* The mode of this boot, and why. */
enum opensc2_mode opensc2_mode(void);
enum opensc2_mode_reason opensc2_mode_reason(void);

/* The policy for boots on USB power (stored on the board; the default is a Kconfig choice). It
 * decides the next boot's mode; the rule for USB power that appears while wireless
 * (CONFIG_OPENSC2_USB_PLUG_IN_REBOOT) follows it at once. */
enum opensc2_policy opensc2_policy(void);
int opensc2_policy_set(enum opensc2_policy policy);

/* Reboot now, the way the real controller does: in wired mode leave the bus first (one all-zero
 * state report, 10 ms, USB off), then set the mark for `how` and reboot. `why` is a short text for
 * the USB session log (NULL: the reason of the power request being handled). Call it from a thread
 * (it sleeps 10 ms), not from the USB stack's own thread. Does not return. */
void opensc2_reboot(enum opensc2_reboot how, const char *why);

/* The same reboot `delay_ms` from now, from the system work queue; returns at once. For callers
 * that may block afterwards, such as a shell command whose message waits for a terminal to read it
 * (with no terminal on the port the shell can wait for good). `why` must stay valid (a literal).
 * A second call before the first has run replaces it. */
void opensc2_reboot_later(enum opensc2_reboot how, const char *why, uint32_t delay_ms);

/* Wired mode: the state of the USB device (OPENSC2_USB_OFF in wireless mode). */
enum opensc2_usb_state opensc2_usb_state(void);
int opensc2_usb_info(struct opensc2_usb_info *info);

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
 * controller setting 68 (90 % by default): on above 5 points more, off below 5 points less. With
 * it off, set the bit yourself. */
int opensc2_set_trigger(enum opensc2_side side, uint16_t value);
int opensc2_set_trackpad(enum opensc2_side side, const struct opensc2_trackpad *pad);
int opensc2_set_imu(const struct opensc2_imu *imu);

/* Battery state, sent as input report 0x43 every CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS on
 * the link in use and answered to vendor command 0xBE. Until it is set: 100 %, "charging done". */
int opensc2_set_battery(const struct opensc2_battery *battery);

/* Send any other input report (ID first, at most 64 bytes, the length the host expects for that
 * ID) on the link in use: through the puck it rides with the next state reports, on USB it goes
 * out on the interrupt endpoint. -ENOMEM when the queue is full, -ENOTCONN without a link. */
int opensc2_send_report(const uint8_t *report, size_t len);

/* Haptic stream flow control, input report 0x44 (channel: left grip 0, right grip 1, left pad 3,
 * right pad 4; flags as documented in docs/api.md). */
int opensc2_send_haptic_status(uint8_t channel_id, uint32_t flags);

/* ---- Bonds ----------------------------------------------------------------------------------- */

int opensc2_bond_get(uint8_t slot, struct opensc2_bond *bond);
/* Store a bond (both IDs zero: delete it). Takes effect when the radio next listens. */
int opensc2_bond_set(uint8_t slot, const struct opensc2_bond *bond);
/* Use the puck with bond slot 0 or 1 from now on: opensc2_select_transport() with
 * OPENSC2_TRANSPORT_PUCK_1 or _2. Restarts listening; stored once the puck connects. */
int opensc2_select_slot(uint8_t slot);
uint8_t opensc2_selected_slot(void);

/* ---- Wireless transports ----------------------------------------------------------------------
 * A wireless boot runs one transport: the puck's radio with one of the two bonds, or (with
 * CONFIG_OPENSC2_BLE) Bluetooth LE in one of two slots. The numbers are the real controller's
 * user/wireless_transport, which Steam reads with vendor command 0xED.
 */
enum opensc2_transport {
	OPENSC2_TRANSPORT_BLE_1 = 0,    /* Bluetooth LE, slot 1 */
	OPENSC2_TRANSPORT_BLE_2 = 1,    /* Bluetooth LE, slot 2 */
	OPENSC2_TRANSPORT_PUCK_1 = 2,   /* the puck with bond slot 0: the default */
	OPENSC2_TRANSPORT_PUCK_2 = 3,   /* the puck with bond slot 1 */
};

/* Run `transport` from now on: the one running stops and hands over the radio. Saved once its link
 * is up (Bluetooth: connected and encrypted; the puck: connected), as the October 2026 firmware
 * does, so a choice whose link never comes up is not kept; in wired mode it is stored at once, for
 * the next wireless boot. -ENOTSUP for Bluetooth in a build without CONFIG_OPENSC2_BLE. */
int opensc2_select_transport(enum opensc2_transport transport);
/* The same with the pairing flag the real controller's transport chords give when held 4 s:
 * Bluetooth opens the slot for a new host (pairing mode on the slot's other identity, which is
 * reset first, so the bond of the slot's old host stays until a new host has paired); the puck's
 * radio ignores the flag, as the real controller's does. In wired mode the choice is stored and
 * the flag is dropped. */
int opensc2_select_transport_pairing(enum opensc2_transport transport);
/* The transport this boot runs. */
enum opensc2_transport opensc2_selected_transport(void);

/* ---- Identity -------------------------------------------------------------------------------- */

/* The identity in use. */
void opensc2_identity_get(struct opensc2_identity *identity);
/* Store an identity on the board (Zephyr settings). Empty / zero fields are not stored and keep
 * their configured value (the shell's `opensc2 id set` stores one field as given, 0 included).
 * Validated first (-EINVAL). Takes effect at once for new connections (on USB: at the next boot,
 * since the strings are set when the device attaches). */
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
