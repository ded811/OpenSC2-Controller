/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only, in the binary built with CONFIG_OPENSC2_TEST_HOOKS: the hardware test hooks
 * (test_core.c, test_notes.c) on the fake environment of hooks_stub.c, with the real command
 * channel (commands.c): the claiming rule and the held answers, arming, the lease and the end of a
 * disarm, injection and frame-locked sequences, the lizard rule and the guard, the ring and its
 * entries, the counters, the host emulation's allowlist and masking, the restore, the console's
 * lines. Bytes are the ones of docs/test-protocol.md.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "stubs.h"
#include "hooks_stub.h"
#include "retained.h"
#include "test_core.h"

#define CORE (&osc2_test_core_obj)
#define ENV  (&osc2_test_env_obj)

static uint8_t hans[OSC2_TEST_ANSWER_MAX];   /* the last answer */
static uint8_t hans_len;
static uint8_t next_id = 1;

/* A request {key, version, request ID, op, arguments}; returns its length. */
static uint8_t make_req(uint8_t *out, uint8_t op, const void *args, uint8_t alen)
{
	opensc2_put_le32(out, OSC2_TEST_KEY);
	out[4] = OSC2_TEST_VERSION;
	out[5] = next_id++;
	out[6] = op;
	if (alen) {
		memcpy(&out[OSC2_TEST_HEADER_LEN], args, alen);
	}
	return (uint8_t)(OSC2_TEST_HEADER_LEN + alen);
}

/* On USB: the SET, then its GET. The answer's status; -1 when the SET stalled, -2 the GET. */
static int usb_op(uint8_t op, const void *args, uint8_t alen)
{
	uint8_t req[OSC2_TEST_REQUEST_MAX], reply[64];
	uint8_t n = make_req(req, op, args, alen);

	if (usb_set(OSC2_TEST_OPCODE, req, n) != 0) {
		return -1;
	}
	if (usb_get(reply, 64) < 0) {
		return -2;
	}
	if (reply[1] != OSC2_TEST_OPCODE || reply[2] < 4) {
		return -3;
	}
	hans_len = reply[2];
	memcpy(hans, &reply[3], hans_len);
	return hans[3];
}

/* On the console ("raw"). */
static int con_op(uint8_t op, const void *args, uint8_t alen)
{
	uint8_t req[OSC2_TEST_REQUEST_MAX];
	uint8_t n = make_req(req, op, args, alen);

	if (osc2_test_core_raw(CORE, ENV, req, n, hans, &hans_len) != 0) {
		return -1;
	}
	return hans[3];
}

#define RES (&hans[4])

static int arm_on(bool usb, uint32_t nonce, uint16_t lease, uint8_t flags)
{
	uint8_t args[7];

	opensc2_put_le32(args, nonce);
	opensc2_put_le16(&args[4], lease);
	args[6] = flags;
	return usb ? usb_op(OSC2_TEST_OP_ARM, args, 7) : con_op(OSC2_TEST_OP_ARM, args, 7);
}

static void lizard_off_usb(void)
{
	CHECK_EQ(usb_set(0x81, NULL, 0), 0);
}

/* ---- the ring, read back -------------------------------------------------------------------- */

struct entry {
	uint32_t seq, t;
	uint8_t kind, link, len;
	uint8_t d[OSC2_TEST_ENTRY_DATA_MAX];
	uint8_t raw[OSC2_TEST_ENTRY_MAX];
};

static struct entry ents[400];
static int n_ents;
static uint32_t ring_first, ring_next;

/* Every entry from `from` on, through RING_READ on the console (no USB or radio request moves). */
static void read_ring(uint32_t from)
{
	n_ents = 0;
	for (;;) {
		uint8_t args[5], count;
		size_t at = 9;

		opensc2_put_le32(args, from);
		args[4] = 0;
		if (con_op(OSC2_TEST_OP_RING_READ, args, sizeof(args)) != OSC2_TEST_OK) {
			break;
		}
		ring_first = opensc2_get_le32(&RES[0]);
		ring_next = opensc2_get_le32(&RES[4]);
		count = RES[8];
		CHECK(hans_len - 4 <= 9 + OSC2_TEST_READ_MAX);
		if (from < ring_first) {
			from = ring_first;
		}
		for (uint8_t i = 0; i < count && n_ents < (int)(sizeof(ents) / sizeof(ents[0])); i++) {
			const uint8_t *e = &RES[at];
			struct entry *x = &ents[n_ents++];

			x->seq = from++;
			x->t = opensc2_get_le32(e);
			x->kind = e[4];
			x->link = e[5];
			x->len = e[6];
			memcpy(x->d, &e[7], x->len);
			memcpy(x->raw, e, (size_t)(7 + x->len));
			at += (size_t)(7 + x->len);
		}
		CHECK_EQ(at, (size_t)(hans_len - 4));   /* whole entries only */
		if (count == 0 || from >= ring_next) {
			break;
		}
	}
}

static int find_kind(uint8_t kind, int start)
{
	for (int i = start; i < n_ents; i++) {
		if (ents[i].kind == kind) {
			return i;
		}
	}
	return -1;
}

static int count_kind(uint8_t kind)
{
	int n = 0;

	for (int i = 0; i < n_ents; i++) {
		n += ents[i].kind == kind;
	}
	return n;
}

/* ---- frames, as report.c builds them ---------------------------------------------------------- */

static uint8_t frame_seq;

static void build_frame(const struct opensc2_state *app, struct opensc2_state *s,
			uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	*s = *app;
	osc2_test_input(s);
	opensc2_encode_state(s, frame_seq++, report);
	osc2_test_frame(report);
}

static uint32_t frame_buttons(void)
{
	struct opensc2_state app, s;
	uint8_t report[OPENSC2_STATE_REPORT_LEN];

	memset(&app, 0, sizeof(app));
	build_frame(&app, &s, report);
	return s.buttons;
}

static void state52_buttons(uint8_t s52[OSC2_TEST_STATE52_LEN], uint32_t buttons, uint32_t nonce)
{
	memset(s52, 0, OSC2_TEST_STATE52_LEN);
	opensc2_put_le32(&s52[0], buttons);
	opensc2_put_le32(&s52[28], nonce);
}

static int inject(bool usb, uint8_t flags, uint32_t buttons, uint32_t nonce)
{
	uint8_t args[1 + OSC2_TEST_STATE52_LEN];

	args[0] = flags;
	state52_buttons(&args[1], buttons, nonce);
	return usb ? usb_op(OSC2_TEST_OP_INJECT, args, sizeof(args)) :
		     con_op(OSC2_TEST_OP_INJECT, args, sizeof(args));
}

static int seq_load(uint8_t step, uint8_t hold, uint32_t buttons)
{
	uint8_t args[2 + OSC2_TEST_STATE52_LEN];

	args[0] = step;
	args[1] = hold;
	state52_buttons(&args[2], buttons, 0x5E000000u | step);
	return con_op(OSC2_TEST_OP_SEQ_LOAD, args, sizeof(args));
}

static int seq_run(uint8_t count, uint8_t flags)
{
	uint8_t args[2] = { count, flags };

	return con_op(OSC2_TEST_OP_SEQ_RUN, args, sizeof(args));
}

static void get_status(uint8_t st[OSC2_TEST_STATUS_LEN])
{
	CHECK_EQ(con_op(OSC2_TEST_OP_STATUS, NULL, 0), OSC2_TEST_OK);
	memcpy(st, RES, OSC2_TEST_STATUS_LEN);
}

static void poll_at(int64_t ms)
{
	stub.now_ms = ms;
	osc2_test_core_poll(CORE, ENV);
}

static bool contains(const uint8_t *hay, size_t hay_len, const void *needle, size_t len)
{
	for (size_t i = 0; i + len <= hay_len; i++) {
		if (memcmp(&hay[i], needle, len) == 0) {
			return true;
		}
	}
	return false;
}

/* ---- the console, line by line ---------------------------------------------------------------- */

static char con_lines[64][256];
static int con_n;

static void collect(void *ctx, const char *line)
{
	(void)ctx;
	if (con_n < 64) {
		snprintf(con_lines[con_n++], sizeof(con_lines[0]), "%s", line);
	}
}

/* "opensc2 test <words>". */
static int con(const char *words)
{
	char buf[256];
	const char *argv[16];
	int argc = 0;
	char *p = buf;

	snprintf(buf, sizeof(buf), "%s", words);
	while (*p && argc < 16) {
		while (*p == ' ') {
			p++;
		}
		if (!*p) {
			break;
		}
		argv[argc++] = p;
		while (*p && *p != ' ') {
			p++;
		}
		if (*p) {
			*p++ = '\0';
		}
	}
	con_n = 0;
	return osc2_test_console_run(argc, argv, collect, NULL);
}

#define CHECK_LINE(i, text)                                                             \
	do {                                                                            \
		checks++;                                                               \
		if ((i) >= con_n || strcmp(con_lines[i], text) != 0) {                  \
			printf("  FAIL %s:%d: line %d is \"%s\", not \"%s\"\n", __FILE__,   \
			       __LINE__, (int)(i), (i) < con_n ? con_lines[i] : "(none)",  \
			       text);                                                   \
			current_failed = 1;                                             \
		}                                                                       \
	} while (0)

/* ================================================================================================
 * The claiming rule and the transports
 * ============================================================================================== */

OPENSC2_TEST(hooks_claim_rules)
{
	uint8_t req[OSC2_TEST_REQUEST_MAX], rec[2 + OSC2_TEST_REQUEST_MAX], reply[64];
	const uint8_t tag1 = 1;
	uint8_t n, mark[4] = { 1, 2, 3, 4 };

	stub_reset();
	CHECK_EQ(usb_set(0xAE, &tag1, 1), 0);   /* a stored request that must survive */
	n = make_req(req, OSC2_TEST_OP_STATUS, NULL, 0);
	/* Unarmed: a wrong key, version 2, n < 7 and every op but ARM take the usual path. */
	req[0] ^= 0x01;
	CHECK(usb_set(OSC2_TEST_OPCODE, req, n) < 0);
	req[0] ^= 0x01;
	req[4] = 2;
	CHECK(usb_set(OSC2_TEST_OPCODE, req, n) < 0);
	req[4] = OSC2_TEST_VERSION;
	CHECK(usb_set(OSC2_TEST_OPCODE, req, 6) < 0);
	CHECK(usb_set(OSC2_TEST_OPCODE, req, n) < 0);   /* STATUS */
	n = make_req(req, OSC2_TEST_OP_MARK, mark, 4);
	CHECK(usb_set(OSC2_TEST_OPCODE, req, n) < 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], 0xAE);   /* the stored request is the one before */
	CHECK(!osc2_test_core_armed(CORE));
	/* The radio: an error result, nothing claimed. */
	rec[0] = OSC2_TEST_OPCODE;
	rec[1] = n;
	memcpy(&rec[2], req, n);
	CHECK(osc2_command_set(OSC2_LINK_RADIO, rec, (uint8_t)(2 + n)) < 0);
	CHECK(!osc2_test_core_armed(CORE));
	for (int i = 0; i < OSC2_TEST_CNT_COUNT; i++) {
		CHECK_EQ(hk.counters[i], 0);   /* nothing counts while unarmed */
	}

	/* ARM is claimed while unarmed: the SET succeeds and its answer is held. */
	CHECK_EQ(arm_on(true, 0x11223344, 0, 0), OSC2_TEST_OK);
	CHECK(osc2_test_core_armed(CORE));
	/* Armed, a request with a wrong key still takes the usual path, and the failed SET keeps the
	 * stored request and its held answer. */
	n = make_req(req, OSC2_TEST_OP_STATUS, NULL, 0);
	req[1] ^= 0x10;
	CHECK(usb_set(OSC2_TEST_OPCODE, req, n) < 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], OSC2_TEST_OPCODE);
	CHECK_EQ(reply[5], OSC2_TEST_OP_ARM);
	/* Armed, any keyed op is claimed, also on the radio link. */
	CHECK_EQ(usb_op(OSC2_TEST_OP_STATUS, NULL, 0), OSC2_TEST_OK);
	n = make_req(req, OSC2_TEST_OP_MARK, mark, 4);
	rec[1] = n;
	memcpy(&rec[2], req, n);
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, (uint8_t)(2 + n)), 0);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_arm_answer)
{
	/* ARM, request ID 5, nonce 0x12345678, lease 15 s, as in docs/test-protocol.md. */
	static const uint8_t set[17] = { 0x01, 0x7E, 0x0E, 0x1E, 0xAB, 0x57, 0x7E, 0x01, 0x05,
					 0x01, 0x78, 0x56, 0x34, 0x12, 0x0F, 0x00, 0x00 };
	static const uint8_t armed_entry[11] = { 0, 0, 0, 0, 0x01, 0x01, 0x04,
						 0x78, 0x56, 0x34, 0x12 };
	uint8_t frame[64] = { 0 }, reply[64], st[OSC2_TEST_STATUS_LEN];
	const uint8_t *caps = &reply[7];

	stub_reset();
	memcpy(frame, set, sizeof(set));
	CHECK_EQ(osc2_usb_feature_set(frame, sizeof(frame)), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[0], 0x01);
	CHECK_EQ(reply[1], 0x7E);
	CHECK_EQ(reply[2], 0x28);   /* 4 + 36 */
	CHECK_EQ(reply[3], 0x01);   /* version */
	CHECK_EQ(reply[4], 0x05);   /* request ID */
	CHECK_EQ(reply[5], 0x01);   /* op */
	CHECK_EQ(reply[6], 0x00);   /* OK */
	CHECK_EQ(caps[0], 1);
	CHECK_EQ(caps[1], 1);       /* wired */
	CHECK_EQ(opensc2_get_le32(&caps[2]), OSC2_TEST_FLAG_TRIGGER_CLICK_AUTO |
		 OSC2_TEST_FLAG_LIZARD_MODE | OSC2_TEST_FLAG_STEAM_WATCHDOG | OSC2_TEST_FLAG_USB |
		 OSC2_TEST_FLAG_RULES);
	CHECK_EQ(opensc2_get_le16(&caps[6]), 3500);
	CHECK_EQ(opensc2_get_le16(&caps[8]), 500);
	CHECK_EQ(opensc2_get_le16(&caps[10]), 512);   /* CONFIG_OPENSC2_TEST_RING_SIZE here */
	CHECK_EQ(caps[12], 4);                        /* CONFIG_OPENSC2_TEST_SEQ_DEPTH here */
	CHECK_EQ(caps[13], 27);
	CHECK_EQ(opensc2_get_le16(&caps[14]), 2000);
	CHECK_EQ(opensc2_get_le16(&caps[16]), 15);
	CHECK_EQ(caps[18], 0);      /* policy auto */
	CHECK_EQ(caps[19], 0);
	CHECK(memcmp(&caps[20], "opensc2-0.2\0", 12) == 0);
	/* The product name's CRC-32 ("Steam Controller", as Python's binascii.crc32). */
	CHECK_EQ(opensc2_get_le32(&caps[32]), osc2_crc32(0, "Steam Controller", 16));
	CHECK_EQ(osc2_crc32(0, "Steam Controller", 16), 0x8A55859Eu);
	for (int i = 43; i < 64; i++) {
		CHECK_EQ(reply[i], 0);
	}
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_TEST_REQUESTS], 1);   /* the ARM itself */
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_FRAMES], 0);
	/* Entry 0 is ARMED (link 1, the transport), entry 1 the lizard resync. */
	read_ring(0);
	CHECK_EQ(n_ents, 2);
	CHECK(memcmp(ents[0].raw, armed_entry, sizeof(armed_entry)) == 0);
	CHECK_EQ(ents[1].kind, OSC2_TEST_KIND_LIZARD);
	CHECK_EQ(ents[1].d[0], OSC2_TEST_LIZARD_RESYNC);
	CHECK_EQ(ents[1].link, OSC2_TEST_LINK_USB);
	/* STATUS: armed, the guard, lizard mode on; the nonce. */
	get_status(st);
	CHECK_EQ(st[0], OSC2_TEST_ST_ARMED | OSC2_TEST_ST_GUARD | OSC2_TEST_ST_LIZARD);
	CHECK_EQ(st[1], 1);
	CHECK_EQ(st[4], OPENSC2_USB_CONFIGURED);
	CHECK_EQ(opensc2_get_le32(&st[8]), 0x12345678);
	CHECK_EQ(opensc2_get_le32(&st[16]), 0);
	CHECK_EQ(st[28], 0xFF);
	/* An ARM while armed is a fresh start: the ring and the counters restart. */
	CHECK_EQ(arm_on(true, 0x0BADCAFE, 0, 0), OSC2_TEST_OK);
	read_ring(0);
	CHECK_EQ(n_ents, 2);
	CHECK_EQ(opensc2_get_le32(ents[0].d), 0x0BADCAFE);
	/* Values the ARM refuses. */
	CHECK_EQ(arm_on(true, 1, 601, 0), OSC2_TEST_BAD_ARG);
	CHECK_EQ(arm_on(true, 1, 15, 0x02), OSC2_TEST_BAD_ARG);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_answer_per_link)
{
	const uint8_t tag1 = 1, sel9[2] = { 0x89, 1 }, nine = 9;
	uint8_t req[OSC2_TEST_REQUEST_MAX], rec[2 + OSC2_TEST_REQUEST_MAX], reply[64], out[63];
	uint8_t host_set[3] = { 0x89, 1, 9 }, n, usb_id, radio_id;

	(void)sel9;
	stub_reset();
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	/* USB and the test link interleaved: each answers its own request. */
	CHECK_EQ(usb_set(0xAE, &tag1, 1), 0);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, host_set, sizeof(host_set)), OSC2_TEST_OK);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], 0xAE);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(RES[0], 0x89);
	CHECK_EQ(RES[1], 3);
	CHECK_EQ(RES[3], nine);
	CHECK_EQ(opensc2_get_le16(&RES[4]), 1);   /* lizard mode on */
	/* A claimed request on the radio is held for the radio's GETs only. */
	n = make_req(req, OSC2_TEST_OP_STATUS, NULL, 0);
	radio_id = req[5];
	rec[0] = OSC2_TEST_OPCODE;
	rec[1] = n;
	memcpy(&rec[2], req, n);
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, (uint8_t)(2 + n)), 0);
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, out), 2 + 4 + OSC2_TEST_STATUS_LEN);
	CHECK_EQ(out[0], OSC2_TEST_OPCODE);
	CHECK_EQ(out[3], radio_id);
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, out), 2 + 4 + OSC2_TEST_STATUS_LEN);   /* again */
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], 0xAE);
	CHECK_EQ(usb_op(OSC2_TEST_OP_STATUS, NULL, 0), OSC2_TEST_OK);
	usb_id = hans[1];
	CHECK(usb_id != radio_id);
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, out), 2 + 4 + OSC2_TEST_STATUS_LEN);
	CHECK_EQ(out[3], radio_id);
	/* A SET the hooks did not claim replaces the radio's request: its held answer is dropped. */
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, (const uint8_t[]){ 0x83, 0 }, 2), 0);
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, out), 2 + 30);
	CHECK_EQ(out[0], 0x83);
	CHECK(!CORE->held[OSC2_LINK_RADIO].valid);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_answer_after_disarm)
{
	uint8_t req[OSC2_TEST_REQUEST_MAX], reply[64], out[63], rec[2 + OSC2_TEST_REQUEST_MAX];
	uint8_t n, id;

	stub_reset();
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	n = make_req(req, OSC2_TEST_OP_DISARM, NULL, 0);
	id = req[5];
	CHECK_EQ(usb_set(OSC2_TEST_OPCODE, req, n), 0);
	CHECK(!osc2_test_core_armed(CORE));
	/* The DISARM's own answer stays readable, as often as asked. */
	for (int i = 0; i < 2; i++) {
		CHECK_EQ(usb_get(reply, 64), 64);
		CHECK_EQ(reply[1], 0x7E);
		CHECK_EQ(reply[2], 4);
		CHECK_EQ(reply[3], 0x01);
		CHECK_EQ(reply[4], id);
		CHECK_EQ(reply[5], 0x02);
		CHECK_EQ(reply[6], 0x00);
	}
	/* A keyed STATUS now is not claimed: a STALL, and the failed SET keeps the held answer. */
	n = make_req(req, OSC2_TEST_OP_STATUS, NULL, 0);
	CHECK(usb_set(OSC2_TEST_OPCODE, req, n) < 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[4], id);
	/* The next successful SET drops it. */
	CHECK_EQ(usb_set(0x83, NULL, 0), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[1], 0x83);
	CHECK(!CORE->held[OSC2_LINK_USB].valid);

	/* The lease running out drops every held answer: a GET of the stored 0x7E then STALLs, and
	 * the radio answers {0x7E, 0}. */
	stub.now_ms = 0;
	CHECK_EQ(arm_on(true, 2, 1, 0), OSC2_TEST_OK);
	CHECK_EQ(usb_op(OSC2_TEST_OP_STATUS, NULL, 0), OSC2_TEST_OK);
	n = make_req(req, OSC2_TEST_OP_STATUS, NULL, 0);
	rec[0] = OSC2_TEST_OPCODE;
	rec[1] = n;
	memcpy(&rec[2], req, n);
	CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, (uint8_t)(2 + n)), 0);
	poll_at(1000);
	CHECK(!osc2_test_core_armed(CORE));
	CHECK(usb_get(reply, 64) < 0);
	CHECK_EQ(osc2_command_get(OSC2_LINK_RADIO, out), 2);
	CHECK_EQ(out[0], 0x7E);
	CHECK_EQ(out[1], 0);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_quiet)
{
	uint8_t req[OSC2_TEST_REQUEST_MAX], reply[64], n;

	stub_reset();
	CHECK(!osc2_test_quiet(OSC2_LINK_USB));
	/* A claimed SET and the GET the hooks answer are quiet (once each). */
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);   /* SET and GET */
	n = make_req(req, OSC2_TEST_OP_STATUS, NULL, 0);
	CHECK_EQ(usb_set(OSC2_TEST_OPCODE, req, n), 0);
	CHECK(osc2_test_quiet(OSC2_LINK_USB));
	CHECK(!osc2_test_quiet(OSC2_LINK_USB));
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK(osc2_test_quiet(OSC2_LINK_USB));
	/* A 0x7E the hooks do not claim is logged as today. */
	req[0] ^= 0xFF;
	CHECK(usb_set(OSC2_TEST_OPCODE, req, n) < 0);
	CHECK(!osc2_test_quiet(OSC2_LINK_USB));
	/* Ordinary commands are logged. */
	CHECK_EQ(usb_set(0x83, NULL, 0), 0);
	CHECK(!osc2_test_quiet(OSC2_LINK_USB));
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK(!osc2_test_quiet(OSC2_LINK_USB));
	/* The radio's claims are the radio's. */
	{
		uint8_t rec[2 + OSC2_TEST_REQUEST_MAX];

		n = make_req(req, OSC2_TEST_OP_STATUS, NULL, 0);
		rec[0] = OSC2_TEST_OPCODE;
		rec[1] = n;
		memcpy(&rec[2], req, n);
		CHECK_EQ(osc2_command_set(OSC2_LINK_RADIO, rec, (uint8_t)(2 + n)), 0);
		CHECK(!osc2_test_quiet(OSC2_LINK_USB));
		CHECK(osc2_test_quiet(OSC2_LINK_RADIO));
	}
}

/* ================================================================================================
 * The lease and the end of arming
 * ============================================================================================== */

OPENSC2_TEST(hooks_lease)
{
	uint8_t st[OSC2_TEST_STATUS_LEN];

	stub_reset();
	CHECK_EQ(arm_on(true, 7, 2, 0), OSC2_TEST_OK);
	CHECK_EQ(osc2_test_core_next(CORE, ENV), 2000);
	CHECK(hk.deadline_calls > 0);
	/* Every claimed request refreshes it. */
	stub.now_ms = 1500;
	CHECK_EQ(usb_op(OSC2_TEST_OP_STATUS, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(osc2_test_core_next(CORE, ENV), 2000);
	poll_at(3000);
	CHECK(osc2_test_core_armed(CORE));
	poll_at(3500);
	CHECK(!osc2_test_core_armed(CORE));
	get_status(st);   /* the console's STATUS answers unarmed */
	CHECK_EQ(st[0] & OSC2_TEST_ST_ARMED, 0);
	CHECK(st[0] & OSC2_TEST_ST_GUARD);
	CHECK(st[0] & OSC2_TEST_ST_COMPLETING);
	CHECK_EQ(osc2_test_core_next(CORE, ENV), 50);
	/* The guard stays on 50 ms more, then the resync, then the guard is off. */
	hk.resync_calls = 0;
	poll_at(3549);
	CHECK(osc2_test_core_guard(CORE));
	CHECK_EQ(hk.resync_calls, 0);
	poll_at(3550);
	CHECK(!osc2_test_core_guard(CORE));
	CHECK_EQ(hk.resync_calls, 1);
	CHECK_EQ(osc2_test_core_next(CORE, ENV), -1);
	get_status(st);
	CHECK_EQ(st[0] & (OSC2_TEST_ST_GUARD | OSC2_TEST_ST_COMPLETING), 0);

	/* An ARM inside the 50 ms cancels the completion. */
	stub.now_ms = 10000;
	CHECK_EQ(arm_on(false, 8, 1, 0), OSC2_TEST_OK);
	poll_at(11000);
	CHECK(!osc2_test_core_armed(CORE));
	CHECK(osc2_test_core_guard(CORE));
	stub.now_ms = 11020;
	CHECK_EQ(arm_on(false, 9, 0, 0), OSC2_TEST_OK);
	CHECK(!CORE->completing);
	poll_at(11070);
	CHECK(osc2_test_core_armed(CORE));
	CHECK(osc2_test_core_guard(CORE));
	CHECK_EQ(osc2_test_core_next(CORE, ENV), 15000 - 50);   /* the default lease */
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_restore)
{
	const uint8_t changes[6] = { 68, 40, 0, 45, 10, 0 }, color[4] = { 10, 20, 30, 40 };
	const uint8_t wd_off[3] = { 71, 0, 0 };
	uint8_t st[OSC2_TEST_STATUS_LEN];

	stub_reset();
	/* ARM with bit 0: the settings and the colour are taken; a second ARM keeps them. */
	CHECK_EQ(arm_on(true, 1, 0, OSC2_TEST_ARM_RESTORE), OSC2_TEST_OK);
	get_status(st);
	CHECK(st[0] & OSC2_TEST_ST_SNAPSHOT);
	CHECK_EQ(usb_set(0x87, changes, sizeof(changes)), 0);
	CHECK_EQ(usb_set(0xC5, color, sizeof(color)), 0);
	lizard_off_usb();
	CHECK_EQ(usb_set(0x87, wd_off, sizeof(wd_off)), 0);
	CHECK_EQ(arm_on(true, 2, 0, 0), OSC2_TEST_OK);
	get_status(st);
	CHECK(st[0] & OSC2_TEST_ST_SNAPSHOT);
	/* DISARM writes back what differs: all but 9 and 71 first, then 71, then 9; the colour. */
	hk.written_count = 0;
	hk.order_len = 0;
	CHECK_EQ(usb_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.written_count, 4);
	CHECK_EQ(hk.written[0], 45);
	CHECK_EQ(hk.written[1], 68);
	CHECK_EQ(hk.written[2], 71);
	CHECK_EQ(hk.written[3], 9);
	CHECK(strcmp(hk.order, "WWWWC") == 0);
	CHECK_EQ(osc2.settings[68], 90);
	CHECK_EQ(osc2.settings[45], 50);
	CHECK_EQ(osc2.settings[71], 1);
	CHECK_EQ(osc2.settings[9], 1);
	CHECK_EQ(osc2.led.red, 0);
	CHECK_EQ(osc2.led.white, 0);
	CHECK_EQ(osc2.led.brightness, 50);
	CHECK(!CORE->snap_on);

	/* Without bit 0 nothing is written back. */
	poll_at(100);
	CHECK_EQ(arm_on(true, 3, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(usb_set(0x87, changes, sizeof(changes)), 0);
	hk.written_count = 0;
	CHECK_EQ(usb_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.written_count, 0);
	CHECK_EQ(osc2.settings[68], 40);

	/* The lease running out restores too (a runner that died). */
	poll_at(200);
	CHECK_EQ(arm_on(true, 4, 1, OSC2_TEST_ARM_RESTORE), OSC2_TEST_OK);
	CHECK_EQ(usb_set(0x87, (const uint8_t[]){ 68, 99, 0 }, 3), 0);
	poll_at(1200);
	CHECK(!osc2_test_core_armed(CORE));
	CHECK_EQ(osc2.settings[68], 40);   /* the value at that ARM */
	CHECK_EQ(hk.violations, 0);
}

/* ================================================================================================
 * Injection, sequences, the lizard rule, the guard
 * ============================================================================================== */

OPENSC2_TEST(hooks_inject_frames)
{
	struct opensc2_state app, s;
	uint8_t report[OPENSC2_STATE_REPORT_LEN], st[OSC2_TEST_STATUS_LEN];

	stub_reset();
	memset(&app, 0, sizeof(app));
	app.stick[OPENSC2_LEFT].x = 77;   /* the application's own state */
	/* Unarmed nothing is replaced and nothing counted. */
	build_frame(&app, &s, report);
	CHECK(memcmp(&s, &app, sizeof(s)) == 0);
	CHECK(!osc2_test_skip_lizard());

	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	lizard_off_usb();
	CHECK_EQ(inject(true, 0, OPENSC2_BTN_A, 0xA5A50001u), OSC2_TEST_OK);
	CHECK_EQ(opensc2_get_le32(RES), 0);   /* frames at the request */
	build_frame(&app, &s, report);
	CHECK_EQ(s.buttons, OPENSC2_BTN_A);
	CHECK_EQ(s.stick[OPENSC2_LEFT].x, 0);
	CHECK_EQ(s.imu.timestamp_us, 0xA5A50001u);
	CHECK_EQ(opensc2_get_le32(&report[2]), 1);
	CHECK_EQ(opensc2_get_le32(&report[30]), 0xA5A50001u);
	CHECK(osc2_test_skip_lizard());
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_FRAMES], 1);
	get_status(st);
	CHECK(st[0] & OSC2_TEST_ST_INJECT);
	CHECK_EQ(opensc2_get_le32(&st[24]), 1);
	CHECK_EQ(st[7], report[1]);
	/* INJECT_OFF: the application's state again. */
	CHECK_EQ(usb_op(OSC2_TEST_OP_INJECT_OFF, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(opensc2_get_le32(RES), 1);
	build_frame(&app, &s, report);
	CHECK(memcmp(&s, &app, sizeof(s)) == 0);
	CHECK(!osc2_test_skip_lizard());
	/* The injected state never becomes the application's (osc2.state is not touched). */
	CHECK_EQ(osc2.state.buttons, 0);
	/* After a disarm nothing is injected any more. */
	CHECK_EQ(inject(true, 0, OPENSC2_BTN_B, 0xA5A50002u), OSC2_TEST_OK);
	CHECK_EQ(usb_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	build_frame(&app, &s, report);
	CHECK(memcmp(&s, &app, sizeof(s)) == 0);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_sequence)
{
	static const uint32_t three[6] = { 1, 1, 2, 4, 4, 4 };
	uint8_t st[OSC2_TEST_STATUS_LEN];
	int i;

	stub_reset();
	CHECK_EQ(arm_on(false, 1, 0, 0), OSC2_TEST_OK);
	lizard_off_usb();
	CHECK_EQ(seq_load(0, 2, 1), OSC2_TEST_OK);
	CHECK_EQ(seq_load(1, 1, 2), OSC2_TEST_OK);
	CHECK_EQ(seq_load(2, 3, 4), OSC2_TEST_OK);
	CHECK_EQ(seq_load(4, 1, 8), OSC2_TEST_BAD_ARG);   /* past the depth (4 here) */
	CHECK_EQ(seq_load(1, 0, 8), OSC2_TEST_BAD_ARG);   /* hold 0 */
	CHECK_EQ(seq_run(4, 0), OSC2_TEST_BAD_ARG);       /* step 3 not loaded */
	CHECK_EQ(seq_run(0, 0), OSC2_TEST_BAD_ARG);
	CHECK_EQ(seq_run(5, 0), OSC2_TEST_BAD_ARG);
	/* A frame half built when SEQ_RUN arrives does not count for step 0. */
	{
		struct opensc2_state app, s;
		uint8_t report[OPENSC2_STATE_REPORT_LEN];

		memset(&app, 0, sizeof(app));
		s = app;
		osc2_test_input(&s);
		CHECK_EQ(seq_run(3, 0), OSC2_TEST_OK);
		CHECK_EQ(opensc2_get_le32(RES), 0);
		opensc2_encode_state(&s, frame_seq++, report);
		osc2_test_frame(report);
	}
	CHECK_EQ(seq_load(0, 1, 9), OSC2_TEST_BUSY);
	CHECK_EQ(seq_run(1, 0), OSC2_TEST_BUSY);
	for (i = 0; i < 6; i++) {
		if (i == 1) {
			get_status(st);
			CHECK(st[0] & OSC2_TEST_ST_SEQ);
			CHECK_EQ(st[28], 0);                       /* step 0 ... */
			CHECK_EQ(opensc2_get_le16(&st[30]), 1);     /* ... one frame left */
			CHECK_EQ(st[29], 3);
		}
		CHECK_EQ(frame_buttons(), three[i]);
	}
	CHECK_EQ(frame_buttons(), 0);   /* back to the application's state */
	read_ring(0);
	CHECK_EQ(count_kind(OSC2_TEST_KIND_SEQ_STEP), 3);
	i = find_kind(OSC2_TEST_KIND_SEQ_STEP, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].d[0], 0);
		CHECK_EQ(opensc2_get_le32(&ents[i].d[1]), 1);   /* frame 0 was the half-built one */
		i = find_kind(OSC2_TEST_KIND_SEQ_STEP, i + 1);
		CHECK_EQ(opensc2_get_le32(&ents[i].d[1]), 3);
		i = find_kind(OSC2_TEST_KIND_SEQ_STEP, i + 1);
		CHECK_EQ(ents[i].d[0], 2);
		CHECK_EQ(opensc2_get_le32(&ents[i].d[1]), 4);
	}
	i = find_kind(OSC2_TEST_KIND_SEQ_END, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(opensc2_get_le32(ents[i].d), 7);
		CHECK_EQ(ents[i].d[4], OSC2_TEST_SEQ_END_BACK);
	}
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_SEQ_STEPS], 3);

	/* Loop: step 0 again after the last; SEQ_STOP ends it (how 2). */
	CHECK_EQ(seq_run(2, OSC2_TEST_SEQ_LOOP), OSC2_TEST_OK);
	for (i = 0; i < 7; i++) {
		CHECK_EQ(frame_buttons(), (uint32_t)((i % 3) < 2 ? 1 : 2));
	}
	CHECK_EQ(con_op(OSC2_TEST_OP_SEQ_STOP, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(frame_buttons(), 0);
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_SEQ_END, find_kind(OSC2_TEST_KIND_SEQ_END, 0) + 1);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].d[4], OSC2_TEST_SEQ_END_STOP);
	}
	/* Keep the last step: it becomes the static state. */
	CHECK_EQ(seq_run(2, OSC2_TEST_SEQ_KEEP_LAST), OSC2_TEST_OK);
	CHECK_EQ(frame_buttons(), 1);
	CHECK_EQ(frame_buttons(), 1);
	CHECK_EQ(frame_buttons(), 2);
	CHECK_EQ(frame_buttons(), 2);
	get_status(st);
	CHECK(st[0] & OSC2_TEST_ST_INJECT);
	CHECK_EQ(st[0] & OSC2_TEST_ST_SEQ, 0);
	/* A sequence wins over the static state, which comes back when it ends. */
	CHECK_EQ(seq_run(1, 0), OSC2_TEST_OK);
	CHECK_EQ(frame_buttons(), 1);
	CHECK_EQ(frame_buttons(), 1);
	CHECK_EQ(frame_buttons(), 2);
	/* INJECT with bit 0 stops a running sequence (how 2). */
	CHECK_EQ(seq_run(3, OSC2_TEST_SEQ_LOOP), OSC2_TEST_OK);
	CHECK_EQ(frame_buttons(), 1);
	CHECK_EQ(inject(false, OSC2_TEST_INJECT_STOP_SEQ, 0x40, 0x5E0000FF), OSC2_TEST_OK);
	CHECK_EQ(frame_buttons(), 0x40);
	CHECK_EQ(con_op(OSC2_TEST_OP_INJECT_OFF, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(frame_buttons(), 0);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_lizard_lock)
{
	const uint8_t on9[3] = { 9, 1, 0 };
	struct opensc2_state app, s;
	uint8_t report[OPENSC2_STATE_REPORT_LEN], st[OSC2_TEST_STATUS_LEN];
	int i, enter;

	stub_reset();
	memset(&app, 0, sizeof(app));
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	/* Lizard mode on (setting 9 = 1): refused without the lizard test, nothing injected. */
	CHECK_EQ(inject(true, 0, OPENSC2_BTN_A, 1), OSC2_TEST_DENIED);
	get_status(st);
	CHECK_EQ(st[0] & OSC2_TEST_ST_INJECT, 0);
	CHECK_EQ(seq_load(0, 1, 1), OSC2_TEST_OK);
	CHECK_EQ(seq_run(1, 0), OSC2_TEST_DENIED);
	/* With the lizard test lizard mode runs on the injected state. */
	CHECK_EQ(inject(true, OSC2_TEST_INJECT_LIZARD, OPENSC2_BTN_A, 2), OSC2_TEST_OK);
	build_frame(&app, &s, report);
	CHECK_EQ(s.buttons, OPENSC2_BTN_A);
	CHECK(!osc2_test_skip_lizard());
	CHECK_EQ(usb_op(OSC2_TEST_OP_INJECT_OFF, NULL, 0), OSC2_TEST_OK);

	/* Lizard mode off: injected, and the lizard step is skipped for that frame ... */
	lizard_off_usb();
	CHECK_EQ(inject(true, 0, OPENSC2_BTN_A, 3), OSC2_TEST_OK);
	build_frame(&app, &s, report);
	CHECK(osc2_test_skip_lizard());
	/* ... also when lizard mode comes on between the injection and the lizard step. */
	s = app;
	osc2_test_input(&s);
	CHECK_EQ(s.buttons, OPENSC2_BTN_A);
	CHECK_EQ(usb_set(0x87, on9, sizeof(on9)), 0);   /* LIZARD enter */
	CHECK(osc2_test_skip_lizard());
	opensc2_encode_state(&s, frame_seq++, report);
	osc2_test_frame(report);
	/* The next frame stops the injection: INJECT_STOP, built from the application's state. */
	build_frame(&app, &s, report);
	CHECK(memcmp(&s, &app, sizeof(s)) == 0);
	CHECK(!osc2_test_skip_lizard());
	get_status(st);
	CHECK_EQ(st[0] & OSC2_TEST_ST_INJECT, 0);
	read_ring(0);
	enter = -1;
	for (i = 0; i < n_ents; i++) {
		if (ents[i].kind == OSC2_TEST_KIND_LIZARD && ents[i].d[0] == OSC2_TEST_LIZARD_ENTER) {
			enter = i;
		}
	}
	i = find_kind(OSC2_TEST_KIND_INJECT_STOP, 0);
	CHECK(enter >= 0 && i > enter);
	if (i >= 0) {
		CHECK_EQ(ents[i].d[0], 1);
		CHECK_EQ(opensc2_get_le32(&ents[i].d[1]), 3);   /* frames 0..2 were injected */
		CHECK_EQ(ents[i].d[5], 0x01);
	}
	/* A sequence is stopped the same way: SEQ_END how 3, then INJECT_STOP what 2. */
	lizard_off_usb();
	CHECK_EQ(seq_run(1, OSC2_TEST_SEQ_LOOP), OSC2_TEST_OK);
	CHECK_EQ(frame_buttons(), 1);
	CHECK_EQ(usb_set(0x87, on9, sizeof(on9)), 0);
	CHECK_EQ(frame_buttons(), 0);
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_SEQ_END, 0);
	CHECK(i >= 0);
	if (i >= 0) {
		CHECK_EQ(ents[i].d[4], OSC2_TEST_SEQ_END_LIZARD);
		CHECK_EQ(ents[i + 1].kind, OSC2_TEST_KIND_INJECT_STOP);
		CHECK_EQ(ents[i + 1].d[5], 0x02);
	}
	/* With the lizard test a sequence runs with lizard mode on. */
	CHECK_EQ(seq_run(1, OSC2_TEST_SEQ_LIZARD | OSC2_TEST_SEQ_LOOP), OSC2_TEST_OK);
	CHECK_EQ(frame_buttons(), 1);
	CHECK(!osc2_test_skip_lizard());
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_arm_release)
{
	stub_reset();
	hk.release_result = true;
	/* Not armed, guard off: release what lizard mode holds, before the guard; then resync. */
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.release_calls, 1);
	CHECK(!hk.guard_at_release);
	CHECK(strcmp(hk.order, "RS") == 0);
	/* Armed: no release (nothing reached the host), but a resync. */
	CHECK_EQ(arm_on(true, 2, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.release_calls, 1);
	CHECK(strcmp(hk.order, "RSS") == 0);
	/* Inside the 50 ms after a disarm the guard is still on: no release either. */
	stub.now_ms = 1000;
	CHECK_EQ(usb_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	stub.now_ms = 1020;
	CHECK_EQ(arm_on(true, 3, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.release_calls, 1);
	/* After the completion, a fresh ARM releases again. */
	stub.now_ms = 2000;
	CHECK_EQ(usb_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	poll_at(2050);
	CHECK(!osc2_test_core_guard(CORE));
	CHECK_EQ(arm_on(true, 4, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.release_calls, 2);
	CHECK(!hk.guard_at_release);
	read_ring(0);
	CHECK_EQ(ents[0].kind, OSC2_TEST_KIND_ARMED);
	CHECK_EQ(ents[1].kind, OSC2_TEST_KIND_LIZARD);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_divert)
{
	static const uint8_t mouse[6] = { 0x40, 0, 0, 0, 0, 0 };
	static const uint8_t keyboard[9] = { 0x41, 0, 0, 0x28, 0, 0, 0, 0, 0 };
	static const uint8_t state[6] = { 0x42, 0, 0, 0, 0, 0 }, battery[3] = { 0x43, 4, 100 };
	static const uint8_t status44[6] = { 0x44, 3, 2, 0, 0, 0 };
	int i;

	stub_reset();
	/* Guard off: nothing is held back. */
	CHECK(!osc2_test_divert(mouse, sizeof(mouse)));
	CHECK(!osc2_test_divert(keyboard, sizeof(keyboard)));
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	/* Guard on: 0x40 and 0x41 are held back, the frame index with them; other IDs pass. */
	CHECK(osc2_test_divert(mouse, sizeof(mouse)));
	for (i = 0; i < 3; i++) {
		(void)frame_buttons();
	}
	CHECK(osc2_test_divert(keyboard, sizeof(keyboard)));
	CHECK(!osc2_test_divert(state, sizeof(state)));
	CHECK(!osc2_test_divert(battery, sizeof(battery)));
	CHECK(!osc2_test_divert(status44, sizeof(status44)));
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_LIZARD_DIVERTED], 2);
	read_ring(0);
	CHECK_EQ(count_kind(OSC2_TEST_KIND_LIZARD_RPT), 2);
	i = find_kind(OSC2_TEST_KIND_LIZARD_RPT, 0);
	CHECK_EQ(ents[i].d[0], 0x01);
	CHECK_EQ(opensc2_get_le32(&ents[i].d[1]), 0xFFFFFFFFu);   /* no frame since ARM yet */
	CHECK(memcmp(&ents[i].d[5], mouse, sizeof(mouse)) == 0);
	CHECK_EQ(ents[i].link, OSC2_TEST_LINK_USB);
	i = find_kind(OSC2_TEST_KIND_LIZARD_RPT, i + 1);
	CHECK_EQ(opensc2_get_le32(&ents[i].d[1]), 2);              /* the last frame built */
	CHECK_EQ(ents[i].len, 5 + sizeof(keyboard));
	CHECK(memcmp(&ents[i].d[5], keyboard, sizeof(keyboard)) == 0);
	/* Wireless, the entries name the radio. */
	osc2.mode = OPENSC2_MODE_WIRELESS;
	CHECK(osc2_test_divert(mouse, sizeof(mouse)));
	read_ring(0);
	CHECK_EQ(ents[n_ents - 1].link, OSC2_TEST_LINK_RADIO);
	osc2.mode = OPENSC2_MODE_WIRED;
	/* After a disarm the guard holds them back 50 ms more, recording nothing; then not at all. */
	stub.now_ms = 100;
	CHECK_EQ(usb_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK(osc2_test_divert(mouse, sizeof(mouse)));
	CHECK_EQ(CORE->ring.next, 0);
	poll_at(150);
	CHECK(!osc2_test_divert(mouse, sizeof(mouse)));
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_app_send)
{
	uint8_t canary_mouse[1 + 6] = { 0, 0x40 }, canary_kb[1 + 9] = { 0, 0x41 };
	uint8_t raw45[1 + 46] = { 0, 0x45 }, haptic[6] = { 1, 3, 0x02, 0, 0, 0 };
	int i;

	stub_reset();
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	/* The guard canary: an all-zero 0x40 and an empty 0x41 come back held back. */
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, canary_mouse, sizeof(canary_mouse)), OSC2_TEST_OK);
	CHECK_EQ(opensc2_get_le32(RES), 0);
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, canary_kb, sizeof(canary_kb)), OSC2_TEST_OK);
	CHECK_EQ(opensc2_get_le32(RES), 0);
	CHECK_EQ(hk.app_send_len, 0);   /* nothing went out */
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_LIZARD_DIVERTED], 2);
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_APP_REPORTS_SENT], 2);
	read_ring(0);
	CHECK_EQ(ents[2].kind, OSC2_TEST_KIND_LIZARD_RPT);
	CHECK(memcmp(&ents[2].d[5], &canary_mouse[1], 6) == 0);
	CHECK_EQ(ents[3].kind, OSC2_TEST_KIND_LIZARD_RPT);
	CHECK(memcmp(&ents[3].d[5], &canary_kb[1], 9) == 0);
	/* A raw report and the haptic stream status go to the application's send. */
	for (i = 0; i < 45; i++) {
		raw45[2 + i] = (uint8_t)i;
	}
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, raw45, sizeof(raw45)), OSC2_TEST_OK);
	CHECK_EQ(hk.app_send_len, 46);
	CHECK(memcmp(hk.app_send_last, &raw45[1], 46) == 0);
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, haptic, sizeof(haptic)), OSC2_TEST_OK);
	CHECK_EQ(hk.app_haptic_calls, 1);
	CHECK_EQ(hk.app_haptic_channel, 3);
	CHECK_EQ(hk.app_haptic_flags, 2);
	/* The send's result comes back as it is. */
	hk.app_send_rc = -ENOTCONN;
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, raw45, sizeof(raw45)), OSC2_TEST_OK);
	CHECK_EQ((int32_t)opensc2_get_le32(RES), -ENOTCONN);
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_APP_REPORTS_SENT], 3);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_battery)
{
	/* The battery line of the shared vectors: charging, 55 %, 3900 / 3300 / 5000 mV, -250 mA,
	 * 500 mA, temperature 0x1234, gauge flags 0xA5, send now. */
	static const uint8_t args[16] = { 0x02, 0x37, 0x3c, 0x0f, 0xe4, 0x0c, 0x88, 0x13,
					  0x06, 0xff, 0xf4, 0x01, 0x34, 0x12, 0xa5, 0x01 };
	static const uint8_t r43[15] = { 0x43, 0x02, 0x37, 0x3c, 0x0f, 0xe4, 0x0c, 0x88,
					 0x13, 0x06, 0xff, 0xf4, 0x01, 0x34, 0x12 };
	static const uint8_t rbe[15] = { 0x34, 0x12, 0x3c, 0x0f, 0xe4, 0x0c, 0x88, 0x13,
					 0x06, 0xff, 0xf4, 0x01, 0x37, 0xa5, 0x02 };
	struct opensc2_battery b = { .charge_state = 4, .level = 100 }, kept = b;
	uint8_t report[OPENSC2_BATTERY_REPORT_LEN], reply[64];

	stub_reset();
	osc2_test_battery(&b);   /* unarmed: untouched */
	CHECK(memcmp(&b, &kept, sizeof(b)) == 0);
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(usb_op(OSC2_TEST_OP_BATTERY, args, sizeof(args)), OSC2_TEST_OK);
	CHECK_EQ(hk.battery_now_calls, 1);
	osc2_test_battery(&b);
	opensc2_encode_battery(&b, report);
	CHECK(memcmp(report, r43, sizeof(r43)) == 0);
	/* 0xBE answers it too. */
	CHECK_EQ(usb_set(0xBE, NULL, 0), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[2], 15);
	CHECK(memcmp(&reply[3], rbe, sizeof(rbe)) == 0);
	/* Without send-now no report is asked for; BATTERY_OFF gives the application's back. */
	{
		uint8_t later[16];

		memcpy(later, args, sizeof(later));
		later[15] = 0;
		CHECK_EQ(usb_op(OSC2_TEST_OP_BATTERY, later, sizeof(later)), OSC2_TEST_OK);
		CHECK_EQ(hk.battery_now_calls, 1);
	}
	CHECK_EQ(usb_op(OSC2_TEST_OP_BATTERY_OFF, NULL, 0), OSC2_TEST_OK);
	b = kept;
	osc2_test_battery(&b);
	CHECK(memcmp(&b, &kept, sizeof(b)) == 0);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_deaf)
{
	uint8_t ms[2];

	stub_reset();
	osc2.mode = OPENSC2_MODE_WIRELESS;
	stub.link_active[OSC2_LINK_RADIO] = true;
	hk.radio_connected = true;
	CHECK_EQ(arm_on(false, 1, 0, 0), OSC2_TEST_OK);
	opensc2_put_le16(ms, 800);
	CHECK_EQ(con_op(OSC2_TEST_OP_RADIO_DEAF, ms, 2), OSC2_TEST_OK);
	CHECK_EQ(con_op(OSC2_TEST_OP_RADIO_DEAF, ms, 2), OSC2_TEST_BUSY);
	CHECK_EQ(osc2_test_take_deaf(), 800);
	CHECK_EQ(osc2_test_take_deaf(), 0);
	opensc2_put_le16(ms, 99);
	CHECK_EQ(con_op(OSC2_TEST_OP_RADIO_DEAF, ms, 2), OSC2_TEST_BAD_ARG);
	opensc2_put_le16(ms, 5001);
	CHECK_EQ(con_op(OSC2_TEST_OP_RADIO_DEAF, ms, 2), OSC2_TEST_BAD_ARG);
	hk.radio_connected = false;
	opensc2_put_le16(ms, 100);
	CHECK_EQ(con_op(OSC2_TEST_OP_RADIO_DEAF, ms, 2), OSC2_TEST_NO_LINK);
	/* The end of arming drops a request the radio thread has not taken. */
	hk.radio_connected = true;
	CHECK_EQ(con_op(OSC2_TEST_OP_RADIO_DEAF, ms, 2), OSC2_TEST_OK);
	CHECK_EQ(con_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(osc2_test_take_deaf(), 0);
	CHECK_EQ(hk.violations, 0);
}

/* ================================================================================================
 * The ring and its entries, the counters
 * ============================================================================================== */

OPENSC2_TEST(hooks_ring)
{
	uint8_t args[5];
	uint32_t first, marks_from = 0;
	int i;

	stub_reset();
	CHECK_EQ(arm_on(false, 1, 0, 0), OSC2_TEST_OK);
	for (i = 0; i < 300; i++) {
		uint8_t marker[4];

		opensc2_put_le32(marker, (uint32_t)i);
		CHECK_EQ(con_op(OSC2_TEST_OP_MARK, marker, 4), OSC2_TEST_OK);
		CHECK_EQ(opensc2_get_le32(RES), (uint32_t)(2 + i));   /* after ARMED and the resync */
	}
	read_ring(0);
	first = ring_first;
	CHECK(first > 0);   /* the 512-byte ring wrapped */
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_RING_EVICTED], first);
	CHECK_EQ(ring_next, 302);
	CHECK_EQ(n_ents, (int)(ring_next - first));
	for (i = 0; i < n_ents; i++) {
		CHECK_EQ(ents[i].kind, OSC2_TEST_KIND_MARK);
		CHECK_EQ(opensc2_get_le32(ents[i].d), ents[i].seq - 2);
		CHECK_EQ(ents[i].link, OSC2_TEST_LINK_TEST);
	}
	(void)marks_from;
	/* From any start, whole entries, as many as fit in 48 bytes (4 MARKs) or `max`. */
	opensc2_put_le32(args, first + 5);
	args[4] = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_RING_READ, args, 5), OSC2_TEST_OK);
	CHECK_EQ(RES[8], 4);
	CHECK_EQ(opensc2_get_le32(&RES[9 + 7]), first + 5 - 2);
	args[4] = 2;
	CHECK_EQ(con_op(OSC2_TEST_OP_RING_READ, args, 5), OSC2_TEST_OK);
	CHECK_EQ(RES[8], 2);
	/* Before `first`: from `first`. At `next`: nothing. Past `next`: BAD_ARG. */
	opensc2_put_le32(args, 0);
	args[4] = 1;
	CHECK_EQ(con_op(OSC2_TEST_OP_RING_READ, args, 5), OSC2_TEST_OK);
	CHECK_EQ(opensc2_get_le32(&RES[9 + 7]), first - 2);
	opensc2_put_le32(args, ring_next);
	CHECK_EQ(con_op(OSC2_TEST_OP_RING_READ, args, 5), OSC2_TEST_OK);
	CHECK_EQ(RES[8], 0);
	opensc2_put_le32(args, ring_next + 1);
	CHECK_EQ(con_op(OSC2_TEST_OP_RING_READ, args, 5), OSC2_TEST_BAD_ARG);
	/* RING_CLEAR: empty, the numbers go on. */
	CHECK_EQ(con_op(OSC2_TEST_OP_RING_CLEAR, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(opensc2_get_le32(RES), 302);
	read_ring(0);
	CHECK_EQ(n_ents, 0);
	CHECK_EQ(ring_first, 302);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_ring_entries)
{
	static const uint8_t armed[] = { 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x04,
					 0x78, 0x56, 0x34, 0x12 };
	static const uint8_t rumble[] = { 0xe8, 0x03, 0x00, 0x00, 0x11, 0x01, 0x0a, 0x10, 0x27,
					  0x20, 0x4e, 0xe8, 0x03, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t lizard_rpt[] = { 0x88, 0x13, 0x00, 0x00, 0x16, 0x01, 0x0e, 0x01,
					      0x05, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x28,
					      0x00, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t vendor87[] = { 0x10, 0x27, 0x00, 0x00, 0x18, 0x01, 0x0a, 0x00, 0x87,
					    0x03, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x64, 0x00 };
	static const uint8_t vendor_ee[] = { 0x00, 0xee, 0x21, 0x00, 0x00, 0x00, 0x00, 0x01, 0x18 };
	static const uint8_t pcm[] = { 0x06, 0x87, 0x00, 0x04, 0x04, 0xcd, 0xfb, 0x3c, 0xb6 };
	static const uint8_t output81[] = { 0x81, 0x08, 0x01, 0, 0, 0, 0, 0, 0, 0 };
	static const uint8_t pulse[] = { 0x00, 0x81, 0x00, 0x02, 0, 0, 0, 0, 0, 0 };
	static const uint8_t led[] = { 0x64, 0x00, 0x32, 0x0a, 0x00, 0x32 };
	static const uint8_t keyboard[9] = { 0x41, 0, 0, 0x28, 0, 0, 0, 0, 0 };
	struct opensc2_rumble r = { .left_speed = 10000, .right_speed = 20000, .intensity = 1000 };
	const uint8_t set87[3] = { 45, 100, 0 }, color[4] = { 100, 0, 50, 10 };
	uint8_t key_and_bond[9 + OPENSC2_BOND_LEN], out87[6] = { 0x87, 0x00, 1, 2, 3, 4 };
	uint8_t out81[8] = { 0x81 };
	int i;

	stub_reset();
	/* ARMED over USB at t 0 (the worked example). */
	CHECK_EQ(arm_on(true, 0x12345678, 0, 0), OSC2_TEST_OK);
	read_ring(0);
	CHECK(memcmp(ents[0].raw, armed, sizeof(armed)) == 0);
	/* RUMBLE, as the dispatch notes it, 1000 µs later. */
	hk.extra_us = 1000;
	osc2_test_note_rumble(OSC2_LINK_USB, &r);
	/* LIZARD_RPT: a keyboard Enter held back after frame 5, at 5000 µs. */
	for (i = 0; i < 6; i++) {
		(void)frame_buttons();
	}
	hk.extra_us = 5000;
	CHECK(osc2_test_divert(keyboard, sizeof(keyboard)));
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_RUMBLE, 0);
	CHECK(i >= 0 && memcmp(ents[i].raw, rumble, sizeof(rumble)) == 0);
	i = find_kind(OSC2_TEST_KIND_LIZARD_RPT, 0);
	CHECK(i >= 0 && memcmp(ents[i].raw, lizard_rpt, sizeof(lizard_rpt)) == 0);
	/* VENDOR SET 0x87 {45,100} over USB at 10000 µs: after its SETTING and LED entries. */
	hk.extra_us = 10000;
	CHECK_EQ(usb_set(0x87, set87, sizeof(set87)), 0);
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_SETTING, 0);
	CHECK(i >= 0 && ents[i + 1].kind == OSC2_TEST_KIND_LED && ents[i + 2].kind ==
	      OSC2_TEST_KIND_VENDOR);
	i = find_kind(OSC2_TEST_KIND_VENDOR, 0);
	CHECK(i >= 0 && memcmp(ents[i].raw, vendor87, sizeof(vendor87)) == 0);
	/* VENDOR SET 0xEE esb/bond with 24 bytes: the key and the value length only. */
	memcpy(key_and_bond, "esb/bond", 9);
	memset(&key_and_bond[9], 0x5A, OPENSC2_BOND_LEN);
	CHECK_EQ(usb_set(0xEE, key_and_bond, sizeof(key_and_bond)), 0);
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_VENDOR, find_kind(OSC2_TEST_KIND_VENDOR, 0) + 1);
	CHECK(i >= 0 && ents[i].len == sizeof(vendor_ee) && memcmp(ents[i].d, vendor_ee,
								    sizeof(vendor_ee)) == 0);
	/* HAPTIC PCM data on the test link: 0x87 target 0, 01 02 03 04. */
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_OUT, out87, sizeof(out87)), OSC2_TEST_OK);
	CHECK_EQ((int8_t)RES[0], 1);
	/* OUTPUT 0x81 with 8 bytes, and its pulse to the right pad. */
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_OUT, out81, sizeof(out81)), OSC2_TEST_OK);
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_HAPTIC, 0);
	CHECK(i >= 0 && ents[i].link == OSC2_TEST_LINK_TEST && ents[i].len == sizeof(pcm) &&
	      memcmp(ents[i].d, pcm, sizeof(pcm)) == 0);
	i = find_kind(OSC2_TEST_KIND_HAPTIC, i + 1);
	CHECK(i >= 0 && ents[i].len == sizeof(pulse) && memcmp(ents[i].d, pulse, sizeof(pulse)) == 0);
	CHECK(i >= 1 && ents[i - 1].kind == OSC2_TEST_KIND_OUTPUT &&
	      ents[i - 1].len == sizeof(output81) &&
	      memcmp(ents[i - 1].d, output81, sizeof(output81)) == 0);
	/* SETTING all back to defaults, and an LED entry with the colour. */
	CHECK_EQ(usb_set(0x8E, NULL, 0), 0);
	CHECK_EQ(usb_set(0xC5, color, sizeof(color)), 0);
	read_ring(0);
	for (i = 0; i < n_ents; i++) {
		if (ents[i].kind == OSC2_TEST_KIND_SETTING && ents[i].d[0] == 0xFF) {
			break;
		}
	}
	CHECK(i < n_ents && ents[i].len == 3 && ents[i].d[1] == 0 && ents[i].d[2] == 0);
	i = n_ents - 2;   /* the 0xC5's LED, then its VENDOR */
	CHECK(i >= 0 && ents[i].kind == OSC2_TEST_KIND_LED && memcmp(ents[i].d, led, sizeof(led)) == 0);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_vendor_mask)
{
	uint8_t d[19], payload[61];
	uint8_t key_and_bond[9 + OPENSC2_BOND_LEN], reply[64];
	struct opensc2_bond bond = { .puck_id = 0x11111111, .controller_id = 0x22222222 };
	const uint8_t tag1 = 1, sel0 = 0;

	/* The safe payload of each opcode. */
	for (int i = 0; i < 61; i++) {
		payload[i] = (uint8_t)(0xA0 + i);
	}
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0x87, 15, 0, payload, 15, 61), 7 + 12);
	CHECK(memcmp(&d[7], payload, 12) == 0);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0x87, 4, -EINVAL, payload, 4, 61), 7 + 3);
	CHECK_EQ((int32_t)opensc2_get_le32(&d[3]), -EINVAL);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0x89, 1, 0, payload, 1, 61), 8);
	CHECK_EQ(d[7], 0xA0);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xAE, 21, 0, payload, 21, 61), 8);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xF2, 0, 0, payload, 0, 61), 7);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xA1, 1, 0, payload, 1, 61), 8);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xC5, 2, 0, payload, 2, 61), 11);
	CHECK(d[7] == 0xA0 && d[8] == 0xA1 && d[9] == 0 && d[10] == 0);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xDC, 2, -EINVAL, payload, 2, 61), 9);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xE2, 0, 0, payload, 0, 61), 9);
	CHECK(d[7] == 0 && d[8] == 0);
	/* The 0x95 key as the command reads it: behind n = 0 too. */
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0x95, 0, 0, payload, 0, 61), 11);
	CHECK(memcmp(&d[7], payload, 4) == 0);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0x86, 0, 0, payload, 0, 2), 11);
	CHECK(d[7] == 0xA0 && d[8] == 0xA1 && d[9] == 0 && d[10] == 0);
	/* Commands that keep nothing, a bond write among them. */
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xA2, 24, 0, payload, 24, 61), 7);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0x81, 0, 0, payload, 0, 61), 7);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xC1, 16, -ENOTSUP, payload, 16, 61), 7);
	CHECK_EQ(osc2_test_enc_vendor(d, true, 0x83, 30, 0, payload, 0, 0), 7);
	CHECK_EQ(d[0], 1);
	CHECK_EQ(d[2], 30);
	/* Settings by name: the key and the value length. */
	memcpy(payload, "esb/bond_2", 11);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xEE, 35, 0, payload, 35, 61), 9);
	CHECK(d[7] == 2 && d[8] == 24);
	memcpy(payload, "cal/joy_l", 10);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xEF, 10, 0, payload, 10, 61), 9);
	CHECK(d[7] == 0 && d[8] == 0);
	memcpy(payload, "opensc2/unit_serial", 20);
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xEE, 24, 0, payload, 24, 61), 9);
	CHECK(d[7] == 5 && d[8] == 4);
	memset(payload, 'x', 8);   /* no NUL */
	CHECK_EQ(osc2_test_enc_vendor(d, false, 0xED, 8, 0, payload, 8, 61), 9);
	CHECK(d[7] == 0 && d[8] == 0);

	/* Through the command channel: neither a bond nor a serial ever reaches the ring. */
	stub_reset();
	memcpy(bond.puck_serial, "FAKEPUCK00001", 13);
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	memcpy(key_and_bond, "esb/bond", 9);
	opensc2_bond_pack(&bond, &key_and_bond[9]);
	CHECK_EQ(usb_set(0xEE, key_and_bond, sizeof(key_and_bond)), 0);
	CHECK_EQ(usb_set(0xED, key_and_bond, 9), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(reply[2], OPENSC2_BOND_LEN);
	CHECK_EQ(usb_set(0xA2, &key_and_bond[9], OPENSC2_BOND_LEN), 0);
	CHECK_EQ(usb_set(0xAE, &tag1, 1), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(usb_set(0xF2, &sel0, 1), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK(!contains(CORE->ring.buf, sizeof(CORE->ring.buf), &key_and_bond[9], OPENSC2_BOND_LEN));
	CHECK(!contains(CORE->ring.buf, sizeof(CORE->ring.buf), &key_and_bond[9], 8));
	CHECK(!contains(CORE->ring.buf, sizeof(CORE->ring.buf), "FAKEPUCK", 8));
	CHECK(!contains(CORE->ring.buf, sizeof(CORE->ring.buf), "OSC0123456789", 13));
	CHECK(!contains(CORE->ring.buf, sizeof(CORE->ring.buf), "OSB0123456789", 13));
	read_ring(0);
	CHECK(count_kind(OSC2_TEST_KIND_VENDOR) >= 8);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_vendor_entries)
{
	const uint8_t set87[3] = { 45, 100, 0 };
	const uint8_t audio[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x09, 0x05, 0xFF };
	uint8_t reply[64], frame[64] = { 1, 0x83 };
	int i;

	stub_reset();
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(usb_set(0x87, set87, sizeof(set87)), 0);
	CHECK(usb_get(reply, 64) < 0);                  /* a set-only command */
	CHECK(usb_set(0xC1, audio, sizeof(audio)) < 0); /* unknown */
	CHECK_EQ(usb_set(0x83, NULL, 0), 0);
	CHECK_EQ(usb_get(reply, 64), 64);
	CHECK_EQ(usb_op(OSC2_TEST_OP_STATUS, NULL, 0), OSC2_TEST_OK);   /* none for 0x7E */
	CHECK_EQ(osc2_usb_feature_set(frame, 63), -EMSGSIZE);
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_VENDOR_SET_OK], 2);
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_VENDOR_SET_ERR], 1);
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_VENDOR_GET_OK], 1);
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_VENDOR_GET_ERR], 1);
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_USB_FRAME_ERR], 1);
	CHECK_EQ(hk.counters[OSC2_TEST_CNT_TEST_REQUESTS], 2);
	read_ring(0);
	CHECK_EQ(count_kind(OSC2_TEST_KIND_VENDOR), 5);
	for (i = 0; i < n_ents; i++) {
		if (ents[i].kind == OSC2_TEST_KIND_VENDOR) {
			CHECK(ents[i].d[1] != OSC2_TEST_OPCODE);
			CHECK_EQ(ents[i].link, OSC2_TEST_LINK_USB);
		}
	}
	i = find_kind(OSC2_TEST_KIND_VENDOR, 0);
	CHECK_EQ(ents[i].d[1], 0x87);
	i = find_kind(OSC2_TEST_KIND_VENDOR, i + 1);   /* the GET of the 0x87 */
	CHECK_EQ(ents[i].d[0], 1);
	CHECK_EQ(ents[i].d[2], 0);
	CHECK_EQ((int32_t)opensc2_get_le32(&ents[i].d[3]), -ENOTSUP);
	CHECK_EQ(ents[i].len, 7 + 3);   /* the stored request's triplet */
	i = find_kind(OSC2_TEST_KIND_VENDOR, i + 1);
	CHECK_EQ(ents[i].d[1], 0xC1);
	CHECK((int32_t)opensc2_get_le32(&ents[i].d[3]) < 0);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_settings_entries)
{
	const uint8_t five[5][3] = { { 37, 1, 0 }, { 45, 100, 0 }, { 45, 150, 0 },
				     { 45, 0xFB, 0xFF }, { 37, 0, 0 } };
	int i, leds = 0, settings = 0, last_setting = -1;

	stub_reset();
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	for (i = 0; i < 5; i++) {
		CHECK_EQ(usb_set(0x87, five[i], 3), 0);
	}
	read_ring(0);
	for (i = 0; i < n_ents; i++) {
		if (ents[i].kind == OSC2_TEST_KIND_LED) {
			leds++;
		}
		if (ents[i].kind == OSC2_TEST_KIND_SETTING) {
			settings++;
			last_setting = i;
			CHECK_EQ(ents[i + 1].kind, OSC2_TEST_KIND_LED);   /* the SETTING first */
		}
	}
	CHECK_EQ(leds, 5);       /* one for every 0x87 that names 37 or 45 */
	CHECK_EQ(settings, 4);   /* 37 = 1, 45 = 100, 45 = 0, 37 = 0 */
	CHECK(last_setting > 0 && ents[last_setting].d[0] == 37 && ents[last_setting].d[1] == 0);
	/* 0x81 while on: LIZARD leave, then SETTING 9 = 0; again: switches down only. */
	CHECK_EQ(con_op(OSC2_TEST_OP_RING_CLEAR, NULL, 0), OSC2_TEST_OK);
	lizard_off_usb();
	lizard_off_usb();
	read_ring(ring_next);
	CHECK_EQ(n_ents, 3 + 2);   /* LIZARD, SETTING, VENDOR; then LIZARD, VENDOR */
	CHECK(ents[0].kind == OSC2_TEST_KIND_LIZARD && ents[0].d[0] == OSC2_TEST_LIZARD_LEAVE);
	CHECK(ents[1].kind == OSC2_TEST_KIND_SETTING && ents[1].d[0] == 9 && ents[1].d[1] == 0);
	CHECK(ents[3].kind == OSC2_TEST_KIND_LIZARD && ents[3].d[0] == OSC2_TEST_LIZARD_DOWN);
	/* 0x8E while lizard mode is off: LIZARD enter, SETTING all, LED. */
	CHECK_EQ(con_op(OSC2_TEST_OP_RING_CLEAR, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(usb_set(0x8E, NULL, 0), 0);
	read_ring(ring_next);
	CHECK(n_ents >= 3 && ents[0].kind == OSC2_TEST_KIND_LIZARD &&
	      ents[0].d[0] == OSC2_TEST_LIZARD_ENTER);
	CHECK(ents[1].kind == OSC2_TEST_KIND_SETTING && ents[1].d[0] == 0xFF);
	CHECK_EQ(ents[2].kind, OSC2_TEST_KIND_LED);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_host_out)
{
	const uint8_t rumble[10] = { 0x80, 0x00, 0xE8, 0x03, 0x10, 0x27, 0x00, 0x20, 0x4E, 0x00 };
	const uint8_t nothing[2] = { 0x8A, 0x00 }, short80[3] = { 0x80, 1, 2 };
	int i;

	stub_reset();
	CHECK_EQ(arm_on(false, 1, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_OUT, rumble, sizeof(rumble)), OSC2_TEST_OK);
	CHECK_EQ((int8_t)RES[0], 1);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_OUT, nothing, sizeof(nothing)), OSC2_TEST_OK);
	CHECK_EQ((int8_t)RES[0], 0);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_OUT, short80, sizeof(short80)), OSC2_TEST_OK);
	CHECK_EQ((int8_t)RES[0], -1);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_OUT, NULL, 0), OSC2_TEST_BAD_LEN);
	read_ring(0);
	i = find_kind(OSC2_TEST_KIND_OUTPUT, 0);
	CHECK(i >= 0 && ents[i].d[0] == 0x80 && ents[i].d[1] == 10 && ents[i].d[2] == 1);
	CHECK(i >= 0 && ents[i].link == OSC2_TEST_LINK_TEST);
	CHECK(i >= 0 && ents[i + 1].kind == OSC2_TEST_KIND_RUMBLE);
	CHECK_EQ(opensc2_get_le16(&ents[i + 1].d[0]), 10000);
	CHECK_EQ(opensc2_get_le16(&ents[i + 1].d[2]), 20000);
	CHECK_EQ(opensc2_get_le16(&ents[i + 1].d[4]), 1000);
	i = find_kind(OSC2_TEST_KIND_OUTPUT, i + 1);
	CHECK(i >= 0 && ents[i].d[0] == 0x8A && ents[i].d[2] == 0);
	CHECK(i >= 0 && ents[i + 1].kind == OSC2_TEST_KIND_OUTPUT);   /* nothing else for 0x8A */
	CHECK((int8_t)ents[i + 1].d[2] == -1);
	CHECK_EQ(hk.host_out_calls, 3);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_paging)
{
	uint8_t args[2], reply[64];

	stub_reset();
	CHECK_EQ(arm_on(false, 1, 0, 0), OSC2_TEST_OK);
	for (int i = 0; i < OSC2_TEST_CNT_COUNT; i++) {
		hk.counters[i] = 1000u + (uint32_t)i;
	}
	args[0] = 0;
	args[1] = 13;
	CHECK_EQ(con_op(OSC2_TEST_OP_CNT_READ, args, 2), OSC2_TEST_OK);
	CHECK(RES[0] == 0 && RES[1] == 13);
	CHECK_EQ(opensc2_get_le32(&RES[2 + 4 * 12]), 1012);
	args[0] = 13;
	CHECK_EQ(con_op(OSC2_TEST_OP_CNT_READ, args, 2), OSC2_TEST_OK);
	CHECK_EQ(RES[1], 13);
	CHECK_EQ(opensc2_get_le32(&RES[2]), 1013);
	args[0] = 26;
	CHECK_EQ(con_op(OSC2_TEST_OP_CNT_READ, args, 2), OSC2_TEST_OK);
	CHECK_EQ(RES[1], 1);   /* clipped at the end of the table */
	CHECK_EQ(opensc2_get_le32(&RES[2]), 1026);
	args[0] = 27;
	CHECK_EQ(con_op(OSC2_TEST_OP_CNT_READ, args, 2), OSC2_TEST_OK);
	CHECK_EQ(RES[1], 0);
	args[1] = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_CNT_READ, args, 2), OSC2_TEST_BAD_ARG);
	args[1] = 14;
	CHECK_EQ(con_op(OSC2_TEST_OP_CNT_READ, args, 2), OSC2_TEST_BAD_ARG);
	CHECK_EQ(con_op(OSC2_TEST_OP_CNT_RESET, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.counters[5], 0);

	osc2.settings[27] = -20;
	for (uint8_t first = 0; first < 86; first += 27) {
		args[0] = first;
		args[1] = 27;
		CHECK_EQ(con_op(OSC2_TEST_OP_SETTINGS_READ, args, 2), OSC2_TEST_OK);
		CHECK_EQ(RES[1], first == 81 ? 5 : 27);
		for (uint8_t i = 0; i < RES[1]; i++) {
			CHECK_EQ((int16_t)opensc2_get_le16(&RES[2 + 2 * i]), osc2.settings[first + i]);
		}
	}
	args[0] = 86;
	args[1] = 1;
	CHECK_EQ(con_op(OSC2_TEST_OP_SETTINGS_READ, args, 2), OSC2_TEST_OK);
	CHECK_EQ(RES[1], 0);
	args[0] = 0;
	args[1] = 28;
	CHECK_EQ(con_op(OSC2_TEST_OP_SETTINGS_READ, args, 2), OSC2_TEST_BAD_ARG);
	/* The same values as 0x89. */
	for (uint8_t number = 0; number < 86; number += 9) {
		args[0] = number;
		args[1] = 1;
		CHECK_EQ(con_op(OSC2_TEST_OP_SETTINGS_READ, args, 2), OSC2_TEST_OK);
		CHECK_EQ(usb_set(0x89, &number, 1), 0);
		CHECK_EQ(usb_get(reply, 64), 64);
		CHECK_EQ(opensc2_get_le16(&reply[4]), opensc2_get_le16(&RES[2]));
	}
	CHECK_EQ(hk.violations, 0);
}

/* ================================================================================================
 * The host emulation: the allowlist and the masking
 * ============================================================================================== */

static bool allowed_opcode(uint8_t op)
{
	static const uint8_t list[] = { 0x81, 0x83, 0x85, 0x87, 0x89, 0x8E, 0x9F, 0xA1,
					0xAE, 0xBE, 0xC5, 0xDB, 0xE9, 0xED, 0xF2 };

	for (size_t i = 0; i < sizeof(list); i++) {
		if (list[i] == op) {
			return true;
		}
	}
	return false;
}

OPENSC2_TEST(hooks_host_set_allow)
{
	static const uint8_t turn_off[4] = { 0x10, 0x97, 0xDF, 0xAB };
	static const uint8_t factory[4] = { 0x17, 0xA9, 0xC1, 0xEF };
	static const uint8_t bootloader[4] = { 0xC0, 0xBA, 0xAA, 0xEC };
	uint8_t args[2 + 4];

	stub_reset();
	CHECK_EQ(arm_on(false, 1, 0, 0), OSC2_TEST_OK);
	/* Every opcode: only the allowlist reaches the command channel. */
	for (int op = 0; op <= 0xFF; op++) {
		bool allowed = allowed_opcode((uint8_t)op);

		hk.host_set_calls = 0;
		args[0] = (uint8_t)op;
		args[1] = 4;
		memcpy(&args[2], factory, 4);   /* a payload that would be harmful where it counts */
		CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, sizeof(args)),
			 allowed ? OSC2_TEST_OK : OSC2_TEST_DENIED);
		CHECK_EQ(hk.host_set_calls, allowed ? 1 : 0);
	}
	CHECK_EQ(stub.power_requests, 0);   /* no factory reset, no bootloader, no reboot */
	CHECK_EQ(stub.saved_bonds, 0);
	/* 0x95 only with the turn-off key, read from the record's payload as the command does. */
	args[0] = 0x95;
	args[1] = 4;
	memcpy(&args[2], turn_off, 4);
	hk.host_set_calls = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, 6), OSC2_TEST_OK);
	CHECK_EQ(hk.host_set_calls, 1);
	CHECK_EQ(stub.power_requests, 0);   /* wired: the turn-off is ignored */
	memcpy(&args[2], bootloader, 4);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, 6), OSC2_TEST_DENIED);
	args[1] = 2;   /* 10 97: the key reads 0x00009710, a plain reboot */
	memcpy(&args[2], turn_off, 2);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, 4), OSC2_TEST_DENIED);
	args[1] = 0;   /* n = 0: the key reads 0 */
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, 2), OSC2_TEST_DENIED);
	/* n = 0 with the key behind it in a longer record: not a valid HOST_SET at all. */
	memcpy(&args[2], turn_off, 4);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, 6), OSC2_TEST_BAD_LEN);
	CHECK_EQ(hk.host_set_calls, 1);
	CHECK(!osc2_test_host_set_allowed(0x95, turn_off, 0));
	CHECK(osc2_test_host_set_allowed(0x95, turn_off, 4));
	CHECK(osc2_test_host_set_allowed(0x95, turn_off, 9));
	CHECK(!osc2_test_host_set_allowed(0x86, factory, 4));
	CHECK(!osc2_test_host_set_allowed(0x86, turn_off, 4));
	CHECK(!osc2_test_host_set_allowed(0x90, NULL, 0));
	CHECK(!osc2_test_host_set_allowed(OSC2_TEST_OPCODE, turn_off, 4));
	CHECK(!osc2_test_host_set_allowed(0xFE, NULL, 0));
	CHECK(!osc2_test_host_set_allowed(0xA2, NULL, 0));
	CHECK(!osc2_test_host_set_allowed(0xDC, NULL, 0));
	for (int op = 0xEE; op <= 0xF0; op++) {
		CHECK(!osc2_test_host_set_allowed((uint8_t)op, NULL, 0));
	}
	CHECK(!osc2_test_host_set_allowed(0xC0, NULL, 0));
	CHECK(!osc2_test_host_set_allowed(0xC3, NULL, 0));
	CHECK(!osc2_test_host_set_allowed(0xD8, NULL, 0));
	CHECK(!osc2_test_host_set_allowed(0xE2, NULL, 0));
	/* Wireless, the allowed turn-off is acted on (a POWER request), as from a host. */
	osc2.mode = OPENSC2_MODE_WIRELESS;
	args[1] = 4;
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, 6), OSC2_TEST_OK);
	CHECK_EQ(stub.power, OPENSC2_POWER_TURN_OFF);
	CHECK_EQ(hk.violations, 0);
}

static void host_set(uint8_t opcode, const void *payload, uint8_t n)
{
	uint8_t args[2 + 52];

	args[0] = opcode;
	args[1] = n;
	if (n) {
		memcpy(&args[2], payload, n);
	}
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, (uint8_t)(2 + n)), OSC2_TEST_OK);
}

OPENSC2_TEST(hooks_host_get_mask)
{
	uint8_t tag, sel, expect[20], key_and_bond[9 + OPENSC2_BOND_LEN];
	struct opensc2_bond bond = { .puck_id = 0x11111111, .controller_id = 0x22222222 };
	struct opensc2_identity id;

	stub_reset();
	id = osc2.identity;
	CHECK_EQ(arm_on(false, 1, 0, 0), OSC2_TEST_OK);
	/* 0xAE tag 1 (the unit serial): masked, the CRC-32 of the 20 answer bytes instead. */
	tag = 1;
	host_set(0xAE, &tag, 1);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	opensc2_encode_string_attribute(&id, 1, expect);
	CHECK(RES[0] == 0xAE && RES[1] == 20 && RES[2] == 0x01);
	CHECK_EQ(opensc2_get_le32(&RES[3]), osc2_crc32(0, expect, 20));
	CHECK_EQ(hans_len, 4 + 7);
	CHECK(!contains(hans, hans_len, "OSC0123456789", 13));
	/* Tags 0 and 4 (the board serial), and no tag at all (read as 0): masked. Tag 3: not. */
	tag = 4;
	host_set(0xAE, &tag, 1);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(RES[2], 0x01);
	host_set(0xAE, NULL, 0);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(RES[2], 0x01);
	CHECK(!contains(hans, hans_len, "OSB0123456789", 13));
	tag = 3;
	host_set(0xAE, &tag, 1);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK(RES[1] == 20 && RES[2] == 0);
	CHECK(memcmp(&RES[4], "opensc2-0.2", 11) == 0);
	/* 0xF2 selector 0 holds the unit serial: masked; selector 2 not. */
	sel = 0;
	host_set(0xF2, &sel, 1);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK(RES[1] == 41 && RES[2] == 0x01);
	sel = 2;
	host_set(0xF2, &sel, 1);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK(RES[1] == 9 && RES[2] == 0 && RES[3] == 2);
	/* 0xA1 selector 1, the chip ID: masked; selector 0 answers nothing. */
	sel = 1;
	host_set(0xA1, &sel, 1);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK(RES[1] == 21 && RES[2] == 0x01);
	sel = 0;
	host_set(0xA1, &sel, 1);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK(RES[1] == 0 && RES[2] == 0);
	/* 0xED of a bond: masked (the CRC of the same 24 bytes); empty: n 0; the transport: not. */
	memcpy(key_and_bond, "esb/bond", 9);
	host_set(0xED, key_and_bond, 9);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK(RES[0] == 0xED && RES[1] == 0 && RES[2] == 0);
	osc2.bonds[0] = bond;
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	opensc2_bond_pack(&bond, &key_and_bond[9]);
	CHECK(RES[1] == OPENSC2_BOND_LEN && RES[2] == 0x01);
	CHECK_EQ(opensc2_get_le32(&RES[3]), osc2_crc32(0, &key_and_bond[9], OPENSC2_BOND_LEN));
	CHECK(!contains(hans, hans_len, &key_and_bond[9], 8));
	host_set(0xED, "user/wireless_transport", 24);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK(RES[1] == 1 && RES[2] == 0 && RES[3] == 2);
	/* 0x83: in full. A set-only command: {opcode, 0}. */
	host_set(0x83, NULL, 0);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK(RES[0] == 0x83 && RES[1] == 30 && RES[2] == 0 && RES[3] == 1);
	CHECK_EQ(hans_len, 4 + 3 + 30);
	host_set(0x87, (const uint8_t[]){ 27, 0xEC, 0xFF }, 3);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, NULL, 0), OSC2_TEST_OK);
	CHECK(RES[0] == 0x87 && RES[1] == 0 && RES[2] == 0);
	/* The pure rule. */
	CHECK(!osc2_test_host_get_masked((const uint8_t[]){ 0xAE, 1, 1 }, 3, 0));
	CHECK(osc2_test_host_get_masked((const uint8_t[]){ 0xAE, 1, 0 }, 3, 20));
	CHECK(!osc2_test_host_get_masked((const uint8_t[]){ 0xAE, 1, 2 }, 3, 20));
	CHECK(osc2_test_host_get_masked((const uint8_t[]){ 0xF2, 0 }, 2, 41));
	CHECK(!osc2_test_host_get_masked((const uint8_t[]){ 0x89, 1, 0 }, 3, 3));
	CHECK_EQ(hk.violations, 0);
}

/* ================================================================================================
 * The order of the checks, and the console
 * ============================================================================================== */

OPENSC2_TEST(hooks_status_order)
{
	uint8_t args[60] = { 0 }, ms[2];

	stub_reset();
	/* The console, unarmed: op, then length, then armed. */
	CHECK_EQ(con_op(0x99, NULL, 0), OSC2_TEST_BAD_OP);
	CHECK_EQ(con_op(0x99, args, 3), OSC2_TEST_BAD_OP);
	CHECK_EQ(con_op(OSC2_TEST_OP_INJECT, args, 10), OSC2_TEST_BAD_LEN);
	CHECK_EQ(con_op(OSC2_TEST_OP_INJECT, args, 53), OSC2_TEST_NOT_ARMED);
	CHECK_EQ(con_op(OSC2_TEST_OP_MARK, args, 3), OSC2_TEST_BAD_LEN);
	CHECK_EQ(con_op(OSC2_TEST_OP_MARK, args, 4), OSC2_TEST_NOT_ARMED);
	CHECK_EQ(con_op(OSC2_TEST_OP_STATUS, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(con_op(OSC2_TEST_OP_STATUS, args, 1), OSC2_TEST_BAD_LEN);
	opensc2_put_le16(&args[4], 700);
	CHECK_EQ(con_op(OSC2_TEST_OP_ARM, args, 7), OSC2_TEST_BAD_ARG);
	CHECK(!osc2_test_core_armed(CORE));
	CHECK_EQ(arm_on(false, 1, 0, 0), OSC2_TEST_OK);
	/* Armed, lizard mode on: values, then state, then policy. */
	args[0] = 0;
	args[1] = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_SEQ_RUN, args, 2), OSC2_TEST_BAD_ARG);   /* count 0 */
	CHECK_EQ(seq_load(0, 1, 1), OSC2_TEST_OK);
	lizard_off_usb();
	CHECK_EQ(seq_run(1, 0), OSC2_TEST_OK);
	CHECK_EQ(usb_set(0x87, (const uint8_t[]){ 9, 1, 0 }, 3), 0);   /* no frame built since */
	CHECK_EQ(seq_run(1, 0), OSC2_TEST_BUSY);
	CHECK_EQ(seq_load(0, 1, 1), OSC2_TEST_BUSY);
	CHECK_EQ(con_op(OSC2_TEST_OP_SEQ_STOP, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(seq_run(1, 0), OSC2_TEST_DENIED);
	opensc2_put_le16(ms, 50);
	CHECK_EQ(con_op(OSC2_TEST_OP_RADIO_DEAF, ms, 2), OSC2_TEST_BAD_ARG);   /* wired too */
	opensc2_put_le16(ms, 200);
	CHECK_EQ(con_op(OSC2_TEST_OP_RADIO_DEAF, ms, 2), OSC2_TEST_NO_LINK);
	args[0] = 0;
	args[1] = OPENSC2_RPT_STATE;
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, args, 10), OSC2_TEST_DENIED);
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, args, 1), OSC2_TEST_BAD_LEN);
	args[0] = 1;
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, args, 2), OSC2_TEST_BAD_LEN);
	args[0] = 5;
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, args, 2), OSC2_TEST_BAD_ARG);
	CHECK_EQ(con_op(OSC2_TEST_OP_APP_SEND, NULL, 0), OSC2_TEST_BAD_LEN);
	args[0] = 0x83;
	args[1] = 3;
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, 2), OSC2_TEST_BAD_LEN);
	args[0] = 0x86;
	args[1] = 0;
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_SET, args, 2), OSC2_TEST_DENIED);
	CHECK_EQ(con_op(OSC2_TEST_OP_BATTERY, args, 15), OSC2_TEST_BAD_LEN);
	CHECK_EQ(con_op(OSC2_TEST_OP_HOST_GET, args, 1), OSC2_TEST_BAD_LEN);
	args[0] = 0;   /* INJECT without the lizard test, lizard mode on */
	CHECK_EQ(con_op(OSC2_TEST_OP_INJECT, args, 53), OSC2_TEST_DENIED);
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_console_lines)
{
	char line[256];

	stub_reset();
	CHECK_EQ(con("1 sync"), 0);
	CHECK_EQ(con_n, 1);
	CHECK_LINE(0, "HWT 1 END 0");
	CHECK_EQ(con("2 status"), 0);
	CHECK_LINE(0, "HWT 2 STATUS armed=0 guard=0 inject=0 battery=0 seq=idle mode=wired "
		      "reason=0 link=off usb=configured slot=1 lizard=1 frames=0 ring=0..0 up=0 "
		      "nonce=00000000 policy=auto snapshot=0");
	CHECK_LINE(1, "HWT 2 END 0");
	/* raw: any request that passes the key and version checks; the status travels inside. */
	CHECK_EQ(con("3 raw 1eab577e010303"), 0);
	CHECK(strncmp(con_lines[0], "HWT 3 RAW 01030300", 18) == 0);
	CHECK_EQ(strlen(con_lines[0]), 10 + 2 * (4 + OSC2_TEST_STATUS_LEN));
	CHECK_LINE(1, "HWT 3 END 0");
	CHECK_EQ(con("4 raw 1eab577e010424"), 0);
	CHECK_LINE(0, "HWT 4 RAW 01042402");                 /* MARK without its marker */
	CHECK_EQ(con("5 raw 1eab577e01052400000000"), 0);
	CHECK_LINE(0, "HWT 5 RAW 01052403");                 /* MARK while unarmed */
	CHECK_EQ(con("5 raw 1eab577e01059900"), 0);
	CHECK_LINE(0, "HWT 5 RAW 01059901");                 /* an unknown op */
	CHECK_EQ(con("6 raw 1eab577e0106"), -22);
	CHECK_LINE(0, "HWT 6 ERR not a test request");
	CHECK_LINE(1, "HWT 6 END -22");
	CHECK_EQ(con("7 raw 1EAB577F010603"), -22);
	CHECK_LINE(0, "HWT 7 ERR not a test request");
	CHECK_EQ(con("8 raw 1eab577e020603"), -22);
	CHECK_LINE(0, "HWT 8 ERR not a test request");
	CHECK_EQ(con("9 raw 1eab577e01090"), -22);
	CHECK_LINE(0, "HWT 9 ERR bad hex");
	CHECK_LINE(1, "HWT 9 END -22");
	CHECK_EQ(con("9 raw 1eab577e0109zz"), -22);
	CHECK_LINE(0, "HWT 9 ERR bad hex");
	CHECK_EQ(con("21 raw"), -22);
	CHECK_EQ(con_n, 2);
	CHECK_LINE(1, "HWT 21 END -22");
	/* Verbs that need arming. */
	CHECK_EQ(con("10 ring"), -13);
	CHECK_LINE(0, "HWT 10 ERR not armed");
	CHECK_LINE(1, "HWT 10 END -13");
	CHECK_EQ(con("10 cnt"), -13);
	CHECK_EQ(con("10 inject off"), -13);
	CHECK_EQ(con("20 disarm"), -13);
	CHECK_LINE(1, "HWT 20 END -13");
	CHECK_EQ(con("11 frobnicate"), -2);
	CHECK_LINE(0, "HWT 11 ERR unknown verb");
	CHECK_LINE(1, "HWT 11 END -2");
	CHECK_EQ(con("sync"), 0);
	CHECK_LINE(0, "HWT - END 0");
	CHECK_EQ(con("0 sync"), -22);
	CHECK_LINE(1, "HWT - END -22");
	CHECK_EQ(con("65536 sync"), -22);
	CHECK_EQ(con("65535 sync"), 0);
	CHECK_LINE(0, "HWT 65535 END 0");
	CHECK_EQ(con("12"), -22);
	CHECK_LINE(1, "HWT 12 END -22");
	/* arm: the key, a lease; nonce 0 and no restore. */
	CHECK_EQ(con("arm 7e57ab1f"), -22);
	CHECK_LINE(0, "HWT - ERR bad key");
	CHECK_LINE(1, "HWT - END -22");
	CHECK_EQ(con("12 arm 7e57ab1e 601"), -22);
	CHECK(!osc2_test_core_armed(CORE));
	CHECK_EQ(con("12 arm 7E57AB1E 20"), 0);
	CHECK_LINE(0, "HWT 12 ARMED nonce=00000000 lease=20");
	CHECK_LINE(1, "HWT 12 END 0");
	CHECK(!CORE->snap_on);
	CHECK_EQ(con("13 status"), 0);
	CHECK(strncmp(con_lines[0], "HWT 13 STATUS armed=1 guard=1 ", 30) == 0);
	CHECK(strstr(con_lines[0], " ring=0..2 ") != NULL);
	CHECK(strstr(con_lines[0], " snapshot=0") != NULL);
	CHECK_EQ(con("14 ring"), 0);
	CHECK_EQ(con_n, 4);
	CHECK_LINE(0, "HWT 14 EV 0 0.000 test ARMED nonce=00000000");
	CHECK_LINE(1, "HWT 14 EV 1 0.000 test LIZARD what=resync");
	CHECK_LINE(2, "HWT 14 RING first=0 next=2");
	CHECK_LINE(3, "HWT 14 END 0");
	CHECK_EQ(con("14 ring 1"), 0);
	CHECK_EQ(con_n, 3);
	CHECK_LINE(0, "HWT 14 EV 1 0.000 test LIZARD what=resync");
	CHECK_EQ(con("14 ring 3"), -22);   /* past next */
	CHECK_EQ(con("15 cnt"), 0);
	CHECK_EQ(con_n, OSC2_TEST_CNT_COUNT + 1);
	CHECK_LINE(0, "HWT 15 CNT frames=0");
	CHECK_LINE(24, "HWT 15 CNT test_requests=0");
	CHECK_LINE(26, "HWT 15 CNT usb_skipped=0");
	CHECK_LINE(27, "HWT 15 END 0");
	CHECK_EQ(con("16 settings"), 0);
	CHECK_EQ(con_n, 7);
	CHECK_LINE(0, "HWT 16 SETTINGS 0 0 0 0 0 0 0 0 0 0 1 0 200 100 0 5500 0");
	CHECK_LINE(5, "HWT 16 SETTINGS 80 1 0 3 0 1 100");
	CHECK_LINE(6, "HWT 16 END 0");
	CHECK_EQ(con("17 inject off"), 0);
	CHECK_LINE(0, "HWT 17 OK");
	CHECK_EQ(con("17 battery off"), 0);
	CHECK_EQ(con("17 seq stop"), 0);
	CHECK_EQ(con("18 inject"), -22);
	CHECK_EQ(con("18 seq off"), -22);
	/* The console's END 0 while armed refreshes the lease. */
	stub.now_ms = 15000;
	CHECK_EQ(con("22 sync"), 0);
	CHECK_EQ(osc2_test_core_next(CORE, ENV), 20000);
	/* An entry with a time: milliseconds and three digits of microseconds. */
	hk.extra_us = 1234;
	osc2_test_note_setting(45, 100);
	CHECK_EQ(con("23 ring 2"), 0);
	/* Called outside any command: link none. */
	snprintf(line, sizeof(line), "HWT 23 EV 2 15001.234 none SETTING number=45 value=100");
	CHECK_LINE(0, line);
	CHECK_EQ(con("19 disarm"), 0);
	CHECK_LINE(0, "HWT 19 DISARMED");
	CHECK_LINE(1, "HWT 19 END 0");
	CHECK(!osc2_test_core_armed(CORE));
	CHECK_EQ(hk.violations, 0);
}

OPENSC2_TEST(hooks_unarmed_inert)
{
	struct opensc2_state s, kept;
	struct opensc2_battery b = { .level = 42 };
	struct opensc2_led led = { .red = 1 };
	static const uint8_t mouse[6] = { 0x40, 1 };
	uint8_t report[OPENSC2_STATE_REPORT_LEN] = { 0x42 }, out[8];

	stub_reset();
	memset(&s, 0x5A, sizeof(s));
	kept = s;
	osc2_test_input(&s);
	CHECK(memcmp(&s, &kept, sizeof(s)) == 0);
	CHECK(!osc2_test_skip_lizard());
	osc2_test_frame(report);
	osc2_test_battery(&b);
	CHECK_EQ(b.level, 42);
	CHECK(!osc2_test_divert(mouse, sizeof(mouse)));
	CHECK_EQ(osc2_test_take_deaf(), 0);
	CHECK(!osc2_test_quiet(OSC2_LINK_USB));
	CHECK(!osc2_test_answer(OSC2_LINK_USB, out, &out[7]));
	CHECK_EQ(osc2_test_ctx_enter(OSC2_LINK_USB), OSC2_TEST_CTX_UNSET);
	osc2_test_note_led(&led);
	osc2_test_note_setting(9, 0);
	osc2_test_note_power(1, "host: turn off");
	osc2_test_count(OSC2_TEST_CNT_FRAMES);
	CHECK_EQ(usb_set(0x87, (const uint8_t[]){ 45, 10, 0 }, 3), 0);
	CHECK_EQ(CORE->ring.next, 0);
	for (int i = 0; i < OSC2_TEST_CNT_COUNT; i++) {
		CHECK_EQ(hk.counters[i], 0);
	}
	CHECK_EQ(hk.ctx_link, OSC2_TEST_LINK_NONE);
	CHECK_EQ(hk.violations, 0);
}

/* RULES and RULES_SET: the rule word read and changed while armed; the build's comes back at the
 * end of arming (DISARM or the lease) with the guard still on, and at a fresh ARM; a change
 * starts lizard mode over. */
OPENSC2_TEST(hooks_rules)
{
	uint8_t args[4];
	const uint32_t other = OSC2_RULE_TRIGGER_BITS | OSC2_RULE_PAD_CLICK;

	stub_reset();
	/* Unarmed: not claimed (a STALL on USB); armed: the three words. */
	CHECK_EQ(usb_op(OSC2_TEST_OP_RULES, NULL, 0), -1);
	CHECK_EQ(arm_on(true, 1, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(usb_op(OSC2_TEST_OP_RULES, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hans_len, 4 + 12);
	CHECK_EQ(opensc2_get_le32(&RES[0]), OSC2_RULES_DEFAULT);
	CHECK_EQ(opensc2_get_le32(&RES[4]), OSC2_RULES_DEFAULT);
	CHECK_EQ(opensc2_get_le32(&RES[8]), OSC2_RULES_KNOWN);
	/* An unknown bit, a wrong length: refused, nothing changed. */
	opensc2_put_le32(args, OSC2_RULES_KNOWN + 1);
	CHECK_EQ(usb_op(OSC2_TEST_OP_RULES_SET, args, 4), OSC2_TEST_BAD_ARG);
	CHECK_EQ(usb_op(OSC2_TEST_OP_RULES_SET, args, 3), OSC2_TEST_BAD_LEN);
	CHECK_EQ(usb_op(OSC2_TEST_OP_RULES, args, 1), OSC2_TEST_BAD_LEN);
	CHECK_EQ(hk.rules_set_calls, 0);
	/* Set: the word in effect comes back, then lizard mode's resync. */
	memset(hk.order, 0, sizeof(hk.order));
	hk.order_len = 0;
	opensc2_put_le32(args, other);
	CHECK_EQ(usb_op(OSC2_TEST_OP_RULES_SET, args, 4), OSC2_TEST_OK);
	CHECK_EQ(opensc2_get_le32(&RES[0]), other);
	CHECK_EQ(hk.rules, other);
	CHECK(strcmp(hk.order, "US") == 0);
	/* A fresh ARM: the build's again. */
	CHECK_EQ(arm_on(true, 2, 0, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.rules, OSC2_RULES_DEFAULT);
	/* DISARM: the build's again, while the guard is still on. */
	CHECK_EQ(usb_op(OSC2_TEST_OP_RULES_SET, args, 4), OSC2_TEST_OK);
	stub.now_ms = 1000;
	CHECK_EQ(usb_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.rules, OSC2_RULES_DEFAULT);
	CHECK(osc2_test_core_guard(CORE));
	poll_at(1050);
	/* The lease running out: the same. */
	CHECK_EQ(arm_on(true, 3, 1, 0), OSC2_TEST_OK);
	CHECK_EQ(usb_op(OSC2_TEST_OP_RULES_SET, args, 4), OSC2_TEST_OK);
	CHECK_EQ(hk.rules, other);
	poll_at(2100);
	CHECK(!osc2_test_core_armed(CORE));
	CHECK_EQ(hk.rules, OSC2_RULES_DEFAULT);
	/* An arming that never changed it leaves it alone at its end. */
	poll_at(2200);
	hk.rules_set_calls = 0;
	CHECK_EQ(arm_on(true, 4, 0, 0), OSC2_TEST_OK);
	stub.now_ms = 3000;
	CHECK_EQ(usb_op(OSC2_TEST_OP_DISARM, NULL, 0), OSC2_TEST_OK);
	CHECK_EQ(hk.rules_set_calls, 0);
	CHECK_EQ(hk.violations, 0);
}
