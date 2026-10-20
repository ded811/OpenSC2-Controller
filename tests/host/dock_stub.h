/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: what dock_stub.c recorded.
 */
#ifndef OSC2_HOST_DOCK_STUB_H_
#define OSC2_HOST_DOCK_STUB_H_

#include <stdint.h>
#include <opensc2/codec.h>

struct dock_stub_record {
	int samples;        /* osc2_dock_sample() calls */
	uint8_t sequence;   /* of the reports it built */
	uint32_t extra_us;  /* added to the clock */
};

extern struct dock_stub_record dock_stub;
/* The firmware emulator's own report builder, or NULL. */
extern void (*osc2_dock_stub_sampler)(uint8_t report[OPENSC2_STATE_REPORT_LEN]);

#endif /* OSC2_HOST_DOCK_STUB_H_ */
