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

/* What the Bluetooth transport tells the machine, osc2_sys_bt_event(); only while it is the
 * transport in use (started through osc2_bt_transport.start). */
#define OSC2_SYS_BT_GAVE_UP  1   /* advertising ended without a host, or the host ended the link:
				  * "wireless off" (on battery a shutdown) */
#define OSC2_SYS_BT_PAIRING  2   /* pairing-mode advertising started: cue 6 */
#define OSC2_SYS_BT_PAIRED   3   /* a new host has paired (the link is encrypted): cue 7 */
#define OSC2_SYS_BT_STARTED  4   /* the link left OFF (advertising or a host): wireless on again */

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
/* For each state report, with the application's state (before the test hooks' report
 * injection): analog activity for the idle monitor. Any thread; cheap. */
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
/* A Bluetooth transport (once, at start-up; NULL: none). */
void osc2_sys_bluetooth(const struct osc2_bt_transport *bt);
/* What the Bluetooth transport tells the machine (any thread): OSC2_SYS_BT_*. */
void osc2_sys_bt_event(uint8_t event);

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

static inline void osc2_sys_bluetooth(const struct osc2_bt_transport *bt)
{
	(void)bt;
}

static inline void osc2_sys_bt_event(uint8_t event)
{
	(void)event;
}

#endif /* CONFIG_OPENSC2_SYSTEM */

#endif /* OPENSC2_SYSTEM_H_ */
