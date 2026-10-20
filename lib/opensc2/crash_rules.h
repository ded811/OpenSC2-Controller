/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * crash_rules.h: the crash record as pure logic the host tests build. A fatal error leaves a
 * 33-byte record in RAM that a reset keeps; the next boot stores it as the key debug/resetreason
 * (key_rules.h), and vendor command 0xF2 with selector 1 answers it, then marks it seen. The
 * layout is the real controller's (the protocol documentation, not public yet; docs/protocol.md
 * lists the pages):
 *
 *   0   u8    1 for a new record, 0 once a host has read it
 *   1   u32   the firmware's build time
 *   5   16 B  its git revision, text, zero padded
 *   21  u32   Zephyr's fatal-error reason (K_ERR_*: 0 a CPU exception, 3 a kernel oops, 4 a panic)
 *   25  u32   the program counter at the error
 *   29  u32   the link register
 */
#ifndef OPENSC2_CRASH_RULES_H_
#define OPENSC2_CRASH_RULES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSC2_CRASH_LEN        33
#define OSC2_CRASH_ANSWER_LEN (1 + OSC2_CRASH_LEN)   /* 0xF2 selector 1: the selector first */
#define OSC2_CRASH_GIT_LEN    16

/* The record's fields. */
#define OSC2_CRASH_AT_NEW     0
#define OSC2_CRASH_AT_BUILD   1
#define OSC2_CRASH_AT_GIT     5
#define OSC2_CRASH_AT_REASON  21
#define OSC2_CRASH_AT_PC      25
#define OSC2_CRASH_AT_LR      29

/* Where a fatal error leaves its record: RAM that a reset keeps (OpenSC2's own layout; the record
 * inside is the real controller's). A magic word and a CRC-32 tell a record from what was there. */
#define OSC2_CRASH_MAGIC      0x5243534Fu   /* "OSCR" */

struct osc2_crash_slot {
	uint32_t magic;
	uint8_t record[OSC2_CRASH_LEN];
	uint8_t pad[3];
	uint32_t crc;   /* CRC-32 of the record */
};

/* A new record of a fatal error. `git` is cut to 16 characters. */
void osc2_crash_record(uint8_t out[OSC2_CRASH_LEN], uint32_t build_time, const char *git,
		       uint32_t reason, uint32_t pc, uint32_t lr);
/* The fields of a record. */
uint32_t osc2_crash_field(const uint8_t record[OSC2_CRASH_LEN], size_t at);

/* Into the slot, the magic last. */
void osc2_crash_slot_write(struct osc2_crash_slot *s, const uint8_t record[OSC2_CRASH_LEN]);
/* Does the slot hold a record (its magic and CRC)? Power-on RAM almost never passes. */
bool osc2_crash_slot_valid(const struct osc2_crash_slot *s);
void osc2_crash_slot_clear(struct osc2_crash_slot *s);

/* At boot, with a new record `fresh` and the stored `stored`: is it worth a flash write? Not when
 * the same crash is stored and still unread (a crash at every boot writes flash once, not at every
 * boot). */
bool osc2_crash_should_save(const uint8_t stored[OSC2_CRASH_LEN],
			    const uint8_t fresh[OSC2_CRASH_LEN]);

/* 0xF2 selector 1: {1, the record as it is} into `out`; then the record is marked seen. Returns
 * whether that changed it (the caller saves it again). */
bool osc2_crash_answer(uint8_t record[OSC2_CRASH_LEN], uint8_t out[OSC2_CRASH_ANSWER_LEN]);

/* No crash ever recorded: the build-time field is 0 (all 33 bytes are, for the controller). */
bool osc2_crash_empty(const uint8_t record[OSC2_CRASH_LEN]);

#endif /* OPENSC2_CRASH_RULES_H_ */
