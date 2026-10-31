# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# The library's fuel gauge (<opensc2/battery.h>, battery.c, fuel_rules.c). Sourced from
# lib/opensc2/Kconfig.

config OPENSC2_BATTERY_GAUGE
	bool "The real controller's fuel gauge and charge control, run by the library"
	default y if OPENSC2_BATTERY
	help
	  opensc2_battery_start() hands the library the board's battery parts (a battery reading,
	  and optionally a charger's status, values and controls and a cut-off), and the library
	  runs the real controller's fuel gauge: a pass once a second, 40 readings with an average,
	  a level per direction kept over a warm reboot, the charge state machine with its supply
	  check, the charge window (mte/charge_level), and the battery cut-off at power-off. Costs a
	  thread (CONFIG_OPENSC2_BATTERY_GAUGE_STACK_SIZE) that only runs once started. Without it the
	  application sets the battery's fields itself (opensc2_set_battery()). Built; not yet tested
	  on hardware.

if OPENSC2_BATTERY_GAUGE

config OPENSC2_BATTERY_GAUGE_PRIORITY
	int "The fuel gauge's thread priority"
	default 9

config OPENSC2_BATTERY_GAUGE_STACK_SIZE
	int "The fuel gauge's thread stack size"
	default 1024

config OPENSC2_CHARGE_TARGET_MV
	int "Charge voltage target, mV"
	default 4120
	range 3400 4400
	help
	  The real controller charges to 4120 mV, and its gauge reads 100 % there (the discharging
	  curve's stretch). The gauge asks the board's charger for it (set_charge_voltage), and in
	  "not charging" a battery average at or above it counts as a finished charge.

config OPENSC2_CHARGE_HYSTERESIS_MV
	int "Added to the target during the supply check and a charge, mV"
	default 40
	range 0 200
	help
	  The real controller charges to 4160 mV once its supply check starts (4120 + 40), and its
	  charging curve reads 100 % there.

config OPENSC2_SUPPLY_CHECK_START_MV
	int "Supply check: the first input-voltage limit, mV"
	default 4600

config OPENSC2_SUPPLY_CHECK_STEP_MV
	int "Supply check: the limit is lowered by this, mV"
	default 100
	range 1 1000

config OPENSC2_SUPPLY_CHECK_MIN_MV
	int "Supply check: gives up when the limit reaches this, mV"
	default 4300

config OPENSC2_SUPPLY_CHECK_STEP_S
	int "Supply check: seconds per step"
	default 5
	range 1 600

config OPENSC2_SUPPLY_CHECK_REST_S
	int "Supply check: seconds of rest after it gave up (the 0x80 flag)"
	default 60
	range 1 3600

endif # OPENSC2_BATTERY_GAUGE
