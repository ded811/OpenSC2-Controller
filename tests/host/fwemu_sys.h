/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * fwemu_sys.h: between the firmware emulator (fwemu.c) and its system state machine
 * (fwemu_sys.c).
 */
#ifndef OSC2_HOST_FWEMU_SYS_H_
#define OSC2_HOST_FWEMU_SYS_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/opensc2.h>

/* fwemu_sys.c */
bool emu_sys_boot_wireless(void);          /* the next boot is wireless by a boot injection */
void emu_sys_boot(bool warm);              /* at every emulated boot, the mode decided */
void emu_sys_process(void);                /* every millisecond, and after every request */
void emu_sys_battery(const struct opensc2_battery *battery);   /* every battery report */
bool emu_sys_power(enum opensc2_power_request request);       /* true: the machine took it */
void emu_sys_link(uint8_t state, uint8_t reason);
void emu_sys_host_asleep(void);

/* fwemu.c */
void emu_reboot(void);                     /* a plain reboot, now */
void emu_radio(bool on);                   /* the radio link on (connected at once) or off */

#endif /* OSC2_HOST_FWEMU_SYS_H_ */
