/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only (the binary built with CONFIG_OPENSC2_TEST_HOOKS): what the hardware test
 * hooks' fake environment in hooks_stub.c recorded.
 */
#ifndef OSC2_HOST_HOOKS_STUB_H_
#define OSC2_HOST_HOOKS_STUB_H_

#include "test_hooks.h"

struct hooks_record {
	uint32_t extra_us;              /* the test clock: stub.now_ms * 1000 + extra_us */
	int lock_depth, req_depth;
	int violations;                 /* a lock taken twice, or the library called with it held */
	uint32_t counters[OSC2_TEST_CNT_COUNT];
	uint8_t ctx_link;
	int deadline_calls;
	/* calls into the library */
	int host_set_calls;
	uint8_t host_set_last[63];
	uint8_t host_set_len;
	int host_out_calls;
	int app_send_calls;
	uint8_t app_send_last[64];
	uint8_t app_send_len;
	int32_t app_send_rc;
	int app_haptic_calls;
	uint8_t app_haptic_channel;
	uint32_t app_haptic_flags;
	bool radio_connected;
	int battery_now_calls;
	int release_calls;
	bool release_result;
	bool guard_at_release;
	int resync_calls;
	uint8_t link_state, usb_state;
	/* the order of release (R), resync (S), setting writes (W) and LED writes (C) */
	char order[96];
	unsigned int order_len;
	uint8_t written[OPENSC2_SETTINGS_COUNT];   /* setting numbers in the order written */
	unsigned int written_count;
	/* The firmware emulator (fwemu.c) has lizard mode itself: these do report.c's release and
	 * resync on it (NULL in the host tests, which record the calls only). */
	bool (*release_hook)(void);
	void (*resync_hook)(void);
	/* ... and report.c's osc2_output_dispatch() for HOST_OUT (the rumble timeout too) */
	int (*output_hook)(uint8_t link, const uint8_t *report, uint8_t len);
	/* The Bluetooth ops (BLE_STATUS, BLE_DROP, BLE_WINDOW): a stand-in for ble.c's; NULL is a
	 * build without Bluetooth (BAD_OP). */
	uint8_t (*ble_hook)(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			    uint8_t *rlen);
	int ble_op_calls;
};

extern struct hooks_record hk;

#endif /* OSC2_HOST_HOOKS_STUB_H_ */
