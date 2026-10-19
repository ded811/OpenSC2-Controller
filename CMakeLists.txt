# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# The OpenSC2 Zephyr module (see zephyr/module.yml). A project brings in this repository (a plain
# folder named in ZEPHYR_EXTRA_MODULES, a git submodule, or a west workspace: docs/integration.md)
# and sets CONFIG_OPENSC2=y; without it no OpenSC2 code is built.

if(CONFIG_OPENSC2)
  zephyr_include_directories(include)
  add_subdirectory(lib/opensc2)
endif()
