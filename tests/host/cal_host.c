/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: stand-ins for the calibration's glue (cal_flash.c) and for the setting getter of
 * core.c, so cal_store.c runs on a PC. "Flash" is a small table that survives cal_host_boot(), as
 * the storage partition survives a reboot; the deferred work runs when a test calls
 * osc2_cal_flush() (or at cal_host_boot()).
 */
#include <string.h>
#include "internal.h"
#include "cal_store.h"
#include "cal_host.h"

struct cal_host_record cal_host;

int16_t opensc2_setting_get(uint8_t number)
{
	return number < OPENSC2_SETTINGS_COUNT ? osc2.settings[number] : 0;
}

void osc2_cal_defer(void)
{
	cal_host.deferred++;
}

static int slot_of(const char *name)
{
	for (int i = 0; i < CAL_HOST_KEYS; i++) {
		if (cal_host.flash[i].used && strcmp(cal_host.flash[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

int osc2_cal_flash_write(const char *name, const void *value, size_t len)
{
	int i = slot_of(name);

	if (cal_host.fail_writes) {
		return -5;
	}
	for (int k = 0; i < 0 && k < CAL_HOST_KEYS; k++) {
		if (!cal_host.flash[k].used) {
			i = k;
		}
	}
	if (i < 0 || len > sizeof(cal_host.flash[i].value) ||
	    strlen(name) >= sizeof(cal_host.flash[i].name)) {
		return -28;
	}
	cal_host.flash[i].used = true;
	strcpy(cal_host.flash[i].name, name);
	memcpy(cal_host.flash[i].value, value, len);
	cal_host.flash[i].len = (uint8_t)len;
	cal_host.writes++;
	return 0;
}

int osc2_cal_flash_delete(const char *name)
{
	int i = slot_of(name);

	cal_host.deletes++;
	if (i >= 0) {
		cal_host.flash[i].used = false;
	}
	return 0;
}

int cal_host_flash_get(const char *name, uint8_t *value, uint8_t *len)
{
	int i = slot_of(name);

	if (i < 0) {
		return -2;
	}
	memcpy(value, cal_host.flash[i].value, cal_host.flash[i].len);
	*len = cal_host.flash[i].len;
	return 0;
}

void cal_host_erase_all(void)
{
	memset(&cal_host, 0, sizeof(cal_host));
}

void cal_host_boot(void)
{
	osc2_cal_flush();   /* whatever was still deferred reaches flash first */
	osc2_cal_reset();
	for (int i = 0; i < CAL_HOST_KEYS; i++) {
		if (cal_host.flash[i].used) {
			/* stored as opensc2/cal/<name without cal/>: the glue hands over "joy_l" */
			(void)osc2_cal_load(cal_host.flash[i].name + 4, cal_host.flash[i].value,
					    cal_host.flash[i].len);
		}
	}
	cal_host.deferred = 0;
}
