# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT

# The puck's radio link is Nordic's Enhanced ShockBurst, which comes with the nRF Connect SDK
# (CONFIG_OPENSC2 selects CONFIG_ESB). Plain Zephyr has no ESB, so the select does nothing there:
# stop with a clear message instead of a missing <esb.h> later. (Not a module.yml "depends": the
# nRF Connect SDK's module name depends on its folder's name, so that could break a valid
# workspace.)
if(NOT CONFIG_ESB)
  message(FATAL_ERROR "OpenSC2 needs the nRF Connect SDK v3.4.0 (its Enhanced ShockBurst, "
    "CONFIG_ESB); plain Zephyr has no ESB. Build with ZEPHYR_BASE in an nRF Connect SDK v3.4.0 "
    "installation (docs/integration.md, Build problems).")
endif()

zephyr_library_named(opensc2)

zephyr_library_sources(
	cal.c
	codec.c
	core.c
	commands.c
	crash.c
	crash_rules.c
	gauge.c
	hwdt.c
	id_rules.c
	input_rules.c
	key_rules.c
	key_store.c
	lizard.c
	mode.c
	mode_rules.c
	# the trackpads frame by frame (<opensc2/trackpad.h>), the IMU (<opensc2/imu.h>)
	pad.c
	pad_rules.c
	imu.c
	imu_rules.c
	proto_rules.c
	radio.c
	report.c
	report_queue.c
	report_rules.c
	# the retained area's pure logic, and the CRC-32 the crash records and the test hooks use
	retained.c
	sensor_helpers.c
	settings_defaults.c
	storage.c
	wireless.c
	transport_rules.c
	# power, chords, the status LED and cues (CONFIG_OPENSC2_SYSTEM; system.c answers the
	# public getters without it)
	system.c
	sys_rules.c
	chord_rules.c
	idle_rules.c
	led_rules.c
)
# The calibration's and the sensors' arithmetic is the real controller's single-precision floating
# point, with a multiply and an add fused into one rounding only where its build fuses them (fmaf()
# in the code): the compiler may fuse nothing else, or a result could differ by a count.
set_source_files_properties(cal.c sensor_helpers.c input_rules.c report_rules.c
	PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
zephyr_library_sources_ifdef(CONFIG_OPENSC2_USB
	usb.c
	usb_rules.c
	usblog.c
)
# The report descriptor: the wired device's, and Bluetooth's report map, byte for byte.
if(CONFIG_OPENSC2_USB OR CONFIG_OPENSC2_BLE)
  zephyr_library_sources(usb_desc.c)
endif()
zephyr_library_sources_ifdef(CONFIG_OPENSC2_BLE
	ble.c
	ble_rules.c
)
# The haptics synthesiser (Kconfig.hap), or its API answering -ENOTSUP; the pure helpers always.
if(CONFIG_OPENSC2_HAPTICS_SYNTH)
  zephyr_library_sources(
	haptics.c
	haptics_core.c
	haptics_dsp.c
	haptics_rules.c
	haptics_run.c
	haptics_sounds.c
	haptics_tables.c
  )
  zephyr_library_sources_ifdef(CONFIG_OPENSC2_HAPTICS_OUT_I2S haptics_out_i2s.c)
  zephyr_library_sources_ifdef(CONFIG_OPENSC2_HAPTICS_OUT_PWM_DAC haptics_out_pwm.c)
  zephyr_library_sources_ifdef(CONFIG_OPENSC2_HAPTICS_OUT_DRV2605 haptics_out_drv2605.c)
else()
  zephyr_library_sources(haptics_off.c)
endif()
zephyr_library_sources(haptics_help.c)
zephyr_library_sources_ifdef(CONFIG_OPENSC2_SHELL shell.c)
# The calibration keys, commands and raw inputs (stand-ins without CONFIG_OPENSC2_CAL).
zephyr_library_sources(cal_store.c)
zephyr_library_sources_ifdef(CONFIG_OPENSC2_CAL cal_flash.c)
if(CONFIG_OPENSC2_CAL AND CONFIG_OPENSC2_SHELL)
  zephyr_library_sources(cal_shell.c)
endif()
zephyr_library_sources_ifdef(CONFIG_OPENSC2_FUSION fusion.c)
# The hardware test hooks (tools/hwtest).
zephyr_library_sources_ifdef(CONFIG_OPENSC2_TEST_HOOKS
	test_core.c
	test_hooks.c
	test_notes.c
	test_sys.c
	test_sys_notes.c
)
if(CONFIG_OPENSC2_TEST_HOOKS AND CONFIG_OPENSC2_SHELL)
  zephyr_library_sources(test_shell.c)
endif()
# The dock: the frames (<opensc2/dock.h>, for any application), the dock link and its rules, its
# test ops with the hooks, and the UART and pilot glue (dock.c, CONFIG_OPENSC2_DOCK inside).
zephyr_library_sources(dock_codec.c)
zephyr_library_sources_ifdef(CONFIG_OPENSC2_DOCK_LINK
	dock.c
	dock_link.c
	dock_rules.c
)
if(CONFIG_OPENSC2_DOCK_LINK AND CONFIG_OPENSC2_TEST_HOOKS)
  zephyr_library_sources(test_dock.c)
endif()

# The build time reported to Steam when CONFIG_OPENSC2_BUILD_TIME is 0: when CMake configured
# this build (Unix seconds, UTC).
string(TIMESTAMP OPENSC2_NOW "%s" UTC)
zephyr_library_compile_definitions(OPENSC2_CMAKE_BUILD_TIME=${OPENSC2_NOW}u)

# The identity's Kconfig strings, checked here by the rules opensc2_identity_check() applies at
# boot (codec.c): with a value that breaks one, opensc2_init() refuses the identity and the board
# starts only its console, no controller. core.c checks the lengths again. The values are read
# from the .config itself, with Kconfig's \" and \\ undone, not from the CONFIG_* variables:
# Zephyr imports those through CMake lists, which split or drop a value at a ';'.
file(READ "${DOTCONFIG}" opensc2_dotconfig)
function(opensc2_check_identity_string symbol min_len max_len regex rule)
  if(NOT "\n${opensc2_dotconfig}" MATCHES "\n${symbol}=\"([^\r\n]*)\"")
    message(FATAL_ERROR "OpenSC2: ${symbol} was not found in ${DOTCONFIG}")
  endif()
  string(REGEX REPLACE "\\\\(.)" "\\1" value "${CMAKE_MATCH_1}")
  string(LENGTH "${value}" len)
  if(len LESS min_len OR len GREATER max_len OR NOT value MATCHES "${regex}")
    message(FATAL_ERROR "OpenSC2: ${symbol} \"${value}\" must be ${rule}. A board built with "
      "it would not start as a controller: opensc2_init() refuses an invalid identity.")
  endif()
endfunction()
opensc2_check_identity_string(CONFIG_OPENSC2_UNIT_SERIAL 13 13 "^[0-9A-Z]+$"
  "exactly 13 characters, 0-9 and A-Z")
opensc2_check_identity_string(CONFIG_OPENSC2_BOARD_SERIAL 13 13 "^[0-9A-Z]+$"
  "exactly 13 characters, 0-9 and A-Z")
opensc2_check_identity_string(CONFIG_OPENSC2_GIT_REVISION 1 12 "^[!-~]+$"
  "1 to 12 printable ASCII characters (0x21 to 0x7E), no spaces")
if(CONFIG_OPENSC2_USB)
  opensc2_check_identity_string(CONFIG_OPENSC2_USB_PRODUCT 1 31 "^[!-~]([ -~]*[!-~])?$"
    "1 to 31 printable ASCII characters (0x20 to 0x7E), no space at either end")
endif()

# Wired mode without storage builds, but Steam's pairing over the USB-C cable cannot work: Steam
# writes the bond into the board and then reboots it into wireless mode, and a bond kept in RAM
# only is gone after that reboot, so the board never finds the puck.
if(CONFIG_OPENSC2_USB AND NOT CONFIG_OPENSC2_STORAGE)
  message(WARNING "OpenSC2: wired mode (CONFIG_OPENSC2_USB) is on, but the bonds are not stored "
    "(CONFIG_OPENSC2_STORAGE is off; it needs CONFIG_SETTINGS). Steam's pairing over the USB-C "
    "cable writes the bond and then reboots the board into wireless mode, which loses it. Add "
    "CONFIG_FLASH=y, CONFIG_FLASH_MAP=y, CONFIG_NVS=y, CONFIG_SETTINGS=y and "
    "CONFIG_SETTINGS_NVS=y to prj.conf (docs/integration.md, Kconfig).")
endif()

# Without a devicetree region for it, the retained area (the USB session log, and the warm-restart
# mark unless that is in GPREGRET2) and the crash record are in the application's no-init RAM,
# which a bootloader that runs at every reset may use for itself: the Adafruit UF2 bootloader's data
# and stack land there.
if(CONFIG_OPENSC2_USB_LOG OR CONFIG_OPENSC2_WARM_MARK_RAM OR CONFIG_OPENSC2_CRASH_RECORD)
  dt_chosen(opensc2_retained_ram PROPERTY "opensc2,retained-ram")
  if(NOT DEFINED opensc2_retained_ram)
    message(WARNING "OpenSC2: no devicetree region is chosen as opensc2,retained-ram, so the "
      "retained area (the USB session log; the warm-restart mark unless it is in GPREGRET2) and "
      "the crash record are in the application's no-init RAM, which a bootloader may clear at "
      "every reset (the Adafruit UF2 bootloader does). Choose a region that the bootloader leaves "
      "alone: docs/porting.md, Wired mode.")
  endif()
endif()
