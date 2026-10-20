/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * idle_rules.h: the idle monitor of the real controller, as pure logic (the host tests build it).
 * Controller setting 50 is the timeout in seconds (default 900; 0, a negative value or more than
 * 9998 switch it off). The timeout is cut into 16 periods of timeout x 1000 / 16 ms; at the end of
 * each a 16-bit history moves on by one bit, set when there was activity in the period. When all
 * 16 bits are clear the monitor reports "idle", and again at the end of every further idle period.
 * A different value of setting 50 refills the history and starts a new period, as start-up does.
 * The rules are the protocol documentation's (not public yet; docs/protocol.md lists the pages).
 * Who acts on "idle" is the system machine's business (only on battery).
 */
#ifndef OPENSC2_IDLE_RULES_H_
#define OPENSC2_IDLE_RULES_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>

#define OSC2_IDLE_PERIODS          16
#define OSC2_IDLE_MAX_S            9998    /* above this the monitor is off */
#define OSC2_IDLE_STICK_THRESHOLD  4000    /* a stick beyond this on either axis is activity */

struct osc2_idle {
	uint16_t history;      /* bit 0 is the newest period; 1: there was activity */
	bool active;           /* activity in the current period */
	int16_t timeout_s;     /* setting 50 as applied */
	uint32_t period_ms;    /* 0: the monitor is off */
	uint32_t period_end;   /* uptime, ms */
};

/* Is a timeout of `timeout_s` an active monitor? */
bool osc2_idle_enabled(int16_t timeout_s);

/* A fresh start (boot): the history full, a new period from `now_ms`. */
void osc2_idle_start(struct osc2_idle *m, int16_t timeout_s, uint32_t now_ms);

/* Setting 50 as it is now: a different value starts afresh; the same value changes nothing. */
void osc2_idle_setting(struct osc2_idle *m, int16_t timeout_s, uint32_t now_ms);

/* There was activity now. */
void osc2_idle_activity(struct osc2_idle *m);

/* The periods that ended by `now_ms` move the history on. True when one of them ended with the
 * whole history clear: the "idle" event. */
bool osc2_idle_poll(struct osc2_idle *m, uint32_t now_ms);

/* Milliseconds to the end of the current period (0: due), or -1 when the monitor is off. */
int32_t osc2_idle_next(const struct osc2_idle *m, uint32_t now_ms);

/* Was there input in the history or the current period? Always true while the monitor is off
 * (what the fully-charged LED display asks). */
bool osc2_idle_recent(const struct osc2_idle *m);

/* The activity rule for a button word that changed from `before` to `after`: a change that leaves
 * one of the 20 buttons down (holding a button without a change does not count again), or a pad
 * or stick touch bit set. */
bool osc2_idle_buttons_active(uint32_t before, uint32_t after);

/* The activity rule for a state report's inputs (sampled while a link is up): a stick beyond 4000
 * of 32767 on either axis, a trigger above 0, a trackpad or stick touch bit. Buttons and grips do
 * not count here (buttons count by their changes), nor does motion. */
bool osc2_idle_state_active(const struct opensc2_state *s);

#endif /* OPENSC2_IDLE_RULES_H_ */
