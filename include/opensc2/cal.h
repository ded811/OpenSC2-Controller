/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * cal.h: calibration of the sticks, the triggers and the trackpads' pressure, as the real
 * controller does it, following the protocol documentation (not public yet; docs/protocol.md lists
 * the pages).
 *
 * Two parts:
 *   - pure helpers (no kernel): the calibration keys' byte formats, the firmware's millivolts, the
 *     processing of a stick, a trigger and a pad's pressure from a stored calibration, and the
 *     factory recordings behind vendor commands 0xD8, 0xC0 and 0xC3. Applications may use them
 *     directly; the host tests check each against the documented rules;
 *   - the library's calibration (CONFIG_OPENSC2_CAL): the stored keys (cal/joy_l, cal/trg_l,
 *     cal/prs_l, ... kept under "opensc2/cal/" in Zephyr settings), the commands that read and
 *     write them, and raw inputs: an application that gives the library raw ADC counts and raw
 *     pressure (opensc2_set_stick_raw() and friends) gets the real controller's processing,
 *     controller settings 46, 60 and 62 and the factory recordings, once per state report.
 *
 * Units, as on the real controller: a stick or trigger reading is the average of a burst of 12-bit
 * conversions (0..4095). The stored calibration is in "millivolts": counts x 3300 / 4096 for a
 * stick (full scale VDD) and counts x 1650 / 4096 for a trigger (full scale VDD / 2), whatever the
 * supply. Pad pressure and the capacitive words are in the sensor's own units (signed 16-bit).
 */
#ifndef OPENSC2_CAL_H_
#define OPENSC2_CAL_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Millivolts ------------------------------------------------------------------------------ */

#define OPENSC2_CAL_ADC_MAX         4095   /* a 12-bit reading */
#define OPENSC2_CAL_STICK_FULL_MV   3300   /* a stick's full scale, VDD, counted as 3.3 V */
#define OPENSC2_CAL_TRIGGER_FULL_MV 1650   /* a trigger's full scale, VDD / 2 */

/* counts x full_mv / 4096, rounded down (a reading above 4095 counts as 4095). */
uint16_t opensc2_cal_counts_to_mv(uint16_t counts, uint16_t full_mv);
/* mv x 4095 / (full_mv - 1), truncated and limited to 0..65535: back through the channel's
 * full-scale reading, as the stored calibration is turned into counts. */
uint16_t opensc2_cal_mv_to_counts(uint32_t mv, uint16_t full_mv);

/* ---- The calibration keys -------------------------------------------------------------------- */

#define OPENSC2_CAL_STICK_LEN      18   /* cal/joy_l, cal/joy_r */
#define OPENSC2_CAL_TRIGGER_LEN    7    /* cal/trg_l, cal/trg_r */
#define OPENSC2_CAL_PRESSURE_LEN   8    /* cal/prs_l, cal/prs_r */
#define OPENSC2_CAL_TOUCH_LEN      10   /* cal/touch_l, cal/touch_r (layout unknown, unused) */
#define OPENSC2_CAL_GYRO_BIAS_LEN  12   /* cal/sensors/gyroscope/bias: three floats */

/* The format words that make a key usable. A stick or trigger key with any other format word is
 * uncalibrated; 2 and 0x81 behave alike. */
#define OPENSC2_CAL_FORMAT_ANALOG          2
#define OPENSC2_CAL_FORMAT_ANALOG_ALT      0x81
#define OPENSC2_CAL_FORMAT_PRESSURE        1      /* range A..B */
#define OPENSC2_CAL_FORMAT_PRESSURE_SPAN   0x80   /* range 0..B-A */
#define OPENSC2_CAL_PRESSURE_GAIN_ONE      2000   /* gain C: 2000 = 1 */

/* One stick axis, mV: the extremes seen while circling the stick, the rest band seen released. */
struct opensc2_cal_axis {
	uint16_t min, max;
	uint16_t rest_low, rest_high;
};

/* cal/joy_l, cal/joy_r: u16 format, then X and Y (min, max, rest low, rest high), little endian. */
struct opensc2_cal_stick {
	uint16_t format;
	struct opensc2_cal_axis x, y;
};

/* cal/trg_l, cal/trg_r: u16 format, u16 A (the higher mV), u16 B (the lower), u8 direction (0:
 * the voltage rises when pulled, B released and A pulled; any other value: it falls). */
struct opensc2_cal_trigger {
	uint16_t format;
	uint16_t a_mv, b_mv;
	uint8_t direction;
};

/* cal/prs_l, cal/prs_r: u16 format, s16 A (zero point), s16 B (a firm press), u16 C (the output
 * gain in 1/2000; 0 uncalibrated). With format 1, A = 0x7FFF is uncalibrated too. */
struct opensc2_cal_pressure {
	uint16_t format;
	int16_t a, b;
	uint16_t gain;
};

void opensc2_cal_stick_pack(const struct opensc2_cal_stick *c, uint8_t out[OPENSC2_CAL_STICK_LEN]);
void opensc2_cal_stick_unpack(const uint8_t in[OPENSC2_CAL_STICK_LEN], struct opensc2_cal_stick *c);
void opensc2_cal_trigger_pack(const struct opensc2_cal_trigger *c,
			      uint8_t out[OPENSC2_CAL_TRIGGER_LEN]);
void opensc2_cal_trigger_unpack(const uint8_t in[OPENSC2_CAL_TRIGGER_LEN],
				struct opensc2_cal_trigger *c);
void opensc2_cal_pressure_pack(const struct opensc2_cal_pressure *c,
			       uint8_t out[OPENSC2_CAL_PRESSURE_LEN]);
void opensc2_cal_pressure_unpack(const uint8_t in[OPENSC2_CAL_PRESSURE_LEN],
				 struct opensc2_cal_pressure *c);

/* ---- Sticks -----------------------------------------------------------------------------------
 * Each axis is split at its centre (the middle of the rest band, rounded down) into two halves,
 * each scaled on its own so that full deflection comes 90 % of the way to the stored extreme (the
 * 10 % band at the outer end), truncated toward zero, -32767..32767. No dead zone and no filter.
 * The calibration is usable when its format is 2 or 0x81 and, on both axes, the maximum lies at
 * least 200 counts from rest high and the minimum at least 200 counts from rest low (either way
 * round, so a calibration can also turn an axis over); otherwise the stick reads 0, 0.
 */

struct opensc2_stick_axis {
	int32_t centre, min, max;   /* counts */
	int32_t follow;             /* the July range rule's low-pass copy, counts x 256 */
	bool follow_on;
};

struct opensc2_stick_proc {
	bool valid;
	struct opensc2_stick_axis axis[2];   /* X, Y */
	/* Options of OpenSC2's own, off (0, false) by default as on the real controller: */
	uint16_t dead_zone;   /* per axis, report units: |value| up to this reads 0, the rest is
			       * stretched back to full scale (for drifting potentiometer sticks) */
	bool grow_range;      /* July 2026's rule: each half's outer end follows a low-pass copy of
			       * the reading (factor 0.15) past the stored extreme, in RAM only;
			       * October keeps the stored range */
};

/* Set up `p` from a stored calibration (NULL: uncalibrated). The options are kept. */
void opensc2_stick_setup(struct opensc2_stick_proc *p, const struct opensc2_cal_stick *cal);
/* One axis (0 X, 1 Y) from its reading. With grow_range the range may move. */
int16_t opensc2_stick_axis_value(struct opensc2_stick_proc *p, int axis, uint16_t counts);
/* Both axes; 0, 0 while uncalibrated. */
void opensc2_stick_value(struct opensc2_stick_proc *p, uint16_t x_counts, uint16_t y_counts,
			 struct opensc2_stick *out);

/* ---- Triggers ---------------------------------------------------------------------------------
 * The released end is B (direction 0) or A; the pulled end is moved 2 % toward it (A x 0.98, or
 * B / 0.98). With d the distance pulled as a fraction of the travel, the output is
 * 32767 x (d - 0.05) / 0.9, truncated and limited to 0..32767: 0 over the first 5 %, full from
 * 95 % on. No filter. Usable with format 2 or 0x81 and A and B at least 100 counts apart;
 * otherwise 0.
 */

struct opensc2_trigger_proc {
	bool valid;
	bool falls;          /* direction 1: the voltage falls when pulled */
	int32_t a, b;        /* counts */
};

void opensc2_trigger_setup(struct opensc2_trigger_proc *p, const struct opensc2_cal_trigger *cal);
uint16_t opensc2_trigger_value(const struct opensc2_trigger_proc *p, uint16_t counts);

/* ---- Pad pressure -----------------------------------------------------------------------------
 * Each raw word updates a filter p = (3 p + raw) / 4 (toward zero; p starts at 0 and never starts
 * over). With the range lo..hi of the calibration (A..B, or 0..B-A for format 0x80) and
 * R = hi - lo, v = (p - lo - 0.05 R) / (0.9 R): a 5 % band at both ends, v not limited. The
 * pressure is 32767 x (C / 2000) x v, truncated and limited to 0..32767. The click follows v: on
 * above (s + 8) %, off below (s - 8) %, with s controller setting 52 (left) or 53 (right) and -1
 * counting as 100. Without a usable calibration the pressure is 0 and the pad never clicks.
 */

struct opensc2_pressure_proc {
	bool usable;
	int32_t lo, range;    /* raw units */
	uint16_t gain;
	int32_t filtered;     /* p */
	int16_t click_setting;
	bool click;
};

/* Set up from a stored calibration (NULL: unusable). The filter keeps its value; the click
 * clears. */
void opensc2_pressure_setup(struct opensc2_pressure_proc *p, const struct opensc2_cal_pressure *cal);
/* One raw word, with the side's click setting (52 or 53); a change of it clears the click. The
 * filter p follows the raw words only while the calibration is usable; otherwise it keeps its
 * value (and there is no click). */
void opensc2_pressure_sample(struct opensc2_pressure_proc *p, int16_t raw, int16_t click_setting);
/* The report's pressure for the filter's value. */
uint16_t opensc2_pressure_value(const struct opensc2_pressure_proc *p);
/* Setting 60 = 1: the raw word itself, below 0 counted as 0. */
uint16_t opensc2_pressure_raw_value(int16_t raw);

/* ---- The factory recordings (vendor commands 0xD8, 0xC0, 0xC3) -------------------------------
 * Each recording keeps its own step. The command sets the step; the samples of the frames that
 * follow fill the working records; step 3 hands the finished records over. Steps above 3 do
 * nothing; a step 2 or 3 without a step 1 since the recording ended does nothing.
 *   sticks (0xD8, the step in payload[1]): 1 rest band, 2 full circles, 3 save at the next frame,
 *     0 end without saving;
 *   triggers (0xC0, payload[0]): 1 released, 2 pulled, 3 save at the next frame (each trigger
 *     keeps the direction whose A and B lie further apart, direction 0 on a tie), 0 end;
 *   pressure (0xC3, payload[0]): 1 start, 2 the firm press (B rises to the highest raw word),
 *     3 save at once, 0 pause.
 * OpenSC2 saves only the sides that got at least one sample since step 1: a board that gives no
 * raw input for a side keeps that side's stored key.
 */

enum opensc2_cal_rec_kind {
	OPENSC2_CAL_REC_STICKS = 0,
	OPENSC2_CAL_REC_TRIGGERS = 1,
	OPENSC2_CAL_REC_PRESSURE = 2,
};

/* What opensc2_cal_rec_*() hand over: bits of keys whose records are finished. */
#define OPENSC2_CAL_SAVE_STICK_L     OPENSC2_BIT(0)
#define OPENSC2_CAL_SAVE_STICK_R     OPENSC2_BIT(1)
#define OPENSC2_CAL_SAVE_TRIGGER_L   OPENSC2_BIT(2)
#define OPENSC2_CAL_SAVE_TRIGGER_R   OPENSC2_BIT(3)
#define OPENSC2_CAL_SAVE_PRESSURE_L  OPENSC2_BIT(4)
#define OPENSC2_CAL_SAVE_PRESSURE_R  OPENSC2_BIT(5)

struct opensc2_cal_rec {
	uint8_t step[3];        /* per kind: 0 idle, 1, 2, or 3 (save pending) */
	bool active[3];         /* a step 1 since the recording ended */
	uint8_t seen[3];        /* bit 0 left, bit 1 right: sides that got samples */
	struct opensc2_cal_stick stick[2];
	struct opensc2_cal_trigger trigger[2][2];   /* [side][direction] */
	struct opensc2_cal_pressure pressure[2];
};

void opensc2_cal_rec_init(struct opensc2_cal_rec *r);
/* A command's step. For the pressure, step 3 hands its records over at once (the bits returned);
 * otherwise 0. */
uint32_t opensc2_cal_rec_step(struct opensc2_cal_rec *r, enum opensc2_cal_rec_kind kind,
			      uint8_t step);
/* One frame of samples: the sticks' and triggers' millivolts ([side][X, Y] and [side]; a side
 * whose bit is not in `stick_sides` / `trigger_sides` has no sample) and the pads' raw words.
 * Returns the bits of the records finished at this frame (steps 3 of sticks and triggers). */
uint32_t opensc2_cal_rec_frame(struct opensc2_cal_rec *r, const uint16_t stick_mv[2][2],
			       uint8_t stick_sides, const uint16_t trigger_mv[2],
			       uint8_t trigger_sides, const int16_t pressure_raw[2],
			       uint8_t pressure_sides);
/* The finished records (valid until the next step 1 of their kind). */
void opensc2_cal_rec_stick(const struct opensc2_cal_rec *r, enum opensc2_side side,
			   struct opensc2_cal_stick *out);
void opensc2_cal_rec_trigger(const struct opensc2_cal_rec *r, enum opensc2_side side,
			     struct opensc2_cal_trigger *out);
void opensc2_cal_rec_pressure(const struct opensc2_cal_rec *r, enum opensc2_side side,
			      struct opensc2_cal_pressure *out);

/* ---- The library's calibration (CONFIG_OPENSC2_CAL) -------------------------------------------
 * Raw inputs: an input given with one of these is processed by the library at every state report
 * (the stored calibration, settings 30, 31, 34, 35, 46, 52, 53, 60 and 62, the factory
 * recordings), and its processed value replaces whatever the finished-value setters gave it, until
 * opensc2_cal_raw_release() gives it back. Thread safe and cheap, like the other setters; the
 * values are taken at the next state report (give them from the `sample` callback to have them
 * sampled at the last moment). -ENOTSUP without CONFIG_OPENSC2_CAL.
 */

#define OPENSC2_RAW_STICK_L     OPENSC2_BIT(0)
#define OPENSC2_RAW_STICK_R     OPENSC2_BIT(1)
#define OPENSC2_RAW_TRIGGER_L   OPENSC2_BIT(2)
#define OPENSC2_RAW_TRIGGER_R   OPENSC2_BIT(3)
#define OPENSC2_RAW_PRESSURE_L  OPENSC2_BIT(4)
#define OPENSC2_RAW_PRESSURE_R  OPENSC2_BIT(5)
#define OPENSC2_RAW_TOUCH_L     OPENSC2_BIT(6)   /* the stick-touch and grip words */
#define OPENSC2_RAW_TOUCH_R     OPENSC2_BIT(7)
#define OPENSC2_RAW_ALL         0xFFu

/* All raw inputs at once (the hardware test hooks inject this too). */
struct opensc2_raw_inputs {
	uint8_t present;            /* OPENSC2_RAW_*: the inputs below that are given */
	uint16_t stick[2][2];       /* [side][X, Y]: 12-bit counts, right and up the higher */
	uint16_t trigger[2];        /* 12-bit counts */
	int16_t pressure[2];        /* raw pressure words */
	int16_t stick_touch[2];     /* capacitive words, the controller's units */
	int16_t grip[2];
};

/* The inputs in `raw->present` become raw with these values (the others stay as they are). */
int opensc2_set_raw(const struct opensc2_raw_inputs *raw);
/* Give inputs (OPENSC2_RAW_* bits) back to the finished-value setters. */
int opensc2_cal_raw_release(uint8_t inputs);

/* Both axes of a stick, 12-bit ADC counts (0..4095), right and up the higher values. */
int opensc2_set_stick_raw(enum opensc2_side side, uint16_t x_counts, uint16_t y_counts);
/* A trigger, 12-bit ADC counts. */
int opensc2_set_trigger_raw(enum opensc2_side side, uint16_t counts);
/* A pad's raw pressure word. The pad's touch flag stays the application's (opensc2_set_trackpad()):
 * an untouched pad reports pressure 0 and no click unless setting 60 is 1. With
 * CONFIG_OPENSC2_CAL_PRESSURE_CLICK the pad's click bit follows the pressure, as on the real
 * controller. */
int opensc2_set_pressure_raw(enum opensc2_side side, int16_t raw);
/* A capacitive stick-touch word and a grip word (the controller's units: the stick touches above
 * setting 30, default 500; the grip above (setting 34 / 2 + 12.5) % of 1800, default 1125). They
 * set the side's stick-touch and grip bits, each from the mean of its last two words. */
int opensc2_set_touch_raw(enum opensc2_side side, int16_t stick_word, int16_t grip_word);

/* Does the side's stored calibration make it usable? (Uncalibrated, a raw stick or trigger reads 0
 * and a raw pressure 0, as on the real controller.) */
bool opensc2_cal_stick_ok(enum opensc2_side side);
bool opensc2_cal_trigger_ok(enum opensc2_side side);
bool opensc2_cal_pressure_ok(enum opensc2_side side);
/* Is the factory recording of this kind running (a step 1 since it ended)? The recordings take
 * their samples from raw inputs only: an application that keeps an uncalibrated input on finished
 * values gives it raw while its recording runs, or the recording gets nothing to save. */
bool opensc2_cal_recording(enum opensc2_cal_rec_kind kind);

/* The stored values the library keeps for other parts (0 or the default when not stored):
 *   the LED trim of channel 0..3 (R, G, B, W) in percent, as the real controller uses it: the
 *   RAM value whatever it is (0 leaves the channel dark, above 100 is brighter up to full duty),
 *   the default 90, 35, 120 or 80 from a boot without a stored one, 0 after a 0xF0 until the
 *   next boot;
 *   the battery voltage offset in mV (cal/volt_offset);
 *   the gyro bias, three floats (rad/s), as the real controller takes it: -ENOENT when none is
 *   stored (all zero), -ERANGE when one is 2^31 or more in magnitude, an infinity included (the
 *   real controller then does not start its IMU stream), and a NaN given as 0. */
uint8_t opensc2_cal_led_trim(uint8_t channel);
int16_t opensc2_cal_volt_offset(void);
int opensc2_cal_gyro_bias(float bias[3]);

/* Changes whenever a stick, trigger or pressure key changes in RAM (a load at boot, the by-name
 * commands, a finished factory recording; not the LED trims, the battery offset or the gyro bias,
 * which the functions above read afresh each time): an application that processes its own inputs
 * with the helpers above can poll it and set up again. */
uint32_t opensc2_cal_generation(void);

/* The raw stick and trigger mode of setting 46 / 62 for a raw value: 1 the millivolts, 2 the counts
 * themselves; 0 (or any other value): calibrated. Returns whether `mode` is a raw mode. */
bool opensc2_cal_raw_mode(int16_t mode, uint16_t counts, uint16_t full_mv, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_CAL_H_ */
