/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * input_rules.h: the rules the library applies to every state report after the hardware test
 * hooks' injection point, as pure logic, so the host tests run them as the library does:
 *   - a stick deflected beyond 12000 counts as touched (the stick-touch fallback);
 *   - optionally, the pads' clicks from their pressure (settings 52 and 53);
 *   - a pad is blank while the thumb rests on the stick on its side (setting 82);
 *   - the triggers' digital bits (setting 68), and none while setting 62 asks for raw triggers.
 * And two helpers lizard mode uses too: the real controller's quick distance approximation and
 * its touch stabiliser.
 *
 * Which rules run is a word of OSC2_RULE_* bits: the build's choice (Kconfig; report.c), which the
 * hardware test hooks may change while they are armed. lizard.c reads the lizard bits.
 */
#ifndef OPENSC2_INPUT_RULES_H_
#define OPENSC2_INPUT_RULES_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>
#include <opensc2/settings.h>

/* The rule switches. */
#define OSC2_RULE_TRIGGER_BITS        OPENSC2_BIT(0)   /* bits 23 and 27 from setting 68 */
#define OSC2_RULE_RAW_TRIGGERS        OPENSC2_BIT(1)   /* no trigger bit while setting 62 != 0 */
#define OSC2_RULE_STICK_FALLBACK      OPENSC2_BIT(2)   /* a stick beyond 12000 is touched */
#define OSC2_RULE_PAD_BLANK           OPENSC2_BIT(3)   /* setting 82 blanks a pad in 0x42 */
#define OSC2_RULE_PAD_CLICK           OPENSC2_BIT(4)   /* pad clicks from pressure (52, 53) */
#define OSC2_RULE_LIZARD_STABILISER   OPENSC2_BIT(8)   /* lizard mode: the touch stabilisers */
#define OSC2_RULE_LIZARD_SMOOTHING    OPENSC2_BIT(9)   /* lizard mode: the 8-frame average */
#define OSC2_RULE_LIZARD_GLIDE        OPENSC2_BIT(10)  /* lizard mode: the glide after a flick */
#define OSC2_RULE_LIZARD_TICKS        OPENSC2_BIT(11)  /* lizard mode: pad ticks and clicks */
#define OSC2_RULES_KNOWN              0x0F1Fu
#define OSC2_RULES_LIZARD             0x0F00u
/* The rules every option's default gives (all but the click by pressure): what the blank canvas
 * builds, and what the host tests' firmware emulator runs. */
#define OSC2_RULES_DEFAULT                                                                  \
	(OSC2_RULE_TRIGGER_BITS | OSC2_RULE_RAW_TRIGGERS | OSC2_RULE_STICK_FALLBACK |         \
	 OSC2_RULE_PAD_BLANK | OSC2_RULES_LIZARD)
/* The rules of OpenSC2 0.2.0: the trigger bits only. */
#define OSC2_RULES_V0_2                OSC2_RULE_TRIGGER_BITS

/* Beyond this distance from the centre (the approximation below) a stick counts as touched. */
#define OSC2_STICK_TOUCH_RADIUS       12000

/* Hold radii of the touch stabiliser: fine (lizard mode's right pad while setting 80 is 1) and
 * coarse (lizard mode's left pad, always). */
#define OSC2_STABILISER_FINE          4369
#define OSC2_STABILISER_COARSE        8738

/* What the rules keep from one report to the next. */
struct osc2_input_rules {
	bool trigger_bit[2];      /* the triggers' digital bits of the last report */
	bool pad_click[2];        /* each pad's click by pressure, touched or not */
	int16_t click_setting[2]; /* setting 52 or 53 when that click was last worked out */
	int16_t level68;          /* setting 68 when the trigger bits were last worked out */
	uint8_t decided;          /* this report's trigger bits the calibration decided (a side
				   * per bit), osc2_input_rules_decide() */
	bool decided_bit[2];
};

/* Back to boot: no trigger bit, no click. */
void osc2_input_rules_reset(struct osc2_input_rules *r);
/* This report's raw trigger of `side` was processed with its calibration, which decided its
 * digital bit: the trigger rule takes that bit for this report instead of working it out. */
void osc2_input_rules_decide(struct osc2_input_rules *r, int side, bool bit);
/* The triggers were set up again (a calibration key, the dock): their digital bits start from off,
 * as the real controller's set-up clears them. A change of setting 68 does the same by itself. */
void osc2_input_rules_triggers_set_up(struct osc2_input_rules *r);

/* Apply the rules in `rules` to the state of one report, in the real controller's order: the
 * stick-touch fallback, the click by pressure, the blanking of setting 82, the trigger bits.
 * `settings` are the 86 controller settings. */
void osc2_input_rules_apply(struct osc2_input_rules *r, uint32_t rules,
			    const int16_t settings[OPENSC2_SETTINGS_COUNT], struct opensc2_state *s);

/* A pad's click by pressure, as the real controller's pressure stage works it out on the
 * normalised press: with v = pressure / 32767 and t = setting / 100 (setting 52 or 53, a negative
 * one counting as 100) in single-precision floating point, on when v passes t + 0.08, off when it
 * falls below t - 0.08, unchanged in between. */
bool osc2_pad_click(uint16_t pressure, int16_t setting, bool previous);

/* The real controller's quick approximation of sqrt(dx * dx + dy * dy): with M the larger and m
 * the smaller of |dx| and |dy|, (1007 M + 441 m, less 40 M when M < 16 m) / 1024, rounded to the
 * nearest whole number. Within about 3 % of the true length. */
int32_t osc2_approx_len(int32_t dx, int32_t dy);

/* The touch stabiliser. The first touched sample is the output, held there while the thumb stays
 * within the hold radius of it; once the thumb goes further the output follows the thumb's own
 * movement and, in every sample in which the thumb moved, closes an eighth of the remaining gap
 * (rounded down) plus one unit on each axis. A sample without a touch starts it over. */
struct osc2_stabiliser {
	bool on;           /* following a touch */
	bool holding;      /* still within the hold radius of the touch-down point */
	int32_t ax, ay;    /* the touch-down point */
	int32_t ox, oy;    /* the output */
	int32_t px, py;    /* the previous sample */
};

void osc2_stabiliser_reset(struct osc2_stabiliser *st);
/* One sample: `*x`, `*y` in, the stabilised position out. Without a touch it only starts over
 * (the position is left as it is). */
void osc2_stabiliser_step(struct osc2_stabiliser *st, int32_t radius, bool touched, int32_t *x,
			  int32_t *y);

#endif /* OPENSC2_INPUT_RULES_H_ */
