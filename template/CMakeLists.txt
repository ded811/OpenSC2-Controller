# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20.0)

# The OpenSC2 library is a Zephyr module: the OpenSC2-Controller repository. This project finds it
# in one of these places:
#  1. your west workspace, when your west.yml lists it: nothing to do here;
#  2. ZEPHYR_EXTRA_MODULES, in the environment or as -DZEPHYR_EXTRA_MODULES=<folder>;
#  3. a git submodule (or a copy) at modules/opensc2 in this folder;
#  4. the OpenSC2-Controller repository around this folder (template/ as shipped).
# docs/integration.md in the OpenSC2-Controller repository explains each way. A module found
# twice is harmless (Zephyr keys modules by name).
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/modules/opensc2/zephyr/module.yml)
  list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/modules/opensc2)
elseif(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../zephyr/module.yml AND
       EXISTS ${CMAKE_CURRENT_LIST_DIR}/../include/opensc2/opensc2.h)
  list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/..)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
# Rename the project if you like; the build names the .uf2's folder after this folder, not after
# the project.
project(my_controller LANGUAGES C)

target_sources(app PRIVATE src/main.c)
