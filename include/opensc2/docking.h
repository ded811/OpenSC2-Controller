/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * docking.h: the library's dock at run time (docs/api.md, "The dock"): what a board supplies for
 * it, what it can read back, and the way in for a board that carries the dock's frames on a wire
 * of its own. The frames themselves are <opensc2/dock.h> (pure functions).
 *
 * A Steam Controller 2 rests on Valve's puck to charge. The puck powers it through its `+` and `-`
 * pads, signals itself on its `P` pad (the "pilot", which the controller reads as a voltage), and
 * talks to it over a slow serial link on the same contacts. The real controller:
 *   - checks the pilot (three readings 3 ms apart, two inside 2000 to 3300 mV) every 10 ms while
 *     no dock session runs; the first check that passes starts one, and the controller serves the
 *     puck's requests until a frame fails;
 *   - tells the dock's power from USB power by a fresh pilot check the moment its supply input
 *     changes (the dock's `+` and the USB-C port share that input);
 *   - runs its docked states on that: charging with the radio off, a transport chord, the link up
 *     on the dock, lifted off (docs/api.md, Power and the system state machine);
 *   - moves both sticks' calibration by fixed millivolt offsets while it rests on the dock (the
 *     puck's magnets pull Hall-effect sticks), on and off with those states.
 * The library does all of it. A board supplies the parts it has: the devicetree node aliased
 * opensc2-dock (CONFIG_OPENSC2_DOCK: a UART, the pilot on an ADC channel or a window comparator's
 * GPIO, optionally a + sense input), and/or the readers and the offsets below, which can be set
 * and changed at any time.
 *
 * Every function here is callable before and after opensc2_init(), from any thread (not from an
 * interrupt). Without CONFIG_OPENSC2_DOCK_LINK the frame and session calls return -ENOTSUP; the
 * stick offsets work in every build (the docked states belong to CONFIG_OPENSC2_SYSTEM).
 */
#ifndef OPENSC2_DOCKING_H_
#define OPENSC2_DOCKING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <opensc2/cal.h>
#include <opensc2/dock.h>
#include <opensc2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- What the board supplies ----------------------------------------------------------------- */

struct opensc2_dock_board {
	/* One reading of the pilot at the controller's pin, in mV (no divider), or a negative errno
	 * when the reading failed (it then counts as outside the window). A board with a window
	 * comparator instead of an ADC returns 2500 while the comparator says "inside" and 0
	 * otherwise. NULL: the devicetree's pilot (io-channels or pilot-gpios of the opensc2,dock
	 * node), or none: then the board is never docked. Called three times per check, 3 ms apart,
	 * from the dock's thread or the system work queue; it may sleep briefly. */
	int32_t (*pilot_mv)(void *user);
	/* The dock's + on a sense input of the board's own: 1 present, 0 absent, a negative value
	 * when the board does not sense it (its + reaches VBUS, as on the real controller, so VBUS
	 * says it). NULL: the node's plus-gpios, or VBUS. Called from the system work queue every
	 * CONFIG_OPENSC2_POWER_POLL_MS (50 ms); it must not sleep. */
	int (*plus)(void *user);
	/* Called when a session starts or ends, the last pilot check's result or the supply rule's
	 * view changes, or the docked stick offsets go on or off: read opensc2_dock_status() there.
	 * From the thread that made the change (the dock's, the system work queue, the
	 * application's); return quickly. NULL: none. */
	void (*changed)(void *user);
	void *user;
};

/* Take over the pilot and/or the + from the devicetree, and/or hear of changes (a copy is kept;
 * NULL clears all). Works in every build; the readers need CONFIG_OPENSC2_DOCK_LINK. */
int opensc2_dock_set_board(const struct opensc2_dock_board *board);

/* One pilot check now, as between sessions: 1 present (two of three readings inside the window),
 * 0 not, -ENODEV with no pilot to read. Sleeps about 9 ms. A board that serves the dock on a wire
 * of its own calls it every 10 ms while no session runs, and starts a session when it passes. */
int opensc2_dock_pilot_check(void);

/* ---- What the board can read back ------------------------------------------------------------ */

#define OPENSC2_DOCK_SUPPLY_NONE  0   /* no supply */
#define OPENSC2_DOCK_SUPPLY_USB   1   /* a supply that appeared without the pilot */
#define OPENSC2_DOCK_SUPPLY_DOCK  2   /* a supply that appeared with the pilot: the dock's power */

struct opensc2_dock_status {
	bool session;         /* a dock session runs: the puck is being served */
	bool alone;           /* ... and no other link is up: the dock is the controller's link */
	bool pilot;           /* the last pilot check passed */
	uint8_t supply;       /* OPENSC2_DOCK_SUPPLY_*: the supply rule's view (NONE without a pilot
			       * to check; the system state machine's source may be the
			       * application's own, opensc2_power_source()) */
	bool stick_offsets;   /* the docked stick offsets are in effect */
	uint32_t sessions;    /* started since boot */
	uint32_t frames;      /* answered since boot */
	uint32_t errors;      /* frames that ended a session */
	uint32_t slowest_us;  /* the longest time from a frame to its answer */
};

/* The dock as it is now. -ENOTSUP without CONFIG_OPENSC2_DOCK_LINK (stick_offsets still set). */
int opensc2_dock_status(struct opensc2_dock_status *st);

/* ---- A wire of the board's own --------------------------------------------------------------- */
/* With CONFIG_OPENSC2_DOCK_LINK and no CONFIG_OPENSC2_DOCK, a board carries the frames itself
 * (another UART driver, a bridge chip): it checks the pilot (opensc2_dock_pilot_check()), starts a
 * session when it passes, hands each received frame to opensc2_dock_frame() and sends the answer,
 * 73 bytes, at 115200 baud 8N1; any frame that fails ends the session, and the board goes back to
 * checking the pilot. A frame is whatever arrived until a 64 ms silence (or 73 bytes). */

/* Start a session (0, or -EALREADY when one runs): the controller counts as docked. */
int opensc2_dock_session_start(void);
/* End the session (0, or -EALREADY when none runs). */
int opensc2_dock_session_end(void);
/* One received frame of `len` bytes: 0 with the answer in `answer` (send it, then call
 * opensc2_dock_answer_sent()); -EMSGSIZE (not 73 bytes) or -EBADMSG (a bad CRC): no answer, and
 * the session has ended; -ENOTCONN without a session. The answer is built as the real controller
 * builds it, over its previous answer. Runs vendor commands, flash writes included. */
int opensc2_dock_frame(const uint8_t *frame, size_t len, uint8_t answer[OPENSC2_DOCK_FRAME_LEN]);
/* The answer is on its way: with the dock the only link, a GET input's report is built now (the
 * next GET input answers it, as on the real controller). Skipped calls are caught up with at the
 * next frame. */
void opensc2_dock_answer_sent(void);

/* ---- The docked stick offsets ---------------------------------------------------------------- */
/* While the controller rests on the dock (the system state machine's dock-off and dock-on, from
 * entering them until it is lifted off), the real controller adds fixed millivolt offsets to both
 * sticks' stored calibration, each value clamped to 0..65535, and sets the sticks up again on each
 * change. The offsets are the board's (how its sticks react to the dock's magnets): none by
 * default. The library applies them to its own stick processing (CONFIG_OPENSC2_CAL), and
 * opensc2_dock_stick_adjust() does the same for an application that processes its sticks itself
 * (opensc2_cal_generation() changes with them). */

struct opensc2_dock_axis_offsets {
	int16_t min, max;              /* mV added to the stored extremes */
	int16_t rest_low, rest_high;   /* mV added to the stored rest band */
};

struct opensc2_dock_stick_offsets {
	struct opensc2_dock_axis_offsets x, y;
};

/* Set one stick's offsets (NULL: none). -EINVAL for another side. Takes effect at once while
 * docked. opensc2_cal_set_dock_offsets() (<opensc2/cal.h>) sets the same values as two arrays. */
int opensc2_dock_set_stick_offsets(enum opensc2_side side,
				   const struct opensc2_dock_stick_offsets *offsets);
/* Are the offsets in effect (the controller rests on the dock)? */
bool opensc2_dock_stick_offsets_active(void);
/* Without the system state machine (CONFIG_OPENSC2_SYSTEM=n) the application says when the
 * controller rests on the dock, and the offsets follow: 0. With the machine they follow its docked
 * states, and this returns -EPERM. */
int opensc2_dock_set_resting(bool on);
/* Move a stored stick calibration by the side's offsets while they are in effect, on a board of
 * revision 0x48 or later (the identity's board_revision), as the real controller does; only a
 * usable calibration (format 2 or 0x81) is moved, any other is left alone. */
void opensc2_dock_stick_adjust(enum opensc2_side side, struct opensc2_cal_stick *cal);
/* The same, pure: `offsets` added to `cal`, each value clamped to 0..65535. */
void opensc2_dock_stick_offsets_apply(const struct opensc2_dock_stick_offsets *offsets,
				      struct opensc2_cal_stick *cal);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_DOCKING_H_ */
