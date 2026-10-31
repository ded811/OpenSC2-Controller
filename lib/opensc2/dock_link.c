/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * dock_link.c: the dock link (dock_link.h): sessions, the four requests and the copy of the state
 * report, on the library's command channel (OSC2_LINK_DOCK). No thread and no pins: dock.c's UART
 * and the test hooks' frame op call it, and the host tests build it as it is.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/logging/log.h>
#include <opensc2/docking.h>
#include "internal.h"
#include "dock_link.h"
#include "test_dock.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define REPORT_BODY (OPENSC2_STATE_REPORT_LEN - 1)   /* 0x42 without its ID */

static struct {
	struct k_spinlock lock;   /* everything below */
	bool active;
	uint8_t starter;
	uint8_t last_end;
	uint32_t sessions, frames, errors, slowest_us;
	uint32_t session_frames;
	bool cache_valid;
	uint8_t cache[REPORT_BODY];
	bool sample_due;                      /* GET input with the dock alone: a report to build */
	bool pilot;                           /* the last pilot check passed */
	struct osc2_dock_power_rule supply;   /* the supply rule on the board's readings */
} dock;

/* The answer, kept from frame to frame and session to session as the real controller keeps it:
 * each request rewrites only what it defines (dock_rules.h). Under frame_lock. */
static uint8_t last_answer[OPENSC2_DOCK_FRAME_LEN];

/* The probe's record, taken once per boot at the first probe (after opensc2_init(): the dock
 * thread waits for it), as the real controller fills it once at start-up: an identity stored
 * later reaches the dock at the next boot, as it reaches USB. Under frame_lock. */
static bool identity_taken;
static uint8_t identity_rec[OPENSC2_IDENTITY_RECORD_LEN];

/* One frame at a time, from the UART or from the test hooks: the stored request and the answer
 * of one must not mix with the other's. Taken before the command lock (commands.c). */
static K_MUTEX_DEFINE(frame_lock);

/* ---- sessions -------------------------------------------------------------------------------- */

void osc2_dock_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&dock.lock);

	dock.active = false;
	dock.starter = 0;
	dock.last_end = OSC2_DOCK_END_NONE;
	dock.sessions = dock.frames = dock.errors = dock.slowest_us = dock.session_frames = 0;
	dock.cache_valid = false;
	memset(dock.cache, 0, sizeof(dock.cache));
	dock.sample_due = false;
	dock.pilot = false;
	memset(&dock.supply, 0, sizeof(dock.supply));
	k_spin_unlock(&dock.lock, key);
	memset(last_answer, 0, sizeof(last_answer));
	identity_taken = false;
}

void osc2_dock_session_start(uint8_t starter)
{
	k_spinlock_key_t key = k_spin_lock(&dock.lock);
	bool was = dock.active;

	if (!was) {
		dock.active = true;
		dock.starter = starter;
		dock.sessions++;
		dock.session_frames = 0;
	}
	k_spin_unlock(&dock.lock, key);
	if (!was) {
		LOG_INF("Docked: dock session started (%s)",
			starter == OSC2_DOCK_BY_TEST ? "test hooks" :
			starter == OSC2_DOCK_BY_APP ? "application" : "pilot");
		osc2_test_note_dock_session(OSC2_TEST_DOCK_START, starter, 0);
	}
}

void osc2_dock_session_end(uint8_t reason)
{
	k_spinlock_key_t key = k_spin_lock(&dock.lock);
	bool was = dock.active;
	uint32_t frames = dock.session_frames;

	if (was) {
		dock.active = false;
		dock.last_end = reason;
	}
	k_spin_unlock(&dock.lock, key);
	if (was) {
		LOG_INF("Dock session ended (reason %u, %u frames)", reason, (unsigned int)frames);
		osc2_test_note_dock_session(OSC2_TEST_DOCK_END, reason, frames);
	}
}

bool osc2_dock_active(void)
{
	k_spinlock_key_t key = k_spin_lock(&dock.lock);
	bool on = dock.active;

	if (on && dock.starter == OSC2_DOCK_BY_TEST && !osc2_test_dock_armed()) {
		/* A session of the test hooks ends with their arming (whose ring is gone too). */
		dock.active = false;
		dock.last_end = OSC2_DOCK_END_STOPPED;
		on = false;
	}
	k_spin_unlock(&dock.lock, key);
	return on;
}

bool osc2_dock_alone(void)
{
	return osc2_dock_active() && !osc2_link_active(OSC2_LINK_USB) &&
	       !osc2_link_active(OSC2_LINK_RADIO) && !osc2_link_active(OSC2_LINK_BLE);
}

void osc2_dock_info(struct osc2_dock_info *info)
{
	bool active = osc2_dock_active();
	bool alone = osc2_dock_alone();
	k_spinlock_key_t key = k_spin_lock(&dock.lock);

	info->active = active;
	info->alone = alone;
	info->starter = dock.starter;
	info->last_end = dock.last_end;
	info->sessions = dock.sessions;
	info->frames = dock.frames;
	info->errors = dock.errors;
	info->slowest_us = dock.slowest_us;
	info->cache_valid = dock.cache_valid;
	info->supply = (uint8_t)dock.supply.kind;
	k_spin_unlock(&dock.lock, key);
}

/* ---- the state report's copy ----------------------------------------------------------------- */

void osc2_dock_cache_report(const uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	k_spinlock_key_t key;

	if (!osc2_dock_active()) {
		return;
	}
	key = k_spin_lock(&dock.lock);
	memcpy(dock.cache, &report[1], REPORT_BODY);
	dock.cache_valid = true;
	k_spin_unlock(&dock.lock, key);
}

uint32_t osc2_dock_interval_us(uint32_t other)
{
	return osc2_dock_alone() ? OPENSC2_DOCK_INTERVAL_US : other;
}

/* ---- the supply ------------------------------------------------------------------------------ */

enum osc2_dock_supply osc2_dock_supply_step(bool usb, bool plus, bool (*pilot_check)(void))
{
	struct osc2_dock_power_in in = { .usb = usb, .plus = plus };
	k_spinlock_key_t key = k_spin_lock(&dock.lock);
	bool posts = osc2_dock_power_posts(&dock.supply, usb, plus);
	enum osc2_dock_supply kind;
	int ev;

	k_spin_unlock(&dock.lock, key);
	/* A fresh pilot check at each change, as the real controller makes it the moment its supply
	 * input changes (the check sleeps: outside the lock). */
	if (posts && (usb || plus)) {
		in.pilot = pilot_check != NULL && pilot_check();
	}
	key = k_spin_lock(&dock.lock);
	ev = osc2_dock_power_step(&dock.supply, &in);
	kind = dock.supply.kind;
	k_spin_unlock(&dock.lock, key);
	if (ev >= 0) {
		LOG_INF("Supply: %s", kind == OSC2_DOCK_SUPPLY_DOCK ? "the dock's power" :
				      kind == OSC2_DOCK_SUPPLY_USB ? "USB power" : "none");
	}
	return kind;
}

/* ---- the requests, on the library ------------------------------------------------------------ */

static void op_identity(void *ctx, uint8_t out[OPENSC2_IDENTITY_RECORD_LEN])
{
	ARG_UNUSED(ctx);
	if (!identity_taken) {
		struct opensc2_identity id;

		opensc2_identity_get(&id);
		(void)opensc2_encode_identity(&id, identity_rec);
		identity_taken = true;
	}
	memcpy(out, identity_rec, OPENSC2_IDENTITY_RECORD_LEN);
}

static int32_t op_set(void *ctx, const uint8_t rec[OPENSC2_DOCK_PAYLOAD_LEN])
{
	int32_t rc = osc2_command_set(OSC2_LINK_DOCK, rec, OPENSC2_DOCK_PAYLOAD_LEN);

	ARG_UNUSED(ctx);
	/* An opcode nobody knows: -22 on the dock, as the real controller answers it there. */
	return rc == -ENOTSUP ? OSC2_DOCK_RESULT_INVALID : rc;
}

static int32_t op_get(void *ctx, uint8_t out[OPENSC2_DOCK_PAYLOAD_LEN])
{
	int rc = osc2_command_get_in_place(OSC2_LINK_DOCK, out);

	ARG_UNUSED(ctx);
	if (rc == 0) {
		return OSC2_DOCK_RESULT_GET_OK;
	}
	/* A command that only acts (a SET of 0x87 selected nothing to read): -95 (EOPNOTSUPP). */
	return rc == -ENOTSUP ? OSC2_DOCK_RESULT_NOTHING_TO_READ : rc;
}

static void op_input(void *ctx, uint8_t out[REPORT_BODY])
{
	k_spinlock_key_t key;

	ARG_UNUSED(ctx);
	/* The copy of the latest state report built while the session runs. With another link up
	 * that link builds them, so the dock takes none of its sequence numbers. With the dock alone
	 * each request builds one report, after its answer: as on the real controller, whose input
	 * thread builds it while the answer goes out, the answer carries the report the previous
	 * request built (the first of a session whatever the copy held). */
	key = k_spin_lock(&dock.lock);
	memcpy(out, dock.cache, REPORT_BODY);
	k_spin_unlock(&dock.lock, key);
	if (osc2_dock_alone()) {
		key = k_spin_lock(&dock.lock);
		dock.sample_due = true;
		k_spin_unlock(&dock.lock, key);
	}
}

void osc2_dock_after_answer(void)
{
	k_spinlock_key_t key = k_spin_lock(&dock.lock);
	bool due = dock.sample_due;

	dock.sample_due = false;
	k_spin_unlock(&dock.lock, key);
	if (due) {
		uint8_t report[OPENSC2_STATE_REPORT_LEN];

		osc2_dock_sample(report);   /* leaves its copy here for the next GET input */
	}
}

static const struct osc2_dock_ops ops = {
	.identity = op_identity,
	.set = op_set,
	.get = op_get,
	.input = op_input,
};

enum osc2_dock_outcome osc2_dock_receive(const uint8_t *frame, size_t len,
					 uint8_t ans[OPENSC2_DOCK_FRAME_LEN], uint32_t *handling_us)
{
	enum osc2_dock_outcome outcome;
	k_spinlock_key_t key;
	uint32_t t0, us;
	int32_t result = 0;

	/* A GET input's report that nobody built after its answer: built now, before this frame. */
	osc2_dock_after_answer();
	(void)k_mutex_lock(&frame_lock, K_FOREVER);
	t0 = osc2_dock_now_us();
	outcome = osc2_dock_answer(frame, len, last_answer, &ops, NULL);
	us = osc2_dock_now_us() - t0;
	if (outcome == OSC2_DOCK_ANSWERED) {
		memcpy(ans, last_answer, OPENSC2_DOCK_FRAME_LEN);
		result = (int32_t)opensc2_get_le32(&ans[OPENSC2_DOCK_RESULT_OFF]);
		osc2_test_note_dock_answer(frame, last_answer);
	}
	key = k_spin_lock(&dock.lock);
	if (outcome == OSC2_DOCK_ANSWERED) {
		dock.frames++;
		dock.session_frames++;
	} else {
		dock.errors++;
	}
	if (us > dock.slowest_us) {
		dock.slowest_us = us;
	}
	k_spin_unlock(&dock.lock, key);
	osc2_test_note_dock_frame(len >= 2 ? frame[OPENSC2_DOCK_TYPE_OFF] : 0xFF, (uint8_t)outcome,
				  (uint8_t)MIN(len, (size_t)255), result, us);
	if (outcome == OSC2_DOCK_BAD_SIZE) {
		LOG_WRN("Dock: a frame of %u bytes, not 73: the session ends", (unsigned int)len);
		osc2_dock_session_end(OSC2_DOCK_END_BAD_SIZE);
	} else if (outcome == OSC2_DOCK_BAD_CRC) {
		LOG_WRN("Dock: a frame whose CRC does not check: the session ends");
		osc2_dock_session_end(OSC2_DOCK_END_BAD_CRC);
	} else if (us > 50000u) {
		/* The puck waits 64 ms for the answer (a flash write can take that long). */
		LOG_WRN("Dock: the answer to type %u took %u us", frame[OPENSC2_DOCK_TYPE_OFF],
			(unsigned int)us);
	}
	(void)k_mutex_unlock(&frame_lock);
	if (handling_us) {
		*handling_us = us;
	}
	return outcome;
}

/* ---- the application's own wire (<opensc2/docking.h>) ---------------------------------------- */

int opensc2_dock_session_start(void)
{
	if (osc2_dock_active()) {
		return -EALREADY;
	}
	osc2_dock_session_start(OSC2_DOCK_BY_APP);
	return 0;
}

int opensc2_dock_session_end(void)
{
	if (!osc2_dock_active()) {
		return -EALREADY;
	}
	osc2_dock_session_end(OSC2_DOCK_END_STOPPED);
	return 0;
}

int opensc2_dock_frame(const uint8_t *frame, size_t len, uint8_t answer[OPENSC2_DOCK_FRAME_LEN])
{
	if (!osc2_dock_active()) {
		return -ENOTCONN;
	}
	switch (osc2_dock_receive(frame, len, answer, NULL)) {
	case OSC2_DOCK_ANSWERED:
		return 0;
	case OSC2_DOCK_BAD_SIZE:
		return -EMSGSIZE;
	default:
		return -EBADMSG;
	}
}

void opensc2_dock_answer_sent(void)
{
	osc2_dock_after_answer();
}

void osc2_dock_pilot_seen(bool present)
{
	k_spinlock_key_t key = k_spin_lock(&dock.lock);

	dock.pilot = present;
	k_spin_unlock(&dock.lock, key);
}

int opensc2_dock_status(struct opensc2_dock_status *st)
{
	struct osc2_dock_info info;
	k_spinlock_key_t key;

	osc2_dock_info(&info);
	memset(st, 0, sizeof(*st));
	st->session = info.active;
	st->alone = info.alone;
	key = k_spin_lock(&dock.lock);
	st->pilot = dock.pilot;
	k_spin_unlock(&dock.lock, key);
	st->supply = info.supply;
	st->stick_offsets = opensc2_dock_stick_offsets_active();
	st->sessions = info.sessions;
	st->frames = info.frames;
	st->errors = info.errors;
	st->slowest_us = info.slowest_us;
	return 0;
}
