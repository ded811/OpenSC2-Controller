/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * sys_rules.h: the real controller's system state machine, as pure logic (the host tests build
 * it): the power-on decision, the states on USB power, on the dock and on battery, the chord
 * states, the shutdowns, which LED mode each state shows and which cue it plays. Written from the
 * protocol documentation's description of the machine (not public yet; docs/protocol.md lists the
 * pages), in OpenSC2's own structure: a state, one event at a time, and a record of what to do.
 * system.c feeds it (buttons through chord_rules.h, the idle monitor, battery levels, link and USB
 * events, power requests) and carries the actions out.
 *
 * Where OpenSC2 differs, on purpose (each is in docs/protocol.md's gap list):
 *   - the wired or wireless decision stays with mode_rules.h (made before the machine starts), so
 *     USB power that appears during the battery wait, or after the board was lifted off the dock,
 *     reboots: the next boot is wired, where the real controller enables USB in place;
 *   - the wired start's STEAM rule stays in usb.c (protocol.md gap 18);
 *   - "drop the link" also stops a radio that is still listening (the real controller lets it
 *     search on until its own timer ends);
 *   - the link started from outside the machine (the shell's `opensc2 start`, the application)
 *     while wireless is off on USB power counts as wireless on again;
 *   - a radio stopped locally (opensc2_stop()) is not "wireless off": the machine only hears of a
 *     link that gave up or was turned off by the puck.
 */
#ifndef OPENSC2_SYS_RULES_H_
#define OPENSC2_SYS_RULES_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/opensc2.h>
#include "chord_rules.h"
#include "transport_rules.h"

/* What powers the board, as the machine sees it. */
enum osc2_sys_source {
	OSC2_SRC_BATTERY = 0,   /* no input power */
	OSC2_SRC_USB = 1,       /* USB power, or another supply that is not the dock */
	OSC2_SRC_DOCK = 2,      /* the puck's dock contacts */
};

/* Transports, as user/wireless_transport numbers them (transport_rules.h). */
#define OSC2_TRANSPORT_BT1    OSC2_TRANSPORT_BLE_1
#define OSC2_TRANSPORT_BT2    OSC2_TRANSPORT_BLE_2
#define OSC2_TRANSPORT_PUCK1  OSC2_TRANSPORT_PUCK_1
#define OSC2_TRANSPORT_PUCK2  OSC2_TRANSPORT_PUCK_2

/* LED modes (the values of enum opensc2_led_mode). */
#define OSC2_LED_NONE         0
#define OSC2_LED_BLUETOOTH    1
#define OSC2_LED_PUCK         2
#define OSC2_LED_WIRED        3
#define OSC2_LED_AMBER        4
#define OSC2_LED_OFF          5
#define OSC2_LED_LOW_BATTERY  6
#define OSC2_LED_KEYLOCK      7
/* LED flags. */
#define OSC2_LED_EXTERNAL     0x01   /* modes 1 and 2: shown as on USB power or the dock */
#define OSC2_LED_STEADY       0x02   /* a chord state: the transport's colour, steady */

/* Cues (the values of enum opensc2_cue). */
#define OSC2_CUE_POWER_ON     0
#define OSC2_CUE_POWER_OFF    5
#define OSC2_CUE_BT_PAIRING   6
#define OSC2_CUE_BT_PAIRED    7
#define OSC2_CUE_HOST_ASLEEP  10
#define OSC2_CUE_COUNT        11

/* Timers. */
#define OSC2_SYS_BATTERY_WAIT_MS  1000
#define OSC2_SYS_NO_HOST_MS       500
#define OSC2_SYS_CHORD_HOLD_MS    4000
#define OSC2_SYS_CHORD_PAUSE_MS   500
#define OSC2_SYS_LIFTED_MS        5000
#define OSC2_SYS_SHUTDOWN_MS      500
#define OSC2_SYS_STEAM_POLL_MS    10

enum osc2_sys_state {
	OSC2_SYS_START = 0,         /* power-on, until the decision */
	OSC2_SYS_BATTERY_WAIT,      /* on battery: the stored transport's cue, 1 s for USB or the dock */
	OSC2_SYS_WIRED_WAIT,        /* wired, no host has configured the device yet */
	OSC2_SYS_WIRED,             /* wired, configured by a host */
	OSC2_SYS_USB_LINK_ON,       /* USB power, the wireless link on */
	OSC2_SYS_USB_LINK_OFF,      /* USB power, wireless switched off */
	OSC2_SYS_DOCK_OFF,          /* on the dock, the radio off */
	OSC2_SYS_LIFTED,            /* lifted off the dock with the radio off: 5 s */
	OSC2_SYS_DOCK_CHORD,        /* a transport chord on the dock */
	OSC2_SYS_DOCK_ON,           /* on the dock, the link on */
	OSC2_SYS_CHORD,             /* a transport chord at a battery power-on */
	OSC2_SYS_BATTERY,           /* running on battery */
	OSC2_SYS_SHUTDOWN,          /* the power-off chime, 0.5 s, STEAM up, off (setting 77: reboot) */
	OSC2_SYS_SHUTDOWN_SILENT,   /* off once STEAM is up */
	OSC2_SYS_SHUTDOWN_LOW,      /* the chime, the low-battery LED, 0.5 s, off */
	OSC2_SYS_SHUTDOWN_LOCKED,   /* the keylock LED, 0.5 s, off */
	OSC2_SYS_REBOOT,            /* reboot at once */
	OSC2_SYS_STATES,
};

enum osc2_sys_ev {
	OSC2_SYS_EV_POWER_NONE = 0,   /* input power gone */
	OSC2_SYS_EV_POWER_USB,        /* input power that is not the dock */
	OSC2_SYS_EV_POWER_DOCK,       /* dock power */
	OSC2_SYS_EV_CONFIGURED,       /* a host configured the wired device */
	OSC2_SYS_EV_STEAM_HELD,       /* STEAM alone held 7 s */
	OSC2_SYS_EV_STEAM_PRESSED,    /* STEAM alone held 20 ms */
	OSC2_SYS_EV_IDLE,             /* the idle monitor */
	OSC2_SYS_EV_WIRELESS_OFF,     /* the link gave up, or the puck turned it off */
	OSC2_SYS_EV_LEVEL,            /* a battery level (the argument) */
	OSC2_SYS_EV_KEYLOCK_HELD,     /* the keylock chord held 3 s */
	OSC2_SYS_EV_CHORD_DOWN,       /* a transport chord pressed (the argument: enum osc2_chord) */
	OSC2_SYS_EV_CHORD_UP,         /* released */
	OSC2_SYS_EV_TIMER,            /* the state timer, or the 10 ms look at STEAM while waiting */
	OSC2_SYS_EV_BOOTLOADER,       /* 0x90, 0x95 with the bootloader key */
	OSC2_SYS_EV_REBOOT,           /* 0x95 with another key */
	OSC2_SYS_EV_WARM_REBOOT,      /* 0x95 with the warm key */
	OSC2_SYS_EV_TURN_OFF,         /* 0x9F, 0x95 with the turn-off key */
	OSC2_SYS_EV_STEAM_UP,         /* the STEAM bit went up */
	OSC2_SYS_EV_LINK_STARTED,     /* OpenSC2: the link was started from outside the machine */
	OSC2_SYS_EV_UNPLUGGED,        /* OpenSC2: the wired device lost USB power */
	OSC2_SYS_EVENTS,
};

/* How a shutdown or reboot state ends. */
enum osc2_sys_end {
	OSC2_SYS_END_NONE = 0,
	OSC2_SYS_END_POWER_OFF,     /* power off */
	OSC2_SYS_END_REBOOT,        /* a plain reboot (by the boot rule: wired on USB power) */
};

/* Actions, as bits of osc2_sys_out.actions, in the order system.c carries them out. */
#define OSC2_SYS_DO_STATE         0x0001   /* the state changed: from, to, cause */
#define OSC2_SYS_DO_IR_OFF        0x0002   /* the IR LED off (its setting stays) */
#define OSC2_SYS_DO_LED           0x0004   /* a new LED mode: led_mode, led_flags */
#define OSC2_SYS_DO_DROP          0x0008   /* drop the link */
#define OSC2_SYS_DO_CUE           0x0010   /* play cue `cue` */
#define OSC2_SYS_DO_KEYLOCK_DEL   0x0020   /* delete the keylock */
#define OSC2_SYS_DO_KEYLOCK_SET   0x0040   /* store the keylock (it counts from the next boot) */
#define OSC2_SYS_DO_START         0x0080   /* start transport `transport` (`pairing`) */
#define OSC2_SYS_DO_END           0x0100   /* end: `end` */
#define OSC2_SYS_DO_PASS          0x0200   /* a host request this state acts on: carry it out */

struct osc2_sys {
	uint8_t state;
	uint8_t chosen;          /* the machine's transport, OSC2_TRANSPORT_NONE before a choice */
	uint8_t chord;           /* in a chord state: its chord (enum osc2_chord) */
	uint8_t level;           /* the last battery level (255: none yet) */
	bool timer_on;
	bool steam_wait;         /* a shutdown that waits for STEAM to be up */
	bool ended;              /* power off or reboot asked for: nothing more happens */
	uint32_t timer_at;
	uint32_t busy_until;     /* a chord state's 500 ms pause: events wait until then */
	bool busy;
	/* how this boot started, for the test hooks' status */
	uint8_t boot_state;
	uint8_t boot_cue;        /* 0xFF: none */
	uint8_t boot_chord;
	bool boot_pairing;       /* a chord state ended with the pairing flag */
	/* the shutdown in progress, for the ring */
	uint8_t end_cause;
};

/* What the machine reads when it handles an event. */
struct osc2_sys_in {
	uint8_t stored;          /* user/wireless_transport (0..3; other values: the main puck) */
	uint8_t chord;           /* the chord matching now (enum osc2_chord) */
	uint8_t level;           /* the battery level the machine is told (255: none yet) */
	bool steam;              /* the STEAM bit is set now */
	bool keylock;            /* the keylock is stored */
	bool reboot_not_off;     /* controller setting 77 is not 0 */
	bool link_up;            /* a wireless link is connected */
	bool plug_in_reboot;     /* USB power on battery may reboot into wired (the rule's options) */
};

/* How the board started. */
struct osc2_sys_boot_in {
	bool wired;              /* this boot is wired (mode_rules.h) */
	bool warm;               /* the warm-restart mark brought it back */
	uint8_t source;          /* enum osc2_sys_source */
};

struct osc2_sys_out {
	uint16_t actions;        /* OSC2_SYS_DO_* */
	uint8_t from, to;        /* DO_STATE: the first state left and the last one entered */
	uint8_t cause;           /* the event (DO_STATE) */
	uint8_t led_mode;        /* DO_LED */
	uint8_t led_flags;
	uint8_t cue;             /* DO_CUE */
	uint8_t transport;       /* DO_START */
	bool pairing;
	uint8_t end;             /* DO_END: enum osc2_sys_end */
	uint8_t end_state;       /* DO_END: the shutdown or reboot state that ends */
	uint8_t end_cause;       /* DO_END: the event that led into it */
};

/* The power-on decision. In order: a wired boot (the wired start, cue 0); wireless on USB power
 * (the stored transport's cue, the link started); on the dock (the stored link after a warm
 * reboot, else the radio off); on battery: 0 % off silently, the keylock off, 2 % or less off with
 * the low-battery signal, a held transport chord its chord state, otherwise the battery wait.
 * USB and dock power-ons delete the keylock. */
void osc2_sys_boot(struct osc2_sys *m, const struct osc2_sys_in *in,
		   const struct osc2_sys_boot_in *boot, uint32_t now_ms, struct osc2_sys_out *out);

/* One event (`arg`: the level of OSC2_SYS_EV_LEVEL, the chord of CHORD_DOWN and CHORD_UP). Call
 * it only when osc2_sys_wait() says the machine takes events; a TIMER that is not due, and any
 * event in a state that does not act on it, do nothing. */
void osc2_sys_event(struct osc2_sys *m, const struct osc2_sys_in *in, uint8_t ev, uint8_t arg,
		    uint32_t now_ms, struct osc2_sys_out *out);

/* Milliseconds until the machine takes events again (a chord state's pause), 0 when it does. */
uint32_t osc2_sys_wait(const struct osc2_sys *m, uint32_t now_ms);

/* Milliseconds until a TIMER is due (0: now), or -1 for none. While a shutdown waits for STEAM
 * to be up: every 10 ms. */
int32_t osc2_sys_next(const struct osc2_sys *m, uint32_t now_ms);

/* The event a change of the power source means, or -1: only a change of "power present" counts,
 * so USB to the dock with power all along means nothing. */
int osc2_sys_source_event(uint8_t before, uint8_t after);

/* The transport a chord selects; for STEAM alone the stored one, a stored value above 3 meaning
 * the main puck. */
uint8_t osc2_sys_chord_transport(uint8_t chord, uint8_t stored);

/* The cue that announces a transport: Bluetooth slot 1 cue 1, slot 2 cue 2, the main puck cue 3,
 * the second puck cue 4; 0xFF for a stored value above 3 (no cue). */
uint8_t osc2_sys_transport_cue(uint8_t transport);

/* A cue at the chime level `level` (user/haptic_boot_level: 1 quiet, 2 normal, anything else
 * silent): its built-in script and gain. False when it plays nothing (silent, or no such cue).
 * Cues 0, 1 and 3 play script 1, 2 and 4 script 2, 5 script 5, 6 and 7 scripts 8 and 9 one dB
 * below the level, 9 script 3, 10 script 4; 8 plays script 10 at +2 dB whatever the level. */
bool osc2_cue_map(uint8_t cue, uint8_t level, uint8_t *script, int8_t *gain_db);

/* The level the machine and the LED are told: 100 while the charge state has the "supply check
 * gave up" flag (0x80) and the level is above 0, else the level. */
uint8_t osc2_sys_level(uint8_t charge_state, uint8_t level);

/* Names for logs and the console. */
const char *osc2_sys_state_name(uint8_t state);
const char *osc2_sys_event_name(uint8_t ev);

#endif /* OPENSC2_SYS_RULES_H_ */
