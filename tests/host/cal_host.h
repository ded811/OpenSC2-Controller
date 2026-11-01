/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: what the calibration's stand-ins (cal_host.c) recorded.
 */
#ifndef OSC2_HOST_CAL_HOST_H_
#define OSC2_HOST_CAL_HOST_H_

#include <stdbool.h>
#include <stdint.h>

#define CAL_HOST_KEYS 16

struct cal_host_record {
	struct {
		bool used;
		char name[32];
		uint8_t value[18];
		uint8_t len;
	} flash[CAL_HOST_KEYS];
	int deferred;        /* osc2_cal_defer() calls since the last boot */
	int waits;           /* osc2_cal_flush_wait() calls */
	int writes, deletes;
	int reloads;         /* osc2_cal_flash_reload() calls */
	int imu_sets;        /* opensc2_set_imu() calls */
	int free_falls;      /* opensc2_haptics_free_fall() calls */
	int fall_cancels;    /* opensc2_haptics_free_fall_cancel() calls */
	bool fail_writes;
	int power_source;    /* what opensc2_power_source() answers (enum opensc2_power_source) */
};

extern struct cal_host_record cal_host;

/* A key's stored bytes (name as "cal/joy_l"): 0, or -2 when it is not stored. */
int cal_host_flash_get(const char *name, uint8_t *value, uint8_t *len);
/* Empty "flash" (a new board). */
void cal_host_erase_all(void);
/* A reboot: the deferred work done, RAM back to its defaults, every stored key loaded again. */
void cal_host_boot(void);

#endif /* OSC2_HOST_CAL_HOST_H_ */
