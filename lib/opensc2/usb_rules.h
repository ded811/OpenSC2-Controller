/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * usb_rules.h: when the wired link builds and sends input reports, and the charger test build's
 * rule for a wired boot that no host ever talks to. Pure logic, no kernel (the host tests build
 * it).
 */
#ifndef OPENSC2_USB_RULES_H_
#define OPENSC2_USB_RULES_H_

#include <stdbool.h>
#include <opensc2/opensc2.h>

/* What the wired link looks like at a report tick or a send. A suspended bus and full buffers do
 * not count here: as on the real controller, reports are built and handed to the stack whatever
 * the bus does (they wait in the eight buffers for a resume), and one that finds no buffer free
 * is lost, its sequence number with it. */
struct osc2_usb_view {
	bool configured;     /* the HID interface is configured (the class's interface-ready event) */
	bool detaching;      /* on the way to a reboot */
};

/* What one tick of the report timer does. */
enum osc2_usb_tick {
	OSC2_USB_TICK_IDLE,        /* not configured, or leaving: nothing, nothing counted */
	OSC2_USB_TICK_SEND,        /* build the state report (a sequence number) and send it */
};

enum osc2_usb_tick osc2_usb_tick_action(const struct osc2_usb_view *v);

/* May a report go to the HID class now? 0, or -ENOTCONN (not configured, or leaving). */
int osc2_usb_send_gate(const struct osc2_usb_view *v);

/* ---- the charger test build: no host, then wireless ---------------------------------------------
 * A temporary testing aid for a board with no buttons wired, NOT the real controller's behaviour
 * and not in a default build (CONFIG_OPENSC2_NO_HOST_WIRELESS_MS, 0 by default). The real
 * controller, booted on USB power, waits until a host sets it up or STEAM is pressed, so on a
 * USB charger a board without a STEAM button would wait for good. With the option, a board wired
 * by the boot rule that no host has talked to within the time reboots into wireless mode and
 * connects through the puck while it charges.
 *
 * "A host has talked" means a control request reached the board (a setup packet), or the board was
 * configured. A PC resets the bus and sends its first request within a second of the attach; a
 * charger, a power bank, a powered hub on a PC that is off, and a PC that is asleep (or still
 * booting without touching USB) send none. A bus reset alone does not count, so that a reset the
 * USB controller might report before its pull-up is on cannot hide a charger.
 */
struct osc2_no_host_view {
	uint32_t timeout_ms;               /* CONFIG_OPENSC2_NO_HOST_WIRELESS_MS; 0: the option is off */
	enum opensc2_mode_reason reason;   /* why this boot is wired */
	uint32_t waited_ms;                /* since the device attached */
	bool host_seen;                    /* a setup packet, or a configuration, since the attach */
	bool leaving;                      /* a reboot is already on its way */
};

enum osc2_no_host {
	OSC2_NO_HOST_OFF,       /* wait for good, as the real controller: the option is off, this
				 * boot was asked to be wired (the application, "reboot wired"), or
				 * the board is leaving anyway */
	OSC2_NO_HOST_HOST,      /* a host has talked: it sets the board up in its own time */
	OSC2_NO_HOST_WAIT,      /* nobody yet, and time left */
	OSC2_NO_HOST_WIRELESS,  /* nobody within the time: reboot into wireless mode */
};

/* Only a boot wired by the boot rule (OPENSC2_MODE_USB_POWER) falls back. */
enum osc2_no_host osc2_no_host_rule(const struct osc2_no_host_view *v);

#endif /* OPENSC2_USB_RULES_H_ */
