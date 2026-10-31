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
 *
 * Arithmetic: the real controller works these out in single-precision floating point, and so does
 * this library, in the same order of operations, with a multiply and an add fused into one rounding
 * (fmaf()) exactly where the real controller's build fuses them, so the results are the same to the
 * last count (also the quirks that come from it, such as 90 degrees of the pad rotation giving
 * 8191, not 8192). The library's sources are built with -ffp-contract=off for that reason.
 */
#ifndef OPENSC2_CAL_H_
#define OPENSC2_CAL_H_

#include <stdbool.h>
#include <stddef.h>
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
/* Back through the channel's full-scale reading, as the stored calibration is turned into counts:
 * with F = 4095 x full_mv / 4096 rounded down (the millivolts of a reading of 4095),
 * mv / (F / 4095) in floating point, truncated; 0 for 0, 65535 from 65535 up. */
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

/* ---- The normalising stage -------------------------------------------------------------------
 * What the real controller puts each stick half, each trigger and each pad's pressure through: a
 * reading between `lo` and `hi` becomes 0..1, with a dead band (percent of the range) at both ends
 * or only at the outer one, optionally turned round (`flip`: 1 at `lo`), optionally limited to
 * 0..1, and a digital state that turns on above `threshold` + `hysteresis` and off below
 * `threshold` - `hysteresis` (fractions; measured from the other end when flipped). A smoothed copy
 * of the readings (factor `alpha`, started at the first reading, and again after a smoothed value
 * of exactly 0) can widen the range (`grow`); a range end `hi` of exactly 0 counts as "not set"
 * and is set, with `lo`, from the smoothed copy at the next reading, which leaves a range of 0. A
 * range of 0 reads 0 and leaves the digital state as it was. The set-up helpers below fill the
 * stages in; the fields are public for tests and tools.
 */
struct opensc2_norm {
	float alpha;          /* the smoothed copy's factor; 1: the reading itself */
	float smooth;         /* the smoothed copy (0: take the next reading as it is) */
	float lo, hi;         /* the range, in the reading's units */
	float band;           /* the dead band, percent of the range */
	float threshold;      /* the digital state's level, fraction (0 after a set-up) */
	float hysteresis;     /* fraction (10, so never, after a set-up) */
	bool outer_only;      /* the dead band only at the outer end (the top; the bottom flipped) */
	bool flip;            /* 1 at lo, 0 at hi */
	bool limit;           /* limit the result to 0..1 (after a set-up: yes) */
	bool grow;            /* the range follows the smoothed copy past its ends */
	bool on;              /* the digital state */
};

/* Set a stage up for lo..hi: the dead band, its ends, the factor; limited, growing, no digital
 * level (threshold 0, hysteresis 10), digital state off, no smoothed copy. `flip` is left as it
 * was. */
void opensc2_norm_setup(struct opensc2_norm *n, int32_t lo, int32_t hi, float band_pct,
			bool outer_only, float alpha);
/* One reading: 0..1 (beyond that when not limited). */
float opensc2_norm_step(struct opensc2_norm *n, int32_t reading);

/* ---- Sticks -----------------------------------------------------------------------------------
 * Each axis is split at its centre (the middle of the rest band, rounded down) into two halves,
 * each a normalising stage of its own (centre..max, and min..centre turned round) with a 10 % dead
 * band at its outer end and nothing else: no dead zone, no filter. Full deflection comes 90 % of
 * the way to the stored extreme; the output is the stage's value x 32767 truncated toward zero
 * (minus for the lower half). A reading above the centre takes the upper half, the centre and
 * below the lower. The calibration is usable when its format is 2 or 0x81 and, on both axes, the
 * maximum lies at least 200 counts from rest high and the minimum at least 200 counts from rest
 * low, either side; otherwise the stick reads 0, 0. An extreme on the wrong side of the centre
 * (only a hand-written calibration has one) leaves that half reading 0, as on the real
 * controller; so does an end of 0 counts (the upper half's maximum, or a centre of 0), which the
 * stage takes for "not set".
 */

struct opensc2_stick_axis {
	int32_t centre;               /* counts */
	struct opensc2_norm upper;    /* centre..max */
	struct opensc2_norm lower;    /* min..centre, turned round */
};

struct opensc2_stick_proc {
	bool valid;
	struct opensc2_stick_axis axis[2];   /* X, Y */
	/* Options of OpenSC2's own, off (0, false) by default as on the real controller: */
	uint16_t dead_zone;   /* per axis, report units: |value| up to this reads 0, the rest is
			       * stretched back to full scale (for drifting potentiometer sticks) */
	bool grow_range;      /* July 2026's rule: each half's outer end follows the smoothed copy
			       * of the readings (factor 0.15) past the stored extreme, in RAM only;
			       * October keeps the stored range */
};

/* Set up `p` from a stored calibration (NULL: uncalibrated). The options are kept. */
void opensc2_stick_setup(struct opensc2_stick_proc *p, const struct opensc2_cal_stick *cal);
/* One axis (0 X, 1 Y) from its reading. The halves keep their smoothed copies (and, with
 * grow_range, their ranges move). */
int16_t opensc2_stick_axis_value(struct opensc2_stick_proc *p, int axis, uint16_t counts);
/* Both axes; 0, 0 while uncalibrated. */
void opensc2_stick_value(struct opensc2_stick_proc *p, uint16_t x_counts, uint16_t y_counts,
			 struct opensc2_stick *out);

/* ---- Triggers ---------------------------------------------------------------------------------
 * The released end is B (direction 0) or A; the pulled end is moved 2 % toward it (A x 0.98, or
 * B / 0.98, then truncated to whole counts). A normalising stage with a 5 % dead band at both ends
 * and no filter: the output is its value x 32767, truncated, 0..32767 (0 over the first 5 %, full
 * from 95 % on). Usable with format 2 or 0x81 and A and B at least 100 counts apart; otherwise 0
 * and no digital bit. The digital bit (bit 23 or 27 of report 0x42) is the stage's: on above
 * setting 68 + 5 %, off below setting 68 - 5 %, in floating point, so from 68 = 95 on it never
 * turns on for direction 0 (95 % + 5 % is 1.0, which a full pull reaches but does not pass), while
 * for direction 1 at 95 a fully pulled trigger does (its levels are taken from the other end). A
 * set-up clears it: the real controller sets its triggers up again after any change of a stick or
 * trigger key, of setting 68, and when it docks or leaves the dock.
 */

struct opensc2_trigger_proc {
	bool valid;
	bool falls;          /* direction 1: the voltage falls when pulled */
	int32_t a, b;        /* counts */
	struct opensc2_norm stage;
};

/* Set up from a stored calibration (NULL: uncalibrated), with setting 68's default level (90). */
void opensc2_trigger_setup(struct opensc2_trigger_proc *p, const struct opensc2_cal_trigger *cal);
/* The same with setting 68's value, as the real controller takes it at the set-up. */
void opensc2_trigger_setup_level(struct opensc2_trigger_proc *p,
				 const struct opensc2_cal_trigger *cal, int16_t setting68);
/* One reading: the value 0..32767 in *value, the digital bit returned (false while unusable). */
bool opensc2_trigger_step(struct opensc2_trigger_proc *p, uint16_t counts, uint16_t *value);
/* The value a reading gives, leaving `p` as it is. */
uint16_t opensc2_trigger_value(const struct opensc2_trigger_proc *p, uint16_t counts);

/* ---- Pad pressure -----------------------------------------------------------------------------
 * Each raw word updates a filter p = (3 p + raw) / 4 (whole numbers, toward zero, kept as a signed
 * 16-bit value; p starts at 0, never starts over, and steps only while the calibration is usable).
 * A normalising stage over lo..hi (A..B, or 0..B-A for format 0x80, B-A kept to 16 bits) with a
 * 5 % dead band at both ends, not limited, gives v; the pressure is (C / 2000 x 32767) x v,
 * truncated and limited to 0..32767. The click is the stage's digital state: on above s + 8 %, off
 * below s - 8 %, with s controller setting 52 (left) or 53 (right), a negative one counting as
 * 100. Usable with format 1 (A not 0x7FFF) or 0x80 and C not 0; otherwise the pressure is 0 and
 * the pad never clicks. A range of 0 (B = A) reads 0 and never clicks.
 */

struct opensc2_pressure_proc {
	bool usable;
	int16_t lo, hi;       /* raw units */
	uint16_t gain;        /* C */
	float scale;          /* C / 2000 */
	struct opensc2_norm stage;
	int16_t filtered;     /* p */
	int16_t click_setting;
	bool click;           /* the stage's digital state after the last sample */
	uint16_t last;        /* the pressure the last sample gave */
};

/* Set up from a stored calibration (NULL: unusable). The filter keeps its value; the click clears
 * and the click level is taken again at the next sample. */
void opensc2_pressure_setup(struct opensc2_pressure_proc *p, const struct opensc2_cal_pressure *cal);
/* One raw word, with the side's click setting (52 or 53); a change of it sets the stage up again
 * (the click clears), as the real controller's pad does. */
void opensc2_pressure_sample(struct opensc2_pressure_proc *p, int16_t raw, int16_t click_setting);
/* The report's pressure for the filter's value as it stands (the stage is not stepped). */
uint16_t opensc2_pressure_value(const struct opensc2_pressure_proc *p);
/* Setting 60 = 1: the raw word itself, below 0 counted as 0. */
uint16_t opensc2_pressure_raw_value(int16_t raw);

/* ---- The factory recordings (vendor commands 0xD8, 0xC0, 0xC3) -------------------------------
 * Each recording keeps its own step. The command sets the step (any of 0..3, also without a step
 * 1); a step 1 starts the working records and hooks the recording to the samples; the samples of
 * the frames that follow fill the records while it is hooked. Steps above 3 do nothing.
 *   sticks (0xD8, the step in payload[1]): 1 rest band, 2 full circles, 3 save at the next frame,
 *     0 unhook at the next frame without saving (a step 1 or 2 coming before that frame goes on);
 *   triggers (0xC0, payload[0]): 1 released, 2 pulled, 3 save at the next frame (each trigger
 *     keeps the direction whose A and B lie further apart, |A - B|, direction 0 on a tie), 0 as
 *     for the sticks;
 *   pressure (0xC3, payload[0]): 1 start, 2 the firm press (B rises to the highest raw word, below
 *     0 counted as 0), 0 pause; 3 unhooks and saves at once, with or without a step 1: after a
 *     boot that is two records of zeros (format 0), which leave both pads without pressure and
 *     click, as on the real controller.
 * The real controller always has both sticks, triggers and pads; a board may have fewer. A side is
 * saved when the board gives samples for it (its bit in a frame's sides, at any time since boot) or
 * it got samples since step 1: a side the board does not have keeps its stored key.
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
	uint8_t step[3];        /* per kind: the last step 0..3 a command gave */
	bool active[3];         /* hooked to the samples: a step 1, and not yet unhooked */
	uint8_t seen[3];        /* bit 0 left, bit 1 right: sides that got samples since step 1 */
	uint8_t supplied[3];    /* sides the board has given samples for since boot */
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
/* One raw pressure word of a pad, as the real controller records it: once per trackpad frame
 * (the library's trackpad frames call it; opensc2_cal_rec_frame() takes the pressure of the state
 * report's raw inputs). */
void opensc2_cal_rec_pressure_sample(struct opensc2_cal_rec *r, enum opensc2_side side, int16_t raw);
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

/* A calibration key by its name ("cal/joy_l", ... as a host names it), for a board that brings its
 * own factory calibration (a production tool, values measured another way). _set: the value
 * (exactly the key's size) into RAM, where the processing takes it from the next state report, as
 * vendor command 0xEE does; with `save` also written to the board's storage (after any write still
 * waiting), as 0xEF would. _get: the RAM value as the processing uses it (whatever its format
 * word), its length returned. -ENOENT for a name that is no key, -EINVAL for another size or a
 * buffer too small, -ENOTSUP without CONFIG_OPENSC2_CAL. Any thread but an interrupt (a save
 * writes flash). */
int opensc2_cal_key_set(const char *name, const void *value, size_t len, bool save);
int opensc2_cal_key_get(const char *name, void *out, size_t max);

/* The stick offsets while docked: millivolts added to a side's stored stick calibration (X and
 * Y, each { min, max, rest low, rest high }; the sum limited to 0..65535) while the controller
 * sits on the dock (the power source is OPENSC2_POWER_SOURCE_DOCK) on a board of revision 0x48 or
 * later, as the real controller does for the pull of the dock's magnets on its sticks. Every side
 * starts at 0 (a board whose sticks the dock does not move). A change, and docking or leaving the
 * dock, sets the sticks and triggers up again (the triggers' digital bits clear). Any time, any
 * thread. -EINVAL for a bad side or a NULL. */
int opensc2_cal_set_dock_offsets(enum opensc2_side side, const int16_t x_mv[4],
				 const int16_t y_mv[4]);

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
