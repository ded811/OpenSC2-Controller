/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The 86 numbered "controller settings" Steam writes with vendor command 0x87 and reads with 0x89.
 * They live in RAM and start from their defaults at every boot, as on the real controller. The
 * numbers match the setting names in SDL's controller_constants.h, but the real controller gives
 * many of them a different job; the names below say what the controller does with them, with
 * SDL's name in the comment.
 */
#ifndef OPENSC2_SETTINGS_H_
#define OPENSC2_SETTINGS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENSC2_SETTINGS_COUNT 86
/* Passed to the setting_changed callback after vendor command 0x8E restored every default. */
#define OPENSC2_SETTINGS_ALL   0xFF

#define OPENSC2_SETTING_PAD_ROTATION_ANGLE    2   /* TRACKBALL_ROTATION_ANGLE */
#define OPENSC2_SETTING_LIZARD_MODE           9   /* LIZARD_MODE: 1 on (default), 0 off */
#define OPENSC2_SETTING_MOMENTUM_MIN_VEL      11  /* MINIMUM_MOMENTUM_VEL */
#define OPENSC2_SETTING_MOMENTUM_DECAY        12  /* MOMENTUM_DECAY_AMOUNT */
#define OPENSC2_SETTING_HAPTIC_INCREMENT      14  /* HAPTIC_INCREMENT */
#define OPENSC2_SETTING_PAD_ROTATION          24  /* SMOOTH_ABSOLUTE_MOUSE */
#define OPENSC2_SETTING_RUMBLE_GAIN_DB        27  /* TRACKPAD_OUTER_RADIUS: rumble gain, dB */
#define OPENSC2_SETTING_STICK_TOUCH_ON        30  /* TRACKPAD_OUTER_SPIN_VEL */
#define OPENSC2_SETTING_STICK_TOUCH_OFF       31  /* TRACKPAD_OUTER_SPIN_RADIUS */
#define OPENSC2_SETTING_GRIP_SENSITIVITY      34  /* RELATIVE_MODE_MAX_VEL */
#define OPENSC2_SETTING_GRIP_RELEASE          35  /* RELATIVE_MODE_INVERT_Y */
#define OPENSC2_SETTING_IR_LED_BRIGHTNESS     36  /* DOUBLE_TAP_BEEP_ENABLED */
#define OPENSC2_SETTING_LED_OVERRIDE          37  /* DOUBLE_TAP_BEEP_PERIOD: show 0xC5's colour */
#define OPENSC2_SETTING_LED_CURVE             38  /* DOUBLE_TAP_BEEP_COUNT */
#define OPENSC2_SETTING_LED_BRIGHTNESS        45  /* LED_USER_BRIGHTNESS, 0..100 */
#define OPENSC2_SETTING_RAW_STICKS            46  /* ENABLE_RAW_JOYSTICK: 1 mV, 2 ADC counts */
#define OPENSC2_SETTING_IMU_MODE              48  /* IMU_MODE, OPENSC2_IMU_MODE_* bits */
#define OPENSC2_SETTING_SLEEP_TIMEOUT_S       50  /* SLEEP_INACTIVITY_TIMEOUT, seconds */
#define OPENSC2_SETTING_LEFT_CLICK_PRESSURE   52  /* LEFT_TRACKPAD_CLICK_PRESSURE, percent */
#define OPENSC2_SETTING_RIGHT_CLICK_PRESSURE  53  /* RIGHT_TRACKPAD_CLICK_PRESSURE, percent */
#define OPENSC2_SETTING_RAW_PRESSURE          60  /* PRESSURE_MODE */
#define OPENSC2_SETTING_RAW_TRIGGERS          62  /* TRIGGER_MODE */
#define OPENSC2_SETTING_PAD_FILTER            65  /* TRACKPAD_FILT_CTRL */
#define OPENSC2_SETTING_PAD_CLIP              66  /* TRACKPAD_CLIP */
#define OPENSC2_SETTING_TRIGGER_THRESHOLD     68  /* TRIGGER_THRESHOLD_PERCENT, 40..99 */
#define OPENSC2_SETTING_HAPTICS_ENABLED       70  /* HAPTICS_ENABLED */
#define OPENSC2_SETTING_STEAM_WATCHDOG        71  /* STEAM_WATCHDOG_ENABLE */
#define OPENSC2_SETTING_PAD_TOUCH_ON          72  /* TIMP_TOUCH_THRESHOLD_ON */
#define OPENSC2_SETTING_PAD_TOUCH_OFF         73  /* TIMP_TOUCH_THRESHOLD_OFF */
#define OPENSC2_SETTING_AMPLIFIER_MODE        75  /* TEST_CONTROL */
#define OPENSC2_SETTING_HAPTIC_GAIN_DB        76  /* HAPTIC_MASTER_GAIN_DB */
#define OPENSC2_SETTING_REBOOT_ON_OFF         77  /* THUMB_TOUCH_THRESH */
#define OPENSC2_SETTING_PAD_STABILIZER        80  /* STABILIZER_ENABLED */
#define OPENSC2_SETTING_PAD_IGNORE_ON_STICK   82  /* no SDL name: bit 0 left, bit 1 right */
#define OPENSC2_SETTING_IMU_BIAS              84  /* gyro bias subtraction */
#define OPENSC2_SETTING_IMU_THRESHOLD         85  /* gyro threshold */

/* Bits of setting 48 (SDL's SETTING_GYRO_MODE_*). SDL sets RAW_ACCEL | RAW_GYRO for IMU data. */
#define OPENSC2_IMU_MODE_OFF               0x00
#define OPENSC2_IMU_MODE_STEERING          0x01
#define OPENSC2_IMU_MODE_TILT              0x02
#define OPENSC2_IMU_MODE_SEND_ORIENTATION  0x04
#define OPENSC2_IMU_MODE_SEND_RAW_ACCEL    0x08
#define OPENSC2_IMU_MODE_SEND_RAW_GYRO     0x10

/* The default of a setting, as the protocol documentation gives it (not public yet;
 * docs/protocol.md lists the pages) for all 86; 0 for a number above 85. The defaults of 28 and 29
 * (16500, 15000) lie above their range, as on the real controller. */
int16_t opensc2_setting_default(uint8_t number);

/* `value` clamped to the setting's range, as vendor command 0x87 stores it: the documentation gives
 * every setting's range (docs/api.md lists them). Settings 48, 84 and 85, which the real controller
 * hands to its IMU unclamped, keep any value, and so does a number above 85. */
int16_t opensc2_setting_clamp(uint8_t number, int16_t value);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_SETTINGS_H_ */
