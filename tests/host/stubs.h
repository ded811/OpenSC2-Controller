/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: what the stand-ins in stubs.c recorded.
 */
#ifndef OSC2_HOST_STUBS_H_
#define OSC2_HOST_STUBS_H_

#include "internal.h"
#include "crash.h"
#include "keys.h"

struct stub_record {
	int64_t now_ms;
	bool link_active[OSC2_LINK_COUNT];
	uint32_t radio_events;
	uint32_t saved_bonds;           /* bit per slot */
	int saved_transport, deleted_transport, saved_chime;
	uint8_t stored_transport;       /* the last transport stored (OSC2_TRANSPORT_NONE: none) */
	bool ble_running;               /* the running wireless transport is Bluetooth */
	uint32_t ble_interval_us;       /* the connection interval while Bluetooth is active */
	int led_notified;
	int power_requests;
	enum opensc2_power_request power;
	uint32_t power_delay_ms;
	const char *power_why;
	int defaults_restored;
	int watchdog_armed, watchdog_cancelled;
	char lizard[32];                /* E enter, L leave, U switches up, D switches down */
	unsigned int lizard_calls;
	/* keys.c's blob values, and the keys saved and deleted (a bit per enum osc2_key_id) */
	struct osc2_blob_store blobs;
	uint32_t key_saves, key_deletes;
	int key_factory_resets;
};

extern struct stub_record stub;

/* Back to boot: defaults, a test identity, empty bonds, every stored request reset (and, in the
 * binary with the test hooks, the hooks unarmed and their record cleared). */
void stub_reset(void);

#if defined(CONFIG_OPENSC2_TEST_HOOKS)
/* hooks_stub.c */
void hooks_reset(void);
#endif

#endif /* OSC2_HOST_STUBS_H_ */
