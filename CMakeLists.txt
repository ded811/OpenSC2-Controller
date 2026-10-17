# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# The OpenSC2 Zephyr module (see zephyr/module.yml). Applications add this repository with
# ZEPHYR_EXTRA_MODULES or a west manifest and set CONFIG_OPENSC2=y.

if(CONFIG_OPENSC2)
  zephyr_include_directories(include)
  add_subdirectory(lib/opensc2)
endif()
