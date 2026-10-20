/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * system.h: where the rest of the library meets the system state machine (system.c,
 * CONFIG_OPENSC2_SYSTEM). Each call is a note ("this happened") or a question ("is this power
 * request yours?"). With the option off every one is an empty inline function or answers as
 * v0.2.0 did, so the call sites need no #if. Not a public interface.
 */
#ifndef OPENSC2_SYSTEM_H_
#define OPENSC2_SYSTEM_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <opensc2/opensc2.h>

/* The Bluetooth transports (slots 0 and 1 of user/wireless_transport) plug in here: a build with
 * Bluetooth hands its functions over once at start-up; without one the Bluetooth chords do not
 * exist and a stored Bluetooth transport starts the main puck instead. */
struct osc2_bt_transport {
	int (*start)(uint8_t slot, bool pairing);   /* advertise for slot 0 or 1 (pairing: the
						      * chord was held 4 s) */
	void (*stop)(void);                         /* drop the link (zero report, 100 ms, off) */
	bool (*connected)(void);
	bool (*pairing)(void);                      /* advertising in pairing mode */
};

/* core.c: the application's own button word. */
uint32_t osc2_sys_app_buttons(void);

#if defined(CONFIG_OPENSC2_SYSTEM)

/* opensc2_init(), last: the power-on decision (in the system work queue). `start`: the
 * application lets the library start the link (opensc2_config.start). */
void osc2_sys_init(bool start);

/* The button word the rules see: the application's, or while the test hooks inject one into the
 * system rules, that one (never what INJECT puts into the reports). */
uint32_t osc2_sys_buttons(void);
/* The application changed its buttons (any setter). */
void osc2_sys_buttons_changed(void);
/* The application set its battery (opensc2_set_battery()): from now on its level counts. */
void osc2_sys_battery_given(void);
/* Every battery report, with the application's values (before the test hooks' injection). */
void osc2_sys_battery_report(const struct opensc2_battery *battery);
/* After each state report is built, with the state it was built from: analog activity for the
 * idle monitor. Any thread; cheap. */
void osc2_sys_frame(const struct opensc2_state *state);
/* A controller setting changed (OPENSC2_SETTINGS_ALL: every one). */
void osc2_sys_setting(uint8_t number);
/* The radio link changed state (radio.c), in the radio thread. */
void osc2_sys_link_changed(uint8_t state, uint8_t reason);
/* The puck says the PC is asleep (radio packet 0xE6). */
void osc2_sys_host_asleep(void);
/* Wired: the USB device changed state. */
void osc2_sys_usb_changed(const struct opensc2_usb_info *info);
/* A power request is about to go to the application (osc2_power_now(), the system work queue):
 * true when the machine took it (the turn-off command, the cable pulled), or a host's reboot
 * request the current state does not act on. */
bool osc2_sys_power_request(enum opensc2_power_request request, const char *why);
/* USB power counts for this boot's wired-or-wireless decision (the chip's VBUS detection, unless
 * the power source in effect is a battery or the dock). */
bool osc2_sys_wired_power(void);
/* A cue, from any thread (it plays in the system work queue). */
void osc2_sys_cue_post(uint8_t cue);
/* What a factory reset deletes of the machine's keys (the keylock, the fully-charged LED level). */
void osc2_sys_factory_reset(void);
/* The settings-by-name commands on the machine's keys (user/keylock,
 * user/fully_charged_led_intensity). 0xEE (`opcode`), 0xEF, 0xF0: 0 or a negative errno, -ENOENT
 * for another key. */
int osc2_sys_key(uint8_t opcode, const uint8_t *payload, uint8_t len);
/* 0xED: 0 with the value (length 0 for none), or -ENOENT for another key. */
int osc2_sys_key_read(const uint8_t *payload, uint8_t len, uint8_t *out, uint8_t *out_len);
/* A Bluetooth transport (once, at start-up; NULL: none). */
void osc2_sys_bluetooth(const struct osc2_bt_transport *bt);

#if defined(CONFIG_OPENSC2_SHELL)
struct shell;
/* "opensc2 power": the machine's state. */
int osc2_sys_shell(const struct shell *sh, size_t argc, char **argv);
#endif

#else /* !CONFIG_OPENSC2_SYSTEM */

static inline void osc2_sys_init(bool start)
{
	(void)start;
}

static inline uint32_t osc2_sys_buttons(void)
{
	return osc2_sys_app_buttons();
}

static inline void osc2_sys_buttons_changed(void)
{
}

static inline void osc2_sys_battery_given(void)
{
}

static inline void osc2_sys_battery_report(const struct opensc2_battery *battery)
{
	(void)battery;
}

static inline void osc2_sys_frame(const struct opensc2_state *state)
{
	(void)state;
}

static inline void osc2_sys_setting(uint8_t number)
{
	(void)number;
}

static inline void osc2_sys_link_changed(uint8_t state, uint8_t reason)
{
	(void)state;
	(void)reason;
}

static inline void osc2_sys_host_asleep(void)
{
}

static inline void osc2_sys_usb_changed(const struct opensc2_usb_info *info)
{
	(void)info;
}

static inline bool osc2_sys_power_request(enum opensc2_power_request request, const char *why)
{
	(void)request;
	(void)why;
	return false;
}

bool osc2_usb_power(void);   /* mode.c */

static inline bool osc2_sys_wired_power(void)
{
	return osc2_usb_power();
}

static inline void osc2_sys_cue_post(uint8_t cue)
{
	(void)cue;
}

static inline void osc2_sys_factory_reset(void)
{
}

static inline int osc2_sys_key(uint8_t opcode, const uint8_t *payload, uint8_t len)
{
	(void)opcode;
	(void)payload;
	(void)len;
	return -ENOENT;
}

static inline int osc2_sys_key_read(const uint8_t *payload, uint8_t len, uint8_t *out,
				    uint8_t *out_len)
{
	(void)payload;
	(void)len;
	(void)out;
	(void)out_len;
	return -ENOENT;
}

static inline void osc2_sys_bluetooth(const struct osc2_bt_transport *bt)
{
	(void)bt;
}

#endif /* CONFIG_OPENSC2_SYSTEM */

#endif /* OPENSC2_SYSTEM_H_ */
