/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The status LED, the real controller's way: the library selects a mode (opensc2_led_status(), and
 * the led_status callback when it changes), and the application plays it. The pattern player below
 * is optional, pure C: it turns the status into a colour every 10 ms with the real controller's
 * patterns, colours, fades and the fully-charged display, so an RGB(W) LED looks like the
 * controller's and a one-colour LED shows the rhythm (opensc2_led_player_envelope()). How the
 * patterns look, and where they come from: docs/api.md, Status LED.
 *
 *     static struct opensc2_led_player player;
 *
 *     every 10 ms:
 *         struct opensc2_led_status st;
 *         uint16_t rgbw[4];
 *
 *         opensc2_led_status(&st);
 *         opensc2_led_player_step(&player, &st, k_uptime_get_32(), rgbw);
 *         for each channel: duty = opensc2_led_duty(rgbw[i], trim[i], st.brightness);
 */
#ifndef OPENSC2_LED_H_
#define OPENSC2_LED_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The modes the system state machine selects (the real controller's numbers). */
enum opensc2_led_mode {
	OPENSC2_LED_MODE_NONE = 0,         /* nothing selected yet: dark (from start-up) */
	OPENSC2_LED_MODE_BLUETOOTH = 1,    /* blue, the link pattern */
	OPENSC2_LED_MODE_PUCK = 2,         /* white with a little blue, the link pattern */
	OPENSC2_LED_MODE_WIRED = 3,        /* green, a slow breathe */
	OPENSC2_LED_MODE_AMBER = 4,        /* amber: USB power without a host or with wireless off,
					    * docked with the radio off */
	OPENSC2_LED_MODE_OFF = 5,          /* shutdown, reboot, lifted off the dock */
	OPENSC2_LED_MODE_LOW_BATTERY = 6,  /* red flashes: the low-battery shutdown */
	OPENSC2_LED_MODE_KEYLOCK = 7,      /* white flashes: the keylock shutdown */
};

/* Everything a pattern depends on. */
struct opensc2_led_status {
	uint8_t mode;              /* enum opensc2_led_mode */
	bool external_power;       /* modes 1 and 2: shown as on USB power or the dock */
	bool steady;               /* a chord state: the transport's colour steady, as if connected */
	bool connected;            /* a wireless link is connected (either link) */
	bool pairing;              /* Bluetooth advertises in pairing mode, or the radio pairs over the
				    * air (opensc2_pair_start(), which the real controller lacks) */
	bool input_recent;         /* the idle monitor saw input within its timeout */
	bool shutting_down;        /* a shutdown or reboot runs: the IR LED is off too */
	uint8_t level;             /* the battery level the LED was told (255: none yet; counts as
				    * full) */
	uint8_t full_level;        /* user/fully_charged_led_intensity: 0 off, 1 dim, 2 medium, 3
				    * (or anything else) full */
	uint32_t below_full_ms;    /* uptime when the level last read below 100 % (0: never) */
	uint8_t brightness;        /* percent, setting 45 through the curve of setting 38 */
	bool override;             /* setting 37: show the 0xC5 colour instead */
	uint8_t override_rgbw[4];  /* that colour, percent (as sent: above 100 is brighter, up to
				    * full duty, as on the real controller) */
	bool update_window;        /* the 60 s update window runs (a bootloader request on battery
				    * or the dock): blink green */
	uint8_t update_requests;   /* bootloader requests that opened or restarted it so far
				    * (wraps); the player blinks once more for each new one */
};

/* The status now (the library's own, never NULL fields). */
void opensc2_led_status(struct opensc2_led_status *status);

/* ---- the pattern player (pure; one per LED) -------------------------------------------------- */

#define OPENSC2_LED_FULL_SCALE 1000   /* colours are thousandths of full duty */
#define OPENSC2_LED_STEP_MS    10     /* call opensc2_led_player_step() this often */

struct opensc2_led_segment {
	uint8_t kind;              /* internal */
	uint16_t ms;
	uint16_t rgbw[4];
};

struct opensc2_led_player {
	uint16_t shown[4];         /* the colour shown last */
	uint16_t from[4];          /* where the current fade started */
	struct opensc2_led_segment seg[6];
	uint8_t count, index;
	uint16_t peak;             /* the brightest channel of the cycle (for one-colour LEDs) */
	uint32_t seg_start;
	bool running;
	bool blinking;             /* the update window's green blink is playing */
	uint8_t updates_seen;      /* update requests taken (status.update_requests) */
};

void opensc2_led_player_init(struct opensc2_led_player *player);

/* The colour to show at `now_ms`, R, G, B, W in thousandths, before trims and brightness. A new
 * status shows once the cycle in progress is over, as on the real controller: within 0.1 s from
 * the dark display and the override; from the steady link display (connected on battery, a chord
 * state) within 0.1 s for a mode other than 1 or 2 and otherwise at the end of its 2 s cycle; up
 * to 10.3 s from the keylock pattern. The override also shows between the two fades of a
 * breathing pattern (within 1.8 s). The breathing patterns start every cycle from dark; the other
 * fades start from the colour last shown. An update request (status.update_requests) blinks green
 * (0.1 s fade in, 0.9 s on, 0.9 s dark) for as long as the update window runs, at least once,
 * before the override and the mode's pattern. */
void opensc2_led_player_step(struct opensc2_led_player *player,
			     const struct opensc2_led_status *status, uint32_t now_ms,
			     uint16_t rgbw[4]);

/* For a one-colour LED: how bright the pattern is now against its own peak, 0..1000 (switch the
 * LED on from 500). */
uint16_t opensc2_led_player_envelope(const struct opensc2_led_player *player);

/* The brightness in percent for controller setting 45 (0..100) and setting 38: with 38 not 0 (the
 * default 1) the perceptual curve b = 100 x (6^(s/100) - 1) / 5, cut to a whole number; with 0,
 * b = s. */
uint8_t opensc2_led_brightness(int16_t setting45, int16_t setting38);

/* A channel's duty in thousandths: colour x trim / 100 x brightness / 100, at most 1000. The
 * real controller's factory trims (percent) are 90, 35, 120 and 80 for R, G, B and W. */
#define OPENSC2_LED_TRIM_RED    90
#define OPENSC2_LED_TRIM_GREEN  35
#define OPENSC2_LED_TRIM_BLUE   120
#define OPENSC2_LED_TRIM_WHITE  80
uint16_t opensc2_led_duty(uint16_t colour, uint8_t trim, uint8_t brightness);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_LED_H_ */
