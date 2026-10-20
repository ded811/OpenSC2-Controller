/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * dock_link.h: the dock link (CONFIG_OPENSC2_DOCK_LINK), the controller's end of a dock session,
 * between the frames (dock_rules.c) and the rest of the library:
 *   - a session: while it runs, the dock is an active transport and the controller counts as
 *     docked; any frame that fails (too short, a bad CRC, a receive error) ends it;
 *   - the four requests: the probe's identity record, vendor commands on a stored request of the
 *     dock's own (OSC2_LINK_DOCK; 0x81 and 0x85 do nothing there), and GET input;
 *   - GET input: with another link up, the copy of the latest state report that report.c leaves
 *     here (the dock never takes a sequence number from that link's stream); with the dock alone,
 *     one new report per request;
 *   - attribute 11 of 0x83: 64000 µs while the dock is the only link;
 *   - the supply rule on the board's readings (dock power or USB power), which the system state
 *     machine takes as its power source while the application leaves that to the library.
 * Nothing goes out on the dock but the answers: no lizard reports, no 0x43, no 0x44.
 *
 * Who feeds it frames: dock.c's UART (CONFIG_OPENSC2_DOCK) and the hardware test hooks' dock frame
 * op (test_dock.c). It has no thread and no pins of its own; the host tests build it too. Not a
 * public interface. With the option off every call is an empty inline function.
 */
#ifndef OPENSC2_DOCK_LINK_H_
#define OPENSC2_DOCK_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <opensc2/codec.h>
#include <opensc2/dock.h>
#include "dock_rules.h"

/* Why a session ended. */
enum osc2_dock_end {
	OSC2_DOCK_END_NONE = 0,
	OSC2_DOCK_END_BAD_SIZE = 1,     /* fewer than 73 bytes before the gap timeout */
	OSC2_DOCK_END_BAD_CRC = 2,
	OSC2_DOCK_END_RX_ERROR = 3,     /* a framing, break or overrun error (dock.c) */
	OSC2_DOCK_END_ARM_FAILED = 4,   /* the receiver could not be armed (dock.c) */
	OSC2_DOCK_END_STOPPED = 5,      /* ended on purpose: the test hooks, a shutdown */
};

/* Who started a session. */
enum osc2_dock_starter {
	OSC2_DOCK_BY_PILOT = 0,   /* dock.c: a pilot check passed */
	OSC2_DOCK_BY_TEST = 1,    /* the test hooks (a session that lasts only while they are armed) */
};

struct osc2_dock_info {
	bool active;              /* a session runs (the controller is docked) */
	bool alone;               /* ... and no other link is up: the dock is the current link */
	uint8_t starter;          /* enum osc2_dock_starter of the session (or of the last one) */
	uint8_t last_end;         /* enum osc2_dock_end of the last session */
	uint32_t sessions;        /* started since boot */
	uint32_t frames;          /* answered since boot */
	uint32_t errors;          /* frames that ended a session */
	uint32_t slowest_us;      /* the longest time from a frame to its answer */
	bool cache_valid;         /* a state report was copied since boot */
};

#if defined(CONFIG_OPENSC2_DOCK_LINK)

/* Back to the state of a boot: no session, no copy, the counters at 0 (the firmware starts from
 * zeroed RAM; the host tests and the firmware emulator "boot" many times). */
void osc2_dock_reset(void);
void osc2_dock_session_start(uint8_t starter);
void osc2_dock_session_end(uint8_t reason);
/* A session runs. A test session counts only while the hooks are armed. */
bool osc2_dock_active(void);
/* The dock is the current link: a session runs and no other link is up (USB configured while
 * wired, the radio or Bluetooth connected while wireless). */
bool osc2_dock_alone(void);
void osc2_dock_info(struct osc2_dock_info *info);

/* One frame as it arrived (`len` bytes) and its answer. A bad size or CRC ends the session and
 * leaves `ans` alone. `*handling_us` (may be NULL): how long the request took, frame to answer.
 * Frames from the UART and from the test hooks are handled one at a time. */
enum osc2_dock_outcome osc2_dock_receive(const uint8_t *frame, size_t len,
					 uint8_t ans[OPENSC2_DOCK_FRAME_LEN], uint32_t *handling_us);

/* report.c: every state report built, with its ID (kept while a session runs). */
void osc2_dock_cache_report(const uint8_t report[OPENSC2_STATE_REPORT_LEN]);
/* commands.c: attribute 11 of 0x83, `other` unless the dock is the current link. */
uint32_t osc2_dock_interval_us(uint32_t other);

/* The supply rule (dock_rules.h) on one reading of the board's inputs: `usb` USB power on the USB
 * input, `plus` the dock's + on its sense input, `pilot` the pilot check passed (the last one, or
 * the one that started the running session). Returns the supply in effect, decided when it
 * appeared (a change between USB and the dock with power all along changes nothing). */
enum osc2_dock_supply osc2_dock_supply_step(bool usb, bool plus, bool pilot);
/* The supply as the board sees it now (dock.c): the rule stepped on USB power, the dock's + sense
 * input (`plus-gpios` of the opensc2,dock node) and the pilot. Never the dock without the dock's
 * pins (CONFIG_OPENSC2_DOCK) or without a + sense input. system.c takes OSC2_DOCK_SUPPLY_DOCK as
 * the power source OPENSC2_POWER_SOURCE_DOCK while the application leaves it on AUTO. */
enum osc2_dock_supply osc2_dock_supply(void);

/* Build one state report now (report.c's osc2_report_state(), which also leaves its copy here):
 * GET input with the dock alone. dock.c has it in the firmware; the host tests and the firmware
 * emulator have their own. */
void osc2_dock_sample(uint8_t report[OPENSC2_STATE_REPORT_LEN]);
/* The microsecond clock for the handling times (dock.c, or the host's). */
uint32_t osc2_dock_now_us(void);

#else /* !CONFIG_OPENSC2_DOCK_LINK */

static inline bool osc2_dock_active(void)
{
	return false;
}

static inline bool osc2_dock_alone(void)
{
	return false;
}

static inline void osc2_dock_cache_report(const uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	(void)report;
}

static inline uint32_t osc2_dock_interval_us(uint32_t other)
{
	return other;
}

static inline enum osc2_dock_supply osc2_dock_supply(void)
{
	return OSC2_DOCK_SUPPLY_NONE;   /* no dock link: the dock never powers the board */
}

#endif /* CONFIG_OPENSC2_DOCK_LINK */

#endif /* OPENSC2_DOCK_LINK_H_ */
