/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * report_queue.c: the radio link's queue of extra input reports (report_queue.h). Pure logic.
 */
#include <errno.h>
#include <string.h>
#include "report_queue.h"

void osc2_rq_init(struct osc2_rq *q, struct osc2_rq_item *items, uint8_t cap, uint8_t depth)
{
	q->items = items;
	q->cap = cap;
	q->depth = depth < cap ? depth : cap;
	q->head = 0;
	q->count = 0;
}

int osc2_rq_put(struct osc2_rq *q, const uint8_t *report, uint8_t len, bool droppable)
{
	struct osc2_rq_item *it;

	if (len == 0 || len > OSC2_RQ_REPORT_MAX) {
		return -EINVAL;
	}
	if (q->count >= q->cap || (droppable && q->count >= q->depth)) {
		return -ENOMEM;
	}
	it = &q->items[(q->head + q->count) % q->cap];
	it->len = len;
	memcpy(it->data, report, len);
	q->count++;
	return 0;
}

const struct osc2_rq_item *osc2_rq_peek(const struct osc2_rq *q, uint8_t index)
{
	if (index >= q->count) {
		return NULL;
	}
	return &q->items[(q->head + index) % q->cap];
}

void osc2_rq_take(struct osc2_rq *q, uint8_t n)
{
	if (n > q->count) {
		n = q->count;
	}
	if (q->cap) {
		q->head = (uint8_t)((q->head + n) % q->cap);
	}
	q->count = (uint8_t)(q->count - n);
}

void osc2_rq_purge(struct osc2_rq *q)
{
	q->head = 0;
	q->count = 0;
}

bool osc2_rq_backlog(const struct osc2_rq *q)
{
	return q->count != 0 && q->count >= q->depth;
}
