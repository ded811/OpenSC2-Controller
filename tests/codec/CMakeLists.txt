# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# The pure parts of the library (wire formats, lizard mode, settings defaults, fusion) under
# ztest. They need no radio, so this suite runs on native_sim or any QEMU target:
#   west twister -T tests -p native_sim
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opensc2_codec_tests LANGUAGES C)

set(OPENSC2_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
target_include_directories(app PRIVATE ${OPENSC2_ROOT}/include ${OPENSC2_ROOT}/lib/opensc2)
target_sources(app PRIVATE
	src/main.c
	${OPENSC2_ROOT}/lib/opensc2/codec.c
	${OPENSC2_ROOT}/lib/opensc2/lizard.c
	${OPENSC2_ROOT}/lib/opensc2/settings_defaults.c
	${OPENSC2_ROOT}/lib/opensc2/fusion.c
)
