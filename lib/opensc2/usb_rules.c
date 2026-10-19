/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * usb_rules.c: when the wired link builds and sends input reports. State reports flow from the
 * moment the host configures the interface; they are dropped only while the bus is suspended or
 * every buffer still waits for the host, and never wait themselves. And the charger test build's
 * rule (a testing aid, not the real controller's behaviour): no host, then wireless. Pure logic.
 */
#include <errno.h>
#include "usb_rules.h"

enum osc2_usb_tick osc2_usb_tick_action(const struct osc2_usb_view *v)
{
	if (!v->configured || v->detaching) {
		return OSC2_USB_TICK_IDLE;
	}
	if (v->suspended) {
		return OSC2_USB_TICK_SUSPENDED;
	}
	if (v->buffers_full) {
		return OSC2_USB_TICK_BUSY;
	}
	return OSC2_USB_TICK_SEND;
}

int osc2_usb_send_gate(const struct osc2_usb_view *v)
{
	if (!v->configured || v->detaching) {
		return -ENOTCONN;
	}
	if (v->suspended) {
		return -EAGAIN;
	}
	return 0;
}

enum osc2_no_host osc2_no_host_rule(const struct osc2_no_host_view *v)
{
	if (v->timeout_ms == 0 || v->reason != OPENSC2_MODE_USB_POWER || v->leaving) {
		return OSC2_NO_HOST_OFF;
	}
	if (v->host_seen) {
		return OSC2_NO_HOST_HOST;
	}
	return v->waited_ms >= v->timeout_ms ? OSC2_NO_HOST_WIRELESS : OSC2_NO_HOST_WAIT;
}
