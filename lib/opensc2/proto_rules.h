/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * proto_rules.h: small rules of the vendor commands and reports that are pure logic, so the host
 * tests run them as the library does: the answers in which the real controller reports something
 * other than the value it keeps (0x89 for the IMU's settings, 0xE9's read-back of the LED colour,
 * 0xA1's encoding of the reset causes), and the haptic stream status reports a stop sends.
 */
#ifndef OPENSC2_PROTO_RULES_H_
#define OPENSC2_PROTO_RULES_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>

/* 0x89: what the answer carries for setting `number` holding `value`. Settings 48 (IMU mode), 84
 * (gyro bias subtraction) and 85 (gyro threshold) are handed to the IMU on the real controller,
 * which keeps no copy, so 0x89 answers their defaults whatever was written; the library keeps the
 * written values for the application (opensc2_setting_get()). Every other setting answers its
 * value. */
int16_t osc2_setting_answer(uint8_t number, int16_t value);

/* 0xE9: one channel of the LED override colour (a percentage) as the real controller reads it
 * back. Its conversion truncates before it scales: below 100 a channel reads 0, from 100 to 199
 * it reads 100 and from 200 on 200 (the real controller keeps 0xC5's bytes unclamped). The library
 * copies that, since a host can see it (docs/protocol.md, gaps); it keeps the colour clamped to
 * 100, so its answers are 0 or 100. */
uint8_t osc2_led_readback(uint8_t percent);

/* 0xA1: the real controller's byte of reset-cause flags from Zephyr's hwinfo causes (RESET_*):
 * 0x01 power-on, 0x02 brown-out, 0x08 debug, a low-power wake, a CPU lock-up or any other cause,
 * 0x10 the reset pin, 0x20 the watchdog, 0x40 software. Nothing clears the chip's register, so on
 * the nRF52 the flags of several resets add up until a power-on, as on the real controller. */
#define OSC2_RESET_POWER_ON   0x01
#define OSC2_RESET_BROWNOUT   0x02
#define OSC2_RESET_OTHER      0x08
#define OSC2_RESET_PIN        0x10
#define OSC2_RESET_WATCHDOG   0x20
#define OSC2_RESET_SOFTWARE   0x40
uint8_t osc2_reset_flags(uint32_t causes);

/* The channel IDs of input report 0x44 (opensc2_send_haptic_status()). */
#define OSC2_STATUS_LEFT_GRIP   0
#define OSC2_STATUS_RIGHT_GRIP  1
#define OSC2_STATUS_LEFT_PAD    3
#define OSC2_STATUS_RIGHT_PAD   4
/* Flag 0x02 of report 0x44: the stream ran dry, or was stopped. */
#define OSC2_STATUS_STOPPED     0x02u

/* The 0x44 replies a decoded output report brings on the real controller. Every stop-all (0x81
 * with an on time of 0, 0x82 command 0, a speed of 0 in 0x80 for that grip) stops the stream of
 * each channel it reaches, and so does 0x86's stream stop; stopping a stream reports flag 0x02
 * for its channel, streaming or not. The replies come in the order the real controller queues the
 * stops: the left channel first for 0x81 and 0x82, the right first for 0x86, the right grip before
 * the left for 0x80. Writes the channel IDs (OSC2_STATUS_*) to `ids`; returns how many (0 to 4). */
uint8_t osc2_stop_status(const struct opensc2_output *out, uint8_t ids[4]);

#endif /* OPENSC2_PROTO_RULES_H_ */
