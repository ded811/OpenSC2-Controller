/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The library's test cases as a plain C program, for any PC compiler:
 *   - tests/codec/src/cases.h and cases_usb.h: the pure parts (wire formats, USB descriptors and
 *     framing, lizard mode, the mode rules, the retained area, settings, fusion), also a ztest
 *     suite;
 *   - commands_cases.h (here): the vendor command channel, commands.c built against the small
 *     stand-ins in shim/ and stubs.c;
 *   - proto_cases.h: the vendor commands' answer rules (proto_rules.c) and their answers;
 *   - keys_cases.h: the settings keys of 0xED..0xF0 (key_rules.c) and the values kept for them;
 *   - crash_cases.h: the crash record (crash_rules.c) and 0xF2 selector 1;
 *   - vectors_cases.h: the hardware test suite's shared vectors through the library's own code;
 *   - differential_cases.h: the unarmed command channel, request by request, into a transcript;
 *   - hooks_cases.h, only in the binary built with CONFIG_OPENSC2_TEST_HOOKS: the hardware test
 *     hooks themselves (test_core.c, test_notes.c and the stand-in glue of hooks_stub.c).
 * run.sh and run.ps1 next to this file build it twice, without and with the hooks, run both, and
 * compare the two transcripts.
 *
 *   run_tests [--vectors <folder of the shared vectors>] [--transcript <file to write>]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TESTS 192

typedef void (*test_fn)(void);
static struct {
	const char *name;
	test_fn fn;
} tests[MAX_TESTS];
static int test_count, checks, failures, current_failed;
static const char *vectors_dir;       /* --vectors */
static const char *transcript_path;   /* --transcript */

static void register_test(const char *name, test_fn fn)
{
	if (test_count >= MAX_TESTS) {
		/* A case that does not fit would never run, and the suite would pass without it. */
		fprintf(stderr, "Too many test cases: %s does not fit (MAX_TESTS %d)\n", name, MAX_TESTS);
		exit(1);
	}
	tests[test_count].name = name;
	tests[test_count].fn = fn;
	test_count++;
}

#define OPENSC2_TEST(name)                                                              \
	static void test_##name(void);                                                  \
	__attribute__((constructor)) static void register_##name(void)                  \
	{                                                                               \
		register_test(#name, test_##name);                                      \
	}                                                                               \
	static void test_##name(void)

#define CHECK(cond)                                                                     \
	do {                                                                            \
		checks++;                                                               \
		if (!(cond)) {                                                          \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
			current_failed = 1;                                             \
		}                                                                       \
	} while (0)

#define CHECK_EQ(a, b)                                                                  \
	do {                                                                            \
		long long a_ = (long long)(a), b_ = (long long)(b);                     \
		checks++;                                                               \
		if (a_ != b_) {                                                         \
			printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,      \
			       __LINE__, #a, #b, a_, b_);                               \
			current_failed = 1;                                             \
		}                                                                       \
	} while (0)

#include "../codec/src/cases.h"
#include "../codec/src/cases_usb.h"
#include "commands_cases.h"
#include "proto_cases.h"
#include "keys_cases.h"
#include "crash_cases.h"
#include "transport_cases.h"
#include "ble_cases.h"
#include "vectors_cases.h"
#include "differential_cases.h"
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
#include "hooks_cases.h"
#include "ble_hooks_cases.h"
#endif

int main(int argc, char **argv)
{
	for (int i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "--vectors") == 0) {
			vectors_dir = argv[i + 1];
		} else if (strcmp(argv[i], "--transcript") == 0) {
			transcript_path = argv[i + 1];
		} else {
			fprintf(stderr, "usage: %s [--vectors <folder>] [--transcript <file>]\n", argv[0]);
			return 2;
		}
	}
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	printf("binary: hooks (CONFIG_OPENSC2_TEST_HOOKS)\n");
#else
	printf("binary: plain\n");
#endif
	for (int i = 0; i < test_count; i++) {
		current_failed = 0;
		tests[i].fn();
		printf("%s %s\n", current_failed ? "FAIL" : "PASS", tests[i].name);
		failures += current_failed;
	}
	printf("%d tests, %d checks, %d failed\n", test_count, checks, failures);
	return failures ? 1 : 0;
}
