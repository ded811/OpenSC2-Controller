/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_sys.h: the hardware test hooks for the system state machine (CONFIG_OPENSC2_TEST_HOOKS with
 * CONFIG_OPENSC2_SYSTEM): system injection, where injected buttons, a battery level and a power
 * source reach the library's own rules (INJECT keeps replacing only what the state report
 * carries); a boot-time injection kept in retained RAM for the next boot; the machine's status;
 * simulated link events; and the ring entries for the machine's states, the LED mode, cues,
 * chords, the power source, the idle monitor and how a shutdown ended. Pure C, like test_core.c:
 * the core's lock guards the state, and the rest of the library is reached through
 * `struct osc2_test_sys_env` (system.c, and the host tests' stand-in). test_sys.c is the pure part
 * (the ops, the injected inputs, the byte formats); test_sys_notes.c has the calls the glue makes,
 * on the glue's core, as test_notes.c does for the rest of the library.
 *
 * The byte formats are in docs/test-protocol.md.
 */
#ifndef OPENSC2_TEST_SYS_H_
#define OPENSC2_TEST_SYS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "test_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ops (0x70..0x77 are this module's). */
#define OSC2_TEST_OP_SYS_FIRST       0x70
#define OSC2_TEST_OP_SYS_LAST        0x77
#define OSC2_TEST_OP_SYS_INJECT      0x70
#define OSC2_TEST_OP_SYS_INJECT_OFF  0x71
#define OSC2_TEST_OP_SYS_STATUS      0x72
#define OSC2_TEST_OP_SYS_BOOT        0x73
#define OSC2_TEST_OP_SYS_EVENT       0x74

/* Ring entry kinds (0x50..0x57 are this module's). */
#define OSC2_TEST_KIND_SYS_FIRST     0x50
#define OSC2_TEST_KIND_SYS_LAST      0x57
#define OSC2_TEST_KIND_SYS_STATE     0x50   /* from, to, cause, transport, source */
#define OSC2_TEST_KIND_LED_MODE      0x51   /* mode, flags, level, fully-charged level */
#define OSC2_TEST_KIND_CUE           0x52   /* cue, script, gain s8, played */
#define OSC2_TEST_KIND_SYS_END       0x53   /* state, cause, end, how */
#define OSC2_TEST_KIND_CHORD         0x54   /* what, chord */
#define OSC2_TEST_KIND_SOURCE        0x55   /* from, to, event (0xFF: none) */
#define OSC2_TEST_KIND_IDLE          0x56   /* history u16 */

/* Flags of SYS_INJECT and SYS_BOOT: what is injected. */
#define OSC2_TEST_SYS_BUTTONS        0x01
#define OSC2_TEST_SYS_LEVEL          0x02
#define OSC2_TEST_SYS_SOURCE         0x04
#define OSC2_TEST_SYS_ALL            0x07

#define OSC2_TEST_SYS_ARGS_LEN       8      /* SYS_INJECT and SYS_BOOT */
#define OSC2_TEST_SYS_STATUS_LEN     30
#define OSC2_TEST_SYS_BOOT_WORDS     3      /* retained words of a boot-time injection */
#define OSC2_TEST_SYS_END_WORDS      3      /* retained words of the last end */
#define OSC2_TEST_SYS_BOOT_MAGIC     0x5B007A5Eu
#define OSC2_TEST_SYS_END_MAGIC      0x5E0DE4D5u

/* SYS_EVENT kinds. */
#define OSC2_TEST_SYS_EV_WIRELESS_OFF  1    /* the link gave up (as the radio reports it) */
#define OSC2_TEST_SYS_EV_HOST_ASLEEP   2    /* the puck's "PC asleep" packet, 0xE6 */

/* CHORD entries: what. */
#define OSC2_TEST_CHORD_DOWN          1
#define OSC2_TEST_CHORD_UP            2
#define OSC2_TEST_CHORD_STEAM_PRESS   3
#define OSC2_TEST_CHORD_STEAM_HOLD    4
#define OSC2_TEST_CHORD_KEYLOCK_TAP   5
#define OSC2_TEST_CHORD_KEYLOCK_HOLD  6

/* How a shutdown or reboot came out (SYS_END entries, the last-end record). */
enum osc2_test_sys_how {
	OSC2_TEST_SYS_HOW_NONE = 0,
	OSC2_TEST_SYS_HOW_POWER_OFF = 1,    /* OPENSC2_POWER_OFF asked for */
	OSC2_TEST_SYS_HOW_REBOOT = 2,       /* a reboot state, or setting 77 */
	OSC2_TEST_SYS_HOW_NO_BATTERY = 3,   /* would power off: a reboot, the board has no battery */
	OSC2_TEST_SYS_HOW_TEST = 4,         /* would power off: a reboot, the test hooks are in use */
};

/* What a boot-time injection holds (SYS_BOOT's arguments). */
struct osc2_test_sys_boot {
	uint8_t flags;           /* OSC2_TEST_SYS_* */
	uint32_t buttons;
	uint8_t level;
	uint8_t source;          /* enum osc2_sys_source */
	uint8_t hold;            /* the buttons stay held this many 100 ms after boot (0: until
				  * SYS_INJECT_OFF) */
};

/* The last end, kept across the reboot it caused. */
struct osc2_test_sys_end {
	uint8_t state, cause, end, how;
	uint32_t uptime_ms;
};

/* What the glue provides (system.c; the host tests have a stand-in). */
struct osc2_test_sys_env {
	/* The machine's part of the SYS_STATUS result (every byte but the injection flags). */
	void (*status)(uint8_t out[OSC2_TEST_SYS_STATUS_LEN]);
	/* The injected inputs changed: the machine looks at them again. */
	void (*changed)(void);
	/* Keep a boot-time injection for the next boot (its words), or clear it (NULL). 0, or
	 * -ENOTSUP without a retained RAM area. */
	int (*boot_store)(const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS]);
	/* SYS_EVENT. */
	void (*event)(uint8_t kind);
};

/* ---- the ops (test_core.c's dispatcher hands 0x70..0x77 over) --------------------------------- */

/* Run one op. `console`: the console transport (an op other than SYS_STATUS answers NOT_ARMED
 * there while unarmed). Returns the answer status; the result goes to `result`, `*rlen`. */
uint8_t osc2_test_sys_op(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t op,
			 const uint8_t *args, uint8_t alen, bool console, uint8_t *result,
			 uint8_t *rlen);

/* ARM and the end of arming, with the core's lock held: the runtime injection stops. A boot-time
 * injection's power source and level stay for the boot, and its buttons while their hold time
 * runs (SYS_INJECT_OFF ends them all). */
void osc2_test_sys_stop(void);

#if defined(CONFIG_OPENSC2_TEST_HOOKS)

/* ---- what the glue asks (each returns at once when nothing is injected) ------------------------ */

bool osc2_test_sys_buttons(uint32_t *buttons);
bool osc2_test_sys_level(uint8_t *level);
bool osc2_test_sys_source(uint8_t *source);
/* Never power off now: the hooks are armed, or this boot came from a boot-time injection. */
bool osc2_test_sys_keep_on(void);
/* At boot: the retained words a SYS_BOOT left (the glue reads them once); applies them. */
void osc2_test_sys_boot_apply(const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS], uint32_t now_ms);
/* Milliseconds until the boot injection's buttons are released (0: due), or -1. */
int32_t osc2_test_sys_next(uint32_t now_ms);
/* Call at that deadline. True when the injected buttons changed. */
bool osc2_test_sys_poll(uint32_t now_ms);

/* ---- notes for the ring (while armed) ------------------------------------------------------- */

void osc2_test_sys_note_state(uint8_t from, uint8_t to, uint8_t cause, uint8_t transport,
			      uint8_t source);
void osc2_test_sys_note_led(uint8_t mode, uint8_t flags, uint8_t level, uint8_t full_level);
void osc2_test_sys_note_cue(uint8_t cue, uint8_t script, int8_t gain_db, bool played);
void osc2_test_sys_note_end(uint8_t state, uint8_t cause, uint8_t end, uint8_t how);
void osc2_test_sys_note_chord(uint8_t what, uint8_t chord);
void osc2_test_sys_note_source(uint8_t from, uint8_t to, int event);
void osc2_test_sys_note_idle(uint16_t history);

#else /* !CONFIG_OPENSC2_TEST_HOOKS: the glue's calls cost nothing */

static inline bool osc2_test_sys_buttons(uint32_t *buttons)
{
	(void)buttons;
	return false;
}

static inline bool osc2_test_sys_level(uint8_t *level)
{
	(void)level;
	return false;
}

static inline bool osc2_test_sys_source(uint8_t *source)
{
	(void)source;
	return false;
}

static inline bool osc2_test_sys_keep_on(void)
{
	return false;
}

static inline void osc2_test_sys_boot_apply(const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS],
					    uint32_t now_ms)
{
	(void)words;
	(void)now_ms;
}

static inline int32_t osc2_test_sys_next(uint32_t now_ms)
{
	(void)now_ms;
	return -1;
}

static inline bool osc2_test_sys_poll(uint32_t now_ms)
{
	(void)now_ms;
	return false;
}

static inline void osc2_test_sys_note_state(uint8_t from, uint8_t to, uint8_t cause,
					    uint8_t transport, uint8_t source)
{
	(void)from;
	(void)to;
	(void)cause;
	(void)transport;
	(void)source;
}

static inline void osc2_test_sys_note_led(uint8_t mode, uint8_t flags, uint8_t level,
					  uint8_t full_level)
{
	(void)mode;
	(void)flags;
	(void)level;
	(void)full_level;
}

static inline void osc2_test_sys_note_cue(uint8_t cue, uint8_t script, int8_t gain_db,
					  bool played)
{
	(void)cue;
	(void)script;
	(void)gain_db;
	(void)played;
}

static inline void osc2_test_sys_note_end(uint8_t state, uint8_t cause, uint8_t end, uint8_t how)
{
	(void)state;
	(void)cause;
	(void)end;
	(void)how;
}

static inline void osc2_test_sys_note_chord(uint8_t what, uint8_t chord)
{
	(void)what;
	(void)chord;
}

static inline void osc2_test_sys_note_source(uint8_t from, uint8_t to, int event)
{
	(void)from;
	(void)to;
	(void)event;
}

static inline void osc2_test_sys_note_idle(uint16_t history)
{
	(void)history;
}

#endif /* CONFIG_OPENSC2_TEST_HOOKS */

/* ---- pure helpers (the host tests check each) -------------------------------------------------- */

/* SYS_BOOT's arguments <-> the retained words (a magic word, the buttons, the rest). */
void osc2_test_sys_boot_pack(const struct osc2_test_sys_boot *b,
			     uint32_t words[OSC2_TEST_SYS_BOOT_WORDS]);
bool osc2_test_sys_boot_unpack(const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS],
			       struct osc2_test_sys_boot *b);
void osc2_test_sys_end_pack(const struct osc2_test_sys_end *e,
			    uint32_t words[OSC2_TEST_SYS_END_WORDS]);
bool osc2_test_sys_end_unpack(const uint32_t words[OSC2_TEST_SYS_END_WORDS],
			      struct osc2_test_sys_end *e);
/* A ring entry kind's name, or NULL when it is not one of this module's. */
const char *osc2_test_sys_kind_name(uint8_t kind);
/* The fields of one of this module's entries as " key=value ..." into `out` (`size` bytes, the
 * text so far `*at` long); false for another kind. */
bool osc2_test_sys_fields(char *out, size_t size, size_t *at, uint8_t kind, const uint8_t *d,
			  uint8_t n);

/* ---- the injected inputs (test_sys.c), for test_sys_notes.c ------------------------------------- */

/* The glue's environment; NULL (no system state machine in this build): the ops answer DENIED. */
extern const struct osc2_test_sys_env *osc2_test_sys_env;

/* Nothing injected (the host tests' fresh start; the firmware starts that way). */
void osc2_test_sys_init(void);
/* One injected input (OSC2_TEST_SYS_BUTTONS, _LEVEL or _SOURCE) into `*value`: true when it is
 * injected. Takes the environment's lock. */
bool osc2_test_sys_get(const struct osc2_test_env *env, uint8_t what, uint32_t *value);
/* This boot came from a boot-time injection. */
bool osc2_test_sys_booted(void);
void osc2_test_sys_apply_boot(const struct osc2_test_env *env,
			      const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS], uint32_t now_ms);
int32_t osc2_test_sys_hold_next(const struct osc2_test_env *env, uint32_t now_ms);
bool osc2_test_sys_hold_poll(const struct osc2_test_env *env, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_TEST_SYS_H_ */
