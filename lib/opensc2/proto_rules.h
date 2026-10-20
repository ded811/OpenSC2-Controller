/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * proto_rules.h: small rules of the vendor commands that are pure logic, so the host tests run
 * them as the library does: the answers in which the real controller reports something other than
 * the value it keeps (0x89 for the IMU's settings, 0xE9's read-back of the LED colour).
 */
#ifndef OPENSC2_PROTO_RULES_H_
#define OPENSC2_PROTO_RULES_H_

#include <stdbool.h>
#include <stdint.h>

/* 0x89: what the answer carries for setting `number` holding `value`. Settings 48 (IMU mode), 84
 * (gyro bias subtraction) and 85 (gyro threshold) are handed to the IMU on the real controller,
 * which keeps no copy, so 0x89 answers their defaults whatever was written; the library keeps the
 * written values for the application (opensc2_setting_get()). Every other setting answers its
 * value. */
int16_t osc2_setting_answer(uint8_t number, int16_t value);

/* 0xE9: one channel of the LED override colour (a percentage) as the real controller reads it
 * back. Its conversion truncates before it scales, so a channel reads 100 when it is exactly 100
 * and 0 otherwise. The library copies that, since a host can see it (docs/protocol.md, gaps). */
uint8_t osc2_led_readback(uint8_t percent);

#endif /* OPENSC2_PROTO_RULES_H_ */
