/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only (the binary built with CONFIG_OPENSC2_TEST_HOOKS): the hardware test hooks'
 * glue without a kernel. One core (test_core.c) and the library's calls into it (test_notes.c),
 * on a fake environment that records what it is asked, runs the real command channel
 * (commands.c) for the host emulation, and checks the rules of the two locks: neither is taken
 * twice, and nothing in the library is called while the state lock is held.
 */
#include <string.h>
#include "internal.h"
#include "cal_store.h"
#include "stubs.h"
#include "hooks_stub.h"

struct osc2_test_core osc2_test_core_obj;
struct hooks_record hk;

static void library_call(void)
{
	if (hk.lock_depth != 0) {
		hk.violations++;
	}
}

static void order(char what)
{
	if (hk.order_len < sizeof(hk.order) - 1) {
		hk.order[hk.order_len++] = what;
	}
}

/* ---- the glue ---- */

uint8_t osc2_test_ctx_set(uint8_t link)
{
	uint8_t previous = hk.ctx_link;

	hk.ctx_link = link;
	return previous;
}

void osc2_test_ctx_restore(uint8_t previous)
{
	hk.ctx_link = previous;
}

void osc2_test_deadline_changed(void)
{
	hk.deadline_calls++;
}

int osc2_test_console_run(int argc, const char *const *argv, osc2_test_emit emit, void *ctx)
{
	uint8_t previous = osc2_test_ctx_set(OSC2_TEST_LINK_TEST);
	int rc = osc2_test_console(&osc2_test_core_obj, &osc2_test_env_obj, argc, argv, emit, ctx);

	osc2_test_ctx_restore(previous);
	osc2_test_deadline_changed();
	return rc;
}

void hooks_reset(void)
{
	memset(&hk, 0, sizeof(hk));
	hk.ctx_link = OSC2_TEST_LINK_NONE;
	hk.usb_state = OPENSC2_USB_CONFIGURED;
	osc2_test_core_init(&osc2_test_core_obj);
}

/* ---- the fake environment ---- */

static uint32_t env_now_ms(void)
{
	return (uint32_t)stub.now_ms;
}

static uint32_t env_now_us(void)
{
	return (uint32_t)(stub.now_ms * 1000 + hk.extra_us);
}

static void env_lock(void)
{
	if (hk.lock_depth++ != 0) {
		hk.violations++;
	}
}

static void env_unlock(void)
{
	if (--hk.lock_depth < 0) {
		hk.violations++;
		hk.lock_depth = 0;
	}
}

static void env_req_lock(void)
{
	if (hk.req_depth++ != 0) {
		hk.violations++;
	}
}

static void env_req_unlock(void)
{
	if (--hk.req_depth < 0) {
		hk.violations++;
		hk.req_depth = 0;
	}
}

static void env_cnt_add(uint8_t counter, uint32_t n)
{
	hk.counters[counter] += n;
}

static uint32_t env_cnt_get(uint8_t counter)
{
	return hk.counters[counter];
}

static void env_cnt_reset(void)
{
	memset(hk.counters, 0, sizeof(hk.counters));
}

static uint8_t env_ctx_link(void)
{
	return hk.ctx_link;
}

static uint8_t env_mode_link(void)
{
	return osc2_wired() ? OSC2_TEST_LINK_USB : OSC2_TEST_LINK_RADIO;
}

static void env_caps(struct osc2_test_lib_caps *caps)
{
	library_call();
	caps->mode = osc2_wired() ? 1 : 0;
	caps->build_flags = OSC2_TEST_FLAG_TRIGGER_CLICK_AUTO | OSC2_TEST_FLAG_LIZARD_MODE |
			    OSC2_TEST_FLAG_STEAM_WATCHDOG | OSC2_TEST_FLAG_USB | OSC2_TEST_FLAG_CAL;
	caps->battery_interval_ms = 3500;
	caps->rumble_timeout_ms = 500;
	caps->report_delay_us = 2000;
	caps->policy = (uint8_t)osc2.policy;
	memcpy(caps->git_revision, osc2.identity.git_revision, sizeof(caps->git_revision));
	memcpy(caps->product_name, osc2.identity.product_name, sizeof(caps->product_name));
}

static void env_status(struct osc2_test_lib_status *status)
{
	library_call();
	status->mode = osc2_wired() ? 1 : 0;
	status->mode_reason = (uint8_t)osc2.mode_reason;
	status->link_state = hk.link_state;
	status->usb_state = osc2_wired() ? hk.usb_state : OPENSC2_USB_OFF;
	status->slot = osc2.slot;
	status->last_reason = osc2.last_disconnect_reason;
	status->policy = (uint8_t)osc2.policy;
	status->lizard_on = osc2.settings[OPENSC2_SETTING_LIZARD_MODE] != 0;
	status->uptime_ms = (uint32_t)stub.now_ms;
}

static bool env_lizard_on(void)
{
	library_call();
	return osc2.settings[OPENSC2_SETTING_LIZARD_MODE] != 0;
}

static int16_t env_setting_get(uint8_t number)
{
	library_call();
	return number < OPENSC2_SETTINGS_COUNT ? osc2.settings[number] : 0;
}

static void env_setting_write(uint8_t number, int16_t value)
{
	library_call();
	order('W');
	if (hk.written_count < sizeof(hk.written)) {
		hk.written[hk.written_count++] = number;
	}
	osc2_settings_write(number, value);
}

static void env_led_get(uint8_t rgbw[4])
{
	library_call();
	rgbw[0] = osc2.led.red;
	rgbw[1] = osc2.led.green;
	rgbw[2] = osc2.led.blue;
	rgbw[3] = osc2.led.white;
}

static void env_led_set(const uint8_t rgbw[4])
{
	library_call();
	order('C');
	osc2_led_color_write(rgbw);
}

/* What report.c's osc2_output_dispatch() does with the test link's report, minus the callbacks. */
static int env_host_out(const uint8_t *report, uint8_t len)
{
	struct opensc2_output out[2];
	int n;

	library_call();
	hk.host_out_calls++;
	if (hk.output_hook) {
		return hk.output_hook(OSC2_LINK_TEST, report, len);
	}
	n = opensc2_decode_output(report, len, out);
	osc2_test_note_output(OSC2_LINK_TEST, report, len, n);
	osc2_test_count(OSC2_TEST_CNT_OUT_REPORTS);   /* as report.c counts */
	if (n < 0) {
		osc2_test_count(OSC2_TEST_CNT_OUT_SHORT);
	}
	for (int i = 0; i < n; i++) {
		if (out[i].type == OPENSC2_OUTPUT_RUMBLE) {
			osc2_test_note_rumble(OSC2_LINK_TEST, &out[i].rumble);
		} else if (out[i].type == OPENSC2_OUTPUT_HAPTIC) {
			osc2_test_note_haptic(OSC2_LINK_TEST, &out[i].haptic);
		}
	}
	return n;
}

static int32_t env_host_set(const uint8_t *rec, uint8_t rec_len)
{
	library_call();
	hk.host_set_calls++;
	hk.host_set_len = rec_len;
	memcpy(hk.host_set_last, rec, rec_len);
	return osc2_command_set(OSC2_LINK_TEST, rec, rec_len);
}

static uint8_t env_host_get(uint8_t out[63], uint8_t stored[63], uint8_t *stored_len)
{
	uint8_t n;

	library_call();
	n = osc2_command_get(OSC2_LINK_TEST, out);
	*stored_len = osc2_command_stored(OSC2_LINK_TEST, stored);
	return n;
}

/* opensc2_send_report(): the guard first, as osc2_input_send() has it. */
static int32_t env_app_send(const uint8_t *report, uint8_t len)
{
	int32_t rc = 0;

	library_call();
	hk.app_send_calls++;
	if (!osc2_test_divert(report, len)) {
		hk.app_send_len = len;
		memcpy(hk.app_send_last, report, len);
		rc = hk.app_send_rc;
	}
	if (rc == 0) {
		osc2_test_count(OSC2_TEST_CNT_APP_REPORTS_SENT);
	}
	return rc;
}

static int32_t env_app_haptic(uint8_t channel, uint32_t flags)
{
	library_call();
	hk.app_haptic_calls++;
	hk.app_haptic_channel = channel;
	hk.app_haptic_flags = flags;
	return 0;
}

static bool env_radio_connected(void)
{
	library_call();
	return hk.radio_connected;
}

static void env_battery_now(void)
{
	library_call();
	hk.battery_now_calls++;
}

static bool env_release_held(void)
{
	library_call();
	order('R');
	hk.release_calls++;
	hk.guard_at_release = osc2_test_core_guard(&osc2_test_core_obj);
	if (hk.release_hook) {
		return hk.release_hook();
	}
	return hk.release_result;
}

/* osc2_lizard_resync(): its entry as report.c writes it. */
static void env_lizard_resync(void)
{
	library_call();
	order('S');
	hk.resync_calls++;
	if (hk.resync_hook) {
		hk.resync_hook();
	}
	osc2_test_note_lizard(OSC2_TEST_LIZARD_RESYNC);
}

const struct osc2_test_env osc2_test_env_obj = {
	.now_ms = env_now_ms,
	.now_us = env_now_us,
	.lock = env_lock,
	.unlock = env_unlock,
	.req_lock = env_req_lock,
	.req_unlock = env_req_unlock,
	.cnt_add = env_cnt_add,
	.cnt_get = env_cnt_get,
	.cnt_reset = env_cnt_reset,
	.ctx_link = env_ctx_link,
	.mode_link = env_mode_link,
	.caps = env_caps,
	.status = env_status,
	.lizard_on = env_lizard_on,
	.setting_get = env_setting_get,
	.setting_write = env_setting_write,
	.led_get = env_led_get,
	.led_set = env_led_set,
	.host_out = env_host_out,
	.host_set = env_host_set,
	.host_get = env_host_get,
	.app_send = env_app_send,
	.app_haptic = env_app_haptic,
	.radio_connected = env_radio_connected,
	.battery_now = env_battery_now,
	.release_held = env_release_held,
	.lizard_resync = env_lizard_resync,
	.cal_status = osc2_cal_status,
};
