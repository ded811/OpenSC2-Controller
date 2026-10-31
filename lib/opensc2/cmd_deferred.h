/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * cmd_deferred.h: what vendor commands leave for after their answer (cmd_deferred.c), with the
 * real controller's timing. The host tests record the calls instead (tests/host/stubs.c).
 */
#ifndef OPENSC2_CMD_DEFERRED_H_
#define OPENSC2_CMD_DEFERRED_H_

#include <opensc2/opensc2.h>

/* A host's power request (0x90, 0x95, 0x9F): 20 ms from the first request still waiting, the last
 * request asked for; a turn-off is dropped then while wired. */
void osc2_cmd_power(enum opensc2_power_request request, const char *why);
/* The factory reset (0x86 with its key): flash deleted 10 ms later, the reboot 100 ms after. */
void osc2_cmd_factory_reset(void);
/* 0xA2: `bond` stored as the first bond for the next boot, after the answer (both IDs needed, else
 * the stored one is deleted); the bond in use stays. */
void osc2_cmd_bond_write(const struct opensc2_bond *bond);

#endif /* OPENSC2_CMD_DEFERRED_H_ */
