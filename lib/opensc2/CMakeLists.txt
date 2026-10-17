# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT

zephyr_library_named(opensc2)

zephyr_library_sources(
	codec.c
	core.c
	commands.c
	lizard.c
	radio.c
	settings_defaults.c
	storage.c
)
zephyr_library_sources_ifdef(CONFIG_OPENSC2_SHELL shell.c)
zephyr_library_sources_ifdef(CONFIG_OPENSC2_FUSION fusion.c)

# The build time reported to Steam when CONFIG_OPENSC2_BUILD_TIME is 0: when CMake configured
# this build (Unix seconds, UTC).
string(TIMESTAMP OPENSC2_NOW "%s" UTC)
zephyr_library_compile_definitions(OPENSC2_CMAKE_BUILD_TIME=${OPENSC2_NOW}u)
