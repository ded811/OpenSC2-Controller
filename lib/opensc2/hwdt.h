/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * hwdt.h: the hardware watchdog (hwdt.c). Not a public interface.
 */
#ifndef OPENSC2_HWDT_H_
#define OPENSC2_HWDT_H_

/* opensc2_init(): the watchdog on, and its feeder (CONFIG_OPENSC2_WATCHDOG; otherwise nothing).
 * 0, or a negative errno when the watchdog could not start (the firmware runs on without it). */
int osc2_hwdt_start(void);

#endif /* OPENSC2_HWDT_H_ */
