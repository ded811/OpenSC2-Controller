/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * internal.h: what the parts of the library share. Not a public interface.
 */
#ifndef OPENSC2_INTERNAL_H_
#define OPENSC2_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <opensc2/opensc2.h>
#include "id_rules.h"    /* the stored identity's fields (OSC2_ID_*), the build time floor */
#include "retained.h"    /* the retained area's layout (the session summary) */
#include "transport_rules.h"   /* the wireless transport values (OSC2_TRANSPORT_*) */

/* Report interval of the puck's radio and USB, attribute 11 of vendor command 0x83 while one of
 * them is the current link (Bluetooth reports its connection interval, the dock 64000 µs:
 * OPENSC2_DOCK_INTERVAL_US). */
#define OSC2_REPORT_INTERVAL_US 4000

/* The charger test build (a testing aid, not the real controller's behaviour, off by default): a
 * wired boot that no host talks to within this many ms goes wireless (usb_rules.h). 0: off, and
 * always without USB. */
#if defined(CONFIG_OPENSC2_NO_HOST_WIRELESS_MS)
#define OSC2_NO_HOST_WIRELESS_MS CONFIG_OPENSC2_NO_HOST_WIRELESS_MS
#else
#define OSC2_NO_HOST_WIRELESS_MS 0
#endif

/* The links that carry vendor commands and reports. Each keeps its own stored request. The numbers
 * are also the hardware test hooks' (the ring's link byte), where 3 means "no link" (work queue
 * events): no link has it. */
enum osc2_link {
	OSC2_LINK_RADIO = 0,
	OSC2_LINK_USB = 1,
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	/* The hardware test hooks' host emulation (HOST_OUT, HOST_SET, HOST_GET): commands as from a
	 * host, with a stored request of their own; active exactly when the mode's real link is. */
	OSC2_LINK_TEST = 2,
#endif
	/* 3 is "no link" in the test hooks' ring (OSC2_TEST_LINK_NONE). */
	OSC2_LINK_BLE = 4,    /* Bluetooth LE (CONFIG_OPENSC2_BLE) */
	/* The dock (dock_link.c): commands and the latest state report over the puck's charging pads;
	 * never "active" for lizard mode. */
	OSC2_LINK_DOCK = 5,
	OSC2_LINK_COUNT,
};

struct osc2_ctx {
	bool initialised;
	const struct opensc2_callbacks *cb;
	void *user_data;

	struct k_spinlock lock;       /* guards everything below that the application can change */
	struct opensc2_state state;
	struct opensc2_battery battery;
	struct opensc2_identity base;       /* Kconfig and opensc2_init() */
	struct osc2_id_override stored;     /* from the settings store */
	struct opensc2_identity identity;   /* what is in use: base + stored */
	struct opensc2_bond bonds[OPENSC2_BOND_SLOTS];
	uint8_t slot;                 /* the puck's bond slot the radio uses (0 or 1) */
	uint8_t transport;            /* the chosen wireless transport, OSC2_TRANSPORT_* (in RAM) */
	uint8_t transport_saved;      /* the one stored, or OSC2_TRANSPORT_NONE */
	int16_t settings[OPENSC2_SETTINGS_COUNT];
	struct opensc2_led led;
	uint8_t chime;
	uint8_t last_disconnect_reason;

	/* The mode of this boot (decided once in opensc2_init()). */
	enum opensc2_mode mode;
	enum opensc2_mode_reason mode_reason;
	enum opensc2_policy policy;   /* stored policy, or the Kconfig default */
};

extern struct osc2_ctx osc2;

static inline bool osc2_wired(void)
{
	return osc2.mode == OPENSC2_MODE_WIRED;
}

/* core.c */
void osc2_identity_refresh(void);
/* Store the fields in `fields` (OSC2_ID_*) of `value` on the board, whatever their values: unlike
 * opensc2_identity_store(), a 0 or an empty string is stored too (the shell sets one field at a
 * time, and `id set blbuild 0` means 0). Validated first (-EINVAL). */
int osc2_identity_store_fields(uint8_t fields, const struct opensc2_identity *value);
/* Every setting back to its default. `keep_imu`: settings 48, 84 and 85 keep their value, as when
 * 0x8E or the Steam watchdog resets them on the real controller (they live in the IMU there). */
void osc2_settings_restore_defaults(bool keep_imu);
/* The Steam watchdog: a fresh 10 s when setting 71 is not 0 (otherwise nothing), or cancel it. */
void osc2_watchdog_arm(void);
void osc2_watchdog_cancel(void);
void osc2_notify_led(void);
/* Ask the application (power_request) `delay_ms` from now; `why` names the cause in logs. One
 * request waits at a time: a later one replaces it. */
void osc2_schedule_power(enum opensc2_power_request request, uint32_t delay_ms, const char *why);
/* Ask the application now (or do the library's default), from the system work queue, for a cause
 * that must not take a scheduled request's place (USB power gone). */
void osc2_power_now(enum opensc2_power_request request, const char *why);
/* While a power request is being handled: its cause, else NULL. */
const char *osc2_power_reason(void);
/* Is `link` the one in use and up (USB configured, radio connected, Bluetooth past its gate)? The
 * test link is when the mode's real link is; the dock never is. */
bool osc2_link_active(enum osc2_link link);

/* wireless.c: the wireless transport of this boot (the puck's radio, or Bluetooth). */
/* At init, after the settings are loaded: this boot's transport from the stored choice. */
void osc2_transport_init(void);
/* The link reports go to and the test link stands for: USB while wired, else the running
 * wireless transport's (Bluetooth or the radio). */
enum osc2_link osc2_mode_link(void);
/* The running wireless transport (OSC2_TRANSPORT_*), whether or not its link is up. */
uint8_t osc2_transport_running(void);
/* A wireless link is up: the radio connected, or Bluetooth past its gate. */
bool osc2_wireless_up(void);
/* Start or stop the running wireless transport (opensc2_start(), opensc2_stop()). */
int osc2_wireless_start(void);
int osc2_wireless_stop(void);
/* The link of transport `t` came up: October's rule saves the choice now (a flash write, in the
 * system work queue). */
void osc2_transport_link_up(uint8_t t);

/* storage.c (no-ops without CONFIG_OPENSC2_STORAGE) */
int osc2_storage_load(void);
int osc2_storage_save_bond(uint8_t slot);
/* Store transport `t` (opensc2/transport; for a puck transport also its slot as opensc2/slot,
 * which older firmware reads) and note it in osc2.transport_saved; delete both (the default). */
int osc2_storage_save_transport(uint8_t t);
int osc2_storage_delete_transport(void);
int osc2_storage_save_chime(void);
int osc2_storage_save_identity(void);
int osc2_storage_save_policy(void);
int osc2_storage_factory_reset(void);

/* radio.c */
#define OSC2_EV_START   BIT(0)
#define OSC2_EV_STOP    BIT(1)
#define OSC2_EV_PAIR    BIT(2)
#define OSC2_EV_CANCEL  BIT(3)
#define OSC2_EV_BOND    BIT(4)    /* a bond or the slot selection changed */
#define OSC2_EV_RX      BIT(5)    /* packets are waiting */

int osc2_radio_init(bool start);
void osc2_radio_post(uint32_t events);
void osc2_radio_set_pair_seconds(uint16_t seconds);
enum opensc2_link_state osc2_radio_state(void);
void osc2_radio_info(struct opensc2_link_info *info);
bool osc2_radio_connected(void);
/* The radio is off and ESB released (so another stack may take it). */
bool osc2_radio_idle(void);
/* Extra input reports for the next state reports. `droppable` (the battery report): dropped when
 * the queue holds CONFIG_OPENSC2_REPORT_QUEUE_DEPTH; the others wait for room behind them, up to
 * CONFIG_OPENSC2_REPORT_WAIT_DEPTH more. 0, or -ENOMEM when dropped. */
int osc2_radio_queue_report(const uint8_t *report, size_t len, bool droppable);
/* Reports wait for room in that queue: lizard mode waits with its next frame. */
bool osc2_radio_backlog(void);

/* commands.c: the vendor command channel, one stored request per link. */
void osc2_commands_reset(enum osc2_link link);
/* `rec` is {opcode, length, payload}. Returns 0 or a negative errno: the SET result code on the
 * radio, a STALL on USB. */
int32_t osc2_command_set(enum osc2_link link, const uint8_t *rec, uint8_t rec_len);
/* Radio: answer a GET record with the link's stored request (the last SET, 0x83 before any), into
 * `out` ({opcode, length, data}); returns the bytes written ({opcode, 0} when the command has no
 * answer). The record's own byte is not used: Valve's puck does not set it. */
uint8_t osc2_command_get(enum osc2_link link, uint8_t out[63]);
/* The same, with the command's own result in `*rc` (0, or a negative errno; -ENOTSUP when the
 * stored command has nothing to read), for the dock, which answers it. */
uint8_t osc2_command_get_rc(enum osc2_link link, uint8_t out[63], int *rc);
/* The dock: a GET answered in place over `out`, the link's previous answer, as the real controller
 * builds it: the stored request {opcode, length, data} over its start, then the command's answer
 * {opcode, n, data} over that; nothing else changes. 0, or the command's negative errno (-ENOTSUP:
 * nothing to read, and `out` is left as it was). */
int osc2_command_get_in_place(enum osc2_link link, uint8_t out[63]);
/* A link whose transport can refuse a read (Bluetooth): the stored request's GET as 63 bytes
 * {opcode, length, data, zeros}; returns 63, or a negative errno for a command without an answer
 * or a failed GET (an ATT error there, the counterpart of a STALL on USB). */
int osc2_command_get_strict(enum osc2_link link, uint8_t out[63]);
/* USB: the 64-byte data stage of SET_REPORT(Feature 1). 0, or a negative errno (STALL). */
int osc2_usb_feature_set(const uint8_t *data, uint16_t len);
/* USB: answer GET_REPORT(Feature 1) for the stored request, `reply_len` bytes cut to `wlength`.
 * Returns the bytes written, or a negative errno (STALL). `*opcode` and `*n` say what was
 * answered. */
int osc2_usb_feature_get(uint8_t *out, uint16_t wlength, uint8_t reply_len, uint8_t *opcode,
			 uint8_t *n);
/* A short, shareable description of a request for the session log: setting numbers and values,
 * key names, tags, the 0x95 key; never bond values or serials. */
void osc2_command_describe(const uint8_t *rec, uint8_t rec_len, char *buf, size_t size);
/* The stored request of `link` as a record {opcode, length, payload}; returns its length. */
uint8_t osc2_command_stored(enum osc2_link link, uint8_t rec[2 + OPENSC2_ANSWER_MAX]);
/* Exactly what one triplet of vendor command 0x87 does: clamped, with the side effects of
 * settings 9 (lizard mode) and 71 (the Steam watchdog), the led callback for 37 and 45. */
void osc2_settings_write(uint8_t number, int16_t value);
/* Exactly what vendor command 0xC5 does: the colour, each percentage clamped to 100. */
void osc2_led_color_write(const uint8_t rgbw[4]);

/* report.c: the state report, lizard mode, the battery report and output reports, shared by the
 * links. */
void osc2_report_init(void);
/* Build the next state report: sampled (the `sample` callback), the input rules (input_rules.h),
 * the shared sequence number. `s` gets the input state it was built from, for
 * osc2_report_lizard(). */
void osc2_report_state(uint8_t state[OPENSC2_STATE_REPORT_LEN], struct opensc2_state *s);
/* The application's "no rotation" (all four quaternion words 0) as the state reports carry it: the
 * identity, W = 32767. No real controller sends W = 0: July sent the identity, October its fused
 * rotation (and the last one while the stream is off). Applied to the application's state before
 * the test hooks' injection, which keeps its bytes. */
static inline void osc2_quat_none_to_identity(struct opensc2_state *s)
{
	if (s->imu.quat[0] == 0 && s->imu.quat[1] == 0 && s->imu.quat[2] == 0 &&
	    s->imu.quat[3] == 0) {
		s->imu.quat[0] = OPENSC2_QUAT_ONE;
	}
}
/* One lizard-mode frame on `s`, its reports sent on the link in use (after the state report), its
 * haptic events to the haptic callback. Lizard mode cannot turn on or off in between:
 * osc2_lizard_set() waits, so the reports of a frame never follow the release reports of a later
 * "off" (on USB the commands run in another thread than the frames). Through the puck the frame
 * waits (does not run) while reports wait for room in the radio's queue. */
void osc2_report_lizard(const struct opensc2_state *s);
/* The rule word in effect (input_rules.h): the build's (Kconfig) unless the hardware test hooks
 * set another while they are armed. */
uint32_t osc2_rules(void);
uint32_t osc2_rules_build(void);
void osc2_rules_set(uint32_t rules);
/* Lizard mode turns on or off (with the reports the real controller sends then), or only its two
 * switches change. */
void osc2_lizard_set(bool on);
void osc2_lizard_switches(bool up);
/* Lizard mode takes the inputs it sees next as its starting point (opensc2_lizard_resync()). */
void osc2_lizard_resync(void);
/* While lizard mode holds a key or button towards the host: release it (an empty keyboard report,
 * then an all-zero mouse report, sent at once). Returns whether it sent them. */
bool osc2_lizard_release_held(void);
/* A battery report now (nothing with CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS 0); the period
 * starts again from it. */
void osc2_report_battery_now(void);
/* Send an input report on the link in use. `droppable`: drop it when no buffer is free (state and
 * battery reports) instead of queueing it (lizard reports, the application's). */
int osc2_input_send(const uint8_t *report, size_t len, bool droppable);
/* An output report from the host on `link`. Returns what opensc2_decode_output() gave: the outputs
 * made, 0 for an ID that does nothing, -1 for a report too short. */
int osc2_output_dispatch(enum osc2_link link, const uint8_t *report, size_t len);
/* The link went down: stop a rumble the host can no longer stop. */
void osc2_output_link_down(void);

/* ble.c: Bluetooth LE (CONFIG_OPENSC2_BLE; without it wireless.c answers these two: never
 * active, interval 0). */
/* Bluetooth is the active transport: connected, encrypted, an ATT MTU of 64 or more. */
bool osc2_ble_active(void);
/* The connection interval in µs while Bluetooth is active (attribute 11 of 0x83), else 0. */
uint32_t osc2_ble_interval_us(void);
#if defined(CONFIG_OPENSC2_BLE)
#include "ble_rules.h"   /* struct osc2_ble_status */
/* The Bluetooth thread and report thread; nothing goes on the air yet. */
int osc2_ble_init(void);
/* Bluetooth on as transport `t` (0 or 1): the radio taken over from ESB first, then advertising.
 * With `pair` (a pairing request, the real controller's chord held 4 s) the slot's other identity
 * is reset and advertises in pairing mode. Asynchronous (the Bluetooth thread). */
int osc2_ble_start(uint8_t t, bool pair);
/* Bluetooth off: the zero report, then a disconnect, no advertising. `release` also disables the
 * stack so the puck's radio can have the radio; the radio is started after it when `then_radio`. */
int osc2_ble_stop(bool release, bool then_radio);
/* Bluetooth has the radio (its stack is enabled): ESB must not start. */
bool osc2_ble_owns_radio(void);
/* A pairing request on the running slot: a host connected is left, and the slot's other identity,
 * reset, advertises in pairing mode; the slot's bond stays until a new host has paired. */
int osc2_ble_pair(void);
/* An input report towards the host (osc2_input_send()). */
int osc2_ble_send(const uint8_t *report, size_t len, bool droppable);
/* The Bluetooth link's state in the terms of the radio's (opensc2_link_state()). */
void osc2_ble_info(struct opensc2_link_info *info);
/* For the shell and the test channel: what the Bluetooth link is doing. */
void osc2_ble_status(struct osc2_ble_status *st);
/* The factory reset (0x86): every key of the stack's `bt` settings subtree is deleted (bonds,
 * identities, the stored name), so the next start creates new identities; no advertising until
 * the reboot that follows. 0 or a negative errno. */
int osc2_ble_factory_reset(void);
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
/* The test channel's Bluetooth ops (BLE_STATUS, BLE_DROP, BLE_WINDOW): an answer status
 * (enum osc2_test_status), the result in `result` (`*rlen` bytes). */
uint8_t osc2_ble_test_op(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			 uint8_t *rlen);
#endif
#else
static inline bool osc2_ble_owns_radio(void)
{
	return false;
}
#endif

/* mode.c */
void osc2_mode_boot(enum opensc2_boot_mode app);
/* Start the USB side of this boot's mode: the wired device, or the console (and the plug-in rule)
 * when wireless. A wired start that fails falls back to wireless, with the radio started if
 * `start_radio`. */
void osc2_mode_start(bool start_radio);
/* opensc2_init() failed before the mode was decided: show the console anyway, for its log. */
void osc2_mode_init_failed(void);
bool osc2_usb_power(void);

#if defined(CONFIG_OPENSC2_USB)
/* usb.c: the wired device, and the debug console in wireless mode. A failed wired start ends its
 * log session and releases what it holds of the USB stack, so the console can start instead. The
 * console starts once; later calls do nothing. */
int osc2_usb_wired_start(void);
int osc2_usb_console_start(void);
/* Configured and not on the way out. */
bool osc2_usb_ready(void);
int osc2_usb_send(const uint8_t *report, size_t len, bool droppable);
/* Leave the bus as the real controller does: one all-zero state report, 10 ms, USB off. */
void osc2_usb_detach(const char *why);
void osc2_usb_get_info(struct opensc2_usb_info *info);

/* usblog.c: the retained area (mark and USB session log). */
int osc2_retained_mark_taken(void);              /* the mark found at boot (enum osc2_mark) */
void osc2_retained_mark_set(int mark);           /* enum osc2_mark, just before a reboot */
bool osc2_retained_was_kept(void);               /* the area survived the last reset */
/* The system machine's test words (retained.h): as found at boot, and set one or more (NULL: 0). */
void osc2_retained_sys_at_boot(uint32_t words[OSC2_RET_SYS_WORDS]);
void osc2_retained_sys_set(unsigned int first, const uint32_t *words, unsigned int n);
void osc2_usblog_start_session(void);
void osc2_usblog_end_session(const char *why);
void osc2_usblog_counts(uint32_t requests, uint32_t stalls, uint32_t outputs, uint32_t reports,
			uint32_t dropped);
void osc2_usblog_set_time_base(void);
void osc2_usblog_setup(const uint8_t setup[8]);  /* from the USB driver's thread: just copies */
void osc2_usblog_backend(bool on);               /* the Zephyr log into the session log */
__printf_like(1, 2) void osc2_usblog(const char *fmt, ...);
/* For the shell. */
struct osc2_usblog_info {
	bool kept;            /* the area was intact at this boot */
	uint32_t boots;
	uint32_t size, used;
	uint32_t session_boot, requests, stalls, outputs, reports, dropped;
	char end[OSC2_RET_END_LEN];   /* as long as the retained one: the whole reason */
};
void osc2_usblog_info(struct osc2_usblog_info *info);
void osc2_usblog_lines(void (*cb)(const char *line, size_t len, void *ctx), void *ctx);
void osc2_usblog_clear(void);
#else
static inline void osc2_usblog(const char *fmt, ...)
{
	ARG_UNUSED(fmt);
}
#endif

#endif /* OPENSC2_INTERNAL_H_ */
