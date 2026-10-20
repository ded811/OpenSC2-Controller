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

# The dock build (dock.conf, built but untested on hardware) brings its pins with it. The fragment
# reaches this file as EXTRA_CONF_FILE (west build --no-sysbuild) or in sysbuild's cache file for
# this image (the default build).
set(opensc2_fragments "${EXTRA_CONF_FILE}")
if(DEFINED SYSBUILD_CACHE AND EXISTS "${SYSBUILD_CACHE}")
  file(STRINGS "${SYSBUILD_CACHE}" opensc2_sysbuild_fragments REGEX "^EXTRA_CONF_FILE:")
  string(APPEND opensc2_fragments ";${opensc2_sysbuild_fragments}")
endif()
if(opensc2_fragments MATCHES "(^|[;=/\\\\])dock\\.conf(;|$)")
  list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/dock.overlay)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opensc2_app LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	src/inputs.c
	src/status.c
	src/rumble.c
	src/trackpad.c
	src/imu.c
)
