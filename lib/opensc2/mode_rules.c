/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * mode_rules.c: wired or wireless at boot, as the real controller decides it (a boot with USB power
 * is wired unless the warm-restart mark is set), with the board's own overrides on top (the
 * application, a stored policy, a one-time "wired" mark); and the real controller's rule for USB
 * power that appears while wireless (link down or STEAM held: back wired; link up: charge only).
 * Pure logic.
 */
#include "mode_rules.h"

enum opensc2_mode osc2_mode_decide(const struct osc2_mode_inputs *in,
				   enum opensc2_mode_reason *reason)
{
	if (!in->usb_supported) {
		*reason = OPENSC2_MODE_NO_USB;
		return OPENSC2_MODE_WIRELESS;
	}
	if (in->app == OPENSC2_BOOT_WIRELESS) {
		*reason = OPENSC2_MODE_APP;
		return OPENSC2_MODE_WIRELESS;
	}
	if (in->app == OPENSC2_BOOT_WIRED) {
		*reason = OPENSC2_MODE_APP;
		return OPENSC2_MODE_WIRED;
	}
	if (!in->usb_power) {
		/* On battery a mark does not matter, as on the real controller. */
		*reason = OPENSC2_MODE_NO_USB_POWER;
		return OPENSC2_MODE_WIRELESS;
	}
	if (in->mark == OSC2_MARK_WIRELESS) {
		*reason = OPENSC2_MODE_WARM_RESTART;
		return OPENSC2_MODE_WIRELESS;
	}
	if (in->mark == OSC2_MARK_WIRED) {
		*reason = OPENSC2_MODE_WIRED_RESTART;
		return OPENSC2_MODE_WIRED;
	}
	if (in->policy == OPENSC2_POLICY_WIRELESS) {
		*reason = OPENSC2_MODE_POLICY;
		return OPENSC2_MODE_WIRELESS;
	}
	*reason = OPENSC2_MODE_USB_POWER;
	return OPENSC2_MODE_WIRED;
}

enum osc2_mark osc2_mark_after_chord(enum osc2_mark mark, bool transport_chord_held)
{
	return mark == OSC2_MARK_WIRELESS && transport_chord_held ? OSC2_MARK_NONE : mark;
}

enum osc2_plug_in osc2_plug_in_rule(enum opensc2_policy policy, bool link_up, bool steam_held)
{
	if (policy != OPENSC2_POLICY_AUTO) {
		return OSC2_PLUG_IN_IGNORE;
	}
	if (!link_up) {
		return OSC2_PLUG_IN_LINK_DOWN;
	}
	return steam_held ? OSC2_PLUG_IN_STEAM : OSC2_PLUG_IN_CHARGE;
}

uint32_t osc2_mark_word(enum osc2_mark mark)
{
	switch (mark) {
	case OSC2_MARK_WIRELESS: return OSC2_MARK_WORD_WIRELESS;
	case OSC2_MARK_WIRED:    return OSC2_MARK_WORD_WIRED;
	default:                 return 0;
	}
}

enum osc2_mark osc2_mark_from_words(uint32_t word, uint32_t inverse)
{
	if (word != (uint32_t)~inverse) {
		return OSC2_MARK_NONE;
	}
	if (word == OSC2_MARK_WORD_WIRELESS) {
		return OSC2_MARK_WIRELESS;
	}
	if (word == OSC2_MARK_WORD_WIRED) {
		return OSC2_MARK_WIRED;
	}
	return OSC2_MARK_NONE;
}

uint8_t osc2_mark_byte(enum osc2_mark mark)
{
	switch (mark) {
	case OSC2_MARK_WIRELESS: return OSC2_MARK_BYTE_WIRELESS;
	case OSC2_MARK_WIRED:    return OSC2_MARK_BYTE_WIRED;
	default:                 return 0;
	}
}

enum osc2_mark osc2_mark_from_byte(uint8_t b)
{
	return b == OSC2_MARK_BYTE_WIRELESS ? OSC2_MARK_WIRELESS :
	       b == OSC2_MARK_BYTE_WIRED ? OSC2_MARK_WIRED : OSC2_MARK_NONE;
}
