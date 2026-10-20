/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * hwdt.h: the hardware watchdog (hwdt.c; the host tests have a stand-in). Not a public interface.
 */
#ifndef OPENSC2_HWDT_H_
#define OPENSC2_HWDT_H_

#include <stdbool.h>
#include <stdint.h>

struct osc2_hwdt_info {
	bool running;        /* started (it cannot be stopped) */
	bool starving;       /* the test hooks stopped the feeds */
	uint32_t feeds;      /* since boot */
	uint16_t timeout_ms; /* 5000 */
};

/* opensc2_init(): the watchdog on, and its feeder (CONFIG_OPENSC2_WATCHDOG; otherwise nothing). */
int osc2_hwdt_start(void);
/* The test hooks: no more feeds. `hardware`: the 4.5 s warning only logs, so the hardware watchdog
 * resets after 5 s without a crash record; otherwise the warning crashes first. -ENOTSUP without
 * the watchdog, -ENODEV when it did not start, -EBUSY while already starving. */
int osc2_hwdt_starve(bool hardware);
void osc2_hwdt_info(struct osc2_hwdt_info *info);

#endif /* OPENSC2_HWDT_H_ */
