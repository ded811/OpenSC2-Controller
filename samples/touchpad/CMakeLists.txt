# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)

# The OpenSC2 library is a Zephyr module: the OpenSC2-Controller repository. Inside the repository
# it is found here. In a copy of this folder elsewhere, set ZEPHYR_EXTRA_MODULES to your
# OpenSC2-Controller folder (environment variable, or -DZEPHYR_EXTRA_MODULES=<folder>), or list the
# repository in your west manifest (docs/integration.md).
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../../zephyr/module.yml AND
   EXISTS ${CMAKE_CURRENT_LIST_DIR}/../../include/opensc2/opensc2.h)
  list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opensc2_touchpad LANGUAGES C)

target_sources(app PRIVATE src/main.c)
