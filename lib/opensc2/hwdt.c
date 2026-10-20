/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * hwdt.c: the hardware watchdog, as on the real controller (CONFIG_OPENSC2_WATCHDOG): it resets
 * the chip 5 s after the last feed and pauses while a debugger halts the CPU. A work queue at the
 * lowest application priority feeds it every 2.5 s; each feed re-arms a 4.5 s warning on the
 * system work queue. When the feeds stop (a thread keeps the CPU busy at or above the feeder's
 * priority), the warning comes first: it logs and raises a kernel oops, so the crash record
 * (crash.c) names the hang; a hang that also stops the system work queue ends in a plain watchdog
 * reset, seen only in 0xA1's reset causes. With a debugger attached the warning only logs.
 *
 * The test hooks can stop the feeds on purpose (osc2_hwdt_starve(), docs/test-protocol.md).
 */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include "internal.h"
#include "hwdt.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#if defined(CONFIG_OPENSC2_WATCHDOG)
#include <zephyr/drivers/watchdog.h>

#define WDT_TIMEOUT_MS  5000
#define FEED_MS         2500
#define WARN_MS         4500
#define FEED_STACK      768

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int channel = -1;
static struct k_work_q feed_queue;
static K_THREAD_STACK_DEFINE(feed_stack, FEED_STACK);
static atomic_t feeds;
static atomic_t starving;   /* the test hooks stopped the feeds */
static atomic_t no_crash;   /* ... and the warning only logs: the hardware resets */

/* Is a debugger attached? The core's debug-enable bit (DHCSR.C_DEBUGEN). */
static bool debugger_attached(void)
{
#if defined(CONFIG_CPU_CORTEX_M)
	return (*(volatile uint32_t *)0xE000EDF0u & 1u) != 0;
#else
	return false;
#endif
}

static void warn_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (debugger_attached()) {
		LOG_WRN("Watchdog about to expire, skipped for debugging");
		return;
	}
	if (atomic_get(&no_crash)) {
		LOG_ERR("Watchdog about to expire (test: no crash record, the watchdog resets)");
		return;
	}
	LOG_ERR("Watchdog about to expire");
	k_oops();   /* the crash record names the hang, before the hardware resets without one */
}
static K_WORK_DELAYABLE_DEFINE(warn_work, warn_fn);

static void feed_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(feed_work, feed_fn);

static void feed_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (atomic_get(&starving)) {
		/* The test: the feeder's queue stops here, as behind a busy thread. */
		k_sleep(K_FOREVER);
	}
	(void)wdt_feed(wdt, channel);
	atomic_inc(&feeds);
	(void)k_work_reschedule(&warn_work, K_MSEC(WARN_MS));
	(void)k_work_reschedule_for_queue(&feed_queue, &feed_work, K_MSEC(FEED_MS));
}

int osc2_hwdt_start(void)
{
	const struct wdt_timeout_cfg cfg = {
		.window = { .min = 0, .max = WDT_TIMEOUT_MS },
		.flags = WDT_FLAG_RESET_SOC,
	};
	int err;

	if (channel >= 0) {
		return 0;
	}
	if (!device_is_ready(wdt)) {
		LOG_ERR("The watchdog device is not ready");
		return -ENODEV;
	}
	channel = wdt_install_timeout(wdt, &cfg);
	if (channel < 0) {
		LOG_ERR("Could not set the watchdog up (%d)", channel);
		return channel;
	}
	err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err) {
		LOG_ERR("Could not start the watchdog (%d)", err);
		channel = -1;
		return err;
	}
	{
		const struct k_work_queue_config qcfg = { .name = "opensc2_wdt" };

		k_work_queue_start(&feed_queue, feed_stack, K_THREAD_STACK_SIZEOF(feed_stack),
				   K_LOWEST_APPLICATION_THREAD_PRIO, &qcfg);
	}
	(void)k_work_reschedule_for_queue(&feed_queue, &feed_work, K_NO_WAIT);
	LOG_INF("Watchdog: %d ms, fed every %d ms", WDT_TIMEOUT_MS, FEED_MS);
	return 0;
}

int osc2_hwdt_starve(bool hardware)
{
	if (channel < 0) {
		return -ENODEV;
	}
	if (atomic_get(&starving)) {
		return -EBUSY;
	}
	atomic_set(&no_crash, hardware ? 1 : 0);
	atomic_set(&starving, 1);
	LOG_WRN("Test: the watchdog is no longer fed (%s)",
		hardware ? "the hardware resets" : "the warning crashes first");
	return 0;
}

void osc2_hwdt_info(struct osc2_hwdt_info *info)
{
	info->running = channel >= 0;
	info->starving = atomic_get(&starving) != 0;
	info->feeds = (uint32_t)atomic_get(&feeds);
	info->timeout_ms = WDT_TIMEOUT_MS;
}

#else /* !CONFIG_OPENSC2_WATCHDOG */

int osc2_hwdt_start(void)
{
	return 0;
}

int osc2_hwdt_starve(bool hardware)
{
	ARG_UNUSED(hardware);
	return -ENOTSUP;
}

void osc2_hwdt_info(struct osc2_hwdt_info *info)
{
	info->running = false;
	info->starving = false;
	info->feeds = 0;
	info->timeout_ms = 0;
}

#endif /* CONFIG_OPENSC2_WATCHDOG */
