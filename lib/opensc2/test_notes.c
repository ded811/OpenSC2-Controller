/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_notes.c: the hardware test hooks' calls from the library (test_hooks.h), on the core
 * (test_core.c) with the glue's instance and environment. Pure C: the same file runs in the
 * firmware and in the host tests. Every call returns at once while the hooks are not armed (the
 * guard and the held answers outlive arming a little, as test_core.h says).
 */
#include <string.h>
#include "test_hooks.h"

#define CORE (&osc2_test_core_obj)
#define ENV  (&osc2_test_env_obj)

/* ---- state reports, lizard mode, battery ---- */

bool osc2_test_divert(const uint8_t *report, size_t len)
{
	return osc2_test_core_guard(CORE) && osc2_test_core_divert(CORE, ENV, report, len);
}

void osc2_test_input(struct opensc2_state *s)
{
	if (!osc2_test_core_armed(CORE)) {
		CORE->skip_lizard = 0;
		return;
	}
	(void)osc2_test_core_input(CORE, ENV, ENV->lizard_on(), s);
}

void osc2_test_frame(const uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	if (osc2_test_core_armed(CORE)) {
		osc2_test_core_frame(CORE, ENV, report);
	}
}

bool osc2_test_skip_lizard(void)
{
	return osc2_test_core_skip_lizard(CORE);
}

void osc2_test_battery(struct opensc2_battery *b)
{
	if (osc2_test_core_armed(CORE)) {
		(void)osc2_test_core_battery(CORE, ENV, b);
	}
}

uint16_t osc2_test_take_deaf(void)
{
	return osc2_test_core_armed(CORE) ? osc2_test_core_take_deaf(CORE, ENV) : 0;
}

/* ---- the command channel ---- */

bool osc2_test_claim(uint8_t link, uint8_t opcode, const uint8_t *payload, uint8_t len)
{
	uint8_t previous;
	bool claimed;

	if (opcode != OSC2_TEST_OPCODE || !osc2_test_is_request(payload, len)) {
		return false;
	}
	previous = osc2_test_ctx_set(link);
	claimed = osc2_test_core_claim(CORE, ENV, link, payload, len);
	osc2_test_ctx_restore(previous);
	if (claimed) {
		osc2_test_deadline_changed();
	}
	return claimed;
}

bool osc2_test_answer(uint8_t link, uint8_t *out, uint8_t *out_len)
{
	return osc2_test_core_answer(CORE, ENV, link, out, out_len);
}

void osc2_test_stored(uint8_t link)
{
	if (link >= OSC2_TEST_LINKS ||
	    (!CORE->held[link].valid && !CORE->just_claimed[link])) {
		return;   /* nothing held on this link */
	}
	osc2_test_core_stored(CORE, ENV, link);
}

bool osc2_test_quiet(uint8_t link)
{
	if (link >= OSC2_TEST_LINKS || !CORE->quiet[link]) {
		return false;
	}
	return osc2_test_core_quiet(CORE, ENV, link);
}

uint8_t osc2_test_ctx_enter(uint8_t link)
{
	return osc2_test_core_armed(CORE) ? osc2_test_ctx_set(link) : OSC2_TEST_CTX_UNSET;
}

void osc2_test_ctx_leave(uint8_t previous)
{
	if (previous != OSC2_TEST_CTX_UNSET) {
		osc2_test_ctx_restore(previous);
	}
}

/* ---- counters ---- */

void osc2_test_count_n(uint8_t counter, uint32_t n)
{
	if (osc2_test_core_armed(CORE) && counter < OSC2_TEST_CNT_COUNT) {
		ENV->cnt_add(counter, n);
	}
}

/* ---- notes ---- */

static void note(uint8_t kind, uint8_t link, const uint8_t *data, uint8_t len)
{
	osc2_test_core_note(CORE, ENV, kind, link, data, len);
}

void osc2_test_note_led(const struct opensc2_led *led)
{
	uint8_t d[6];

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_LED, OSC2_TEST_LINK_CTX, d, osc2_test_enc_led(d, led));
	}
}

void osc2_test_note_setting(uint8_t number, int16_t value)
{
	uint8_t d[3];

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_SETTING, OSC2_TEST_LINK_CTX, d,
		     osc2_test_enc_setting(d, number, value));
	}
}

void osc2_test_note_power(uint8_t request, const char *why)
{
	uint8_t d[1 + OSC2_TEST_POWER_TEXT_MAX];

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_POWER, OSC2_TEST_LINK_CTX, d,
		     osc2_test_enc_power(d, request, why));
	}
}

void osc2_test_note_lizard(uint8_t what)
{
	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_LIZARD, OSC2_TEST_LINK_CTX, &what, 1);
	}
}

void osc2_test_note_output(uint8_t link, const uint8_t *report, size_t len, int decoded)
{
	uint8_t d[19];

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_OUTPUT, link, d, osc2_test_enc_output(d, report, len, decoded));
	}
}

void osc2_test_note_rumble(uint8_t link, const struct opensc2_rumble *rumble)
{
	uint8_t d[10];

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_RUMBLE, link, d, osc2_test_enc_rumble(d, rumble));
	}
}

void osc2_test_note_haptic(uint8_t link, const struct opensc2_haptic *haptic)
{
	uint8_t d[12];

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_HAPTIC, link, d, osc2_test_enc_haptic(d, haptic));
	}
}

void osc2_test_note_vendor(uint8_t link, bool get, uint8_t opcode, uint8_t n, int32_t result,
			   const uint8_t *payload, uint8_t len, size_t avail)
{
	uint8_t d[19];

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_VENDOR, link, d,
		     osc2_test_enc_vendor(d, get, opcode, n, result, payload, len, avail));
	}
}

void osc2_test_note_link(uint8_t state, uint8_t reason, uint8_t slot, uint8_t channel,
			 uint8_t protocol)
{
	uint8_t d[5] = { state, reason, slot, channel, protocol };

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_LINK, OSC2_TEST_LINK_RADIO, d, sizeof(d));
	}
}

void osc2_test_note_usb(const struct opensc2_usb_info *info)
{
	uint8_t d[3];

	if (osc2_test_core_armed(CORE)) {
		d[0] = (uint8_t)info->state;
		d[1] = info->active ? 1 : 0;
		d[2] = info->suspended ? 1 : 0;
		note(OSC2_TEST_KIND_USB, OSC2_TEST_LINK_USB, d, sizeof(d));
	}
}

void osc2_test_note_host_asleep(void)
{
	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_HOST_ASLEEP, OSC2_TEST_LINK_RADIO, NULL, 0);
	}
}

void osc2_test_note_version(uint8_t version)
{
	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_VERSION, OSC2_TEST_LINK_RADIO, &version, 1);
	}
}

void osc2_test_note_identity(const struct opensc2_identity *id)
{
	uint8_t d[26];

	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_IDENTITY, OSC2_TEST_LINK_RADIO, d, osc2_test_enc_identity(d, id));
	}
}

void osc2_test_note_channels(const uint8_t channels[4])
{
	if (osc2_test_core_armed(CORE)) {
		note(OSC2_TEST_KIND_CHANNELS, OSC2_TEST_LINK_RADIO, channels, 4);
	}
}

void osc2_test_note_status(uint8_t link, uint8_t channel, uint32_t flags)
{
	uint8_t d[5];

	if (osc2_test_core_armed(CORE)) {
		d[0] = channel;
		opensc2_put_le32(&d[1], flags);
		note(OSC2_TEST_KIND_STOP_STATUS, link, d, sizeof(d));
	}
}

void osc2_test_note_ble(uint8_t event, uint8_t a, uint16_t b, uint32_t c)
{
	uint8_t d[8] = { event, a };

	if (osc2_test_core_armed(CORE)) {
		opensc2_put_le16(&d[2], b);
		opensc2_put_le32(&d[4], c);
		note(OSC2_TEST_KIND_BLE, OSC2_TEST_LINK_BLE, d, sizeof(d));
	}
}
