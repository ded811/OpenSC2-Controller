/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The firmware emulator only: the haptics synthesiser on its clock (haptics_emu.c), and the one
 * call it needs from fwemu.c.
 */
#ifndef OSC2_HOST_HAPTICS_EMU_H_
#define OSC2_HOST_HAPTICS_EMU_H_

#include <stddef.h>
#include <stdint.h>

/* fwemu.c: an input report as the application sends it (0x44). */
void fwemu_send(const uint8_t *report, size_t len);

/* At every boot, and every millisecond of the emulator's clock (uptime in microseconds). */
void haptics_emu_boot(uint64_t now_us);
void haptics_emu_tick(uint64_t now_us);

#endif /* OSC2_HOST_HAPTICS_EMU_H_ */
