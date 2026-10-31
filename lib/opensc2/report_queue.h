/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * report_queue.h: the input reports that ride behind the state report in the radio link's
 * acknowledgements (radio.c): lizard mode's, the battery report and the application's, in the
 * order they were sent. Pure logic (radio.c guards it with a lock), so the host tests run it.
 *
 * The real controller queues `depth` (5) such reports and drops one that finds the queue full
 * (cap == depth, the default). With room beyond `depth` (CONFIG_OPENSC2_REPORT_WAIT_DEPTH,
 * OpenSC2's option) only the ones that must not be lost (lizard mode's and the application's) are
 * taken past `depth`, behind the others, while a droppable one (the battery report) is dropped;
 * whoever can wait (lizard mode) waits while `depth` or more are queued.
 */
#ifndef OPENSC2_REPORT_QUEUE_H_
#define OPENSC2_REPORT_QUEUE_H_

#include <stdbool.h>
#include <stdint.h>

#define OSC2_RQ_REPORT_MAX 64

struct osc2_rq_item {
	uint8_t len;
	uint8_t data[OSC2_RQ_REPORT_MAX];
};

struct osc2_rq {
	struct osc2_rq_item *items;   /* `cap` of them, the caller's */
	uint8_t cap;
	uint8_t depth;                /* reports that queue before only lossless ones are taken */
	uint8_t head;                 /* the oldest */
	uint8_t count;
};

void osc2_rq_init(struct osc2_rq *q, struct osc2_rq_item *items, uint8_t cap, uint8_t depth);
/* Add a report at the end: 0, or -EINVAL (empty, or longer than 64 bytes), or -ENOMEM when it is
 * dropped: a droppable report that finds `depth` or more queued, any report that finds the queue
 * full. */
int osc2_rq_put(struct osc2_rq *q, const uint8_t *report, uint8_t len, bool droppable);
/* The `index`-th report from the oldest, or NULL past the end. */
const struct osc2_rq_item *osc2_rq_peek(const struct osc2_rq *q, uint8_t index);
/* The oldest `n` reports went out. */
void osc2_rq_take(struct osc2_rq *q, uint8_t n);
void osc2_rq_purge(struct osc2_rq *q);
/* `depth` or more reports queued (and at least one): lizard mode waits for room before its next
 * frame. */
bool osc2_rq_backlog(const struct osc2_rq *q);

#endif /* OPENSC2_REPORT_QUEUE_H_ */
