/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The codec test cases (tests/codec/src/cases.h) as a plain C program, for any PC compiler:
 *   gcc -std=c11 -Wall -Wextra -Werror -I../../include -I../../lib/opensc2 run_tests.c \
 *       ../../lib/opensc2/codec.c ../../lib/opensc2/lizard.c \
 *       ../../lib/opensc2/settings_defaults.c ../../lib/opensc2/fusion.c -lm -o run_tests
 * (run.sh and run.ps1 next to this file do exactly that).
 */
#include <stdio.h>

#define MAX_TESTS 64

typedef void (*test_fn)(void);
static struct {
	const char *name;
	test_fn fn;
} tests[MAX_TESTS];
static int test_count, checks, failures, current_failed;

static void register_test(const char *name, test_fn fn)
{
	if (test_count < MAX_TESTS) {
		tests[test_count].name = name;
		tests[test_count].fn = fn;
		test_count++;
	}
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

int main(void)
{
	for (int i = 0; i < test_count; i++) {
		current_failed = 0;
		tests[i].fn();
		printf("%s %s\n", current_failed ? "FAIL" : "PASS", tests[i].name);
		failures += current_failed;
	}
	printf("%d tests, %d checks, %d failed\n", test_count, checks, failures);
	return failures ? 1 : 0;
}
