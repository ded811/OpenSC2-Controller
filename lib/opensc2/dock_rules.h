/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * dock_rules.h: the controller's end of the dock as pure rules (no kernel; the host tests build
 * them), from the protocol documentation (not public yet; docs/protocol.md lists the pages):
 *   - the answer to one received frame (the session's core, DCK-5 to DCK-12);
 *   - the pilot check: the window and the 2-of-3 rule (DCK-3);
 *   - the supply rule: dock power or USB power (DCK-14);
 *   - the docked states: on the dock with the radio off, a chord, the link up, lifted off
 *     (DCK-15 to DCK-17).
 * The library's glue (dock_link.c, dock.c) runs them; where OpenSC2 chose something the
 * documentation leaves open, the comment says so and docs/protocol.md's gap list has it.
 */
#ifndef OPENSC2_DOCK_RULES_H_
#define OPENSC2_DOCK_RULES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <opensc2/dock.h>
#include <opensc2/codec.h>

/* ---- one frame in, one answer out --------------------------------------------------------------
 * A GET of a command that has nothing to read answers -95 (EOPNOTSUPP, the value the real
 * controller sends; a wire value, not this platform's errno); an opcode nobody knows, or a length
 * above 61, -22. */
#define OSC2_DOCK_RESULT_GET_OK            63
#define OSC2_DOCK_RESULT_NOTHING_TO_READ   (-95)
#define OSC2_DOCK_RESULT_INVALID           (-22)

/* What the answer needs from the rest of the library. Each op writes over the previous answer's
 * bytes in place and leaves the rest as they are. */
struct osc2_dock_ops {
	/* The 57-byte identity record (the probe's answer), over the payload's first 57 bytes. */
	void (*identity)(void *ctx, uint8_t out[OPENSC2_IDENTITY_RECORD_LEN]);
	/* Run the SET {opcode, length, data} (all 63 payload bytes): 0, or a negative error as the
	 * dock answers it. The answer's payload stays the previous answer's. */
	int32_t (*set)(void *ctx, const uint8_t rec[OPENSC2_DOCK_PAYLOAD_LEN]);
	/* Answer the dock's stored request in `out`, which holds the previous answer's payload: the
	 * stored request {opcode, length, data} over its start, then the command's answer {opcode,
	 * n, data} over that; nothing else changes. Returns the result: OSC2_DOCK_RESULT_GET_OK, or
	 * a negative error (OSC2_DOCK_RESULT_NOTHING_TO_READ leaves `out` as it was). */
	int32_t (*get)(void *ctx, uint8_t out[OPENSC2_DOCK_PAYLOAD_LEN]);
	/* The state report 0x42 without its ID: 53 bytes over the payload's first 53. */
	void (*input)(void *ctx, uint8_t out[OPENSC2_STATE_REPORT_LEN - 1]);
};

enum osc2_dock_outcome {
	OSC2_DOCK_ANSWERED = 0,
	OSC2_DOCK_BAD_SIZE = 1,   /* fewer than 73 bytes before the gap timeout: the session ends */
	OSC2_DOCK_BAD_CRC = 2,    /* the session ends */
};

/* Handle one received frame of `len` bytes and build the answer in `ans`, as the real controller
 * does: in place, over its previous answer (`ans` holds it on entry; zeros after a boot). Only
 * what the request defines is written, then the start byte 0xAC and the CRC:
 *   - a probe: the identity record over the payload's first 57 bytes; the type, the result and
 *     the payload's last 6 bytes stay the previous answer's;
 *   - a GET: the type, the result and the payload as the `get` op leaves it;
 *   - a SET: the type and the result; the payload stays the previous answer's;
 *   - GET input: the type, result 0 and the report's 53 bytes; the last 10 stay;
 *   - any other type: nothing, so the previous answer goes out again byte for byte.
 * Bytes 2 and 3 are never written. A bad size or CRC leaves `ans` alone (no answer goes out). */
enum osc2_dock_outcome osc2_dock_answer(const uint8_t *req, size_t len,
					uint8_t ans[OPENSC2_DOCK_FRAME_LEN],
					const struct osc2_dock_ops *ops, void *ctx);

/* ---- the pilot --------------------------------------------------------------------------------
 * Read on an analog pin with no divider: gain 1/6 of the 0.6 V internal reference, 12 bits, so
 * 3600 mV full scale. The line counts as present while a reading lies inside 2000 to 3300 mV,
 * both ends included; a failed conversion counts as outside. */
#define OSC2_DOCK_PILOT_MIN_MV    2000
#define OSC2_DOCK_PILOT_MAX_MV    3300
#define OSC2_DOCK_PILOT_READS     3      /* one check: three readings ... */
#define OSC2_DOCK_PILOT_READ_MS   3      /* ... with 3 ms after each */
#define OSC2_DOCK_PILOT_IDLE_MS   10     /* between checks that fail */

/* A 12-bit count at that gain to mV at the pin: count x 3600 / 4096 (a negative count, a failed
 * conversion, stays negative). */
int32_t osc2_dock_pilot_mv(int32_t count);
bool osc2_dock_pilot_in_window(int32_t mv);
/* One check: present when at least two of the three readings are inside the window. `*inside`
 * (may be NULL) gets one bit per reading inside. */
bool osc2_dock_pilot_present(const int32_t mv[OSC2_DOCK_PILOT_READS], uint8_t *inside);

/* ---- the supply rule ---------------------------------------------------------------------------
 * The real controller has one supply input, which the dock's + and the USB-C port share, and a
 * fresh pilot check tells the two apart at the moment that input changes: a supply that appears
 * with the pilot inside its window is the dock, any other is USB. An event is posted for the first
 * reading and whenever "a supply is present" changes, so a change between USB and the dock with
 * power present all along posts nothing. A board may have the + on the chip's VBUS (as the real
 * controller) or on a sense input of its own: "a supply is present" is either. */
enum osc2_dock_supply {
	OSC2_DOCK_SUPPLY_NONE = 0,   /* also the real controller's event numbers 0, 1, 2 */
	OSC2_DOCK_SUPPLY_USB = 1,
	OSC2_DOCK_SUPPLY_DOCK = 2,
};

struct osc2_dock_power_in {
	bool usb;     /* power on the board's USB input (VBUS) */
	bool plus;    /* the dock's + on a sense input of its own (false where it reaches VBUS) */
	bool pilot;   /* a pilot check made now passes (needed only when the reading posts) */
};

struct osc2_dock_power_rule {
	bool reported;                 /* the first reading has been made */
	bool present;
	enum osc2_dock_supply kind;    /* decided when the supply appeared */
};

/* The kind of the supply in `in`: none without power; with power the dock when the pilot passes,
 * else USB (both at once is the dock, as on the real controller). */
enum osc2_dock_supply osc2_dock_supply_kind(const struct osc2_dock_power_in *in);
/* Will this reading post (the first one, or "a supply is present" changed)? Only then does the
 * pilot need checking: the caller checks it, then steps. */
bool osc2_dock_power_posts(const struct osc2_dock_power_rule *rule, bool usb, bool plus);
/* A new reading: the event to post (OSC2_DOCK_SUPPLY_*), or -1 for none. */
int osc2_dock_power_step(struct osc2_dock_power_rule *rule, const struct osc2_dock_power_in *in);

/* ---- the docked states -------------------------------------------------------------------------
 * The part of the real controller's state machine that is about the dock, with events numbered
 * for this module. It decides; the glue does what the actions say (radio, LED, cues, timers,
 * reboot, power-off), and the rest of the state machine (battery, USB) is outside it: AWAY. */
enum osc2_dock_state {
	OSC2_DOCK_AWAY = 0,        /* not in a dock state (battery or USB power) */
	OSC2_DOCK_RADIO_OFF = 1,   /* on the dock, the radio off, charging */
	OSC2_DOCK_LIFTED = 2,      /* lifted off with the radio off: the 5 s debounce */
	OSC2_DOCK_CHORD = 3,       /* on the dock, a transport chord pressed: up to 4 s */
	OSC2_DOCK_LINK_UP = 4,     /* on the dock with the link up */
};

enum osc2_dock_event {
	OSC2_DOCK_EV_POWER_GONE = 0,    /* the supply events 0, 1, 2 */
	OSC2_DOCK_EV_POWER_USB = 1,
	OSC2_DOCK_EV_POWER_DOCK = 2,
	OSC2_DOCK_EV_CHORD = 3,         /* a transport chord pressed (which one: `chord`) */
	OSC2_DOCK_EV_CHORD_UP = 4,      /* that chord released */
	OSC2_DOCK_EV_TIMER = 5,         /* the state's timer: 4 s in a chord, 5 s lifted off */
	OSC2_DOCK_EV_STEAM_7S = 6,      /* STEAM held 7 s */
	OSC2_DOCK_EV_LINK_OFF = 7,      /* the link asks to turn off (the puck's 0xE5, a host
					 * leaving) */
	OSC2_DOCK_EV_TURN_OFF = 8,      /* 0x9F, or 0x95 with the turn-off key */
	OSC2_DOCK_EV_WARM_REBOOT = 9,   /* 0x95 with 0xA427AF52 */
	OSC2_DOCK_EV_REBOOT = 10,       /* 0x95 with another key */
	OSC2_DOCK_EV_BOOTLOADER = 11,   /* 0x90, 0x95 with the bootloader key */
	OSC2_DOCK_EV_OTHER = 12,        /* STEAM pressed, the idle timeout, a battery level, the
					 * keylock chord: every dock state ignores them */
	OSC2_DOCK_EV_COUNT,
};

/* The transports a chord picks. */
enum osc2_dock_chord {
	OSC2_DOCK_CHORD_CURRENT = 0,   /* STEAM: the stored transport */
	OSC2_DOCK_CHORD_BT1 = 1,       /* B + RB: Bluetooth slot 1 */
	OSC2_DOCK_CHORD_BT2 = 2,       /* B + LB: Bluetooth slot 2 */
	OSC2_DOCK_CHORD_PUCK1 = 3,     /* A + RB: the puck, bond 1 */
	OSC2_DOCK_CHORD_PUCK2 = 4,     /* A + LB: a second puck, bond 2 */
	OSC2_DOCK_CHORDS,
};

/* How the controller came onto the dock (the transitions into the dock states from outside). */
enum osc2_dock_entry {
	OSC2_DOCK_AT_POWER_ON = 0,     /* the first supply report of a boot was the dock */
	OSC2_DOCK_IN_BATTERY_WAIT = 1, /* docked within the first second of a battery power-on */
	OSC2_DOCK_ON_BATTERY = 2,      /* placed on the dock while running on battery */
};

/* What a step asks the glue to do. */
#define OSC2_DOCK_DO_STOP_LINK      (1u << 0)   /* switch off the link selected since boot */
#define OSC2_DOCK_DO_START_LINK     (1u << 1)   /* start the chosen transport ... */
#define OSC2_DOCK_DO_PAIRING        (1u << 2)   /* ... with the pairing flag (a chord held 4 s) */
#define OSC2_DOCK_DO_SELECT         (1u << 3)   /* the chord's transport becomes the choice */
#define OSC2_DOCK_DO_CUE            (1u << 4)   /* the transport's cue */
#define OSC2_DOCK_DO_CHIME_OFF      (1u << 5)   /* the power-off chime */
#define OSC2_DOCK_DO_TIMER_4S       (1u << 6)
#define OSC2_DOCK_DO_TIMER_5S       (1u << 7)
#define OSC2_DOCK_DO_TIMER_STOP     (1u << 8)
#define OSC2_DOCK_DO_PAUSE          (1u << 9)   /* take the next event 500 ms later */
#define OSC2_DOCK_DO_REBOOT         (1u << 10)  /* reboot at once, silently */
#define OSC2_DOCK_DO_SHUTDOWN       (1u << 11)  /* turn off silently once STEAM is up */
#define OSC2_DOCK_DO_UPDATE_WINDOW  (1u << 12)  /* open the 60 s firmware-update window */
#define OSC2_DOCK_DO_TO_BATTERY     (1u << 13)  /* leave the dock states: on battery, link up */
#define OSC2_DOCK_DO_TO_CHORD       (1u << 14)  /* leave the dock states: this chord on battery */
#define OSC2_DOCK_DO_LED_CHARGING   (1u << 15)  /* LED: the charging pattern (amber) */
#define OSC2_DOCK_DO_LED_TRANSPORT  (1u << 16)  /* LED: the transport's pattern, external power */
#define OSC2_DOCK_DO_LED_COLOUR     (1u << 17)  /* LED: the chord's transport colour, steady */
#define OSC2_DOCK_DO_LED_OFF        (1u << 18)
#define OSC2_DOCK_DO_KEYLOCK_CLEAR  (1u << 19)  /* a power-on on the dock deletes the keylock */
#define OSC2_DOCK_DO_OFFSETS_ON     (1u << 20)  /* the docked stick offsets on (entering radio-off
						 * or link-up) */
#define OSC2_DOCK_DO_OFFSETS_OFF    (1u << 21)  /* ... off (lifted off) */

struct osc2_dock_sm {
	enum osc2_dock_state state;
	uint8_t chord;          /* the chord of OSC2_DOCK_CHORD */
	bool link_selected;     /* a transport was chosen or started since boot */
};

struct osc2_dock_step {
	enum osc2_dock_state from, to;
	uint32_t actions;       /* OSC2_DOCK_DO_* */
	bool handled;           /* false: the state ignores the event */
};

void osc2_dock_sm_init(struct osc2_dock_sm *sm);
/* The controller comes onto the dock from outside the dock states; `warm`: this boot is a warm
 * restart (only AT_POWER_ON uses it). */
void osc2_dock_sm_enter(struct osc2_dock_sm *sm, enum osc2_dock_entry how, bool warm,
			struct osc2_dock_step *out);
/* One event; `chord` for OSC2_DOCK_EV_CHORD and _CHORD_UP. Outside the dock states (AWAY) every
 * event is ignored: the caller enters them with osc2_dock_sm_enter(). */
void osc2_dock_sm_step(struct osc2_dock_sm *sm, enum osc2_dock_event ev, uint8_t chord,
		       struct osc2_dock_step *out);

#endif /* OPENSC2_DOCK_RULES_H_ */
