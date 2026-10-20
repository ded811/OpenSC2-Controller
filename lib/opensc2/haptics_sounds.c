/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_sounds.c: OpenSC2's own built-in haptic scripts, ids 1 to 17 (output report 0x85, the
 * cues and the drop sounds). The real controller's scripts and recordings are Valve's data; these
 * are short sounds of OpenSC2's own, made of the synthesiser's tones, sweeps and pulses, one for
 * each id so that every id the host may ask for plays something. Where an id has a job on the
 * real controller (a cue, a drop sound) the sound here suits the job; it is not the real one.
 *
 *   id  used for                                       here
 *    1  cues 0, 1, 3: start-up, the first link          a rising sweep, then a high blip
 *    2  cues 2, 4: the second link                      the same sweep, then two blips
 *    3  cue 9 (nothing plays it)                        three short blips
 *    4  cue 10: the PC is asleep                        a slow, wavering hum
 *    5  cue 5: turning off                              a falling sweep, then a low blip
 *    6, 7                                               a rising and a falling sweep
 *    8  cue 6: Bluetooth pairing mode                   three evenly spaced blips
 *    9  cue 7: a new Bluetooth host paired              a blip, then a rising sweep
 *   10  cue 8: the factory charge window                a sweep up and back
 *   11  ..15                                            a thump, a buzz, a double tap, a wobble,
 *                                                       a four-note ladder
 *   16, 17  the drop sounds                             a wobbling "boing" and a rising and falling
 *                                                       "whoop"
 */
#include "haptics_rules.h"

/* delay ms (after the step before), gain offset dB, then the step's own values */
#define TONE(delay, gain, hz, ms, lfo_dhz, depth) \
	{ (delay), OSC2_HAP_STEP_TONE, (gain), (hz), (ms), (lfo_dhz), (depth) }
#define SWEEP(delay, gain, f0, f1, ms) { (delay), OSC2_HAP_STEP_SWEEP, (gain), (f0), (f1), (ms), 0 }
#define PULSE(delay, on_us, off_us, n) { (delay), OSC2_HAP_STEP_PULSE, 0, (on_us), (off_us), (n), 0 }
#define END(delay) { (delay), OSC2_HAP_STEP_END, 0, 0, 0, 0, 0 }

static const struct osc2_hap_step s1[] = {
	SWEEP(1, 0, 260, 520, 100), TONE(120, 0, 700, 70, 0, 0), END(90),
};
static const struct osc2_hap_step s2[] = {
	SWEEP(1, 0, 260, 520, 100), TONE(120, 0, 700, 50, 0, 0), TONE(80, 0, 700, 50, 0, 0),
	END(70),
};
static const struct osc2_hap_step s3[] = {
	TONE(1, 0, 520, 45, 0, 0), TONE(90, 0, 520, 45, 0, 0), TONE(90, 0, 520, 45, 0, 0), END(65),
};
static const struct osc2_hap_step s4[] = {
	TONE(1, -2, 380, 520, 40, 80), END(540),
};
static const struct osc2_hap_step s5[] = {
	SWEEP(1, 0, 600, 240, 170), TONE(190, -3, 200, 60, 0, 0), END(80),
};
static const struct osc2_hap_step s6[] = {
	SWEEP(1, 0, 300, 900, 130), END(150),
};
static const struct osc2_hap_step s7[] = {
	SWEEP(1, 0, 900, 300, 130), END(150),
};
static const struct osc2_hap_step s8[] = {
	TONE(1, 0, 640, 40, 0, 0), TONE(120, 0, 640, 40, 0, 0), TONE(120, 0, 640, 40, 0, 0), END(60),
};
static const struct osc2_hap_step s9[] = {
	TONE(1, 0, 640, 50, 0, 0), SWEEP(80, 0, 640, 1000, 110), END(130),
};
static const struct osc2_hap_step s10[] = {
	SWEEP(1, 0, 400, 800, 200), SWEEP(220, 0, 800, 400, 200), END(220),
};
static const struct osc2_hap_step s11[] = {
	TONE(1, 2, 60, 60, 0, 0), END(80),
};
static const struct osc2_hap_step s12[] = {
	TONE(1, 0, 180, 250, 250, 100), END(270),
};
static const struct osc2_hap_step s13[] = {
	PULSE(1, 2000, 2000, 2), PULSE(150, 2000, 2000, 2), END(30),
};
static const struct osc2_hap_step s14[] = {
	TONE(1, 0, 300, 600, 30, 100), END(620),
};
static const struct osc2_hap_step s15[] = {
	TONE(1, 0, 400, 50, 0, 0), TONE(70, 0, 500, 50, 0, 0), TONE(70, 0, 600, 50, 0, 0),
	TONE(70, 0, 760, 50, 0, 0), END(70),
};
static const struct osc2_hap_step s16[] = {
	TONE(1, 0, 140, 700, 70, 60), SWEEP(700, 0, 140, 60, 380), END(400),
};
static const struct osc2_hap_step s17[] = {
	SWEEP(1, 0, 200, 700, 400), SWEEP(420, 0, 700, 150, 600), TONE(620, 0, 90, 150, 0, 0),
	END(170),
};

#define SCRIPT(s) { (s), (uint8_t)(sizeof(s) / sizeof((s)[0])) }

static const struct {
	const struct osc2_hap_step *steps;
	uint8_t count;
} scripts[OSC2_HAP_SCRIPT_LAST] = {
	SCRIPT(s1), SCRIPT(s2), SCRIPT(s3), SCRIPT(s4), SCRIPT(s5), SCRIPT(s6), SCRIPT(s7),
	SCRIPT(s8), SCRIPT(s9), SCRIPT(s10), SCRIPT(s11), SCRIPT(s12), SCRIPT(s13), SCRIPT(s14),
	SCRIPT(s15), SCRIPT(s16), SCRIPT(s17),
};

const struct osc2_hap_step *osc2_hap_script(uint8_t id, uint8_t *count)
{
	if (id < OSC2_HAP_SCRIPT_FIRST || id > OSC2_HAP_SCRIPT_LAST) {
		*count = 0;
		return NULL;
	}
	*count = scripts[id - 1].count;
	return scripts[id - 1].steps;
}
