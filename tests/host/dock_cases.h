/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the dock.
 *   - dock.txt, the shared vectors (the frames and their CRC, the pilot, the supply rule, the
 *     docked states), through the library's own functions; the runner's self-tests read the same
 *     file;
 *   - the dock link (dock_link.c) on the real command channel (commands.c): sessions, the probe,
 *     the dock's own stored request, the lizard commands doing nothing there, attribute 11, GET
 *     input from the copy or from a new sample, the errors that end a session;
 *   - in the binary with the hardware test hooks: the dock ops 0x90 to 0x93 end to end (loading a
 *     frame in two parts, the answer read back with its serials and bonds masked, the allowlist,
 *     the ring entries, a test session ending with its arming, the console's lines).
 * Values from the protocol documentation (not public yet; docs/protocol.md lists the pages).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <opensc2/dock.h>
#include "dock_link.h"
#include "dock_rules.h"
#include "dock_stub.h"
#include "internal.h"
#include "stubs.h"

/* ---- dock.txt -------------------------------------------------------------------------------- */

static const char *const builtin_dock[] = {
	"CRC 313233343536373839 = 2189",
	"MV 2276 = 2000",
	"PILOT 1999 3301 2500 = 0 04",
	"POWER 1 1 2 IN 1 0 0 = - 1 2",
	"ENTER POWER_ON 1 = LINK_UP 1 KEYLOCK_CLEAR START_LINK LED_TRANSPORT",
	"SM LINK_UP 1 TURN_OFF 0 = RADIO_OFF 1 1 CHIME_OFF LED_CHARGING STOP_LINK",
	"SM RADIO_OFF 0 REBOOT 0 = RADIO_OFF 0 0 NONE",
};

static int dk_lookup(const char *const *names, int count, const char *word)
{
	for (int i = 0; i < count; i++) {
		if (strcmp(names[i], word) == 0) {
			return i;
		}
	}
	return -1;
}

static const char *const dk_states[] = { "AWAY", "RADIO_OFF", "LIFTED", "CHORD", "LINK_UP" };
static const char *const dk_events[] = { "POWER_GONE", "POWER_USB", "POWER_DOCK", "CHORD",
					 "CHORD_UP", "TIMER", "STEAM_7S", "LINK_OFF", "TURN_OFF",
					 "WARM_REBOOT", "REBOOT", "BOOTLOADER", "OTHER" };
static const char *const dk_entries[] = { "POWER_ON", "BATTERY_WAIT", "ON_BATTERY" };
/* The action names, in bit order (OSC2_DOCK_DO_*). */
static const char *const dk_actions[] = { "STOP_LINK", "START_LINK", "PAIRING", "SELECT", "CUE",
					  "CHIME_OFF", "TIMER_4S", "TIMER_5S", "TIMER_STOP",
					  "PAUSE", "REBOOT", "SHUTDOWN", "UPDATE_WINDOW",
					  "TO_BATTERY", "TO_CHORD", "LED_CHARGING", "LED_TRANSPORT",
					  "LED_COLOUR", "LED_OFF", "KEYLOCK_CLEAR" };
#define DK_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Action names from tok[i] to the end: their bits, or -1 for an unknown name. */
static long dk_action_bits(char *tok[], int n, int i)
{
	long bits = 0;

	for (; i < n; i++) {
		int b;

		if (strcmp(tok[i], "NONE") == 0) {
			continue;
		}
		b = dk_lookup(dk_actions, DK_COUNT(dk_actions), tok[i]);
		if (b < 0) {
			return -1;
		}
		bits |= 1L << b;
	}
	return bits;
}

/* Bytes from tok[*i]: "-" for none, else hex tokens joined. */
static int dk_bytes(char *tok[], int n, int *i, uint8_t *out, size_t max)
{
	if (*i < n && strcmp(tok[*i], "-") == 0) {
		(*i)++;
		return 0;
	}
	return vec_hex(tok, n, i, out, max);
}

static void dock_line(char *tok[], int n, void *ctx)
{
	int eq = vec_find(tok, n, "=");
	uint8_t buf[128], want[128];
	int i = 1, len, wlen;

	(void)ctx;
	CHECK(eq > 0 && eq + 1 < n);
	if (eq <= 0 || eq + 1 >= n) {
		return;
	}
	if (strcmp(tok[0], "CRC") == 0) {
		len = dk_bytes(tok, n, &i, buf, sizeof(buf));
		CHECK(len >= 0 && i == eq);
		CHECK_EQ(opensc2_dock_crc16(0, buf, (size_t)len), strtoul(tok[eq + 1], NULL, 16));
	} else if (strcmp(tok[0], "REQ") == 0) {
		uint8_t frame[OPENSC2_DOCK_FRAME_LEN];
		uint8_t type = (uint8_t)strtoul(tok[1], NULL, 16);

		i = 2;
		len = dk_bytes(tok, n, &i, buf, sizeof(buf));
		CHECK(len >= 0 && i == eq);
		i = eq + 1;
		wlen = vec_hex(tok, n, &i, want, sizeof(want));
		CHECK_EQ(wlen, OPENSC2_DOCK_FRAME_LEN);
		CHECK_EQ(opensc2_dock_build_request(type, buf, (size_t)len, frame), 73);
		CHECK(memcmp(frame, want, sizeof(frame)) == 0);
	} else if (strcmp(tok[0], "CHECK") == 0) {
		static const char *const results[] = { "OK", "BAD_SIZE", "BAD_CRC" };

		len = dk_bytes(tok, n, &i, buf, sizeof(buf));
		CHECK(len >= 0 && i == eq);
		CHECK_EQ(opensc2_dock_frame_check(buf, (size_t)len),
			 dk_lookup(results, 3, tok[eq + 1]));
	} else if (strcmp(tok[0], "MV") == 0) {
		CHECK_EQ(osc2_dock_pilot_mv(atoi(tok[1])), atoi(tok[eq + 1]));
	} else if (strcmp(tok[0], "PILOT") == 0) {
		int32_t mv[3] = { atoi(tok[1]), atoi(tok[2]), atoi(tok[3]) };
		uint8_t inside = 0;

		CHECK(eq == 4 && eq + 2 < n);
		CHECK_EQ(osc2_dock_pilot_present(mv, &inside), atoi(tok[eq + 1]) != 0);
		CHECK_EQ(inside, strtoul(tok[eq + 2], NULL, 16));
	} else if (strcmp(tok[0], "POWER") == 0) {
		struct osc2_dock_power_rule r = { .reported = atoi(tok[1]) != 0,
						  .present = atoi(tok[2]) != 0,
						  .kind = (enum osc2_dock_supply)atoi(tok[3]) };
		struct osc2_dock_power_in in = { .usb = atoi(tok[5]) != 0, .plus = atoi(tok[6]) != 0,
						 .pilot = atoi(tok[7]) != 0 };
		int ev;

		CHECK(eq == 8 && eq + 3 < n);
		ev = osc2_dock_power_step(&r, &in);
		CHECK_EQ(ev, strcmp(tok[eq + 1], "-") == 0 ? -1 : atoi(tok[eq + 1]));
		CHECK_EQ(r.present, atoi(tok[eq + 2]) != 0);
		CHECK_EQ(r.kind, atoi(tok[eq + 3]));
	} else if (strcmp(tok[0], "ENTER") == 0) {
		struct osc2_dock_sm sm;
		struct osc2_dock_step st;
		int how = dk_lookup(dk_entries, DK_COUNT(dk_entries), tok[1]);

		CHECK(how >= 0 && eq == 3 && eq + 2 < n);
		if (how < 0 || eq + 2 >= n) {
			return;
		}
		osc2_dock_sm_init(&sm);
		osc2_dock_sm_enter(&sm, (enum osc2_dock_entry)how, atoi(tok[2]) != 0, &st);
		CHECK_EQ(st.to, dk_lookup(dk_states, DK_COUNT(dk_states), tok[eq + 1]));
		CHECK_EQ(sm.state, st.to);
		CHECK_EQ(sm.link_selected, atoi(tok[eq + 2]) != 0);
		CHECK_EQ((long)st.actions, dk_action_bits(tok, n, eq + 3));
	} else if (strcmp(tok[0], "SM") == 0) {
		struct osc2_dock_sm sm;
		struct osc2_dock_step st;
		int from = dk_lookup(dk_states, DK_COUNT(dk_states), tok[1]);
		int ev = dk_lookup(dk_events, DK_COUNT(dk_events), tok[3]);

		CHECK(from >= 0 && ev >= 0 && eq == 5 && eq + 3 < n);
		if (from < 0 || ev < 0 || eq + 3 >= n) {
			return;
		}
		osc2_dock_sm_init(&sm);
		sm.state = (enum osc2_dock_state)from;
		sm.link_selected = atoi(tok[2]) != 0;
		sm.chord = 0;
		osc2_dock_sm_step(&sm, (enum osc2_dock_event)ev, (uint8_t)atoi(tok[4]), &st);
		CHECK_EQ(st.from, from);
		CHECK_EQ(st.to, dk_lookup(dk_states, DK_COUNT(dk_states), tok[eq + 1]));
		CHECK_EQ(sm.state, st.to);
		CHECK_EQ(sm.link_selected, atoi(tok[eq + 2]) != 0);
		CHECK_EQ(st.handled, atoi(tok[eq + 3]) != 0);
		CHECK_EQ((long)st.actions, dk_action_bits(tok, n, eq + 4));
	} else {
		CHECK(false);   /* an unknown dock record */
	}
}

OPENSC2_TEST(dock_vectors)
{
	vec_run_builtin(builtin_dock, sizeof(builtin_dock) / sizeof(builtin_dock[0]), dock_line,
			NULL);
	(void)vec_run_file("dock.txt", dock_line, NULL);
}

/* ---- the dock link on the command channel ---------------------------------------------------- */

static uint8_t dk_ans[OPENSC2_DOCK_FRAME_LEN];

/* One request to the dock link: the outcome; the answer in dk_ans. */
static enum osc2_dock_outcome dk_request(uint8_t type, const void *payload, size_t len)
{
	uint8_t req[OPENSC2_DOCK_FRAME_LEN];

	(void)opensc2_dock_build_request(type, payload, len, req);
	return osc2_dock_receive(req, sizeof(req), dk_ans, NULL);
}

static int32_t dk_result(void)
{
	return (int32_t)opensc2_get_le32(&dk_ans[OPENSC2_DOCK_RESULT_OFF]);
}

/* A SET on the dock, {opcode, n, payload}: the result. */
static int32_t dk_set(uint8_t opcode, const void *payload, uint8_t n)
{
	uint8_t rec[OPENSC2_DOCK_PAYLOAD_LEN] = { opcode, n };

	if (n) {
		memcpy(&rec[2], payload, n);
	}
	CHECK_EQ(dk_request(OPENSC2_DOCK_SET, rec, sizeof(rec)), OSC2_DOCK_ANSWERED);
	return dk_result();
}

static int32_t dk_get(void)
{
	CHECK_EQ(dk_request(OPENSC2_DOCK_GET, NULL, 0), OSC2_DOCK_ANSWERED);
	return dk_result();
}

OPENSC2_TEST(dock_link_probe)
{
	uint8_t rec[OPENSC2_IDENTITY_RECORD_LEN];
	struct osc2_dock_info info;

	stub_reset();
	CHECK(!osc2_dock_active());
	osc2_dock_session_start(OSC2_DOCK_BY_PILOT);
	CHECK(osc2_dock_active());
	CHECK_EQ(dk_request(OPENSC2_DOCK_PROBE, NULL, 0), OSC2_DOCK_ANSWERED);
	CHECK_EQ(dk_ans[0], 0xAC);
	CHECK_EQ(dk_ans[1], 0);
	CHECK_EQ(dk_result(), 0);
	(void)opensc2_encode_identity(&osc2.identity, rec);
	CHECK(memcmp(&dk_ans[8], rec, sizeof(rec)) == 0);
	CHECK_EQ(opensc2_get_le32(&dk_ans[8]), 0x1302);              /* the product ID first */
	CHECK(memcmp(&dk_ans[8 + 16], "OSC0123456789", 13) == 0);     /* the unit serial */
	CHECK_EQ(opensc2_dock_frame_check(dk_ans, sizeof(dk_ans)), OPENSC2_DOCK_FRAME_OK);
	/* a probe has no side effect: the same bytes again */
	{
		uint8_t first[OPENSC2_DOCK_FRAME_LEN];

		memcpy(first, dk_ans, sizeof(first));
		CHECK_EQ(dk_request(OPENSC2_DOCK_PROBE, NULL, 0), OSC2_DOCK_ANSWERED);
		CHECK(memcmp(first, dk_ans, sizeof(first)) == 0);
	}
	osc2_dock_info(&info);
	CHECK_EQ(info.sessions, 1);
	CHECK_EQ(info.frames, 2);
	CHECK_EQ(info.errors, 0);
}

OPENSC2_TEST(dock_link_commands)
{
	static const uint8_t set45[] = { 45, 75, 0 };
	static const uint8_t sel45 = 45;
	uint8_t reply[64];

	stub_reset();
	osc2_dock_session_start(OSC2_DOCK_BY_PILOT);
	/* USB is up (the stand-ins' boot): the dock answers 0x83 with USB's interval */
	CHECK_EQ(dk_get(), 63);
	CHECK_EQ(dk_ans[8], 0x83);
	CHECK_EQ(dk_ans[9], 30);
	CHECK_EQ(dk_ans[8 + 2 + 25], 11);
	CHECK_EQ(opensc2_get_le32(&dk_ans[8 + 2 + 26]), 4000);
	/* the dock alone: 64000 */
	stub.link_active[OSC2_LINK_USB] = false;
	CHECK(osc2_dock_alone());
	CHECK_EQ(dk_get(), 63);
	CHECK_EQ(opensc2_get_le32(&dk_ans[8 + 2 + 26]), 64000);
	/* the same question on USB, the dock alone: the dock is the current link */
	CHECK_EQ(usb_set(0x83, NULL, 0), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(opensc2_get_le32(&reply[29]), 64000);
	stub.link_active[OSC2_LINK_USB] = true;

	/* 0x87 runs as on every link; then a GET has nothing to read: -95 */
	CHECK_EQ(dk_set(0x87, set45, sizeof(set45)), 0);
	CHECK_EQ(osc2.settings[45], 75);
	CHECK_EQ(dk_get(), -95);
	CHECK_EQ(dk_ans[8], 0x87);
	CHECK_EQ(dk_ans[9], 0);
	/* 0x89 selects, the GET reads */
	CHECK_EQ(dk_set(0x89, &sel45, 1), 0);
	CHECK_EQ(dk_get(), 63);
	CHECK_EQ(dk_ans[8], 0x89);
	CHECK_EQ(dk_ans[9], 3);
	CHECK_EQ(dk_ans[10], 45);
	CHECK_EQ(opensc2_get_le16(&dk_ans[11]), 75);
	/* an opcode nobody knows, a length above 61: -22; the stored request stays */
	CHECK_EQ(dk_set(0xC1, NULL, 0), -22);
	{
		uint8_t rec[OPENSC2_DOCK_PAYLOAD_LEN] = { 0x83, 62 };

		CHECK_EQ(dk_request(OPENSC2_DOCK_SET, rec, sizeof(rec)), OSC2_DOCK_ANSWERED);
		CHECK_EQ(dk_result(), -22);
	}
	CHECK_EQ(dk_get(), 63);
	CHECK_EQ(dk_ans[8], 0x89);
	/* the dock's stored request is its own: USB still answers what USB selected */
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], 0x83);

	/* 0x81 and 0x85 from the dock: acknowledged, nothing happens */
	CHECK_EQ(dk_set(0x81, NULL, 0), 0);
	CHECK_EQ(dk_set(0x85, NULL, 0), 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LIZARD_MODE], 1);
	CHECK_EQ(stub.lizard_calls, 0);
	CHECK_EQ(stub.watchdog_armed, 0);
	/* ... while setting 9 can still be written from the dock */
	{
		static const uint8_t off[] = { 9, 0, 0 };

		CHECK_EQ(dk_set(0x87, off, sizeof(off)), 0);
		CHECK_EQ(osc2.settings[OPENSC2_SETTING_LIZARD_MODE], 0);
		CHECK_EQ(stub.lizard[0], 'L');
	}
	/* 0xBE answers over the dock (the battery report 0x43 never goes there) */
	CHECK_EQ(dk_set(0xBE, NULL, 0), 0);
	CHECK_EQ(dk_get(), 63);
	CHECK_EQ(dk_ans[9], OPENSC2_BATTERY_DATA_LEN);
}

OPENSC2_TEST(dock_link_input)
{
	uint8_t report[OPENSC2_STATE_REPORT_LEN];

	stub_reset();
	memset(report, 0, sizeof(report));
	report[0] = OPENSC2_RPT_STATE;
	report[1] = 7;
	report[2] = 0x11;
	/* no session: the copy is not kept */
	osc2_dock_cache_report(report);
	osc2_dock_session_start(OSC2_DOCK_BY_PILOT);
	CHECK_EQ(dk_request(OPENSC2_DOCK_INPUT, NULL, 0), OSC2_DOCK_ANSWERED);
	CHECK_EQ(dk_ans[1], 3);
	CHECK_EQ(dk_result(), 0);
	CHECK_EQ(dk_ans[8], 0);   /* zeros after boot */
	CHECK_EQ(dock_stub.samples, 0);
	/* another link up: the copy of its latest report, no new sample */
	osc2_dock_cache_report(report);
	CHECK_EQ(dk_request(OPENSC2_DOCK_INPUT, NULL, 0), OSC2_DOCK_ANSWERED);
	CHECK_EQ(dk_ans[8], 7);
	CHECK_EQ(dk_ans[9], 0x11);
	CHECK_EQ(dock_stub.samples, 0);
	/* the dock alone: each request takes a sample, answered at once */
	stub.link_active[OSC2_LINK_USB] = false;
	CHECK_EQ(dk_request(OPENSC2_DOCK_INPUT, NULL, 0), OSC2_DOCK_ANSWERED);
	CHECK_EQ(dock_stub.samples, 1);
	CHECK_EQ(dk_ans[8], 0);      /* the stand-in's first sequence number */
	CHECK_EQ(dk_ans[9], 0xD0);
	CHECK_EQ(dk_request(OPENSC2_DOCK_INPUT, NULL, 0), OSC2_DOCK_ANSWERED);
	CHECK_EQ(dock_stub.samples, 2);
	CHECK_EQ(dk_ans[8], 1);      /* one sequence number per request */
	/* 53 bytes after the ID, nothing more */
	for (int i = 8 + 53; i < 71; i++) {
		CHECK_EQ(dk_ans[i], 0);
	}
}

OPENSC2_TEST(dock_link_session_errors)
{
	uint8_t req[OPENSC2_DOCK_FRAME_LEN];
	struct osc2_dock_info info;

	stub_reset();
	osc2_dock_session_start(OSC2_DOCK_BY_PILOT);
	(void)opensc2_dock_build_request(OPENSC2_DOCK_PROBE, NULL, 0, req);
	/* a frame cut short by the gap timeout ends the session */
	CHECK_EQ(osc2_dock_receive(req, 40, dk_ans, NULL), OSC2_DOCK_BAD_SIZE);
	CHECK(!osc2_dock_active());
	osc2_dock_info(&info);
	CHECK_EQ(info.last_end, OSC2_DOCK_END_BAD_SIZE);
	CHECK_EQ(info.errors, 1);
	/* a new session; a bad CRC ends it too */
	osc2_dock_session_start(OSC2_DOCK_BY_PILOT);
	CHECK_EQ(osc2_dock_receive(req, sizeof(req), dk_ans, NULL), OSC2_DOCK_ANSWERED);
	req[20] ^= 1;
	CHECK_EQ(osc2_dock_receive(req, sizeof(req), dk_ans, NULL), OSC2_DOCK_BAD_CRC);
	CHECK(!osc2_dock_active());
	osc2_dock_info(&info);
	CHECK_EQ(info.sessions, 2);
	CHECK_EQ(info.frames, 1);
	CHECK_EQ(info.errors, 2);
	CHECK_EQ(info.last_end, OSC2_DOCK_END_BAD_CRC);
	/* not docked: attribute 11 is the other link's again */
	stub.link_active[OSC2_LINK_USB] = false;
	CHECK_EQ(osc2_dock_interval_us(4000), 4000);
	osc2_dock_session_start(OSC2_DOCK_BY_PILOT);
	CHECK_EQ(osc2_dock_interval_us(4000), OPENSC2_DOCK_INTERVAL_US);
	osc2_dock_session_end(OSC2_DOCK_END_STOPPED);
	osc2_dock_info(&info);
	CHECK_EQ(info.last_end, OSC2_DOCK_END_STOPPED);
}

/* ---- the dock ops of the test hooks ---------------------------------------------------------- */

#if defined(CONFIG_OPENSC2_TEST_HOOKS)

/* Load `frame` (73 bytes) in two DOCK_LOADs; both statuses OK. */
static void dk_load(const uint8_t frame[OPENSC2_DOCK_FRAME_LEN])
{
	uint8_t args[54];

	args[0] = 0;
	memcpy(&args[1], frame, 53);
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_LOAD, args, 54), OSC2_TEST_OK);
	args[0] = 53;
	memcpy(&args[1], &frame[53], 20);
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_LOAD, args, 21), OSC2_TEST_OK);
}

static int dk_run(uint8_t len, uint8_t flags)
{
	uint8_t args[2] = { len, flags };

	return con_op(OSC2_TEST_OP_DOCK_RUN, args, 2);
}

/* The whole answer as the PC reads it back (two DOCK_READs). */
static void dk_read(uint8_t out[OPENSC2_DOCK_FRAME_LEN])
{
	uint8_t args[2] = { 0, 55 };

	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_READ, args, 2), OSC2_TEST_OK);
	CHECK_EQ(hans_len, 4 + 2 + 55);
	memcpy(out, &RES[2], 55);
	args[0] = 55;
	args[1] = 18;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_READ, args, 2), OSC2_TEST_OK);
	memcpy(&out[55], &RES[2], 18);
}

static void dk_arm(void)
{
	CHECK_EQ(arm_on(false, 0xD0C0D0C0u, 0, 0), OSC2_TEST_OK);
}

OPENSC2_TEST(dock_ops_probe)
{
	uint8_t req[OPENSC2_DOCK_FRAME_LEN], back[OPENSC2_DOCK_FRAME_LEN];
	uint8_t real[OPENSC2_DOCK_FRAME_LEN] = { 0xAC, 0 };
	uint8_t serials[28];
	int i;

	stub_reset();
	dk_arm();
	(void)opensc2_dock_build_request(OPENSC2_DOCK_PROBE, NULL, 0, req);
	dk_load(req);
	/* no session, and none asked for */
	CHECK_EQ(dk_run(73, 0), OSC2_TEST_NO_LINK);
	CHECK(!osc2_dock_active());
	CHECK_EQ(dk_run(73, 1), OSC2_TEST_OK);
	CHECK_EQ(hans_len, 4 + 21);
	CHECK_EQ(RES[0], 0);      /* answered */
	CHECK_EQ(RES[1], 1);      /* the session runs */
	CHECK_EQ(RES[2], 0xAC);
	CHECK_EQ(RES[3], 0);
	CHECK_EQ(opensc2_get_le32(&RES[4]), 0);
	CHECK_EQ(RES[8], 0x03);   /* the CRC checks, bytes masked */
	/* what the board sent, rebuilt from the identity: the CRC-32s match */
	(void)opensc2_encode_identity(&osc2.identity, &real[8]);
	opensc2_dock_frame_seal(real, 0xAC);
	CHECK_EQ(opensc2_get_le32(&RES[9]), osc2_crc32(0, real, sizeof(real)));
	memcpy(serials, &real[24], sizeof(serials));
	CHECK_EQ(opensc2_get_le32(&RES[13]), osc2_crc32(0, serials, sizeof(serials)));
	/* read back: the serials as zeros, everything else as sent */
	dk_read(back);
	for (i = 0; i < OPENSC2_DOCK_FRAME_LEN; i++) {
		CHECK_EQ(back[i], i >= 24 && i < 52 ? 0 : real[i]);
	}
	/* the ring: the session's start (by the test hooks), the frame */
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_DOCK_SESSION, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].link, 5);
		CHECK_EQ(ents[i].len, 6);
		CHECK_EQ(ents[i].d[0], 0);
		CHECK_EQ(ents[i].d[1], OSC2_DOCK_BY_TEST);
	}
	i = find_kind(OSC2_TEST_KIND_DOCK_FRAME, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].link, 5);
		CHECK_EQ(ents[i].len, 11);
		CHECK_EQ(ents[i].d[0], 0);    /* the type */
		CHECK_EQ(ents[i].d[1], 0);    /* answered */
		CHECK_EQ(ents[i].d[2], 73);
	}
	/* the session ends with the arming, and does not come back with the next one */
	CHECK_EQ(con_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK(!osc2_dock_active());
	stub.now_ms += 5;
	dk_arm();
	CHECK(!osc2_dock_active());
	CHECK_EQ(dk_run(73, 0), OSC2_TEST_NO_LINK);
	/* a session that outlives nothing: the pilot's (the UART's) is not the hooks' to end */
	osc2_dock_session_start(OSC2_DOCK_BY_PILOT);
	CHECK_EQ(con_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK(osc2_dock_active());
	osc2_dock_session_end(OSC2_DOCK_END_STOPPED);
}

OPENSC2_TEST(dock_ops_commands)
{
	uint8_t req[OPENSC2_DOCK_FRAME_LEN], back[OPENSC2_DOCK_FRAME_LEN];
	uint8_t rec[OPENSC2_DOCK_PAYLOAD_LEN];
	static const uint8_t forbidden[] = { 0x86, 0x90, 0xEE, 0xEF, 0xF0, 0xA2, 0xDC, 0xD8,
					     OSC2_TEST_OPCODE };
	uint32_t got_crc;
	int i;

	stub_reset();
	dk_arm();
	/* nothing that writes flash, erases, resets or enters a bootloader: DENIED, not run */
	for (size_t k = 0; k < sizeof(forbidden); k++) {
		memset(rec, 0, sizeof(rec));
		rec[0] = forbidden[k];
		rec[1] = 4;
		opensc2_put_le32(&rec[2], 0xEFC1A917u);
		(void)opensc2_dock_build_request(OPENSC2_DOCK_SET, rec, sizeof(rec), req);
		dk_load(req);
		CHECK_EQ(dk_run(73, 1), OSC2_TEST_DENIED);
	}
	CHECK_EQ(stub.power_requests, 0);
	CHECK(!osc2_dock_active());   /* refused before a session started */
	/* 0x95 only with the turn-off key, as on HOST_SET */
	memset(rec, 0, sizeof(rec));
	rec[0] = 0x95;
	rec[1] = 4;
	opensc2_put_le32(&rec[2], 0xA427AF52u);
	(void)opensc2_dock_build_request(OPENSC2_DOCK_SET, rec, sizeof(rec), req);
	dk_load(req);
	CHECK_EQ(dk_run(73, 1), OSC2_TEST_DENIED);

	/* an allowed SET: 0xAE 1, then its GET, whose serial comes back masked */
	memset(rec, 0, sizeof(rec));
	rec[0] = 0xAE;
	rec[1] = 1;
	rec[2] = 1;
	(void)opensc2_dock_build_request(OPENSC2_DOCK_SET, rec, sizeof(rec), req);
	dk_load(req);
	CHECK_EQ(dk_run(73, 1), OSC2_TEST_OK);
	CHECK_EQ(RES[0], 0);
	CHECK_EQ(RES[3], 2);
	CHECK_EQ(opensc2_get_le32(&RES[4]), 0);
	CHECK_EQ(RES[8], 0x01);   /* nothing to mask in a SET's answer */
	(void)opensc2_dock_build_request(OPENSC2_DOCK_GET, NULL, 0, req);
	dk_load(req);
	CHECK_EQ(dk_run(73, 0), OSC2_TEST_OK);
	CHECK_EQ(RES[3], 1);
	CHECK_EQ(opensc2_get_le32(&RES[4]), 63);
	CHECK_EQ(RES[8], 0x03);
	got_crc = opensc2_get_le32(&RES[13]);
	dk_read(back);
	CHECK_EQ(back[8], 0xAE);
	CHECK_EQ(back[9], 20);
	for (i = 10; i < 30; i++) {
		CHECK_EQ(back[i], 0);
	}
	{
		uint8_t answer[20] = { 1 };

		memcpy(&answer[1], "OSC0123456789", 13);
		CHECK_EQ(got_crc, osc2_crc32(0, answer, sizeof(answer)));   /* as HOST_GET's */
	}
	/* the VENDOR entries carry the dock's link */
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_VENDOR, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].link, 5);
		CHECK_EQ(ents[i].d[1], 0xAE);
	}
	/* 0x81 through the dock: acknowledged and nothing else (no LIZARD entry) */
	memset(rec, 0, sizeof(rec));
	rec[0] = 0x81;
	(void)opensc2_dock_build_request(OPENSC2_DOCK_SET, rec, sizeof(rec), req);
	dk_load(req);
	CHECK_EQ(dk_run(73, 0), OSC2_TEST_OK);
	CHECK_EQ(opensc2_get_le32(&RES[4]), 0);
	CHECK_EQ(osc2.settings[OPENSC2_SETTING_LIZARD_MODE], 1);
	read_ring(0);
	CHECK_EQ(count_kind(OSC2_TEST_KIND_LIZARD), 1);   /* the ARM's resync only */
}

OPENSC2_TEST(dock_ops_errors)
{
	uint8_t req[OPENSC2_DOCK_FRAME_LEN];
	uint8_t args[8];
	int i;

	stub_reset();
	dk_arm();
	args[0] = 0;
	args[1] = 1;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_READ, args, 2), OSC2_TEST_BUSY);   /* no answer yet */
	(void)opensc2_dock_build_request(OPENSC2_DOCK_INPUT, NULL, 0, req);
	dk_load(req);
	/* nothing arrives: no answer, the session goes on waiting */
	CHECK_EQ(dk_run(0, 1), OSC2_TEST_OK);
	CHECK_EQ(RES[0], 3);
	CHECK_EQ(RES[1], 1);
	/* a short frame ends it */
	CHECK_EQ(dk_run(40, 0), OSC2_TEST_OK);
	CHECK_EQ(RES[0], 1);
	CHECK_EQ(RES[1], 0);
	/* a bad CRC ends a new one */
	req[30] ^= 0x40;
	dk_load(req);
	CHECK_EQ(dk_run(73, 1), OSC2_TEST_OK);
	CHECK_EQ(RES[0], 2);
	CHECK_EQ(RES[1], 0);
	read_ring(0);
	CHECK_EQ(count_kind(OSC2_TEST_KIND_DOCK_SESSION), 4);   /* two starts, two ends */
	i = find_kind(OSC2_TEST_KIND_DOCK_FRAME, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].d[1], 1);    /* size */
		CHECK_EQ(ents[i].d[2], 40);
	}
	/* bad arguments */
	args[0] = 70;
	memset(&args[1], 0, 4);
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_LOAD, args, 5), OSC2_TEST_BAD_ARG);   /* past 73 */
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_LOAD, args, 1), OSC2_TEST_BAD_LEN);
	CHECK_EQ(dk_run(74, 0), OSC2_TEST_BAD_ARG);
	CHECK_EQ(dk_run(73, 2), OSC2_TEST_BAD_ARG);
	args[0] = 60;
	args[1] = 14;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_READ, args, 2), OSC2_TEST_BAD_ARG);   /* past 73 */
	args[0] = 0;
	args[1] = 56;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_READ, args, 2), OSC2_TEST_BAD_ARG);   /* above 55 */
	args[0] = 9;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 1), OSC2_TEST_BAD_ARG);
	args[0] = 2;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 3), OSC2_TEST_BAD_LEN);
	/* unarmed: the console's NOT_ARMED */
	CHECK_EQ(con_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(dk_run(73, 1), OSC2_TEST_NOT_ARMED);
}

OPENSC2_TEST(dock_ops_ctrl)
{
	uint8_t args[8];
	int i;

	stub_reset();
	dk_arm();
	/* STATUS: nothing yet */
	args[0] = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 1), OSC2_TEST_OK);
	CHECK_EQ(hans_len, 4 + 22);
	CHECK_EQ(RES[0], 0);
	CHECK_EQ(RES[20], 0x83);   /* the dock's stored request after a boot */
	/* the pilot: one reading inside is not enough; two start a session */
	args[0] = 2;
	opensc2_put_le16(&args[1], 1999);
	opensc2_put_le16(&args[3], 2500);
	opensc2_put_le16(&args[5], 0xFFFF);
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 7), OSC2_TEST_OK);
	CHECK_EQ(RES[0], 0);
	CHECK_EQ(RES[1], 0x02);
	CHECK_EQ(RES[2], 0);
	opensc2_put_le16(&args[5], 3300);
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 7), OSC2_TEST_OK);
	CHECK_EQ(RES[0], 1);
	CHECK_EQ(RES[1], 0x06);
	CHECK_EQ(RES[2], 1);
	args[0] = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 1), OSC2_TEST_OK);
	CHECK_EQ(RES[0], 0x01 | 0x04);   /* active, a test session; USB is up, so not alone */
	CHECK_EQ(opensc2_get_le32(&RES[2]), 1);
	/* END */
	args[0] = 1;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 1), OSC2_TEST_OK);
	CHECK(!osc2_dock_active());
	/* the supply rule and the docked states: a cold power-on on the dock, recorded only */
	args[0] = 3;
	args[1] = 0;   /* usb */
	args[2] = 1;   /* plus */
	args[3] = 1;   /* pilot */
	args[4] = OSC2_DOCK_AT_POWER_ON;
	args[5] = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 6), OSC2_TEST_OK);
	CHECK_EQ(RES[0], OSC2_DOCK_SUPPLY_DOCK);
	CHECK_EQ(RES[1], OSC2_DOCK_AWAY);
	CHECK_EQ(RES[2], OSC2_DOCK_RADIO_OFF);
	CHECK_EQ(opensc2_get_le32(&RES[4]), OSC2_DOCK_DO_KEYLOCK_CLEAR | OSC2_DOCK_DO_LED_CHARGING);
	CHECK_EQ(stub.radio_events, 0);   /* nothing was done */
	/* a chord, held 4 s */
	args[0] = 4;
	args[1] = OSC2_DOCK_EV_CHORD;
	args[2] = OSC2_DOCK_CHORD_BT1;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 3), OSC2_TEST_OK);
	CHECK_EQ(RES[2], OSC2_DOCK_CHORD);
	args[1] = OSC2_DOCK_EV_TIMER;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 3), OSC2_TEST_OK);
	CHECK_EQ(RES[2], OSC2_DOCK_LINK_UP);
	CHECK(opensc2_get_le32(&RES[4]) & OSC2_DOCK_DO_PAIRING);
	/* lifted off with the link up: on battery */
	args[0] = 3;
	args[1] = 0;
	args[2] = 0;
	args[3] = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 6), OSC2_TEST_OK);
	CHECK_EQ(RES[0], OSC2_DOCK_SUPPLY_NONE);
	CHECK_EQ(RES[2], OSC2_DOCK_AWAY);
	CHECK_EQ(opensc2_get_le32(&RES[4]), OSC2_DOCK_DO_TO_BATTERY);
	/* the ring: one DOCK_STATE per step, one DOCK_PILOT per check */
	read_ring(0);
	CHECK_EQ(count_kind(OSC2_TEST_KIND_DOCK_STATE), 4);
	CHECK_EQ(count_kind(OSC2_TEST_KIND_DOCK_PILOT), 2);
	i = find_kind(OSC2_TEST_KIND_DOCK_STATE, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].len, 10);
		CHECK_EQ(ents[i].d[0], 1);   /* an entry into the dock states */
		CHECK_EQ(ents[i].d[4], OSC2_DOCK_RADIO_OFF);
	}
	/* RESET */
	args[0] = 5;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 1), OSC2_TEST_OK);
	args[0] = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_DOCK_CTRL, args, 1), OSC2_TEST_OK);
	CHECK_EQ(RES[18], OSC2_DOCK_AWAY);
}

OPENSC2_TEST(dock_ops_console_text)
{
	uint8_t req[OPENSC2_DOCK_FRAME_LEN];
	bool session = false, frame = false;

	stub_reset();
	dk_arm();
	(void)opensc2_dock_build_request(OPENSC2_DOCK_INPUT, NULL, 0, req);
	dk_load(req);
	CHECK_EQ(dk_run(73, 1), OSC2_TEST_OK);
	CHECK_EQ(con("ring"), 0);
	for (int i = 0; i < con_n; i++) {
		session |= strstr(con_lines[i], " dock DOCK_SESSION what=start reason=1 frames=0") !=
			   NULL;
		frame |= strstr(con_lines[i], " dock DOCK_FRAME type=3 outcome=answered len=73 rc=0 ")
			 != NULL;
	}
	CHECK(session);
	CHECK(frame);
}

#endif /* CONFIG_OPENSC2_TEST_HOOKS */
