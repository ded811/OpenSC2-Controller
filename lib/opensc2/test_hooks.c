/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_hooks.c: the hardware test hooks (CONFIG_OPENSC2_TEST_HOOKS) on Zephyr: one core instance
 * (test_core.c) with a spinlock around its state and a mutex that lets one request run at a time,
 * the environment it works through (library calls), the counters (atomic: the radio interrupt
 * counts too), which link each thread's command came on, and one work item for the deadlines: the
 * lease running out and the end of a disarm. The calls the library makes are in test_notes.c.
 *
 * Everything is ordinary RAM: unarmed at every boot (a zeroed core is an unarmed one), never
 * stored, nothing that allocates or writes flash.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include "internal.h"
#include "test_hooks.h"

BUILD_ASSERT((int)OSC2_LINK_RADIO == (int)OSC2_TEST_LINK_RADIO &&
	     (int)OSC2_LINK_USB == (int)OSC2_TEST_LINK_USB &&
	     (int)OSC2_LINK_TEST == (int)OSC2_TEST_LINK_TEST && (int)OSC2_LINK_COUNT == OSC2_TEST_LINKS,
	     "the test hooks number the links as enum osc2_link does");

struct osc2_test_core osc2_test_core_obj;

static struct k_spinlock core_lock;
static k_spinlock_key_t core_key;   /* only its holder reads or writes it */
static K_MUTEX_DEFINE(request_lock);
static atomic_t counters[OSC2_TEST_CNT_COUNT];

/* ---- which link this thread's command came on ------------------------------------------------- */

#define CTX_SLOTS 6   /* radio, USB stack, USB report, shell, system work queue, one spare */

static struct {
	k_tid_t thread;
	uint8_t link;
} ctx_slots[CTX_SLOTS];
static struct k_spinlock ctx_lock;

uint8_t osc2_test_ctx_set(uint8_t link)
{
	k_tid_t me = k_current_get();
	uint8_t previous = OSC2_TEST_LINK_NONE;
	int free_slot = -1;
	k_spinlock_key_t key = k_spin_lock(&ctx_lock);

	for (int i = 0; i < CTX_SLOTS; i++) {
		if (ctx_slots[i].thread == me) {
			previous = ctx_slots[i].link;
			ctx_slots[i].link = link;
			k_spin_unlock(&ctx_lock, key);
			return previous;
		}
		if (ctx_slots[i].thread == NULL && free_slot < 0) {
			free_slot = i;
		}
	}
	if (free_slot >= 0) {
		ctx_slots[free_slot].thread = me;
		ctx_slots[free_slot].link = link;
	}
	k_spin_unlock(&ctx_lock, key);
	return previous;
}

void osc2_test_ctx_restore(uint8_t previous)
{
	k_tid_t me = k_current_get();
	k_spinlock_key_t key = k_spin_lock(&ctx_lock);

	for (int i = 0; i < CTX_SLOTS; i++) {
		if (ctx_slots[i].thread == me) {
			if (previous == OSC2_TEST_LINK_NONE) {
				ctx_slots[i].thread = NULL;
			} else {
				ctx_slots[i].link = previous;
			}
			break;
		}
	}
	k_spin_unlock(&ctx_lock, key);
}

static uint8_t env_ctx_link(void)
{
	k_tid_t me = k_current_get();
	uint8_t link = OSC2_TEST_LINK_NONE;
	k_spinlock_key_t key = k_spin_lock(&ctx_lock);

	for (int i = 0; i < CTX_SLOTS; i++) {
		if (ctx_slots[i].thread == me) {
			link = ctx_slots[i].link;
			break;
		}
	}
	k_spin_unlock(&ctx_lock, key);
	return link;
}

/* ---- the environment -------------------------------------------------------------------------- */

static uint32_t env_now_ms(void)
{
	return k_uptime_get_32();
}

static uint32_t env_now_us(void)
{
	return (uint32_t)k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}

static void env_lock(void)
{
	core_key = k_spin_lock(&core_lock);
}

static void env_unlock(void)
{
	k_spin_unlock(&core_lock, core_key);
}

static void env_req_lock(void)
{
	(void)k_mutex_lock(&request_lock, K_FOREVER);
}

static void env_req_unlock(void)
{
	(void)k_mutex_unlock(&request_lock);
}

static void env_cnt_add(uint8_t counter, uint32_t n)
{
	(void)atomic_add(&counters[counter], (atomic_val_t)n);
}

static uint32_t env_cnt_get(uint8_t counter)
{
	return (uint32_t)atomic_get(&counters[counter]);
}

static void env_cnt_reset(void)
{
	for (int i = 0; i < OSC2_TEST_CNT_COUNT; i++) {
		atomic_clear(&counters[i]);
	}
}

static uint8_t env_mode_link(void)
{
	return osc2_wired() ? OSC2_TEST_LINK_USB : OSC2_TEST_LINK_RADIO;
}

static void env_caps(struct osc2_test_lib_caps *caps)
{
	struct opensc2_identity id;
	uint32_t flags = 0;

	flags |= IS_ENABLED(CONFIG_OPENSC2_TRIGGER_CLICK_AUTO) ? OSC2_TEST_FLAG_TRIGGER_CLICK_AUTO : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_LIZARD_MODE) ? OSC2_TEST_FLAG_LIZARD_MODE : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_STEAM_WATCHDOG) ? OSC2_TEST_FLAG_STEAM_WATCHDOG : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_USB) ? OSC2_TEST_FLAG_USB : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_USB_PROFILE_OCTOBER) ?
		 OSC2_TEST_FLAG_USB_PROFILE_OCTOBER : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_USB_CONSOLE) ? OSC2_TEST_FLAG_USB_CONSOLE : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_USB_WIRED_CONSOLE) ? OSC2_TEST_FLAG_USB_WIRED_CONSOLE : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE) ?
		 OSC2_TEST_FLAG_IDENTITY_VENDOR_WRITE : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_STORAGE) ? OSC2_TEST_FLAG_STORAGE : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_USB_LOG) ? OSC2_TEST_FLAG_USB_LOG : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_USB_REMOTE_WAKEUP) ? OSC2_TEST_FLAG_USB_REMOTE_WAKEUP : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_USB_PLUG_IN_REBOOT) ?
		 OSC2_TEST_FLAG_USB_PLUG_IN_REBOOT : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_SHELL) ? OSC2_TEST_FLAG_SHELL : 0;
	flags |= IS_ENABLED(CONFIG_OPENSC2_FUSION) ? OSC2_TEST_FLAG_FUSION : 0;
	flags |= OSC2_NO_HOST_WIRELESS_MS > 0 ? OSC2_TEST_FLAG_NO_HOST_WIRELESS : 0;

	opensc2_identity_get(&id);
	caps->mode = osc2_wired() ? 1 : 0;
	caps->build_flags = flags;
	caps->battery_interval_ms = (uint16_t)MIN(CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS, 65535);
	caps->rumble_timeout_ms = (uint16_t)MIN(CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS, 65535);
	caps->report_delay_us = (uint16_t)CONFIG_OPENSC2_REPORT_DELAY_US;
	caps->policy = (uint8_t)opensc2_policy();
	memcpy(caps->git_revision, id.git_revision, sizeof(caps->git_revision));
	memcpy(caps->product_name, id.product_name, sizeof(caps->product_name));
}

static void env_status(struct osc2_test_lib_status *status)
{
	status->mode = osc2_wired() ? 1 : 0;
	status->mode_reason = (uint8_t)opensc2_mode_reason();
	status->link_state = (uint8_t)osc2_radio_state();
	status->usb_state = (uint8_t)opensc2_usb_state();
	status->slot = opensc2_selected_slot();
	status->last_reason = osc2.last_disconnect_reason;
	status->policy = (uint8_t)opensc2_policy();
	status->lizard_on = opensc2_lizard_mode();
	status->uptime_ms = k_uptime_get_32();
}

static bool env_lizard_on(void)
{
	return opensc2_lizard_mode();
}

static int16_t env_setting_get(uint8_t number)
{
	return opensc2_setting_get(number);
}

static void env_setting_write(uint8_t number, int16_t value)
{
	osc2_settings_write(number, value);
}

static void env_led_get(uint8_t rgbw[4])
{
	struct opensc2_led led;

	opensc2_led_get(&led);
	rgbw[0] = led.red;
	rgbw[1] = led.green;
	rgbw[2] = led.blue;
	rgbw[3] = led.white;
}

static void env_led_set(const uint8_t rgbw[4])
{
	osc2_led_color_write(rgbw);
}

static int env_host_out(const uint8_t *report, uint8_t len)
{
	return osc2_output_dispatch(OSC2_LINK_TEST, report, len);
}

static int32_t env_host_set(const uint8_t *rec, uint8_t rec_len)
{
	return osc2_command_set(OSC2_LINK_TEST, rec, rec_len);
}

static uint8_t env_host_get(uint8_t out[63], uint8_t stored[63], uint8_t *stored_len)
{
	uint8_t n = osc2_command_get(OSC2_LINK_TEST, out);

	*stored_len = osc2_command_stored(OSC2_LINK_TEST, stored);
	return n;
}

static int32_t env_app_send(const uint8_t *report, uint8_t len)
{
	return opensc2_send_report(report, len);
}

static int32_t env_app_haptic(uint8_t channel, uint32_t flags)
{
	return opensc2_send_haptic_status(channel, flags);
}

static bool env_radio_connected(void)
{
	return !osc2_wired() && osc2_radio_connected();
}

static void env_battery_now(void)
{
	osc2_report_battery_now();
}

static bool env_release_held(void)
{
	return osc2_lizard_release_held();
}

static void env_lizard_resync(void)
{
	osc2_lizard_resync();
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
};

/* ---- deadlines: the lease, the end of a disarm (the system work queue) ------------------------- */

static void deadline_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(deadline_work, deadline_fn);

static void deadline_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	osc2_test_core_poll(&osc2_test_core_obj, &osc2_test_env_obj);
	osc2_test_deadline_changed();
}

void osc2_test_deadline_changed(void)
{
	int32_t ms = osc2_test_core_next(&osc2_test_core_obj, &osc2_test_env_obj);

	if (ms < 0) {
		(void)k_work_cancel_delayable(&deadline_work);
	} else {
		(void)k_work_reschedule(&deadline_work, K_MSEC(ms));
	}
}

/* ---- the console ------------------------------------------------------------------------------ */

int osc2_test_console_run(int argc, const char *const *argv, osc2_test_emit emit, void *ctx)
{
	/* Everything the console causes counts as the test link's. */
	uint8_t previous = osc2_test_ctx_set(OSC2_TEST_LINK_TEST);
	int rc = osc2_test_console(&osc2_test_core_obj, &osc2_test_env_obj, argc, argv, emit, ctx);

	osc2_test_ctx_restore(previous);
	osc2_test_deadline_changed();
	return rc;
}
