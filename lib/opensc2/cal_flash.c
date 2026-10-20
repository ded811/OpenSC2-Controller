/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * cal_flash.c: where the calibration keys live (CONFIG_OPENSC2_CAL): Zephyr settings under
 * "opensc2/cal/", next to the library's other keys (storage.c), in the storage partition that a
 * .uf2 reflash leaves alone. A factory reset (0x86) does not delete them. Writes that the frames
 * cause (a finished recording, 0xE2, 0xFE) run here from the system work queue.
 */
#include <errno.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "internal.h"
#include "cal_store.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

static void flush_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	osc2_cal_flush();
}
static K_WORK_DEFINE(flush_work, flush_fn);

void osc2_cal_defer(void)
{
	(void)k_work_submit(&flush_work);
}

#if defined(CONFIG_OPENSC2_STORAGE)
#include <zephyr/settings/settings.h>

#define STORED_NAME_MAX 40   /* "opensc2/" + the longest key, cal/sensors/gyroscope/bias */

static int stored_name(const char *name, char out[STORED_NAME_MAX])
{
	int n = snprintf(out, STORED_NAME_MAX, "opensc2/%s", name);

	return n > 0 && n < STORED_NAME_MAX ? 0 : -ENAMETOOLONG;
}

int osc2_cal_flash_write(const char *name, const void *value, size_t len)
{
	char key[STORED_NAME_MAX];
	int err = stored_name(name, key);

	return err ? err : settings_save_one(key, value, len);
}

int osc2_cal_flash_delete(const char *name)
{
	char key[STORED_NAME_MAX];
	int err = stored_name(name, key);

	return err ? err : settings_delete(key);
}

static int on_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t buf[18];
	ssize_t n;

	if (len > sizeof(buf)) {
		return -EINVAL;
	}
	n = read_cb(cb_arg, buf, len);
	if (n < 0) {
		return (int)n;
	}
	return osc2_cal_load(key, buf, (size_t)n);
}

/* The longest name wins in Zephyr settings: opensc2/cal/... comes here, not to storage.c. */
SETTINGS_STATIC_HANDLER_DEFINE(opensc2_cal, "opensc2/cal", NULL, on_set, NULL, NULL);

#else /* !CONFIG_OPENSC2_STORAGE */

int osc2_cal_flash_write(const char *name, const void *value, size_t len)
{
	ARG_UNUSED(name);
	ARG_UNUSED(value);
	ARG_UNUSED(len);
	return 0;   /* RAM only, as everything else without storage */
}

int osc2_cal_flash_delete(const char *name)
{
	ARG_UNUSED(name);
	return 0;
}

#endif /* CONFIG_OPENSC2_STORAGE */
