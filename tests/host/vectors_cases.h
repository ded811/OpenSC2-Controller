/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only, in both binaries: the shared vectors of the hardware test suite (the .txt
 * files of tests/vectors/hwtest, which the suite's own self-tests read too) run through the
 * library's own code: the state report rules, lizard mode, the test channel's parser, the ring's
 * entry format, the settings table. A built-in copy of each file's first lines always runs; the
 * files run as well when run.ps1 / run.sh pass their folder (--vectors) and they are there.
 *
 * The format: one record per line, '#' starts a comment, hex in either case, and a hex value may
 * be split into several tokens (they are joined up to the next word that is not hex).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opensc2/codec.h>
#include <opensc2/settings.h>
#include "input_rules.h"
#include "lizard.h"
#include "retained.h"
#include "test_core.h"

#define VEC_TOKENS 64

static int vec_split(char *line, char *tok[VEC_TOKENS])
{
	char *hash = strchr(line, '#');
	char *p = line;
	int n = 0;

	if (hash) {
		*hash = '\0';
	}
	while (*p && n < VEC_TOKENS) {
		while (*p && isspace((unsigned char)*p)) {
			p++;
		}
		if (!*p) {
			break;
		}
		tok[n++] = p;
		while (*p && !isspace((unsigned char)*p)) {
			p++;
		}
		if (*p) {
			*p++ = '\0';
		}
	}
	return n;
}

static bool vec_is_hex(const char *t)
{
	if (!*t) {
		return false;
	}
	for (; *t; t++) {
		if (!isxdigit((unsigned char)*t)) {
			return false;
		}
	}
	return true;
}

/* Hex tokens from tok[*i] on, joined; *i moves past them. Returns the bytes, or -1. */
static int vec_hex(char *tok[], int n, int *i, uint8_t *out, size_t max)
{
	char joined[512];
	size_t at = 0;
	int bytes;

	joined[0] = '\0';
	while (*i < n && vec_is_hex(tok[*i])) {
		size_t len = strlen(tok[*i]);

		if (at + len >= sizeof(joined)) {
			return -1;
		}
		memcpy(&joined[at], tok[*i], len + 1);
		at += len;
		(*i)++;
	}
	bytes = osc2_test_unhex(joined, out, max);
	return bytes;
}

static int vec_find(char *tok[], int n, const char *word)
{
	for (int i = 0; i < n; i++) {
		if (strcmp(tok[i], word) == 0) {
			return i;
		}
	}
	return -1;
}

/* Every setting at its default. */
static void vec_default_settings(int16_t settings[OPENSC2_SETTINGS_COUNT])
{
	for (int i = 0; i < OPENSC2_SETTINGS_COUNT; i++) {
		settings[i] = opensc2_setting_default((uint8_t)i);
	}
}

/* "n=v,n=v,..." onto `settings`; false for a malformed list. */
static bool vec_settings(const char *text, int16_t settings[OPENSC2_SETTINGS_COUNT])
{
	const char *p = text;

	while (*p) {
		char *end;
		long number = strtol(p, &end, 10), value;

		if (*end != '=' || number < 0 || number >= OPENSC2_SETTINGS_COUNT) {
			return false;
		}
		value = strtol(end + 1, &end, 10);
		settings[number] = (int16_t)value;
		if (*end == ',') {
			end++;
		} else if (*end != '\0') {
			return false;
		}
		p = end;
	}
	return true;
}

typedef void (*vec_line_fn)(char *tok[], int n, void *ctx);

static void vec_run_line(const char *text, vec_line_fn fn, void *ctx)
{
	char copy[1024];
	char *tok[VEC_TOKENS];
	int before = current_failed, n;

	snprintf(copy, sizeof(copy), "%s", text);
	n = vec_split(copy, tok);
	if (n == 0) {
		return;
	}
	current_failed = 0;
	fn(tok, n, ctx);
	if (current_failed) {
		printf("  in vector: %s\n", text);
	}
	current_failed |= before;
}

static void vec_run_builtin(const char *const *lines, size_t count, vec_line_fn fn, void *ctx)
{
	for (size_t i = 0; i < count; i++) {
		vec_run_line(lines[i], fn, ctx);
	}
}

/* The file `name` in the vectors folder, if both are there. Returns the lines run, or -1. */
static int vec_run_file(const char *name, vec_line_fn fn, void *ctx)
{
	char path[1024], line[1024];
	FILE *f;
	int lines = 0;

	if (!vectors_dir) {
		return -1;
	}
	snprintf(path, sizeof(path), "%s/%s", vectors_dir, name);
	f = fopen(path, "r");
	if (!f) {
		printf("  (vectors: %s not there yet; the built-in copy ran)\n", name);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		vec_run_line(line, fn, ctx);
		lines++;
	}
	fclose(f);
	return lines;
}

/* ---- state.txt: T68 <setting 68> PREV <left><right> [RULES <hex>] [SET <n>=<v>,...]
 *      [CLICK <left><right>] IN <state52> OUT <report bytes 2..53> ---- */

static const char *const builtin_state[] = {
	"# A held, nonce",
	"T68 90 PREV 00 IN 010000000000000000000000000000000000000000000000000000000100A5A50000000000000000000000000000000000000000 OUT 010000000000000000000000000000000000000000000000000000000100A5A50000000000000000000000000000000000000000",
	"# left trigger full",
	"T68 90 PREV 00 IN 00000000FF7F00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000008FF7F00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"# right pad touched, X -32768, Y 100, pressure 40000",
	"T68 90 PREV 00 IN 0000200000000000000000000000000000000000000000806400409C000000000000000000000000000000000000000000000000 OUT 0000200000000000000000000000000000000000000001806400FF7F000000000000000000000000000000000000000000000000",
	"# left pad untouched at (500, -500), pressure 1000",
	"T68 90 PREV 00 IN 00000000000000000000000000000000F4010CFEE803000000000000000000000000000000000000000000000000000000000000 OUT 0000000000000000000000000000000000000000E803000000000000000000000000000000000000000000000000000000000000",
	"# sticks -32768, 32767, 1, -1",
	"T68 90 PREV 00 IN 00000000000000000080FF7F0100FFFF000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000000000000000180FF7F0100FFFF000000000000000000000000000000000000000000000000000000000000000000000000",
	"# bits 23 and 27 injected, triggers 0",
	"T68 90 PREV 00 IN 00008008000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"# hysteresis, left trigger (bit 27), then the same on the right trigger (bit 23)",
	"T68 90 PREV 00 IN 00000000987900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000000987900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 90 PREV 00 IN 00000000997900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000008997900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 90 PREV 10 IN 00000000CB6C00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000008CB6C00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 90 PREV 10 IN 00000000CA6C00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000000CA6C00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 40 PREV 00 IN 00000000993900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000000993900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 40 PREV 00 IN 000000009A3900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 000000089A3900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 99 PREV 00 IN 00000000FE7F00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000000FE7F00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 99 PREV 00 IN 00000000FF7F00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000008FF7F00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 99 PREV 10 IN 00000000507800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000008507800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 99 PREV 10 IN 000000004F7800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 000000004F7800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 90 PREV 00 IN 00000000000098790000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000000000098790000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 90 PREV 00 IN 00000000000099790000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00008000000099790000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 90 PREV 01 IN 000000000000CB6C0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 000080000000CB6C0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 90 PREV 01 IN 000000000000CA6C0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 000000000000CA6C0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 40 PREV 00 IN 00000000000099390000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000000000099390000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 40 PREV 00 IN 0000000000009A390000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 0000800000009A390000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 99 PREV 00 IN 000000000000FE7F0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 000000000000FE7F0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 99 PREV 00 IN 000000000000FF7F0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 000080000000FF7F0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 99 PREV 01 IN 00000000000050780000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00008000000050780000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 99 PREV 01 IN 0000000000004F780000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 0000000000004F780000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"# setting 68 out of range: clamped to 40 and 99 by the rule itself",
	"T68 30 PREV 00 IN 000000009A3900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 000000089A3900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
	"T68 120 PREV 00 IN 00000000FE7F00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000 OUT 00000000FE7F00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
};

/* The library's path: decode (pads' touch and click from the button bits), the input rules
 * (input_rules.c) with the previous trigger bits and pad clicks, encode. RULES is the rule word in
 * hex (without it: the trigger bits only, the rules of OpenSC2 0.2.0); SET changes settings from
 * their defaults (T68 is setting 68); CLICK gives the pads' clicks by pressure before this report
 * (worked out with the same thresholds). */
static void state_line(char *tok[], int n, void *ctx)
{
	uint8_t in[OSC2_TEST_STATE52_LEN], want[OSC2_TEST_STATE52_LEN];
	uint8_t buf[OPENSC2_STATE_REPORT_LEN] = { OPENSC2_RPT_STATE, 0 };
	uint8_t out[OPENSC2_STATE_REPORT_LEN];
	struct opensc2_state s;
	struct osc2_input_rules r;
	int16_t settings[OPENSC2_SETTINGS_COUNT];
	int t68 = vec_find(tok, n, "T68"), prev = vec_find(tok, n, "PREV");
	int rules_at = vec_find(tok, n, "RULES"), set_at = vec_find(tok, n, "SET");
	int click_at = vec_find(tok, n, "CLICK");
	int at_in = vec_find(tok, n, "IN"), at_out = vec_find(tok, n, "OUT");
	uint32_t rules = OSC2_RULES_V0_2;

	(void)ctx;
	CHECK(at_in >= 0 && at_out > at_in);
	if (at_in < 0 || at_out <= at_in) {
		return;
	}
	vec_default_settings(settings);
	if (t68 >= 0 && t68 + 1 < n) {
		settings[OPENSC2_SETTING_TRIGGER_THRESHOLD] = (int16_t)atoi(tok[t68 + 1]);
	}
	if (set_at >= 0 && set_at + 1 < n) {
		CHECK(vec_settings(tok[set_at + 1], settings));
	}
	if (rules_at >= 0 && rules_at + 1 < n) {
		rules = (uint32_t)strtoul(tok[rules_at + 1], NULL, 16);
	}
	osc2_input_rules_reset(&r);
	if (prev >= 0 && prev + 1 < n && strlen(tok[prev + 1]) == 2) {
		r.trigger_bit[OPENSC2_LEFT] = tok[prev + 1][0] == '1';
		r.trigger_bit[OPENSC2_RIGHT] = tok[prev + 1][1] == '1';
	}
	if (click_at >= 0 && click_at + 1 < n && strlen(tok[click_at + 1]) == 2) {
		r.pad_click[OPENSC2_LEFT] = tok[click_at + 1][0] == '1';
		r.pad_click[OPENSC2_RIGHT] = tok[click_at + 1][1] == '1';
		r.click_setting[OPENSC2_LEFT] = settings[OPENSC2_SETTING_LEFT_CLICK_PRESSURE];
		r.click_setting[OPENSC2_RIGHT] = settings[OPENSC2_SETTING_RIGHT_CLICK_PRESSURE];
	}
	at_in++;
	at_out++;
	CHECK_EQ(vec_hex(tok, n, &at_in, in, sizeof(in)), OSC2_TEST_STATE52_LEN);
	CHECK_EQ(vec_hex(tok, n, &at_out, want, sizeof(want)), OSC2_TEST_STATE52_LEN);
	memcpy(&buf[2], in, sizeof(in));
	opensc2_decode_state(buf, &s, NULL);
	osc2_input_rules_apply(&r, rules, settings, &s);
	opensc2_encode_state(&s, 0, out);
	CHECK(memcmp(&out[2], want, sizeof(want)) == 0);
}

OPENSC2_TEST(state_vectors)
{
	vec_run_builtin(builtin_state, sizeof(builtin_state) / sizeof(builtin_state[0]), state_line,
			NULL);
	(void)vec_run_file("state.txt", state_line, NULL);
}

/* ---- lizard.txt ------------------------------------------------------------------------------ */

static const char *const builtin_lizard[] = {
	"START",
	"FRAME -> -",
	"FRAME buttons=0x00000001 -> 410000280000000000",
	"FRAME buttons=0x00000005 -> -",
	"FRAME buttons=0x00000007 -> 410000282900000000",
	"FRAME buttons=0x00000042 -> 410000292900000000",
	"FRAME buttons=0x0000007F -> 410000010101010101",
	"FRAME -> 410000000000000000",
	"FRAME buttons=0x00000400 -> 410000510000000000",
	"FRAME -> 410000000000000000",
	"FRAME buttons=0x00000800 -> 4100004F0000000000",
	"FRAME -> 410000000000000000",
	"FRAME buttons=0x00001000 -> 410000500000000000",
	"FRAME -> 410000000000000000",
	"FRAME buttons=0x00002000 -> 410000520000000000",
	"FRAME -> 410000000000000000",
	"FRAME buttons=0x00004000 -> 4100002B0000000000",
	"FRAME -> 410000000000000000",
	"# pointer, right pad",
	"FRAME buttons=0x00200000 rpad=0,0 -> -",
	"FRAME buttons=0x00200000 rpad=1280,0 -> 40000A000000",
	"FRAME buttons=0x00200000 rpad=1280,1280 -> 400000F60000",
	"FRAME buttons=0x00200000 rpad=1280,1280 -> 400000000000",
	"FRAME buttons=0x00200000 rpad=1280,1280 -> -",
	"FRAME -> -",
	"# scroll, left pad",
	"FRAME buttons=0x02000000 lpad=0,0 -> -",
	"FRAME buttons=0x02000000 lpad=3072,2048 -> 40000000FE03",
	"FRAME -> 400000000000",
	"# mouse buttons",
	"FRAME buttons=0x00800000 -> 400100000000",
	"FRAME buttons=0x00800000 -> 400100000000",
	"FRAME buttons=0x00800000 -> 400100000000",
	"FRAME -> 400000000000",
	"FRAME buttons=0x08000000 -> 400200000000",
	"FRAME -> 400000000000",
	"FRAME buttons=0x00600000 rpad=0,0 -> 400100000000",
	"FRAME -> 400000000000",
	"# thumb on the right stick (bit 20) blanks the right pad while s82 has bit 1",
	"FRAME buttons=0x00300000 rpad=0,0 -> -",
	"FRAME buttons=0x00300000 rpad=1280,0 -> -",
	"FRAME s82=0 buttons=0x00300000 rpad=1280,0 -> -",
	"FRAME s82=0 buttons=0x00300000 rpad=2560,0 -> 40000A000000",
	"FRAME s82=0 buttons=0x00300000 rpad=2560,0 -> 400000000000",
	"FRAME -> -",
	"# remainders: truncation toward zero",
	"FRAME buttons=0x00200000 rpad=0,0 -> -",
	"FRAME buttons=0x00200000 rpad=100,0 -> -",
	"FRAME buttons=0x00200000 rpad=200,0 -> 400001000000",
	"FRAME buttons=0x00200000 rpad=200,0 -> 400000000000",
	"FRAME buttons=0x00200000 rpad=0,0 -> 4000FF000000",
	"FRAME buttons=0x00200000 rpad=-100,0 -> 400000000000",
	"FRAME buttons=0x00200000 rpad=-200,0 -> 4000FF000000",
	"FRAME -> 400000000000",
	"# transitions",
	"LEAVE -> 410000000000000000 400000000000",
	"LEAVE -> -",
	"ENTER -> 400000000000 410000000000000000",
	"FRAME buttons=0x00000001 -> -",
	"FRAME -> 410000000000000000",
	"SWITCHES down -> -",
	"FRAME buttons=0x00000001 -> -",
	"FRAME buttons=0x00800000 -> -",
	"SWITCHES up -> -",
	"FRAME buttons=0x00000001 -> -",
	"FRAME -> 410000000000000000",
	"# resync",
	"FRAME buttons=0x00200000 rpad=0,0 -> -",
	"FRAME buttons=0x00200000 rpad=640,0 -> 400005000000",
	"RESYNC -> -",
	"FRAME buttons=0x00200001 rpad=5000,0 -> -",
	"FRAME buttons=0x00200000 rpad=5000,0 -> 410000000000000000",
};

struct lizard_run {
	struct opensc2_lizard lz;
	uint32_t rules;
	int16_t settings[OPENSC2_SETTINGS_COUNT];
};

static void lizard_start(struct lizard_run *run)
{
	opensc2_lizard_reset(&run->lz);
	run->rules = 0;
	vec_default_settings(run->settings);
}

/* The words of a step's haptic events, in opensc2_lizard_haptics()'s order. */
static int lizard_event_words(uint32_t made, const char *words[4])
{
	struct opensc2_haptic h[4];
	uint8_t count = opensc2_lizard_haptics(made, h);

	for (uint8_t i = 0; i < count; i++) {
		bool tick = h[i].command.command == OPENSC2_HAPTIC_CMD_CLICK_A;

		CHECK_EQ(h[i].report_id, 0);
		CHECK_EQ(h[i].kind, OPENSC2_HAPTIC_COMMAND);
		CHECK_EQ(h[i].command.gain_db, tick ? 8 : 6);
		CHECK_EQ(h[i].channels, h[i].target ? OPENSC2_HAPTIC_RIGHT_PAD : OPENSC2_HAPTIC_LEFT_PAD);
		words[i] = tick ? (h[i].target ? "TICK_R" : "TICK_L") :
				  (h[i].target ? "CLICK_R" : "CLICK_L");
	}
	return count;
}

static bool parse_xy(const char *text, int16_t *x, int16_t *y)
{
	char *end;
	long a = strtol(text, &end, 10), b;

	if (*end != ',') {
		return false;
	}
	b = strtol(end + 1, &end, 10);
	*x = (int16_t)a;
	*y = (int16_t)b;
	return *end == '\0';
}

static void lizard_line(char *tok[], int n, void *ctx)
{
	struct lizard_run *run = ctx;
	uint8_t want[64], got[64], keyboard[9], mouse[6];
	int arrow = vec_find(tok, n, "->"), want_len = 0, got_len = 0, events_at = n;
	uint32_t made = 0;

	if (strcmp(tok[0], "START") == 0) {
		lizard_start(run);
		return;
	}
	if (strcmp(tok[0], "RULES") == 0) {
		CHECK(n == 2);
		run->rules = n == 2 ? (uint32_t)strtoul(tok[1], NULL, 16) : 0;
		return;
	}
	if (strcmp(tok[0], "SET") == 0) {
		CHECK(n == 2 && vec_settings(tok[1], run->settings));
		return;
	}
	CHECK(arrow > 0);
	if (arrow <= 0) {
		return;
	}
	if (arrow + 1 < n && strcmp(tok[arrow + 1], "-") != 0) {
		int i = arrow + 1;

		want_len = vec_hex(tok, n, &i, want, sizeof(want));
		CHECK(want_len > 0);
		events_at = i;
	} else {
		events_at = arrow + 2;
	}
	if (strcmp(tok[0], "FRAME") == 0) {
		struct opensc2_state s;
		int16_t s82 = run->settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK];
		const char *words[4];
		int count;

		memset(&s, 0, sizeof(s));
		for (int i = 1; i < arrow; i++) {
			if (strncmp(tok[i], "buttons=", 8) == 0) {
				s.buttons = (uint32_t)strtoul(&tok[i][8], NULL, 0);
			} else if (strncmp(tok[i], "lpad=", 5) == 0) {
				CHECK(parse_xy(&tok[i][5], &s.pad[OPENSC2_LEFT].x, &s.pad[OPENSC2_LEFT].y));
			} else if (strncmp(tok[i], "rpad=", 5) == 0) {
				CHECK(parse_xy(&tok[i][5], &s.pad[OPENSC2_RIGHT].x,
					       &s.pad[OPENSC2_RIGHT].y));
			} else if (strncmp(tok[i], "s82=", 4) == 0) {
				s82 = (int16_t)strtol(&tok[i][4], NULL, 0);
			} else {
				CHECK(false);   /* an unknown FRAME field */
			}
		}
		s.pad[OPENSC2_LEFT].touched = (s.buttons & OPENSC2_TOUCH_LEFT_PAD) != 0;
		s.pad[OPENSC2_LEFT].clicked = (s.buttons & OPENSC2_CLICK_LEFT_PAD) != 0;
		s.pad[OPENSC2_RIGHT].touched = (s.buttons & OPENSC2_TOUCH_RIGHT_PAD) != 0;
		s.pad[OPENSC2_RIGHT].clicked = (s.buttons & OPENSC2_CLICK_RIGHT_PAD) != 0;
		{
			/* s82= changes setting 82 for this frame only */
			int16_t keep = run->settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK];

			run->settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK] = s82;
			made = opensc2_lizard_step(&run->lz, &s, run->settings, run->rules, keyboard,
						   mouse);
			run->settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK] = keep;
		}
		/* The haptic events after the reports, in order. */
		count = lizard_event_words(made, words);
		CHECK_EQ(n - events_at, count);
		for (int i = 0; i < count && events_at + i < n; i++) {
			CHECK(strcmp(tok[events_at + i], words[i]) == 0);
		}
		/* osc2_report_lizard() sends the keyboard report first. */
		if (made & OPENSC2_LIZARD_KEYBOARD) {
			memcpy(&got[got_len], keyboard, sizeof(keyboard));
			got_len += (int)sizeof(keyboard);
		}
		if (made & OPENSC2_LIZARD_MOUSE) {
			memcpy(&got[got_len], mouse, sizeof(mouse));
			got_len += (int)sizeof(mouse);
		}
	} else if (strcmp(tok[0], "ENTER") == 0) {
		/* osc2_lizard_set(true): the mouse report, then the keyboard one. */
		made = opensc2_lizard_enter(&run->lz, keyboard, mouse);
		if (made) {
			memcpy(&got[0], mouse, sizeof(mouse));
			memcpy(&got[sizeof(mouse)], keyboard, sizeof(keyboard));
			got_len = (int)(sizeof(mouse) + sizeof(keyboard));
		}
	} else if (strcmp(tok[0], "LEAVE") == 0) {
		/* osc2_lizard_set(false): the keyboard report, then the mouse one. */
		made = opensc2_lizard_leave(&run->lz, keyboard, mouse);
		if (made) {
			memcpy(&got[0], keyboard, sizeof(keyboard));
			memcpy(&got[sizeof(keyboard)], mouse, sizeof(mouse));
			got_len = (int)(sizeof(mouse) + sizeof(keyboard));
		}
	} else if (strcmp(tok[0], "SWITCHES") == 0) {
		CHECK(arrow == 2);
		opensc2_lizard_switches(&run->lz, strcmp(tok[1], "up") == 0);
	} else if (strcmp(tok[0], "RESYNC") == 0) {
		opensc2_lizard_resync(&run->lz);
	} else {
		CHECK(false);   /* an unknown lizard action */
	}
	if (strcmp(tok[0], "FRAME") != 0) {
		CHECK_EQ(n, events_at);   /* only a frame has haptic events */
	}
	CHECK_EQ(got_len, want_len);
	if (got_len == want_len && want_len > 0) {
		CHECK(memcmp(got, want, (size_t)want_len) == 0);
	}
}

OPENSC2_TEST(lizard_vectors)
{
	struct lizard_run run;

	lizard_start(&run);
	vec_run_builtin(builtin_lizard, sizeof(builtin_lizard) / sizeof(builtin_lizard[0]),
			lizard_line, &run);
	lizard_start(&run);
	(void)vec_run_file("lizard.txt", lizard_line, &run);
}

/* The resync and the release the hardware test hooks use around arming. */
OPENSC2_TEST(lizard_resync)
{
	struct opensc2_lizard lz;
	struct opensc2_state s;
	uint8_t keyboard[9], mouse[6];
	static const uint8_t empty_kb[9] = { 0x41 }, zero_mouse[6] = { 0x40 };
	int16_t defaults[OPENSC2_SETTINGS_COUNT];

	vec_default_settings(defaults);

	/* A held key: the release sends both empty reports, then has nothing left to release. */
	opensc2_lizard_reset(&lz);
	memset(&s, 0, sizeof(s));
	(void)opensc2_lizard_step(&lz, &s, defaults, 0, keyboard, mouse);
	s.buttons = OPENSC2_BTN_A;
	CHECK_EQ(opensc2_lizard_step(&lz, &s, defaults, 0, keyboard, mouse), OPENSC2_LIZARD_KEYBOARD);
	CHECK_EQ(opensc2_lizard_release(&lz, keyboard, mouse),
		 OPENSC2_LIZARD_KEYBOARD | OPENSC2_LIZARD_MOUSE);
	CHECK(memcmp(keyboard, empty_kb, sizeof(keyboard)) == 0);
	CHECK(memcmp(mouse, zero_mouse, sizeof(mouse)) == 0);
	CHECK_EQ(opensc2_lizard_release(&lz, keyboard, mouse), 0);

	/* An owed zero mouse report counts as held too. */
	opensc2_lizard_reset(&lz);
	memset(&s, 0, sizeof(s));
	s.buttons = OPENSC2_TOUCH_RIGHT_PAD;
	s.pad[OPENSC2_RIGHT].touched = true;
	(void)opensc2_lizard_step(&lz, &s, defaults, 0, keyboard, mouse);   /* keyboard sync, touch-down */
	s.pad[OPENSC2_RIGHT].x = 640;
	CHECK_EQ(opensc2_lizard_step(&lz, &s, defaults, 0, keyboard, mouse), OPENSC2_LIZARD_MOUSE);
	CHECK_EQ(opensc2_lizard_release(&lz, keyboard, mouse),
		 OPENSC2_LIZARD_KEYBOARD | OPENSC2_LIZARD_MOUSE);
	CHECK_EQ(opensc2_lizard_step(&lz, &s, defaults, 0, keyboard, mouse), 0);   /* nothing owed now */

	/* Nothing held, or lizard mode off: nothing to release. */
	opensc2_lizard_reset(&lz);
	CHECK_EQ(opensc2_lizard_release(&lz, keyboard, mouse), 0);
	s.buttons = OPENSC2_BTN_B;
	(void)opensc2_lizard_leave(&lz, keyboard, mouse);
	CHECK_EQ(opensc2_lizard_release(&lz, keyboard, mouse), 0);

	/* A resync keeps lizard mode on and its switches as they are, and adopts what is held. */
	(void)opensc2_lizard_enter(&lz, keyboard, mouse);
	opensc2_lizard_switches(&lz, false);
	opensc2_lizard_resync(&lz);
	CHECK(lz.active);
	CHECK(!lz.keyboard_on);
	CHECK(!lz.buttons_on);
	opensc2_lizard_switches(&lz, true);
	memset(&s, 0, sizeof(s));
	s.buttons = OPENSC2_BTN_A;
	CHECK_EQ(opensc2_lizard_step(&lz, &s, defaults, 0, keyboard, mouse), 0);   /* adopted */
	s.buttons = 0;
	CHECK_EQ(opensc2_lizard_step(&lz, &s, defaults, 0, keyboard, mouse), OPENSC2_LIZARD_KEYBOARD);
}

/* ---- testproto.txt ---------------------------------------------------------------------------- */

static const char *const builtin_testproto[] = {
	"REQ 1eab577e010501785634120f0000 ARMED 0 -> CLAIM 1 OP 01 ARGS 7",
	"REQ 1eab577e010603 ARMED 0 -> CLAIM 0",
	"REQ 1eab577e010603 ARMED 1 -> CLAIM 1 OP 03 ARGS 0",
	"REQ 1eab577f010603 ARMED 1 -> CLAIM 0",
	"REQ 1eab577e020603 ARMED 1 -> CLAIM 0",
	"REQ 1eab577e0106 ARMED 1 -> CLAIM 0",
	"REQ 1eab577e010924efbeadde ARMED 1 -> CLAIM 1 OP 24 ARGS 4",
	"BATTERY 1eab577e010b1202373c0fe40c881306fff4013412a501 -> R43 4302373c0fe40c881306fff4013412 "
	"RBE 34123c0fe40c881306fff401370102",
};

static void testproto_line(char *tok[], int n, void *ctx)
{
	uint8_t req[OSC2_TEST_REQUEST_MAX + 8];
	int i = 1, len;

	(void)ctx;
	if (n < 2) {
		CHECK(false);   /* a short line */
		return;
	}
	len = vec_hex(tok, n, &i, req, sizeof(req));
	CHECK(len >= 0);
	if (len < 0) {
		return;
	}
	if (strcmp(tok[0], "REQ") == 0) {
		int armed = vec_find(tok, n, "ARMED"), claim = vec_find(tok, n, "CLAIM");
		int op = vec_find(tok, n, "OP"), args = vec_find(tok, n, "ARGS");
		bool want;

		CHECK(armed > 0 && claim > 0);
		if (armed <= 0 || claim <= 0 || claim + 1 >= n || armed + 1 >= n) {
			return;
		}
		want = atoi(tok[claim + 1]) != 0;
		CHECK_EQ(osc2_test_claims(req, (uint8_t)len, atoi(tok[armed + 1]) != 0), want);
		if (want && op > 0 && op + 1 < n) {
			CHECK_EQ(req[6], strtoul(tok[op + 1], NULL, 16));
		}
		if (want && args > 0 && args + 1 < n) {
			CHECK_EQ(len - OSC2_TEST_HEADER_LEN, atoi(tok[args + 1]));
		}
	} else if (strcmp(tok[0], "BATTERY") == 0) {
		uint8_t r43[OPENSC2_BATTERY_REPORT_LEN], rbe[OPENSC2_BATTERY_DATA_LEN];
		uint8_t want43[32], wantbe[32];
		int at43 = vec_find(tok, n, "R43"), atbe = vec_find(tok, n, "RBE");
		struct opensc2_battery b;
		uint8_t flags;

		CHECK(osc2_test_is_request(req, (uint8_t)len));
		CHECK_EQ(req[6], OSC2_TEST_OP_BATTERY);
		CHECK_EQ(len - OSC2_TEST_HEADER_LEN, OSC2_TEST_BATTERY_ARGS);
		CHECK(at43 > 0 && atbe > at43);
		if (at43 <= 0 || atbe <= at43 || len - OSC2_TEST_HEADER_LEN != OSC2_TEST_BATTERY_ARGS) {
			return;
		}
		at43++;
		atbe++;
		CHECK_EQ(vec_hex(tok, n, &at43, want43, sizeof(want43)), OPENSC2_BATTERY_REPORT_LEN);
		CHECK_EQ(vec_hex(tok, n, &atbe, wantbe, sizeof(wantbe)), OPENSC2_BATTERY_DATA_LEN);
		osc2_test_battery_args(&req[OSC2_TEST_HEADER_LEN], &b, &flags);
		opensc2_encode_battery(&b, r43);
		opensc2_encode_battery_data(&b, rbe);
		CHECK(memcmp(r43, want43, sizeof(r43)) == 0);
		CHECK(memcmp(rbe, wantbe, sizeof(rbe)) == 0);
	} else {
		CHECK(false);   /* an unknown testproto record */
	}
}

OPENSC2_TEST(testproto_vectors)
{
	vec_run_builtin(builtin_testproto, sizeof(builtin_testproto) / sizeof(builtin_testproto[0]),
			testproto_line, NULL);
	(void)vec_run_file("testproto.txt", testproto_line, NULL);
}

/* ---- ring.txt --------------------------------------------------------------------------------- */

static const char *const builtin_ring[] = {
	"ENTRY 0000000001010478563412                 # ARMED nonce 0x12345678, link 1, t 0",
	"ENTRY e803000011010a1027204ee80300000000     # RUMBLE 10000/20000, intensity 1000, link 1",
	"ENTRY 8813000016010e0105000000410000280000000000   # LIZARD_RPT diverted, frame 5, Enter",
	"ENTRY 1027000018010a008703000000002d6400     # VENDOR SET 0x87 {45,100} rc 0, link 1",
	"ENTRY 0000000018010900ee21000000000118       # VENDOR SET 0xEE esb/bond, n 33: key 1, 24",
	"ENTRY 000000001202090687000404cdfb3cb6       # HAPTIC PCM data, 0x87 target 0, 01 02 03 04",
	"ENTRY 0000000010020a81080100000000000000     # OUTPUT 0x81, 8 bytes, decoded 1, test link",
	"ENTRY 0000000012020a00810002000000000000     # HAPTIC pulse, 0x81 target 0 = right pad",
	"ENTRY 00000000140303ff0000                   # SETTING all back to defaults, link none",
	"ENTRY 000000001301066400320a0032             # LED 100 0 50 10, override 0, brightness 50",
};

static void ring_line(char *tok[], int n, void *ctx)
{
	uint8_t e[64], again[OSC2_TEST_ENTRY_MAX];
	char text[256];
	int i = 1, len;

	(void)ctx;
	CHECK(strcmp(tok[0], "ENTRY") == 0);
	len = vec_hex(tok, n, &i, e, sizeof(e));
	CHECK(len >= OSC2_TEST_ENTRY_HEADER && len <= OSC2_TEST_ENTRY_MAX);
	if (len < OSC2_TEST_ENTRY_HEADER || len > OSC2_TEST_ENTRY_MAX) {
		return;
	}
	CHECK_EQ(len, OSC2_TEST_ENTRY_HEADER + e[6]);
	CHECK_EQ(osc2_test_entry(again, opensc2_get_le32(e), e[4], e[5], &e[7], e[6]), len);
	CHECK(memcmp(again, e, (size_t)len) == 0);
	osc2_test_fmt_entry(text, sizeof(text), 0, e, (size_t)len);
	CHECK(strncmp(text, "0 ", 2) == 0);
	CHECK(strstr(text, "KIND_") == NULL);   /* every kind here has a name */
}

OPENSC2_TEST(ring_vectors)
{
	vec_run_builtin(builtin_ring, sizeof(builtin_ring) / sizeof(builtin_ring[0]), ring_line,
			NULL);
	(void)vec_run_file("ring.txt", ring_line, NULL);
}

/* ---- settings.txt: DEFAULT n v, RANGE n lo hi -------------------------------------------------- */

static const char *const builtin_settings[] = {
	"DEFAULT 1 2", "DEFAULT 3 1200", "DEFAULT 9 1", "DEFAULT 10 7000", "DEFAULT 11 200",
	"DEFAULT 12 100", "DEFAULT 13 50", "DEFAULT 14 5500", "DEFAULT 15 923", "DEFAULT 16 382",
	"DEFAULT 17 2", "DEFAULT 18 8000", "DEFAULT 19 1770", "DEFAULT 20 1630", "DEFAULT 21 5",
	"DEFAULT 22 2", "DEFAULT 23 2", "DEFAULT 24 20", "DEFAULT 25 40", "DEFAULT 27 -10",
	"DEFAULT 28 16500", "DEFAULT 29 15000", "DEFAULT 30 500", "DEFAULT 31 400",
	"DEFAULT 32 800", "DEFAULT 34 100", "DEFAULT 35 80", "DEFAULT 38 1", "DEFAULT 40 20",
	"DEFAULT 41 3900", "DEFAULT 42 1", "DEFAULT 44 50", "DEFAULT 45 50", "DEFAULT 49 2",
	"DEFAULT 50 900", "DEFAULT 51 250", "DEFAULT 52 40", "DEFAULT 53 40", "DEFAULT 54 100",
	"DEFAULT 55 100", "DEFAULT 56 10", "DEFAULT 57 10", "DEFAULT 63 150", "DEFAULT 64 4",
	"DEFAULT 65 1", "DEFAULT 66 1", "DEFAULT 68 90", "DEFAULT 69 1", "DEFAULT 70 1",
	"DEFAULT 71 1", "DEFAULT 72 1300", "DEFAULT 73 1000", "DEFAULT 74 3", "DEFAULT 76 -3",
	"DEFAULT 78 1", "DEFAULT 79 2", "DEFAULT 80 1", "DEFAULT 82 3", "DEFAULT 83 1",
	"DEFAULT 84 1", "DEFAULT 85 100", "RANGE 0 0 10", "RANGE 1 0 10", "RANGE 2 0 360",
	"RANGE 3 0 25000", "RANGE 4 0 1", "RANGE 5 0 1", "RANGE 6 0 255", "RANGE 7 0 255",
	"RANGE 8 0 1", "RANGE 9 0 1", "RANGE 10 0 16384", "RANGE 11 0 1000", "RANGE 12 1 1000",
	"RANGE 13 1 500", "RANGE 14 0 32767", "RANGE 15 0 2000", "RANGE 16 0 2000", "RANGE 17 1 10",
	"RANGE 18 0 20000", "RANGE 19 0 4096", "RANGE 20 0 4096", "RANGE 21 0 500", "RANGE 22 0 8",
	"RANGE 23 0 8", "RANGE 24 0 30", "RANGE 25 3 99", "RANGE 26 0 1", "RANGE 27 -24 12",
	"RANGE 28 0 4096", "RANGE 29 0 4096", "RANGE 30 100 1000", "RANGE 31 0 800",
	"RANGE 32 100 1200", "RANGE 33 0 1", "RANGE 34 25 400", "RANGE 35 25 100", "RANGE 36 0 100",
	"RANGE 37 0 1", "RANGE 38 0 1", "RANGE 39 0 1", "RANGE 40 1 180", "RANGE 41 0 25000",
	"RANGE 42 0 1", "RANGE 43 0 1", "RANGE 44 0 100", "RANGE 45 0 100", "RANGE 46 0 2",
	"RANGE 47 0 16", "RANGE 49 1 2", "RANGE 50 0 32767", "RANGE 51 0 32767", "RANGE 52 -1 100",
	"RANGE 53 -1 100", "RANGE 54 0 100", "RANGE 55 0 100", "RANGE 56 0 100", "RANGE 57 0 100",
	"RANGE 58 0 100", "RANGE 59 0 100", "RANGE 60 0 1", "RANGE 61 0 15", "RANGE 62 0 2",
	"RANGE 63 0 300", "RANGE 64 1 16", "RANGE 65 0 1", "RANGE 66 0 1", "RANGE 67 0 7",
	"RANGE 68 40 99", "RANGE 69 0 1", "RANGE 70 0 2", "RANGE 71 0 1", "RANGE 72 0 16000",
	"RANGE 73 0 16000", "RANGE 74 0 3", "RANGE 75 0 1", "RANGE 76 -24 6", "RANGE 77 0 1",
	"RANGE 78 0 1", "RANGE 79 1 4", "RANGE 80 0 2", "RANGE 81 0 1", "RANGE 82 0 3",
	"RANGE 83 0 1",
};

static void settings_line(char *tok[], int n, void *ctx)
{
	(void)ctx;
	if (strcmp(tok[0], "DEFAULT") == 0 && n >= 3) {
		CHECK_EQ(opensc2_setting_default((uint8_t)atoi(tok[1])), atoi(tok[2]));
	} else if (strcmp(tok[0], "RANGE") == 0 && n >= 4) {
		uint8_t number = (uint8_t)atoi(tok[1]);
		int lo = atoi(tok[2]), hi = atoi(tok[3]);

		CHECK_EQ(opensc2_setting_clamp(number, (int16_t)lo), lo);
		CHECK_EQ(opensc2_setting_clamp(number, (int16_t)hi), hi);
		CHECK_EQ(opensc2_setting_clamp(number, -32768), lo);
		CHECK_EQ(opensc2_setting_clamp(number, 32767), hi);
		if (lo > -32768) {
			CHECK_EQ(opensc2_setting_clamp(number, (int16_t)(lo - 1)), lo);
		}
		if (hi < 32767) {
			CHECK_EQ(opensc2_setting_clamp(number, (int16_t)(hi + 1)), hi);
		}
	} else {
		CHECK(false);   /* an unknown settings record */
	}
}

OPENSC2_TEST(settings_vectors)
{
	int listed[OPENSC2_SETTINGS_COUNT] = { 0 };

	vec_run_builtin(builtin_settings, sizeof(builtin_settings) / sizeof(builtin_settings[0]),
			settings_line, NULL);
	(void)vec_run_file("settings.txt", settings_line, NULL);
	/* Every number the table does not list defaults to 0 and is not clamped. */
	for (size_t i = 0; i < sizeof(builtin_settings) / sizeof(builtin_settings[0]); i++) {
		int number;

		if (sscanf(builtin_settings[i], "DEFAULT %d", &number) == 1 && number >= 0 &&
		    number < OPENSC2_SETTINGS_COUNT) {
			listed[number] |= 1;
		}
		if (sscanf(builtin_settings[i], "RANGE %d", &number) == 1 && number >= 0 &&
		    number < OPENSC2_SETTINGS_COUNT) {
			listed[number] |= 2;
		}
	}
	for (int number = 0; number < OPENSC2_SETTINGS_COUNT; number++) {
		if (!(listed[number] & 1)) {
			CHECK_EQ(opensc2_setting_default((uint8_t)number), 0);
		}
		if (!(listed[number] & 2)) {
			CHECK_EQ(opensc2_setting_clamp((uint8_t)number, -32768), -32768);
			CHECK_EQ(opensc2_setting_clamp((uint8_t)number, 32767), 32767);
		}
	}
}
