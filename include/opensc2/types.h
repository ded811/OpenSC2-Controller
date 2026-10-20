/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Data types of the OpenSC2 library: inputs, outputs, identity and bonds. Plain C with no Zephyr
 * dependency, so the encoders in <opensc2/codec.h> can also be tested on a PC.
 *
 * Units and ranges are the ones of the Steam Controller 2's state report 0x42; see docs/api.md
 * for a table of every field and where it shows up in Steam.
 */
#ifndef OPENSC2_TYPES_H_
#define OPENSC2_TYPES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <dt-bindings/opensc2/inputs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENSC2_BIT(n) (1UL << (n))

/* ---- Buttons and touch bits (the 32-bit button word of report 0x42) -------------------------- */

#define OPENSC2_BTN_A                   OPENSC2_BIT(OPENSC2_KEY_A)
#define OPENSC2_BTN_B                   OPENSC2_BIT(OPENSC2_KEY_B)
#define OPENSC2_BTN_X                   OPENSC2_BIT(OPENSC2_KEY_X)
#define OPENSC2_BTN_Y                   OPENSC2_BIT(OPENSC2_KEY_Y)
#define OPENSC2_BTN_QAM                 OPENSC2_BIT(OPENSC2_KEY_QAM)
#define OPENSC2_BTN_R3                  OPENSC2_BIT(OPENSC2_KEY_R3)
#define OPENSC2_BTN_VIEW                OPENSC2_BIT(OPENSC2_KEY_VIEW)
#define OPENSC2_BTN_R4                  OPENSC2_BIT(OPENSC2_KEY_R4)
#define OPENSC2_BTN_R5                  OPENSC2_BIT(OPENSC2_KEY_R5)
#define OPENSC2_BTN_R                   OPENSC2_BIT(OPENSC2_KEY_R)
#define OPENSC2_BTN_DPAD_DOWN           OPENSC2_BIT(OPENSC2_KEY_DPAD_DOWN)
#define OPENSC2_BTN_DPAD_RIGHT          OPENSC2_BIT(OPENSC2_KEY_DPAD_RIGHT)
#define OPENSC2_BTN_DPAD_LEFT           OPENSC2_BIT(OPENSC2_KEY_DPAD_LEFT)
#define OPENSC2_BTN_DPAD_UP             OPENSC2_BIT(OPENSC2_KEY_DPAD_UP)
#define OPENSC2_BTN_MENU                OPENSC2_BIT(OPENSC2_KEY_MENU)
#define OPENSC2_BTN_L3                  OPENSC2_BIT(OPENSC2_KEY_L3)
#define OPENSC2_BTN_STEAM               OPENSC2_BIT(OPENSC2_KEY_STEAM)
#define OPENSC2_BTN_L4                  OPENSC2_BIT(OPENSC2_KEY_L4)
#define OPENSC2_BTN_L5                  OPENSC2_BIT(OPENSC2_KEY_L5)
#define OPENSC2_BTN_L                   OPENSC2_BIT(OPENSC2_KEY_L)
#define OPENSC2_TOUCH_RIGHT_STICK       OPENSC2_BIT(OPENSC2_KEY_RIGHT_STICK_TOUCH)
#define OPENSC2_TOUCH_RIGHT_PAD         OPENSC2_BIT(OPENSC2_KEY_RIGHT_PAD_TOUCH)
#define OPENSC2_CLICK_RIGHT_PAD         OPENSC2_BIT(OPENSC2_KEY_RIGHT_PAD_CLICK)
#define OPENSC2_CLICK_RIGHT_TRIGGER     OPENSC2_BIT(OPENSC2_KEY_RIGHT_TRIGGER_CLICK)
#define OPENSC2_TOUCH_LEFT_STICK        OPENSC2_BIT(OPENSC2_KEY_LEFT_STICK_TOUCH)
#define OPENSC2_TOUCH_LEFT_PAD          OPENSC2_BIT(OPENSC2_KEY_LEFT_PAD_TOUCH)
#define OPENSC2_CLICK_LEFT_PAD          OPENSC2_BIT(OPENSC2_KEY_LEFT_PAD_CLICK)
#define OPENSC2_CLICK_LEFT_TRIGGER      OPENSC2_BIT(OPENSC2_KEY_LEFT_TRIGGER_CLICK)
#define OPENSC2_TOUCH_RIGHT_GRIP        OPENSC2_BIT(OPENSC2_KEY_RIGHT_GRIP_TOUCH)
#define OPENSC2_TOUCH_LEFT_GRIP         OPENSC2_BIT(OPENSC2_KEY_LEFT_GRIP_TOUCH)

/* The 20 physical buttons (bits 0..19). */
#define OPENSC2_BUTTONS_MASK            0x000FFFFFUL

/* ---- Inputs --------------------------------------------------------------------------------- */

enum opensc2_side {
	OPENSC2_LEFT = 0,
	OPENSC2_RIGHT = 1,
};

/* Full scale of the signed axes: sticks and trackpad positions run from -32767 to 32767. */
#define OPENSC2_AXIS_MAX     32767
/* Full scale of triggers and trackpad pressure: 0 to 32767. */
#define OPENSC2_TRIGGER_MAX  32767

struct opensc2_stick {
	int16_t x;   /* -32767 (left) .. 32767 (right) */
	int16_t y;   /* -32767 (down) .. 32767 (up) */
};

struct opensc2_trackpad {
	int16_t x;          /* -32767 (left) .. 32767 (right); 0 while not touched */
	int16_t y;          /* -32767 (bottom) .. 32767 (top): up is positive; 0 while not touched */
	uint16_t pressure;  /* 0 .. 32767 */
	bool touched;       /* sets the pad's touch bit */
	bool clicked;       /* sets the pad's click bit */
};

/* IMU scale factors (report 0x42 units). */
#define OPENSC2_ACCEL_PER_G          16384   /* 32767 = 2 g */
#define OPENSC2_GYRO_FULL_SCALE_DPS  2000    /* 32767 = 2000 degrees per second */
#define OPENSC2_QUAT_ONE             32767   /* 32767 = 1.0 */

struct opensc2_imu {
	uint32_t timestamp_us;  /* time of the sample in microseconds, wrapping */
	int16_t accel[3];       /* X, Y, Z; 16384 per g */
	int16_t gyro[3];        /* X, Y, Z; 32767 = 2000 deg/s */
	int16_t quat[4];        /* orientation quaternion W, X, Y, Z; 32767 = 1.0 (all 0: none) */
};

struct opensc2_state {
	uint32_t buttons;                 /* OPENSC2_BTN_* and touch bits */
	uint16_t trigger[2];              /* [OPENSC2_LEFT], [OPENSC2_RIGHT]: 0 .. 32767 */
	struct opensc2_stick stick[2];
	struct opensc2_trackpad pad[2];
	struct opensc2_imu imu;
};

/* Battery, input report 0x43 (and vendor command 0xBE). Charge states as SDL's EChargeState. */
enum opensc2_charge_state {
	OPENSC2_CHARGE_RESET = 0,
	OPENSC2_CHARGE_DISCHARGING = 1,
	OPENSC2_CHARGE_CHARGING = 2,
	OPENSC2_CHARGE_SRC_VALIDATE = 3,
	OPENSC2_CHARGE_DONE = 4,
};

/* Added to the charge state while the charger has given up on a supply that does not charge the
 * battery (the real controller tries again 60 s later). Report 0x43 carries it; while it is set
 * and the level is above 0, the LED and the system state machine are told 100 %. */
#define OPENSC2_CHARGE_GAVE_UP 0x80

struct opensc2_battery {
	uint8_t charge_state;        /* enum opensc2_charge_state */
	uint8_t level;               /* state of charge, percent 0..100 */
	uint16_t battery_mv;
	uint16_t system_mv;
	uint16_t input_mv;
	int16_t current_ma;          /* battery current, positive while charging */
	uint16_t input_current_ma;
	uint16_t temperature;        /* raw value, as a charger reports it */
	uint8_t gauge_flags;         /* only in vendor command 0xBE */
};

/* ---- Outputs -------------------------------------------------------------------------------- */

/* Haptic channels (the four actuators of the real controller). */
#define OPENSC2_HAPTIC_LEFT_PAD    OPENSC2_BIT(0)
#define OPENSC2_HAPTIC_RIGHT_PAD   OPENSC2_BIT(1)
#define OPENSC2_HAPTIC_LEFT_GRIP   OPENSC2_BIT(2)
#define OPENSC2_HAPTIC_RIGHT_GRIP  OPENSC2_BIT(3)

/* Output report 0x80: classic two-motor rumble. The host repeats it to keep rumbling. */
struct opensc2_rumble {
	uint16_t left_speed;     /* 0..65535, the "low frequency" motor; 0 stops it */
	uint16_t right_speed;    /* 0..65535, the "high frequency" motor */
	uint16_t intensity;      /* level step; the real controller maps 16..16000 to +16..-12 dB */
	int8_t left_gain_db;     /* sent by the host, ignored by current controller firmware */
	int8_t right_gain_db;
	uint8_t type;
	bool timeout;            /* set when the library stops a rumble the host did not repeat */
};

enum opensc2_haptic_kind {
	OPENSC2_HAPTIC_PULSE,        /* 0x81 */
	OPENSC2_HAPTIC_COMMAND,      /* 0x82 */
	OPENSC2_HAPTIC_TONE,         /* 0x83 */
	OPENSC2_HAPTIC_SWEEP,        /* 0x84 */
	OPENSC2_HAPTIC_SCRIPT,       /* 0x85 */
	OPENSC2_HAPTIC_PCM_CONTROL,  /* 0x86 */
	OPENSC2_HAPTIC_PCM_DATA,     /* 0x87, 0x88, 0x89 */
};

enum opensc2_haptic_command {
	OPENSC2_HAPTIC_CMD_STOP_ALL = 0,
	OPENSC2_HAPTIC_CMD_CLICK_A = 1,
	OPENSC2_HAPTIC_CMD_CLICK_B = 2,
};

enum opensc2_pcm_operation {
	OPENSC2_PCM_NOTHING = 0,
	OPENSC2_PCM_STOP = 1,
	OPENSC2_PCM_SET_FORMAT = 2,
};

enum opensc2_pcm_encoding {
	OPENSC2_PCM_S16LE = 0,
	OPENSC2_PCM_S8 = 1,
	OPENSC2_PCM_MULAW = 2,
	OPENSC2_PCM_UNKNOWN = 3,
};

struct opensc2_haptic {
	enum opensc2_haptic_kind kind;
	uint8_t report_id;
	uint8_t target;     /* the target byte as sent */
	uint8_t channels;   /* OPENSC2_HAPTIC_* mask decoded from the target (0: none) */
	union {
		struct {
			uint16_t on_us, off_us, count;
		} pulse;
		struct {
			uint8_t command;     /* enum opensc2_haptic_command */
			int8_t gain_db;
		} command;
		struct {
			int8_t gain_db;
			uint16_t freq_hz;
			uint16_t duration_ms;
			uint16_t lfo_hz;
			uint8_t lfo_depth;   /* percent */
		} tone;
		struct {
			int8_t gain_db;
			uint16_t duration_ms;
			uint16_t start_hz, end_hz;
			bool antiphase;      /* targets 6 and 7: the right channel plays inverted */
		} sweep;
		struct {
			uint8_t id;          /* built-in script 1..17 of the real controller */
			int8_t gain_db;
		} script;
		struct {
			uint8_t operation;   /* enum opensc2_pcm_operation */
			uint8_t format;      /* raw format byte */
			uint8_t encoding;    /* enum opensc2_pcm_encoding */
			uint16_t rate_hz;    /* 8000, 4000, 2000 or 1000 */
		} pcm_control;
		struct {
			const uint8_t *data; /* valid during the callback only */
			uint8_t len;
		} pcm;
	};
};

enum opensc2_output_type {
	OPENSC2_OUTPUT_NONE,
	OPENSC2_OUTPUT_RUMBLE,
	OPENSC2_OUTPUT_HAPTIC,
};

struct opensc2_output {
	enum opensc2_output_type type;
	union {
		struct opensc2_rumble rumble;
		struct opensc2_haptic haptic;
	};
};

/* The status LED colour Steam sets with vendor command 0xC5, and the settings that go with it. */
struct opensc2_led {
	uint8_t red, green, blue, white;  /* percent 0..100 */
	bool override;     /* controller setting 37: show this colour instead of the status pattern */
	uint8_t brightness;  /* controller setting 45, percent 0..100 (default 50) */
};

/* ---- Identity -------------------------------------------------------------------------------- */

#define OPENSC2_SERIAL_LEN        13   /* exactly 13 characters, 0-9 and A-Z */
#define OPENSC2_GIT_REVISION_MAX  12   /* the identity record has 13 bytes, NUL included */
#define OPENSC2_PRODUCT_ID        0x1302
#define OPENSC2_PRODUCT_NAME_MAX  31   /* the USB product string: a 64-byte string descriptor */

struct opensc2_identity {
	char unit_serial[OPENSC2_SERIAL_LEN + 1];    /* the serial Steam shows */
	char board_serial[OPENSC2_SERIAL_LEN + 1];   /* the PCBA serial */
	char git_revision[OPENSC2_GIT_REVISION_MAX + 1];
	uint16_t product_id;           /* 0x1302 for a Steam Controller 2 */
	uint32_t board_revision;       /* hardware ID; real controllers: 0x45, 0x48..0x4B */
	uint32_t build_time;           /* firmware build time, Unix seconds */
	uint32_t bootloader_build_time;  /* 0 when there is no bootloader to report */
	/* The wired USB device's product string ("Steam Controller" on the real controller); 1 to 31
	 * printable ASCII characters, no space at either end. Never sent over the radio. */
	char product_name[OPENSC2_PRODUCT_NAME_MAX + 1];
};

/* The 57-byte identity record (radio packet 0xF5). */
#define OPENSC2_IDENTITY_RECORD_LEN 57

/* ---- Bonds ----------------------------------------------------------------------------------- */

#define OPENSC2_BOND_SLOTS 2

/* A pairing with one puck, 24 bytes little endian (also the vendor command 0xA2 format). */
struct opensc2_bond {
	uint32_t puck_id;          /* chosen by the puck */
	uint32_t controller_id;    /* chosen by the controller when pairing */
	char puck_serial[16];      /* the puck's serial, zero padded; informational */
};

#define OPENSC2_BOND_LEN 24

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_TYPES_H_ */
