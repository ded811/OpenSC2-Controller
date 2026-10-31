/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * transport_rules.h: the stored wireless transport ("user/wireless_transport" on the real
 * controller), as pure rules the host tests build: which values mean what, what a host writing the
 * key does, the choice at boot, and October's rule that a choice is saved only once its link is
 * up. No kernel.
 */
#ifndef OPENSC2_TRANSPORT_RULES_H_
#define OPENSC2_TRANSPORT_RULES_H_

#include <stdbool.h>
#include <stdint.h>

/* The values of user/wireless_transport, as the real controller numbers them (the protocol
 * documentation, not public yet; docs/protocol.md lists the pages). The same numbers are
 * enum opensc2_transport. */
#define OSC2_TRANSPORT_BLE_1     0   /* Bluetooth LE, slot 1 */
#define OSC2_TRANSPORT_BLE_2     1   /* Bluetooth LE, slot 2 */
#define OSC2_TRANSPORT_PUCK_1    2   /* the puck with bond 1: the default */
#define OSC2_TRANSPORT_PUCK_2    3   /* the puck with bond 2 */
#define OSC2_TRANSPORT_DEFAULT   OSC2_TRANSPORT_PUCK_1
#define OSC2_TRANSPORT_NONE      0xFF   /* no choice yet (the system state machine's) */

static inline bool osc2_transport_is_ble(uint8_t t)
{
	return t == OSC2_TRANSPORT_BLE_1 || t == OSC2_TRANSPORT_BLE_2;
}

static inline bool osc2_transport_is_puck(uint8_t t)
{
	return t == OSC2_TRANSPORT_PUCK_1 || t == OSC2_TRANSPORT_PUCK_2;
}

/* A transport this build can run: the puck's two always, Bluetooth's two with `ble`. */
bool osc2_transport_usable(uint8_t t, bool ble);

/* The puck's bond slot (0 or 1) of a puck transport, the Bluetooth slot (0 or 1) of a Bluetooth
 * one; -1 for the other kind. */
int osc2_transport_puck_slot(uint8_t t);
int osc2_transport_ble_slot(uint8_t t);

/* The transport a value of user/wireless_transport runs, as the real controller picks it: 0 and 1
 * Bluetooth slots 1 and 2, 3 the puck with bond 2, every other value (2, and anything a host
 * wrote with 0xEE) the puck with bond 1. Without Bluetooth (`ble` false) 0 and 1 run the puck
 * with bond 1 too. The key itself keeps the byte as written: 0xED reads it back. */
uint8_t osc2_transport_runs(uint8_t value, bool ble);

/* The value a boot starts with (the real controller's RAM value of the key): the stored one
 * (`stored`, any byte), else the puck slot an older firmware stored (opensc2/slot, `legacy_slot`
 * 0 or 1, -1 for none), else the default. */
uint8_t osc2_transport_at_boot(bool stored, uint8_t value, int legacy_slot);

/* October's rule: the running transport is saved whenever its link comes up (the real controller
 * writes it at every connection and reads it back into the key). A write is due unless it is the
 * stored value already. */
bool osc2_transport_save_due(uint8_t up, bool stored, uint8_t saved);

/* Text for logs and the shell: "puck 1", "puck 2", "Bluetooth 1", "Bluetooth 2", "?". */
const char *osc2_transport_name(uint8_t t);

/* The shell's word for a transport: "puck", "puck2", "ble", "ble2"; and back (-1 when unknown). */
const char *osc2_transport_word(uint8_t t);
int osc2_transport_from_word(const char *word);

#endif /* OPENSC2_TRANSPORT_RULES_H_ */
