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

/* status.c: the status LED, and an RGB LED that follows Steam's colour if one is wired. */
int status_init(void);
void status_link(enum opensc2_link_state state);
void status_led(const struct opensc2_led *led);

/* rumble.c: a vibration motor or LRA driver on a PWM channel, if one is wired. */
int rumble_init(void);
void rumble_set(const struct opensc2_rumble *rumble);
void rumble_haptic(const struct opensc2_haptic *haptic);

/* trackpad.c: touch devices of Zephyr's input subsystem as the trackpads, if chosen. */
int trackpad_init(void);

/* imu.c: a sensor of Zephyr's sensor API as the IMU, if aliased as imu0. */
int imu_init(void);

/* main.c */
void app_pair_button(void);
void app_button_pressed(void);

#endif /* APP_H_ */
