/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_sys.c: the test hooks for the system state machine (test_sys.h): the ops, the injected
 * inputs (under the core's lock) and the byte formats. Pure C: the core and its environment come
 * in as arguments, the machine through the glue's `osc2_test_sys_env`; test_sys_notes.c has the
 * calls the glue makes. The same file runs in the firmware and in the host tests.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sys_rules.h"
#include "test_sys.h"

const struct osc2_test_sys_env *osc2_test_sys_env;

/* What is injected into the system rules. */
static struct {
	bool buttons_on, level_on, source_on;
	uint32_t buttons;
	uint8_t level;
	uint8_t source;
	bool boot;               /* this boot came from a boot-time injection */
	bool hold_on;            /* its buttons are released at release_ms */
	uint32_t release_ms;
} sys;

/* ---- pure helpers ---------------------------------------------------------------------------- */

void osc2_test_sys_boot_pack(const struct osc2_test_sys_boot *b,
			     uint32_t words[OSC2_TEST_SYS_BOOT_WORDS])
{
	words[0] = OSC2_TEST_SYS_BOOT_MAGIC;
	words[1] = b->buttons;
	words[2] = (uint32_t)b->flags | ((uint32_t)b->level << 8) | ((uint32_t)b->source << 16) |
		   ((uint32_t)b->hold << 24);
}

bool osc2_test_sys_boot_unpack(const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS],
			       struct osc2_test_sys_boot *b)
{
	memset(b, 0, sizeof(*b));
	if (words[0] != OSC2_TEST_SYS_BOOT_MAGIC) {
		return false;
	}
	b->buttons = words[1];
	b->flags = (uint8_t)(words[2] & OSC2_TEST_SYS_ALL);
	b->level = (uint8_t)(words[2] >> 8);
	b->source = (uint8_t)(words[2] >> 16);
	b->hold = (uint8_t)(words[2] >> 24);
	if (b->source > OSC2_SRC_DOCK) {
		b->flags &= (uint8_t)~OSC2_TEST_SYS_SOURCE;
	}
	return b->flags != 0;
}

void osc2_test_sys_end_pack(const struct osc2_test_sys_end *e,
			    uint32_t words[OSC2_TEST_SYS_END_WORDS])
{
	words[0] = OSC2_TEST_SYS_END_MAGIC;
	words[1] = (uint32_t)e->state | ((uint32_t)e->cause << 8) | ((uint32_t)e->end << 16) |
		   ((uint32_t)e->how << 24);
	words[2] = e->uptime_ms;
}

bool osc2_test_sys_end_unpack(const uint32_t words[OSC2_TEST_SYS_END_WORDS],
			      struct osc2_test_sys_end *e)
{
	memset(e, 0, sizeof(*e));
	if (words[0] != OSC2_TEST_SYS_END_MAGIC) {
		return false;
	}
	e->state = (uint8_t)words[1];
	e->cause = (uint8_t)(words[1] >> 8);
	e->end = (uint8_t)(words[1] >> 16);
	e->how = (uint8_t)(words[1] >> 24);
	e->uptime_ms = words[2];
	return true;
}

const char *osc2_test_sys_kind_name(uint8_t kind)
{
	switch (kind) {
	case OSC2_TEST_KIND_SYS_STATE: return "SYS_STATE";
	case OSC2_TEST_KIND_LED_MODE:  return "LED_MODE";
	case OSC2_TEST_KIND_CUE:       return "CUE";
	case OSC2_TEST_KIND_SYS_END:   return "SYS_END";
	case OSC2_TEST_KIND_CHORD:     return "CHORD";
	case OSC2_TEST_KIND_SOURCE:    return "SOURCE";
	case OSC2_TEST_KIND_IDLE:      return "IDLE";
	default:                       return NULL;
	}
}

static void add(char *out, size_t size, size_t *at, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*at + 1 >= size) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(&out[*at], size - *at, fmt, ap);
	va_end(ap);
	if (n > 0) {
		*at = *at + (size_t)n < size - 1 ? *at + (size_t)n : size - 1;
	}
}

bool osc2_test_sys_fields(char *out, size_t size, size_t *at, uint8_t kind, const uint8_t *d,
			  uint8_t n)
{
	static const char *const chord_what[] = { "?", "down", "up", "steam-press", "steam-hold",
						  "keylock-tap", "keylock-hold" };

	switch (kind) {
	case OSC2_TEST_KIND_SYS_STATE:
		if (n < 5) {
			return false;
		}
		add(out, size, at, " from=%s to=%s cause=%s transport=%u source=%u",
		    osc2_sys_state_name(d[0]), osc2_sys_state_name(d[1]), osc2_sys_event_name(d[2]),
		    (unsigned int)d[3], (unsigned int)d[4]);
		return true;
	case OSC2_TEST_KIND_LED_MODE:
		if (n < 4) {
			return false;
		}
		add(out, size, at, " mode=%u flags=%u level=%u full=%u", (unsigned int)d[0],
		    (unsigned int)d[1], (unsigned int)d[2], (unsigned int)d[3]);
		return true;
	case OSC2_TEST_KIND_CUE:
		if (n < 4) {
			return false;
		}
		add(out, size, at, " cue=%u script=%u gain=%d played=%u", (unsigned int)d[0],
		    (unsigned int)d[1], (int)(int8_t)d[2], (unsigned int)d[3]);
		return true;
	case OSC2_TEST_KIND_SYS_END:
		if (n < 4) {
			return false;
		}
		add(out, size, at, " state=%s cause=%s end=%u how=%u", osc2_sys_state_name(d[0]),
		    osc2_sys_event_name(d[1]), (unsigned int)d[2], (unsigned int)d[3]);
		return true;
	case OSC2_TEST_KIND_CHORD:
		if (n < 2) {
			return false;
		}
		add(out, size, at, " what=%s chord=%u", d[0] < 7 ? chord_what[d[0]] : "?",
		    (unsigned int)d[1]);
		return true;
	case OSC2_TEST_KIND_SOURCE:
		if (n < 3) {
			return false;
		}
		add(out, size, at, " from=%u to=%u event=%s", (unsigned int)d[0], (unsigned int)d[1],
		    d[2] == 0xFF ? "none" : osc2_sys_event_name(d[2]));
		return true;
	case OSC2_TEST_KIND_IDLE:
		if (n < 2) {
			return false;
		}
		add(out, size, at, " history=%04x", (unsigned int)opensc2_get_le16(d));
		return true;
	default:
		return false;
	}
}

/* ---- the ops --------------------------------------------------------------------------------- */

void osc2_test_sys_stop(void)
{
	/* With the core's lock held. A boot-time injection's source and level stay for the boot
	 * (the board would otherwise fall back to its real power at once), and its buttons until
	 * their hold time ends. */
	if (!sys.boot) {
		sys.level_on = false;
		sys.source_on = false;
	}
	if (!sys.hold_on) {
		sys.buttons_on = false;
	}
}

void osc2_test_sys_disarmed(void)
{
	if (osc2_test_sys_env) {
		(void)osc2_test_sys_env->boot_store(NULL);
	}
}

static uint8_t sys_inject(const struct osc2_test_env *env, const uint8_t *args)
{
	uint8_t flags = args[0], source = args[6];

	if ((flags & ~OSC2_TEST_SYS_ALL) != 0 || source > OSC2_SRC_DOCK || args[7] != 0) {
		return OSC2_TEST_BAD_ARG;
	}
	env->lock();
	if (flags & OSC2_TEST_SYS_BUTTONS) {
		sys.buttons_on = true;
		sys.hold_on = false;
		sys.buttons = opensc2_get_le32(&args[1]);
	}
	if (flags & OSC2_TEST_SYS_LEVEL) {
		sys.level_on = true;
		sys.level = args[5];
	}
	if (flags & OSC2_TEST_SYS_SOURCE) {
		sys.source_on = true;
		sys.source = source;
	}
	env->unlock();
	osc2_test_sys_env->changed();
	return OSC2_TEST_OK;
}

static uint8_t sys_boot(const uint8_t *args)
{
	struct osc2_test_sys_boot b = {
		.flags = args[0],
		.buttons = opensc2_get_le32(&args[1]),
		.level = args[5],
		.source = args[6],
		.hold = args[7],
	};
	uint32_t words[OSC2_TEST_SYS_BOOT_WORDS];

	if ((b.flags & ~OSC2_TEST_SYS_ALL) != 0 || b.source > OSC2_SRC_DOCK) {
		return OSC2_TEST_BAD_ARG;
	}
	osc2_test_sys_boot_pack(&b, words);
	if (osc2_test_sys_env->boot_store(b.flags ? words : NULL) != 0) {
		return OSC2_TEST_DENIED;   /* no retained RAM area in this build */
	}
	return OSC2_TEST_OK;
}

uint8_t osc2_test_sys_op(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t op,
			 const uint8_t *args, uint8_t alen, bool console, uint8_t *result,
			 uint8_t *rlen)
{
	uint8_t want;

	*rlen = 0;
	switch (op) {
	case OSC2_TEST_OP_SYS_INJECT:
	case OSC2_TEST_OP_SYS_BOOT:
		want = OSC2_TEST_SYS_ARGS_LEN;
		break;
	case OSC2_TEST_OP_SYS_INJECT_OFF:
	case OSC2_TEST_OP_SYS_STATUS:
		want = 0;
		break;
	case OSC2_TEST_OP_SYS_EVENT:
		want = 1;
		break;
	default:
		return OSC2_TEST_BAD_OP;
	}
	if (alen != want) {
		return OSC2_TEST_BAD_LEN;
	}
	if (console && op != OSC2_TEST_OP_SYS_STATUS && !c->armed) {
		return OSC2_TEST_NOT_ARMED;
	}
	if (!osc2_test_sys_env) {
		return OSC2_TEST_DENIED;   /* no system state machine in this build */
	}
	switch (op) {
	case OSC2_TEST_OP_SYS_INJECT:
		return sys_inject(env, args);
	case OSC2_TEST_OP_SYS_INJECT_OFF:
		env->lock();
		memset(&sys, 0, sizeof(sys));   /* a boot-time injection ends too */
		env->unlock();
		osc2_test_sys_env->changed();
		return OSC2_TEST_OK;
	case OSC2_TEST_OP_SYS_STATUS:
		osc2_test_sys_env->status(result);
		env->lock();
		result[7] = (uint8_t)(result[7] & ~0x1Eu);
		result[7] |= sys.buttons_on ? 0x02 : 0;
		result[7] |= sys.level_on ? 0x04 : 0;
		result[7] |= sys.source_on ? 0x08 : 0;
		result[7] |= sys.boot ? 0x10 : 0;
		env->unlock();
		*rlen = OSC2_TEST_SYS_STATUS_LEN;
		return OSC2_TEST_OK;
	case OSC2_TEST_OP_SYS_BOOT:
		return sys_boot(args);
	default: /* OSC2_TEST_OP_SYS_EVENT */
		if (args[0] != OSC2_TEST_SYS_EV_WIRELESS_OFF &&
		    args[0] != OSC2_TEST_SYS_EV_HOST_ASLEEP) {
			return OSC2_TEST_BAD_ARG;
		}
		osc2_test_sys_env->event(args[0]);
		return OSC2_TEST_OK;
	}
}

/* ---- the injected inputs, for test_sys_notes.c (with the environment's lock) ----------------- */

void osc2_test_sys_init(void)
{
	memset(&sys, 0, sizeof(sys));
}

bool osc2_test_sys_get(const struct osc2_test_env *env, uint8_t what, uint32_t *value)
{
	bool on = false;

	env->lock();
	switch (what) {
	case OSC2_TEST_SYS_BUTTONS:
		on = sys.buttons_on;
		*value = sys.buttons;
		break;
	case OSC2_TEST_SYS_LEVEL:
		on = sys.level_on;
		*value = sys.level;
		break;
	case OSC2_TEST_SYS_SOURCE:
		on = sys.source_on;
		*value = sys.source;
		break;
	default:
		break;
	}
	env->unlock();
	return on;
}

bool osc2_test_sys_booted(void)
{
	return sys.boot;
}

void osc2_test_sys_apply_boot(const struct osc2_test_env *env,
			      const uint32_t words[OSC2_TEST_SYS_BOOT_WORDS], uint32_t now_ms)
{
	struct osc2_test_sys_boot b;

	if (!osc2_test_sys_boot_unpack(words, &b)) {
		return;
	}
	env->lock();
	sys.boot = true;
	if (b.flags & OSC2_TEST_SYS_BUTTONS) {
		sys.buttons_on = true;
		sys.buttons = b.buttons;
		sys.hold_on = b.hold != 0;
		sys.release_ms = now_ms + (uint32_t)b.hold * 100u;
	}
	if (b.flags & OSC2_TEST_SYS_LEVEL) {
		sys.level_on = true;
		sys.level = b.level;
	}
	if (b.flags & OSC2_TEST_SYS_SOURCE) {
		sys.source_on = true;
		sys.source = b.source;
	}
	env->unlock();
}

int32_t osc2_test_sys_hold_next(const struct osc2_test_env *env, uint32_t now_ms)
{
	int32_t left = -1;

	env->lock();
	if (sys.hold_on) {
		left = (int32_t)(sys.release_ms - now_ms);
		if (left < 0) {
			left = 0;
		}
	}
	env->unlock();
	return left;
}

bool osc2_test_sys_hold_poll(const struct osc2_test_env *env, uint32_t now_ms)
{
	bool released = false;

	env->lock();
	if (sys.hold_on && (int32_t)(now_ms - sys.release_ms) >= 0) {
		/* The boot injection's buttons let go: every button up. */
		sys.hold_on = false;
		sys.buttons = 0;
		released = true;
	}
	env->unlock();
	return released;
}
