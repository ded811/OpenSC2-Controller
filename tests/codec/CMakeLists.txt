# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# The pure parts of the library (wire formats, USB descriptors and framing, when the wired link
# sends reports, lizard mode, the mode rules, the retained area, settings, fusion) under ztest. They
# need no radio, so this suite runs on native_sim or any QEMU target:
#   west twister -T tests -p native_sim
# (The command-channel tests need stand-ins for the rest of the library: they run on a PC only,
# tests/host.)
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
	${OPENSC2_ROOT}/lib/opensc2/usb_desc.c
	${OPENSC2_ROOT}/lib/opensc2/usb_rules.c
	${OPENSC2_ROOT}/lib/opensc2/mode_rules.c
	${OPENSC2_ROOT}/lib/opensc2/retained.c
	${OPENSC2_ROOT}/lib/opensc2/id_rules.c
	${OPENSC2_ROOT}/lib/opensc2/dock_codec.c
	${OPENSC2_ROOT}/lib/opensc2/dock_rules.c
)
