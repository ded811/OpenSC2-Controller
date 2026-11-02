/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * app.h: the parts of the blank-canvas firmware. Each hardware part is optional and set up from
 * the board overlay (boards/promicro_nrf52840_nrf52840_uf2.overlay).
 */
#ifndef APP_H_
#define APP_H_

#include <opensc2/opensc2.h>

/* inputs.c: buttons (gpio-keys) and sticks and triggers (ADC). */
int inputs_init(void);
/* CONFIG_APP_ANALOG_SAMPLE: the sticks and triggers, read in the library's `sample` callback. */
void inputs_sample(struct opensc2_state *state);
/* Before powering off: STEAM (without a STEAM key, every key) as the pin that wakes the chip.
 * Returns how many pins were set up. */
int inputs_wake_pins(void);

/* status.c: the status LED (the library's LED status through its pattern player), an RGB(W) LED
 * and an IR LED if they are wired. */
int status_init(void);
void status_mode(enum opensc2_mode mode);
void status_link(enum opensc2_link_state state);
/* The link is Bluetooth's (its own patterns, in blue), set before status_link(). */
void status_bluetooth(bool on);
void status_usb(enum opensc2_usb_state state);
void status_led(const struct opensc2_led *led);
/* A controller setting changed (36 is the IR LED's brightness). */
void status_setting(uint8_t number, int16_t value);
/* Wired, the pairing button held 3 s: the LED stays dark ("release now") while `on`. */
void status_hold_cue(bool on);
/* Everything dark, right before the board powers off. */
void status_off(void);

/* rumble.c: a vibration motor or LRA driver on a PWM channel, if one is wired. */
int rumble_init(void);
void rumble_set(const struct opensc2_rumble *rumble);
void rumble_haptic(const struct opensc2_haptic *haptic);

/* trackpad.c: touch devices of Zephyr's input subsystem as the trackpads, if chosen. */
int trackpad_init(void);

/* imu.c: a sensor of Zephyr's sensor API as the IMU, if aliased as imu0 (imu_fifo.c with
 * CONFIG_APP_IMU_STREAM). A controller setting changed: the IMU's own (48) are handed over. */
int imu_init(void);
void imu_setting(uint8_t number, int16_t value);
/* imu_board.c: the real controller's set-up of an LSM6DSV16X (stream build), before the stream. */
void imu_board_setup(void);


/* battery.c: report 0x43's battery: the level (CONFIG_APP_BATTERY) and a charger's status pin
 * (zephyr,user charger-status-gpios), as far as the board has them. */
int battery_init(void);

/* main.c */
void app_pair_button(bool pressed);
void app_button_pressed(void);

#endif /* APP_H_ */
