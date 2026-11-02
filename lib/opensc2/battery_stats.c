/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * battery_stats.c: a Zephyr statistics group "battery" filled at every battery report, as the real
 * controller keeps one (CONFIG_OPENSC2_BATTERY_STATS, with CONFIG_STATS): the battery, system and
 * input millivolts, the battery and input currents, the level, the temperature, and a status
 * entry that, as there, is never written. A tool that reads Zephyr statistics (mcumgr's SMP, the
 * shell's `stat`) sees them; no host of the controller reads them.
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/stats/stats.h>
#include "internal.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

STATS_SECT_START(osc2_battery)
STATS_SECT_ENTRY16(voltage_mv)
STATS_SECT_ENTRY16(current_ma)
STATS_SECT_ENTRY16(system_mv)
STATS_SECT_ENTRY16(input_mv)
STATS_SECT_ENTRY16(input_ma)
STATS_SECT_ENTRY16(level)
STATS_SECT_ENTRY16(temperature)
STATS_SECT_ENTRY16(status)
STATS_SECT_END;

STATS_NAME_START(osc2_battery)
STATS_NAME(osc2_battery, voltage_mv)
STATS_NAME(osc2_battery, current_ma)
STATS_NAME(osc2_battery, system_mv)
STATS_NAME(osc2_battery, input_mv)
STATS_NAME(osc2_battery, input_ma)
STATS_NAME(osc2_battery, level)
STATS_NAME(osc2_battery, temperature)
STATS_NAME(osc2_battery, status)
STATS_NAME_END(osc2_battery);

static STATS_SECT_DECL(osc2_battery) osc2_battery;   /* named as its section: the names map */

static int battery_stats_init(void)
{
	int err = STATS_INIT_AND_REG(osc2_battery, STATS_SIZE_16, "battery");

	if (err < 0) {
		LOG_ERR("Battery statistics not registered (%d)", err);
	}
	return 0;
}
SYS_INIT(battery_stats_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

void osc2_battery_stats(const struct opensc2_battery *b)
{
	STATS_SET(osc2_battery, voltage_mv, b->battery_mv);
	STATS_SET(osc2_battery, current_ma, (uint16_t)b->current_ma);
	STATS_SET(osc2_battery, system_mv, b->system_mv);
	STATS_SET(osc2_battery, input_mv, b->input_mv);
	STATS_SET(osc2_battery, input_ma, b->input_current_ma);
	STATS_SET(osc2_battery, temperature, b->temperature);
	STATS_SET(osc2_battery, level, b->level);
}
