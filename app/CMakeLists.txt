# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)

# The OpenSC2 library is a Zephyr module: the OpenSC2-Controller repository. Inside the repository
# it is found here. In a copy of this folder elsewhere, set ZEPHYR_EXTRA_MODULES to your
# OpenSC2-Controller folder (environment variable, or -DZEPHYR_EXTRA_MODULES=<folder>), or list the
# repository in your west manifest (docs/integration.md).
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../zephyr/module.yml AND
   EXISTS ${CMAKE_CURRENT_LIST_DIR}/../include/opensc2/opensc2.h)
  list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/..)
endif()

# A build variant: a Kconfig fragment of this folder given as EXTRA_CONF_FILE brings the devicetree
# overlay of the same name with it, when there is one (sensors.conf and sensors.overlay). Under
# sysbuild the fragment's name is in the sysbuild cache, not yet a variable here.
set(variant_confs ${EXTRA_CONF_FILE})
if(NOT variant_confs AND DEFINED SYSBUILD_CACHE AND EXISTS "${SYSBUILD_CACHE}")
  file(STRINGS "${SYSBUILD_CACHE}" variant_line REGEX "^EXTRA_CONF_FILE:[A-Z]+=")
  string(REGEX REPLACE "^EXTRA_CONF_FILE:[A-Z]+=" "" variant_confs "${variant_line}")
endif()
foreach(conf ${variant_confs})
  get_filename_component(variant ${conf} NAME_WE)
  if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/${variant}.overlay)
    list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/${variant}.overlay)
  endif()
endforeach()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opensc2_app LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	src/inputs.c
	src/status.c
	src/rumble.c
	src/trackpad.c
	src/imu.c
	src/battery.c
	src/i2c_recover.c
)
