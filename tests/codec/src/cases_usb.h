/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Test cases for wired mode's pure parts: the USB descriptors and the report descriptor, the
 * framing of feature report 1, the boot-time mode rule and the marks, when the wired link sends
 * reports, the retained RAM area and the setting ranges. Written once, run as a ztest suite and on
 * a PC (see cases.h). The expected values are the real controller's descriptors, report list and
 * setting ranges as the protocol documentation the same authors wrote gives them (not public yet;
 * docs/protocol.md lists the pages). The rest comes directly from the same authors' study of the
 * controller's firmware: the sizes of the mouse and keyboard collections and the order of the
 * vendor reports (which the vendor list below follows). Each is written out by hand;
 * lib/opensc2/usb_desc.c names the source of every descriptor detail.
 */
#include <errno.h>
#include <string.h>
#include <opensc2/codec.h>
#include <opensc2/settings.h>
#include "mode_rules.h"
#include "retained.h"
#include "usb_rules.h"

/* ---- the report descriptor, walked item by item ---------------------------------------------- */

struct hid_walk {
	int top;                        /* top-level collections seen */
	uint16_t page[4], usage[4];     /* usage page and usage of each */
	size_t start[4], end[4];        /* byte offsets: first item, after End Collection */
	uint32_t bits[256][3];          /* report ID -> bits of input, output, feature */
	uint8_t order[32];              /* report IDs in the order the vendor collection declares */
	int order_n;
	int errors;
};

static void hid_walk(const uint8_t *d, size_t len, struct hid_walk *w)
{
	uint32_t page = 0, size = 0, count = 0, id = 0, usage = 0;
	int depth = 0;
	size_t i = 0;

	memset(w, 0, sizeof(*w));
	while (i < len) {
		uint8_t prefix = d[i];
		size_t n = (prefix & 3) == 3 ? 4 : (prefix & 3);
		uint8_t type = (prefix >> 2) & 3, tag = prefix >> 4;
		uint32_t v = 0;

		if (i + 1 + n > len) {
			w->errors++;
			return;
		}
		for (size_t k = 0; k < n; k++) {
			v |= (uint32_t)d[i + 1 + k] << (8 * k);
		}
		if (type == 0) {   /* main */
			if (tag == 0x8 || tag == 0x9 || tag == 0xB) {   /* input, output, feature */
				int t = tag == 0x8 ? 0 : tag == 0x9 ? 1 : 2;

				if (w->bits[id][0] + w->bits[id][1] + w->bits[id][2] == 0 &&
				    w->top == 3 && w->order_n < 32) {
					w->order[w->order_n++] = (uint8_t)id;
				}
				w->bits[id][t] += size * count;
			} else if (tag == 0xA) {   /* collection */
				if (depth == 0 && w->top < 4) {
					w->page[w->top] = (uint16_t)page;
					w->usage[w->top] = (uint16_t)usage;
					w->start[w->top] = i;
					w->top++;
				}
				depth++;
			} else if (tag == 0xC) {   /* end collection */
				depth--;
				if (depth == 0) {
					w->end[w->top - 1] = i + 1;
				}
			}
		} else if (type == 1) {   /* global */
			if (tag == 0x0) {
				page = v;
			} else if (tag == 0x7) {
				size = v;
			} else if (tag == 0x8) {
				id = v & 0xFF;
			} else if (tag == 0x9) {
				count = v;
			}
		} else if (type == 2 && tag == 0x0) {   /* local usage */
			usage = v;
		}
		i += 1 + n;
	}
	if (depth != 0) {
		w->errors++;
	}
}

OPENSC2_TEST(usb_report_descriptor)
{
	static struct hid_walk w;
	static const struct {
		uint8_t id, type, bytes;
	} vendor[] = {
		{ 0x42, 0, 53 }, { 0x44, 0, 5 }, { 0x79, 0, 1 }, { 0x43, 0, 14 }, { 0x7B, 0, 12 },
		{ 0x45, 0, 45 }, { 0x80, 1, 9 }, { 0x81, 1, 7 }, { 0x82, 1, 3 }, { 0x83, 1, 9 },
		{ 0x84, 1, 8 }, { 0x85, 1, 3 }, { 0x86, 1, 3 }, { 0x87, 1, 63 }, { 0x89, 1, 63 },
		{ 0x88, 1, 63 }, { 0x01, 2, 63 }, { 0x02, 2, 63 },
	};

	CHECK_EQ(sizeof(opensc2_hid_report_desc), 372);
	hid_walk(opensc2_hid_report_desc, sizeof(opensc2_hid_report_desc), &w);
	CHECK_EQ(w.errors, 0);
	/* Mouse, keyboard, vendor: in that order, so the vendor collection is Windows' Col03. */
	CHECK_EQ(w.top, 3);
	CHECK_EQ(w.page[0], 0x01);
	CHECK_EQ(w.usage[0], 0x02);
	CHECK_EQ(w.page[1], 0x01);
	CHECK_EQ(w.usage[1], 0x06);
	CHECK_EQ(w.page[2], 0xFF00);
	CHECK_EQ(w.usage[2], 0x01);
	/* 67 + 41 + 264 bytes (the sizes from the same authors' study, as the top says). */
	CHECK_EQ(w.end[0] - 0, 67);
	CHECK_EQ(w.end[1] - w.end[0], 41);
	CHECK_EQ(w.end[2] - w.end[1], 264);
	/* The mouse and keyboard reports, and no keyboard output (no LED reports). */
	CHECK_EQ(w.bits[0x40][0], 5 * 8);
	CHECK_EQ(w.bits[0x41][0], 8 * 8);
	CHECK_EQ(w.bits[0x41][1], 0);
	/* Every vendor report with its size and type, in the order the real descriptor declares
	 * them (the same authors' study, as the top of this file says). */
	CHECK_EQ(w.order_n, (int)(sizeof(vendor) / sizeof(vendor[0])));
	for (unsigned int i = 0; i < sizeof(vendor) / sizeof(vendor[0]); i++) {
		CHECK_EQ(w.order[i], vendor[i].id);
		CHECK_EQ(w.bits[vendor[i].id][vendor[i].type], vendor[i].bytes * 8u);
	}
	/* Not declared. */
	CHECK_EQ(w.bits[0x8A][1] + w.bits[0x46][0] + w.bits[0x47][0], 0);
	/* The longest reports with their ID: input 54, output and feature 64. */
	CHECK_EQ(w.bits[0x42][0] / 8 + 1, OPENSC2_STATE_REPORT_LEN);
	CHECK_EQ(w.bits[0x01][2] / 8 + 1, OPENSC2_USB_FEATURE_LEN);
}

OPENSC2_TEST(usb_descriptors)
{
	static const uint8_t july_device[18] = {
		0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40, 0xDE, 0x28, 0x02, 0x13,
		0x07, 0x03, 0x01, 0x02, 0x03, 0x01,
	};
	static const uint8_t october_device[18] = {
		0x12, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00, 0x40, 0xDE, 0x28, 0x02, 0x13,
		0x04, 0x04, 0x01, 0x02, 0x03, 0x01,
	};
	static const uint8_t config[41] = {
		0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0xA0, 0xFA,
		0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
		0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x74, 0x01,
		0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01,
		0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x01,
	};
	uint8_t out[41];

	CHECK_EQ(opensc2_usb_device_descriptor(&opensc2_usb_profile_july, out), 18);
	CHECK(memcmp(out, july_device, 18) == 0);
	CHECK_EQ(opensc2_usb_device_descriptor(&opensc2_usb_profile_october, out), 18);
	CHECK(memcmp(out, october_device, 18) == 0);
	CHECK_EQ(opensc2_usb_config_descriptor(out), 41);
	CHECK(memcmp(out, config, 41) == 0);
	CHECK_EQ(opensc2_usb_hid_descriptor(out), 9);
	CHECK(memcmp(out, &config[18], 9) == 0);
	CHECK_EQ(opensc2_usb_profile_july.get_reply_len, 64);
	CHECK(!opensc2_usb_profile_july.bos_usb20_ext);
	CHECK_EQ(opensc2_usb_profile_october.get_reply_len, 63);
	CHECK(opensc2_usb_profile_october.bos_usb20_ext);
}

/* ---- feature report 1 ------------------------------------------------------------------------ */

OPENSC2_TEST(usb_feature_framing)
{
	uint8_t data[65] = { 0x01, 0xAE, 0x01, 0x01 };
	uint8_t reply[64];
	const uint8_t answer[3] = { 0x11, 0x22, 0x33 };
	const uint8_t *rec = NULL;

	CHECK_EQ(opensc2_usb_feature_check(data, 64, &rec), OPENSC2_USB_SET_OK);
	CHECK(rec == &data[1]);
	CHECK_EQ(opensc2_usb_feature_check(data, 63, &rec), OPENSC2_USB_SET_LENGTH);
	CHECK_EQ(opensc2_usb_feature_check(data, 65, &rec), OPENSC2_USB_SET_LENGTH);
	data[0] = 2;   /* the dispatcher also takes report ID 2 in the data */
	CHECK_EQ(opensc2_usb_feature_check(data, 64, &rec), OPENSC2_USB_SET_OK);
	data[0] = 3;
	CHECK_EQ(opensc2_usb_feature_check(data, 64, &rec), OPENSC2_USB_SET_ID);
	data[0] = 1;
	data[2] = 61;
	CHECK_EQ(opensc2_usb_feature_check(data, 64, &rec), OPENSC2_USB_SET_OK);
	data[2] = 62;
	CHECK_EQ(opensc2_usb_feature_check(data, 64, &rec), OPENSC2_USB_SET_N);

	/* 64 bytes whatever wLength (65 from Steam's first reads), zeros after the answer. */
	memset(reply, 0xEE, sizeof(reply));
	CHECK_EQ(opensc2_usb_feature_reply(0xED, answer, 3, 64, 65, reply), 64);
	CHECK_EQ(reply[0], 0x01);
	CHECK_EQ(reply[1], 0xED);
	CHECK_EQ(reply[2], 3);
	CHECK_EQ(reply[5], 0x33);
	CHECK_EQ(reply[6], 0);
	CHECK_EQ(reply[63], 0);
	CHECK_EQ(opensc2_usb_feature_reply(0xED, answer, 3, 63, 64, reply), 63);   /* October */
	CHECK_EQ(opensc2_usb_feature_reply(0xED, answer, 3, 64, 8, reply), 8);     /* short wLength */
}

OPENSC2_TEST(usb_setup_text)
{
	const uint8_t string3[8] = { 0x80, 0x06, 0x03, 0x03, 0x09, 0x04, 0x02, 0x02 };
	const uint8_t report_desc[8] = { 0x81, 0x06, 0x00, 0x22, 0x00, 0x00, 0xB4, 0x01 };
	const uint8_t set_idle[8] = { 0x21, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	const uint8_t get_feature[8] = { 0xA1, 0x01, 0x01, 0x03, 0x00, 0x00, 0x41, 0x00 };
	char text[80];

	opensc2_usb_setup_text(string3, text, sizeof(text));
	CHECK(strcmp(text, "80 06 0303 0409 0202 GET_DESCRIPTOR string 3") == 0);
	opensc2_usb_setup_text(report_desc, text, sizeof(text));
	CHECK(strcmp(text, "81 06 2200 0000 01B4 GET_DESCRIPTOR report 0") == 0);
	opensc2_usb_setup_text(set_idle, text, sizeof(text));
	CHECK(strcmp(text, "21 0A 0000 0000 0000 SET_IDLE 0 ms, report 0") == 0);
	opensc2_usb_setup_text(get_feature, text, sizeof(text));
	CHECK(strcmp(text, "A1 01 0301 0000 0041 GET_REPORT feature 1") == 0);
}

/* ---- modes ----------------------------------------------------------------------------------- */

static enum opensc2_mode decide(bool usb, bool power, enum osc2_mark mark,
				enum opensc2_policy policy, enum opensc2_boot_mode app,
				enum opensc2_mode_reason *reason)
{
	struct osc2_mode_inputs in = {
		.usb_supported = usb,
		.usb_power = power,
		.mark = mark,
		.policy = policy,
		.app = app,
	};

	return osc2_mode_decide(&in, reason);
}

OPENSC2_TEST(mode_rules)
{
	const enum opensc2_policy AUTO = OPENSC2_POLICY_AUTO, NEVER = OPENSC2_POLICY_WIRELESS;
	const enum opensc2_boot_mode DFLT = OPENSC2_BOOT_DEFAULT;
	enum opensc2_mode_reason r;

	/* The real controller's rule. */
	CHECK_EQ(decide(true, true, OSC2_MARK_NONE, AUTO, DFLT, &r), OPENSC2_MODE_WIRED);
	CHECK_EQ(r, OPENSC2_MODE_USB_POWER);
	CHECK_EQ(decide(true, true, OSC2_MARK_WIRELESS, AUTO, DFLT, &r), OPENSC2_MODE_WIRELESS);
	CHECK_EQ(r, OPENSC2_MODE_WARM_RESTART);
	CHECK_EQ(decide(true, false, OSC2_MARK_NONE, AUTO, DFLT, &r), OPENSC2_MODE_WIRELESS);
	CHECK_EQ(r, OPENSC2_MODE_NO_USB_POWER);
	/* On battery a mark does not matter. */
	CHECK_EQ(decide(true, false, OSC2_MARK_WIRELESS, AUTO, DFLT, &r), OPENSC2_MODE_WIRELESS);
	CHECK_EQ(r, OPENSC2_MODE_NO_USB_POWER);
	CHECK_EQ(decide(true, false, OSC2_MARK_WIRED, AUTO, DFLT, &r), OPENSC2_MODE_WIRELESS);
	/* The board's own overrides. */
	CHECK_EQ(decide(true, true, OSC2_MARK_NONE, NEVER, DFLT, &r), OPENSC2_MODE_WIRELESS);
	CHECK_EQ(r, OPENSC2_MODE_POLICY);
	CHECK_EQ(decide(true, true, OSC2_MARK_WIRED, NEVER, DFLT, &r), OPENSC2_MODE_WIRED);
	CHECK_EQ(r, OPENSC2_MODE_WIRED_RESTART);
	CHECK_EQ(decide(true, true, OSC2_MARK_NONE, AUTO, OPENSC2_BOOT_WIRELESS, &r),
		 OPENSC2_MODE_WIRELESS);
	CHECK_EQ(r, OPENSC2_MODE_APP);
	CHECK_EQ(decide(true, true, OSC2_MARK_WIRELESS, AUTO, OPENSC2_BOOT_WIRED, &r),
		 OPENSC2_MODE_WIRED);
	CHECK_EQ(decide(true, false, OSC2_MARK_NONE, NEVER, OPENSC2_BOOT_WIRED, &r),
		 OPENSC2_MODE_WIRED);
	/* Without USB support always wireless. */
	CHECK_EQ(decide(false, true, OSC2_MARK_WIRED, AUTO, OPENSC2_BOOT_WIRED, &r),
		 OPENSC2_MODE_WIRELESS);
	CHECK_EQ(r, OPENSC2_MODE_NO_USB);
}

OPENSC2_TEST(plug_in_rule)
{
	const enum opensc2_policy AUTO = OPENSC2_POLICY_AUTO, NEVER = OPENSC2_POLICY_WIRELESS;

	/* The real controller's rule for a cable plugged in on battery. */
	CHECK_EQ(osc2_plug_in_rule(AUTO, true, false), OSC2_PLUG_IN_CHARGE);
	CHECK_EQ(osc2_plug_in_rule(AUTO, false, false), OSC2_PLUG_IN_LINK_DOWN);
	CHECK_EQ(osc2_plug_in_rule(AUTO, true, true), OSC2_PLUG_IN_STEAM);
	CHECK_EQ(osc2_plug_in_rule(AUTO, false, true), OSC2_PLUG_IN_LINK_DOWN);
	/* The policy "wireless" (also when set after boot): never back to wired. */
	CHECK_EQ(osc2_plug_in_rule(NEVER, false, false), OSC2_PLUG_IN_IGNORE);
	CHECK_EQ(osc2_plug_in_rule(NEVER, true, true), OSC2_PLUG_IN_IGNORE);
}

OPENSC2_TEST(warm_marks)
{
	uint32_t w = osc2_mark_word(OSC2_MARK_WIRELESS);

	CHECK_EQ(osc2_mark_from_words(w, ~w), OSC2_MARK_WIRELESS);
	CHECK_EQ(osc2_mark_from_words(w, w), OSC2_MARK_NONE);          /* no complement */
	CHECK_EQ(osc2_mark_from_words(w ^ 1, ~(w ^ 1)), OSC2_MARK_NONE); /* another value */
	w = osc2_mark_word(OSC2_MARK_WIRED);
	CHECK_EQ(osc2_mark_from_words(w, ~w), OSC2_MARK_WIRED);
	CHECK_EQ(osc2_mark_from_words(0, ~0u), OSC2_MARK_NONE);
	CHECK_EQ(osc2_mark_from_byte(osc2_mark_byte(OSC2_MARK_WIRELESS)), OSC2_MARK_WIRELESS);
	CHECK_EQ(osc2_mark_from_byte(osc2_mark_byte(OSC2_MARK_WIRED)), OSC2_MARK_WIRED);
	CHECK_EQ(osc2_mark_from_byte(0), OSC2_MARK_NONE);
	CHECK_EQ(osc2_mark_from_byte(0x57), OSC2_MARK_NONE);   /* the UF2 bootloader's own value */
}

/* ---- when the wired link sends reports --------------------------------------------------------
 * As on the real controller, state reports are built from the configuration until the host
 * deconfigures the device (a bus reset) or the board leaves the bus; a suspended bus and full
 * buffers change nothing here (the stack keeps up to eight reports for the resume, and a report
 * that finds no buffer is lost with its sequence number).
 */

OPENSC2_TEST(usb_report_gate)
{
	struct osc2_usb_view v = { 0 };

	/* Attached, not configured yet: nothing is built or counted. */
	CHECK_EQ(osc2_usb_tick_action(&v), OSC2_USB_TICK_IDLE);
	CHECK_EQ(osc2_usb_send_gate(&v), -ENOTCONN);

	/* Configured: state reports flow from now on, a suspended bus included. */
	v.configured = true;
	CHECK_EQ(osc2_usb_tick_action(&v), OSC2_USB_TICK_SEND);
	CHECK_EQ(osc2_usb_send_gate(&v), 0);

	/* A bus reset deconfigures: nothing until the host configures it again. */
	v.configured = false;
	CHECK_EQ(osc2_usb_tick_action(&v), OSC2_USB_TICK_IDLE);
	CHECK_EQ(osc2_usb_send_gate(&v), -ENOTCONN);
	v.configured = true;
	CHECK_EQ(osc2_usb_tick_action(&v), OSC2_USB_TICK_SEND);

	/* On the way to a reboot: nothing. */
	v.detaching = true;
	CHECK_EQ(osc2_usb_tick_action(&v), OSC2_USB_TICK_IDLE);
	CHECK_EQ(osc2_usb_send_gate(&v), -ENOTCONN);
}

/* ---- the charger test build's rule: no host, then wireless -----------------------------------
 * A testing aid (CONFIG_OPENSC2_NO_HOST_WIRELESS_MS), not the real controller's behaviour: the
 * real controller waits for good, which is the rule with the option at 0.
 */

static enum osc2_no_host no_host(uint32_t timeout_ms, enum opensc2_mode_reason reason,
				 uint32_t waited_ms, bool host_seen, bool leaving)
{
	struct osc2_no_host_view v = {
		.timeout_ms = timeout_ms,
		.reason = reason,
		.waited_ms = waited_ms,
		.host_seen = host_seen,
		.leaving = leaving,
	};

	return osc2_no_host_rule(&v);
}

OPENSC2_TEST(no_host_rule)
{
	const enum opensc2_mode_reason RULE = OPENSC2_MODE_USB_POWER;

	/* The default build (0): the real controller's rule, whatever happens. */
	CHECK_EQ(no_host(0, RULE, 0, false, false), OSC2_NO_HOST_OFF);
	CHECK_EQ(no_host(0, RULE, 600000, false, false), OSC2_NO_HOST_OFF);

	/* The test build on a charger: nobody talks, so after the time it goes wireless. */
	CHECK_EQ(no_host(5000, RULE, 0, false, false), OSC2_NO_HOST_WAIT);
	CHECK_EQ(no_host(5000, RULE, 4999, false, false), OSC2_NO_HOST_WAIT);
	CHECK_EQ(no_host(5000, RULE, 5000, false, false), OSC2_NO_HOST_WIRELESS);
	CHECK_EQ(no_host(5000, RULE, 120000, false, false), OSC2_NO_HOST_WIRELESS);

	/* On a PC: its first request (or the configuration) comes within a second, and then the
	 * board waits for the host as long as it takes, also past the time. */
	CHECK_EQ(no_host(5000, RULE, 150, true, false), OSC2_NO_HOST_HOST);
	CHECK_EQ(no_host(5000, RULE, 60000, true, false), OSC2_NO_HOST_HOST);

	/* Boots asked to be wired wait as before: the application's choice, "reboot wired". */
	CHECK_EQ(no_host(5000, OPENSC2_MODE_APP, 60000, false, false), OSC2_NO_HOST_OFF);
	CHECK_EQ(no_host(5000, OPENSC2_MODE_WIRED_RESTART, 60000, false, false), OSC2_NO_HOST_OFF);

	/* Already on the way to a reboot (STEAM, the pairing button, Steam's 0x95): nothing more. */
	CHECK_EQ(no_host(5000, RULE, 60000, false, true), OSC2_NO_HOST_OFF);
}

/* ---- the retained area ----------------------------------------------------------------------- */

#define AREA_TEST_SIZE (sizeof(struct osc2_retained) + 64)

struct line_log {
	int count;
	char last[80];
	char first[80];
};

static void collect_line(const char *line, size_t len, void *ctx)
{
	struct line_log *l = ctx;

	if (len >= sizeof(l->last)) {
		len = sizeof(l->last) - 1;
	}
	if (l->count == 0) {
		memcpy(l->first, line, len);
		l->first[len] = '\0';
	}
	memcpy(l->last, line, len);
	l->last[len] = '\0';
	l->count++;
}

OPENSC2_TEST(retained_area)
{
	static uint32_t area[(AREA_TEST_SIZE + 3) / 4];
	struct osc2_retained *r = (struct osc2_retained *)area;
	struct line_log lines;
	char buf[80];

	memset(area, 0x5A, sizeof(area));   /* power-on RAM */
	CHECK(!osc2_ret_valid(r, AREA_TEST_SIZE));
	osc2_ret_format(r, AREA_TEST_SIZE);
	CHECK(osc2_ret_valid(r, AREA_TEST_SIZE));
	CHECK_EQ(r->size, 64);
	CHECK(!osc2_ret_valid(r, AREA_TEST_SIZE + 4));   /* another size: not ours */

	/* A header field changed without sealing is caught; the mark is outside the CRC. */
	r->boots++;
	CHECK(!osc2_ret_valid(r, AREA_TEST_SIZE));
	osc2_ret_seal(r);
	CHECK(osc2_ret_valid(r, AREA_TEST_SIZE));
	r->mark = 0x12345678;
	CHECK(osc2_ret_valid(r, AREA_TEST_SIZE));

	osc2_ret_append(r, "one\n", 4);
	osc2_ret_append(r, "two\n", 4);
	memset(&lines, 0, sizeof(lines));
	osc2_ret_lines(r, buf, sizeof(buf), collect_line, &lines);
	CHECK_EQ(lines.count, 2);
	CHECK(strcmp(lines.first, "one") == 0);
	CHECK(strcmp(lines.last, "two") == 0);

	/* Wrap around: the oldest, partly overwritten line is skipped. */
	for (int i = 0; i < 10; i++) {
		char line[16];

		line[0] = 'L';
		line[1] = (char)('0' + i);
		memcpy(&line[2], "-abcdefghi\n", 11);
		osc2_ret_append(r, line, 13);
	}
	CHECK(osc2_ret_valid(r, AREA_TEST_SIZE));
	CHECK_EQ(r->used, 64);
	memset(&lines, 0, sizeof(lines));
	osc2_ret_lines(r, buf, sizeof(buf), collect_line, &lines);
	CHECK_EQ(lines.count, 4);   /* 64 bytes hold four whole 13-byte lines after the cut */
	CHECK(strcmp(lines.last, "L9-abcdefghi") == 0);
	CHECK(strcmp(lines.first, "L6-abcdefghi") == 0);

	osc2_ret_clear(r);
	CHECK(osc2_ret_valid(r, AREA_TEST_SIZE));
	memset(&lines, 0, sizeof(lines));
	osc2_ret_lines(r, buf, sizeof(buf), collect_line, &lines);
	CHECK_EQ(lines.count, 0);

	CHECK_EQ(osc2_crc32(0, "123456789", 9), 0xCBF43926u);   /* the standard check value */
}

/* Why the last wired session ended, as the shell shows it at the next boot. */
OPENSC2_TEST(retained_end_reason)
{
	static uint32_t area[(AREA_TEST_SIZE + 3) / 4];
	struct osc2_retained *r = (struct osc2_retained *)area;

	osc2_ret_format(r, AREA_TEST_SIZE);
	/* Steam's reboot into wireless mode (26 characters) is kept whole. */
	osc2_ret_set_end(r, "host: reboot into wireless");
	CHECK(osc2_ret_valid(r, AREA_TEST_SIZE));
	CHECK(strcmp(r->session.end, "host: reboot into wireless") == 0);
	/* A longer text is cut to OSC2_RET_END_LEN - 1 characters and stays terminated. */
	osc2_ret_set_end(r, "0123456789012345678901234567890123456789");
	CHECK(osc2_ret_valid(r, AREA_TEST_SIZE));
	CHECK_EQ(strlen(r->session.end), OSC2_RET_END_LEN - 1);
	CHECK(memcmp(r->session.end, "0123456789012345678901234567890", OSC2_RET_END_LEN - 1) == 0);
	/* A shorter one leaves nothing of the one before. */
	osc2_ret_set_end(r, "pair button");
	CHECK(strcmp(r->session.end, "pair button") == 0);
	CHECK_EQ(r->session.end[OSC2_RET_END_LEN - 1], 0);
	/* Clearing the log clears it too. */
	osc2_ret_clear(r);
	CHECK_EQ(r->session.end[0], 0);
}

/* ---- settings ranges ------------------------------------------------------------------------- */

OPENSC2_TEST(setting_ranges)
{
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_LIZARD_MODE, 5), 1);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_LIZARD_MODE, -1), 0);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_STEAM_WATCHDOG, 2), 1);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_TRIGGER_THRESHOLD, 10), 40);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_TRIGGER_THRESHOLD, 120), 99);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_LED_BRIGHTNESS, 200), 100);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_MOMENTUM_DECAY, 0), 1);
	/* -1 lies within the click thresholds' range (Steam writes it), -2 does not */
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_LEFT_CLICK_PRESSURE, -1), -1);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_LEFT_CLICK_PRESSURE, -2), -1);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_PAD_IGNORE_ON_STICK, 4), 3);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_HAPTIC_GAIN_DB, 30), 6);
	/* the IMU's three go to the IMU unclamped */
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_IMU_MODE, 0x18), 0x18);
	CHECK_EQ(opensc2_setting_clamp(OPENSC2_SETTING_IMU_THRESHOLD, 32767), 32767);
}
