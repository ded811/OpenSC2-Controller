# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../..)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opensc2_imu LANGUAGES C)
target_sources(app PRIVATE src/main.c)
