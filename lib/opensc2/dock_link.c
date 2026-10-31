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
	struct osc2_dock_power_rule supply;   /* the supply rule on the board's readings */
} dock;

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
	memset(&dock.supply, 0, sizeof(dock.supply));
	k_spin_unlock(&dock.lock, key);
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
			starter == OSC2_DOCK_BY_TEST ? "test hooks" : "pilot");
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

enum osc2_dock_supply osc2_dock_supply_step(bool usb, bool plus, bool pilot)
{
	const struct osc2_dock_power_in in = { .usb = usb, .plus = plus, .pilot = pilot };
	k_spinlock_key_t key = k_spin_lock(&dock.lock);
	int ev = osc2_dock_power_step(&dock.supply, &in);
	enum osc2_dock_supply kind = dock.supply.kind;

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
	struct opensc2_identity id;

	ARG_UNUSED(ctx);
	opensc2_identity_get(&id);
	(void)opensc2_encode_identity(&id, out);
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
	int rc;

	ARG_UNUSED(ctx);
	(void)osc2_command_get_rc(OSC2_LINK_DOCK, out, &rc);
	if (rc == 0) {
		return OSC2_DOCK_RESULT_GET_OK;
	}
	/* A command that only acts (a SET of 0x87 selected nothing to read): -95 (EOPNOTSUPP). */
	return rc == -ENOTSUP || rc == -EOPNOTSUPP ? OSC2_DOCK_RESULT_NOTHING_TO_READ : rc;
}

static void op_input(void *ctx, uint8_t out[REPORT_BODY])
{
	k_spinlock_key_t key;

	ARG_UNUSED(ctx);
	if (osc2_dock_alone()) {
		/* The dock is the only link: each request takes one sample. The real controller
		 * answers with the report its previous request built (one poll old, inferred from
		 * its thread priorities); OpenSC2 answers with the one just built. */
		uint8_t report[OPENSC2_STATE_REPORT_LEN];

		osc2_dock_sample(report);
		memcpy(out, &report[1], REPORT_BODY);
		return;
	}
	/* Another link carries the reports: the copy of the latest one, at most one interval old,
	 * so the dock adds nothing to that link's stream and takes none of its sequence numbers. */
	key = k_spin_lock(&dock.lock);
	memcpy(out, dock.cache, REPORT_BODY);
	k_spin_unlock(&dock.lock, key);
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

	(void)k_mutex_lock(&frame_lock, K_FOREVER);
	t0 = osc2_dock_now_us();
	outcome = osc2_dock_answer(frame, len, ans, &ops, NULL);
	us = osc2_dock_now_us() - t0;
	if (outcome == OSC2_DOCK_ANSWERED) {
		result = (int32_t)opensc2_get_le32(&ans[OPENSC2_DOCK_RESULT_OFF]);
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
