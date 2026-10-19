/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * mode_rules.h: the rule that decides at boot whether the board is wired or wireless, and the
 * values of the warm-restart marks. Pure logic, no kernel (the host tests build it).
 */
#ifndef OPENSC2_MODE_RULES_H_
#define OPENSC2_MODE_RULES_H_

#include <opensc2/opensc2.h>

/* A mark left in memory that start-up does not clear, read and cleared at the next boot. */
enum osc2_mark {
	OSC2_MARK_NONE = 0,
	OSC2_MARK_WIRELESS,      /* the warm restart: come back wireless (Steam's 0x95 0xA427AF52) */
	OSC2_MARK_WIRED,         /* come back wired once, whatever the policy */
};

/* The 32-bit forms kept in RAM (each stored with its complement) and the byte forms for a retention
 * register. */
#define OSC2_MARK_WORD_WIRELESS  0x57A2C0DEu
#define OSC2_MARK_WORD_WIRED     0x3D1EC0DEu
#define OSC2_MARK_BYTE_WIRELESS  0xA5u
#define OSC2_MARK_BYTE_WIRED     0x5Au

struct osc2_mode_inputs {
	bool usb_supported;            /* built with the USB transport */
	bool usb_power;                /* VBUS present at boot */
	enum osc2_mark mark;           /* the mark found at boot */
	enum opensc2_policy policy;
	enum opensc2_boot_mode app;    /* what the application asked for */
};

/* The mode of this boot and why (`*reason`). In order: no USB support means wireless; the
 * application's choice; the wireless mark on USB power; the wired mark on USB power; the policy
 * "wireless"; USB power means wired; otherwise wireless. */
enum opensc2_mode osc2_mode_decide(const struct osc2_mode_inputs *in,
				   enum opensc2_mode_reason *reason);

/* USB power appeared while wireless (the cable plugged in on battery): what the real controller
 * does. */
enum osc2_plug_in {
	OSC2_PLUG_IN_IGNORE,       /* the policy is "wireless": never wired */
	OSC2_PLUG_IN_CHARGE,       /* the link is up: stay wireless, only charge */
	OSC2_PLUG_IN_LINK_DOWN,    /* reboot and come back wired: the link is down */
	OSC2_PLUG_IN_STEAM,        /* reboot and come back wired: STEAM held while plugging in */
};

/* `policy` as it is when USB power appears (a change at run time counts at once). */
enum osc2_plug_in osc2_plug_in_rule(enum opensc2_policy policy, bool link_up, bool steam_held);

/* Mark <-> word and byte forms; unknown values read as OSC2_MARK_NONE. */
uint32_t osc2_mark_word(enum osc2_mark mark);
enum osc2_mark osc2_mark_from_words(uint32_t word, uint32_t inverse);
uint8_t osc2_mark_byte(enum osc2_mark mark);
enum osc2_mark osc2_mark_from_byte(uint8_t b);

#endif /* OPENSC2_MODE_RULES_H_ */
