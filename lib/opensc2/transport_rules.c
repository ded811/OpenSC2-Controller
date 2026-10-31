/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * transport_rules.c: the stored wireless transport (transport_rules.h). Pure logic.
 */
#include <string.h>
#include "transport_rules.h"

bool osc2_transport_usable(uint8_t t, bool ble)
{
	return osc2_transport_is_puck(t) || (ble && osc2_transport_is_ble(t));
}

int osc2_transport_puck_slot(uint8_t t)
{
	return osc2_transport_is_puck(t) ? t - OSC2_TRANSPORT_PUCK_1 : -1;
}

int osc2_transport_ble_slot(uint8_t t)
{
	return osc2_transport_is_ble(t) ? t - OSC2_TRANSPORT_BLE_1 : -1;
}

uint8_t osc2_transport_runs(uint8_t value, bool ble)
{
	if (osc2_transport_is_ble(value)) {
		return ble ? value : OSC2_TRANSPORT_PUCK_1;
	}
	return value == OSC2_TRANSPORT_PUCK_2 ? OSC2_TRANSPORT_PUCK_2 : OSC2_TRANSPORT_PUCK_1;
}

uint8_t osc2_transport_at_boot(bool stored, uint8_t value, int legacy_slot)
{
	if (stored) {
		return value;
	}
	if (legacy_slot == 0 || legacy_slot == 1) {
		return (uint8_t)(OSC2_TRANSPORT_PUCK_1 + legacy_slot);
	}
	return OSC2_TRANSPORT_DEFAULT;
}

bool osc2_transport_save_due(uint8_t up, bool stored, uint8_t saved)
{
	return !stored || up != saved;
}

const char *osc2_transport_name(uint8_t t)
{
	static const char *const names[] = { "Bluetooth 1", "Bluetooth 2", "puck 1", "puck 2" };

	return t <= OSC2_TRANSPORT_PUCK_2 ? names[t] : "?";
}

static const char *const words[] = { "ble", "ble2", "puck", "puck2" };

const char *osc2_transport_word(uint8_t t)
{
	return t <= OSC2_TRANSPORT_PUCK_2 ? words[t] : "?";
}

int osc2_transport_from_word(const char *word)
{
	for (int t = 0; t <= OSC2_TRANSPORT_PUCK_2; t++) {
		if (strcmp(word, words[t]) == 0) {
			return t;
		}
	}
	return -1;
}
