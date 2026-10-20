/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_hooks.h: where the library calls the hardware test hooks (CONFIG_OPENSC2_TEST_HOOKS). Each
 * call is a note ("this happened"), a question ("is this report held back?") or a replacement
 * ("the injected state instead"). While the hooks are not armed every call returns at once, and
 * with the option off every one is an empty inline function, so the call sites need no #if. Not a
 * public interface.
 *
 * Links are the library's enum osc2_link values (0 radio, 1 USB, 2 test), or
 * OSC2_TEST_LINK_NONE.
 */
#ifndef OPENSC2_TEST_HOOKS_H_
#define OPENSC2_TEST_HOOKS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <opensc2/opensc2.h>
#include "test_core.h"

/* osc2_test_ctx_enter() with the hooks unarmed: nothing to restore. */
#define OSC2_TEST_CTX_UNSET 0xFF

#if defined(CONFIG_OPENSC2_TEST_HOOKS)

/* ---- state reports, lizard mode, battery ---- */

/* The lizard guard, first thing in osc2_input_send(): true when a mouse or keyboard report is
 * held back (recorded in the ring instead). */
bool osc2_test_divert(const uint8_t *report, size_t len);
/* In osc2_report_state(), after the application's state was read: the injected state replaces
 * it. Never where the state is written back as the application's. */
void osc2_test_input(struct opensc2_state *s);
/* After each state report is encoded. */
void osc2_test_frame(const uint8_t report[OPENSC2_STATE_REPORT_LEN]);
/* First thing in osc2_report_lizard(): this frame was built from injected inputs without the
 * lizard test, so lizard mode must not see it. */
bool osc2_test_skip_lizard(void);
/* The injected battery, if any, replaces `*b` (the 0x43 report and the 0xBE answer). */
void osc2_test_battery(struct opensc2_battery *b);
/* RADIO_DEAF: milliseconds the radio thread stays deaf, once (0: none). */
uint16_t osc2_test_take_deaf(void);
/* In osc2_cal_frame(): the raw injection replaces the application's raw inputs; returns the nonce
 * for the IMU timestamp, or 0. */
uint32_t osc2_test_raw_inputs(struct opensc2_raw_inputs *raw);

/* ---- the command channel ---- */

/* A SET whose opcode the library does not know: the test channel claims a keyed 0x7E (true: the
 * SET succeeds, nothing else runs). */
bool osc2_test_claim(uint8_t link, uint8_t opcode, const uint8_t *payload, uint8_t len);
/* A GET of 0x7E: the link's held answer (true), or the usual path. */
bool osc2_test_answer(uint8_t link, uint8_t *out, uint8_t *out_len);
/* A SET on `link` succeeded and replaced the stored request. */
void osc2_test_stored(uint8_t link);
/* Was the request just handled on `link` a claimed 0x7E SET, or a GET the hooks answered? (The
 * USB session log counts those but does not log them line by line.) */
bool osc2_test_quiet(uint8_t link);
/* The link of the command this thread runs, for the notes it causes; returns what to give back
 * to osc2_test_ctx_leave(). */
uint8_t osc2_test_ctx_enter(uint8_t link);
void osc2_test_ctx_leave(uint8_t previous);

/* ---- counters (safe in interrupts) ---- */

void osc2_test_count_n(uint8_t counter, uint32_t n);

static inline void osc2_test_count(uint8_t counter)
{
	osc2_test_count_n(counter, 1);
}

/* ---- notes for the ring ---- */

void osc2_test_note_led(const struct opensc2_led *led);
void osc2_test_note_setting(uint8_t number, int16_t value);
void osc2_test_note_power(uint8_t request, const char *why);
void osc2_test_note_lizard(uint8_t what);
void osc2_test_note_output(uint8_t link, const uint8_t *report, size_t len, int decoded);
void osc2_test_note_rumble(uint8_t link, const struct opensc2_rumble *rumble);
void osc2_test_note_haptic(uint8_t link, const struct opensc2_haptic *haptic);
/* `n`: a SET's length byte or a GET's answer length; `payload`/`len`: the request's payload and
 * its length; `avail`: the bytes after the length byte that exist (61 on USB). */
void osc2_test_note_vendor(uint8_t link, bool get, uint8_t opcode, uint8_t n, int32_t result,
			   const uint8_t *payload, uint8_t len, size_t avail);
void osc2_test_note_link(uint8_t state, uint8_t reason, uint8_t slot, uint8_t channel,
			 uint8_t protocol);
void osc2_test_note_usb(const struct opensc2_usb_info *info);
void osc2_test_note_host_asleep(void);
void osc2_test_note_version(uint8_t version);
void osc2_test_note_identity(const struct opensc2_identity *id);
void osc2_test_note_channels(const uint8_t channels[4]);

/* ---- what the glue provides (test_hooks.c; the host tests have a stand-in) ----
 * test_notes.c implements everything above on the core with these. */

extern struct osc2_test_core osc2_test_core_obj;
extern const struct osc2_test_env osc2_test_env_obj;
/* The link of this thread's command, whatever the hooks' state (the request paths); returns the
 * previous one for osc2_test_ctx_restore(). */
uint8_t osc2_test_ctx_set(uint8_t link);
void osc2_test_ctx_restore(uint8_t previous);
/* A request may have moved the lease's end or started the end of a disarm: time them again. */
void osc2_test_deadline_changed(void);
/* "opensc2 test <words>": the console command, its lines to `emit`. Returns the END code. */
int osc2_test_console_run(int argc, const char *const *argv, osc2_test_emit emit, void *ctx);

#if defined(CONFIG_OPENSC2_SHELL)
struct shell;
/* "opensc2 test ..." (test_shell.c). */
int osc2_test_shell(const struct shell *sh, size_t argc, char **argv);
#endif

#else /* !CONFIG_OPENSC2_TEST_HOOKS */

static inline bool osc2_test_divert(const uint8_t *report, size_t len)
{
	(void)report;
	(void)len;
	return false;
}

static inline void osc2_test_input(struct opensc2_state *s)
{
	(void)s;
}

static inline void osc2_test_frame(const uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	(void)report;
}

static inline bool osc2_test_skip_lizard(void)
{
	return false;
}

static inline void osc2_test_battery(struct opensc2_battery *b)
{
	(void)b;
}

static inline uint16_t osc2_test_take_deaf(void)
{
	return 0;
}

static inline uint32_t osc2_test_raw_inputs(struct opensc2_raw_inputs *raw)
{
	(void)raw;
	return 0;
}

static inline bool osc2_test_claim(uint8_t link, uint8_t opcode, const uint8_t *payload,
				   uint8_t len)
{
	(void)link;
	(void)opcode;
	(void)payload;
	(void)len;
	return false;
}

static inline bool osc2_test_answer(uint8_t link, uint8_t *out, uint8_t *out_len)
{
	(void)link;
	(void)out;
	(void)out_len;
	return false;
}

static inline void osc2_test_stored(uint8_t link)
{
	(void)link;
}

static inline bool osc2_test_quiet(uint8_t link)
{
	(void)link;
	return false;
}

static inline uint8_t osc2_test_ctx_enter(uint8_t link)
{
	(void)link;
	return OSC2_TEST_CTX_UNSET;
}

static inline void osc2_test_ctx_leave(uint8_t previous)
{
	(void)previous;
}

static inline void osc2_test_count_n(uint8_t counter, uint32_t n)
{
	(void)counter;
	(void)n;
}

static inline void osc2_test_count(uint8_t counter)
{
	(void)counter;
}

static inline void osc2_test_note_led(const struct opensc2_led *led)
{
	(void)led;
}

static inline void osc2_test_note_setting(uint8_t number, int16_t value)
{
	(void)number;
	(void)value;
}

static inline void osc2_test_note_power(uint8_t request, const char *why)
{
	(void)request;
	(void)why;
}

static inline void osc2_test_note_lizard(uint8_t what)
{
	(void)what;
}

static inline void osc2_test_note_output(uint8_t link, const uint8_t *report, size_t len,
					 int decoded)
{
	(void)link;
	(void)report;
	(void)len;
	(void)decoded;
}

static inline void osc2_test_note_rumble(uint8_t link, const struct opensc2_rumble *rumble)
{
	(void)link;
	(void)rumble;
}

static inline void osc2_test_note_haptic(uint8_t link, const struct opensc2_haptic *haptic)
{
	(void)link;
	(void)haptic;
}

static inline void osc2_test_note_vendor(uint8_t link, bool get, uint8_t opcode, uint8_t n,
					 int32_t result, const uint8_t *payload, uint8_t len,
					 size_t avail)
{
	(void)link;
	(void)get;
	(void)opcode;
	(void)n;
	(void)result;
	(void)payload;
	(void)len;
	(void)avail;
}

static inline void osc2_test_note_link(uint8_t state, uint8_t reason, uint8_t slot,
				       uint8_t channel, uint8_t protocol)
{
	(void)state;
	(void)reason;
	(void)slot;
	(void)channel;
	(void)protocol;
}

static inline void osc2_test_note_usb(const struct opensc2_usb_info *info)
{
	(void)info;
}

static inline void osc2_test_note_host_asleep(void)
{
}

static inline void osc2_test_note_version(uint8_t version)
{
	(void)version;
}

static inline void osc2_test_note_identity(const struct opensc2_identity *id)
{
	(void)id;
}

static inline void osc2_test_note_channels(const uint8_t channels[4])
{
	(void)channels;
}

#endif /* CONFIG_OPENSC2_TEST_HOOKS */

#endif /* OPENSC2_TEST_HOOKS_H_ */
