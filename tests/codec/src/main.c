/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The pure test cases (cases.h, cases_usb.h, cases_input.h, cases_dock.h) as a Zephyr ztest suite.
 */
#include <zephyr/ztest.h>

#define OPENSC2_TEST(name) ZTEST(opensc2_codec, test_##name)
#define CHECK(cond) zassert_true(cond, "%s", #cond)
#define CHECK_EQ(a, b)                                                                  \
	zassert_equal((long long)(a), (long long)(b), "%s == %s (%lld != %lld)", #a, #b,  \
		      (long long)(a), (long long)(b))

#include "cases.h"
#include "cases_usb.h"
#include "cases_input.h"
#include "cases_dock.h"

ZTEST_SUITE(opensc2_codec, NULL, NULL, NULL, NULL, NULL);
