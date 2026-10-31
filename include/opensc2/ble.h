/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Bluetooth LE's state for the application (CONFIG_OPENSC2_BLE): what a custom board may want to
 * show or act on beyond opensc2_link_info(), such as which identities hold a bond, the ATT MTU, the
 * connection interval and how much of the advertising window is left. Bluetooth itself is started,
 * stopped and opened for pairing with the transport functions of opensc2.h
 * (opensc2_select_transport(), opensc2_select_transport_pairing(), opensc2_start(),
 * opensc2_stop(), opensc2_pair_start()); its name and Device Information come from the identity
 * (opensc2_identity_store()) and Kconfig; its Battery Service level from the battery the
 * application reports. docs/api.md, Bluetooth LE, has every detail.
 */
#ifndef OPENSC2_BLE_H_
#define OPENSC2_BLE_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct opensc2_ble_status {
	bool enabled;           /* the Bluetooth stack is on */
	bool running;           /* Bluetooth is the running wireless transport */
	bool advertising;
	bool pairing_mode;      /* advertising open to any host (the identity has no bond) */
	bool connected;         /* a host is connected (the status LED's "connected") */
	bool active;            /* past the gate: encrypted, ATT MTU 64 or more; reports flow */
	bool wireless_off;      /* the window ran out or the host left, until the next start */
	uint8_t slot;           /* 0 or 1 (OPENSC2_TRANSPORT_BLE_1, _2) */
	uint8_t identity;       /* the identity in use: 1 or 2 for slot 0, 3 or 4 for slot 1 */
	uint8_t bonded;         /* bit n: identity n holds a host's bond */
	uint8_t security;       /* the link's security level (2: encrypted) */
	uint8_t last_reason;    /* the HCI reason of the last disconnect, 0 before any */
	uint16_t mtu;           /* the last ATT MTU heard of (kept across links, as on the real
				 * controller) */
	uint32_t interval_us;   /* the connection interval of the current (or last) link */
	uint32_t window_left_ms;   /* left of the advertising window, 0 when none runs */
	uint32_t reports;       /* state reports sent on this link */
	uint32_t dropped;       /* state reports that could not be sent */
	char name[32];          /* the device name (the board's, or one a host wrote over GAP) */
	char address[32];       /* the identity's own address, as text ("" while the stack is off) */
};

#if defined(CONFIG_OPENSC2_BLE)
/* Bluetooth's state now. 0, or -EACCES before opensc2_init(). Any thread. */
int opensc2_ble_status(struct opensc2_ble_status *status);
#else
static inline int opensc2_ble_status(struct opensc2_ble_status *status)
{
	(void)status;
	return -ENOTSUP;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_BLE_H_ */
