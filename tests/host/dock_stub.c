/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: what the dock link (dock_link.c) needs from the kernel, which dock.c has in the
 * firmware: a state report built now (GET input with the dock alone) and a microsecond clock. The
 * firmware emulator (fwemu.c) builds its reports itself and sets `osc2_dock_stub_sampler`; the
 * host tests count the calls and build a report with a fixed body.
 */
#include <string.h>
#include "internal.h"
#include "dock_link.h"
#include "dock_stub.h"
#include "stubs.h"

struct dock_stub_record dock_stub;
void (*osc2_dock_stub_sampler)(uint8_t report[OPENSC2_STATE_REPORT_LEN]);

void osc2_dock_sample(uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	dock_stub.samples++;
	if (osc2_dock_stub_sampler) {
		osc2_dock_stub_sampler(report);
		return;
	}
	/* As report.c's osc2_report_state(): a report built, its copy left with the dock. */
	memset(report, 0, OPENSC2_STATE_REPORT_LEN);
	report[0] = OPENSC2_RPT_STATE;
	report[1] = dock_stub.sequence++;
	report[2] = 0xD0;   /* a body to tell it from the copy */
	osc2_dock_cache_report(report);
}

uint32_t osc2_dock_now_us(void)
{
	uint32_t now = (uint32_t)(stub.now_ms * 1000) + dock_stub.extra_us;

	/* The dock link reads the clock before and after a request: step_us is its handling time. */
	dock_stub.extra_us += dock_stub.step_us;
	return now;
}
