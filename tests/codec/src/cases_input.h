/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Test cases for the radio's queue of extra reports (report_queue.c). Written once, run two
 * ways, like cases.h: as a Zephyr ztest suite (tests/codec) and as a plain C program on a PC
 * (tests/host). The including file defines OPENSC2_TEST(name), CHECK(cond) and CHECK_EQ(a, b).
 *
 * The expected values follow the real controller's behaviour as the protocol documentation (not
 * public yet; docs/protocol.md lists its pages) describes it, worked out by hand.
 */
#include <errno.h>
#include <string.h>
#include <opensc2/codec.h>
#include <opensc2/settings.h>
#include "report_queue.h"

/* ---- the radio's queue of extra reports ------------------------------------------------------ */

OPENSC2_TEST(report_queue)
{
	struct osc2_rq_item items[5];
	struct osc2_rq q;
	uint8_t r[64] = { 0x40 };

	osc2_rq_init(&q, items, 5, 3);
	CHECK(!osc2_rq_backlog(&q));
	CHECK_EQ(osc2_rq_put(&q, r, 0, false), -EINVAL);
	CHECK_EQ(osc2_rq_put(&q, r, 65, false), -EINVAL);
	/* Up to the depth everything queues. */
	for (uint8_t i = 1; i <= 3; i++) {
		r[1] = i;
		CHECK_EQ(osc2_rq_put(&q, r, 6, i == 2), 0);
	}
	CHECK(osc2_rq_backlog(&q));
	/* Past it a droppable report is dropped; the others wait behind, up to the capacity. */
	r[1] = 9;
	CHECK_EQ(osc2_rq_put(&q, r, 15, true), -ENOMEM);
	r[1] = 4;
	CHECK_EQ(osc2_rq_put(&q, r, 9, false), 0);
	r[1] = 5;
	CHECK_EQ(osc2_rq_put(&q, r, 6, false), 0);
	r[1] = 6;
	CHECK_EQ(osc2_rq_put(&q, r, 6, false), -ENOMEM);
	CHECK_EQ(q.count, 5);
	/* In order, oldest first. */
	for (uint8_t i = 0; i < 5; i++) {
		CHECK_EQ(osc2_rq_peek(&q, i)->data[1], i + 1);
	}
	CHECK_EQ(osc2_rq_peek(&q, 3)->len, 9);
	CHECK(osc2_rq_peek(&q, 5) == NULL);
	/* Two went out: the rest moves up, still in order; three left is still a backlog. */
	osc2_rq_take(&q, 2);
	CHECK_EQ(osc2_rq_peek(&q, 0)->data[1], 3);
	CHECK(osc2_rq_backlog(&q));
	osc2_rq_take(&q, 1);
	CHECK(!osc2_rq_backlog(&q));
	/* Around the end of the storage. */
	for (uint8_t i = 6; i <= 8; i++) {
		r[1] = i;
		CHECK_EQ(osc2_rq_put(&q, r, 6, false), 0);
	}
	CHECK_EQ(osc2_rq_peek(&q, 0)->data[1], 4);
	CHECK_EQ(osc2_rq_peek(&q, 4)->data[1], 8);
	osc2_rq_take(&q, 9);   /* more than there are: all */
	CHECK_EQ(q.count, 0);
	CHECK(!osc2_rq_backlog(&q));
	CHECK_EQ(osc2_rq_put(&q, r, 6, true), 0);
	osc2_rq_purge(&q);
	CHECK(osc2_rq_peek(&q, 0) == NULL);
	/* A depth of 0: lossless reports only, and any of them is a backlog. */
	osc2_rq_init(&q, items, 2, 0);
	CHECK_EQ(osc2_rq_put(&q, r, 6, true), -ENOMEM);
	CHECK_EQ(osc2_rq_put(&q, r, 6, false), 0);
	CHECK(osc2_rq_backlog(&q));
}
