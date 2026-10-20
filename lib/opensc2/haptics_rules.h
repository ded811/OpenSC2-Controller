/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_rules.h: what the haptics synthesiser (CONFIG_OPENSC2_HAPTICS_SYNTH) does with the
 * output reports, as pure rules: which commands, script requests and stream writes each decoded
 * output becomes, the October rumble emulation's numbers, the script player, the cues and the
 * drop sounds' timing. No kernel; the host tests run them as the firmware does.
 */
#ifndef OPENSC2_HAPTICS_RULES_H_
#define OPENSC2_HAPTICS_RULES_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>
#include "haptics_core.h"

/* ---- output reports --------------------------------------------------------------------------- */

#define OSC2_HAP_ACT_CMDS 4

struct osc2_hap_actions {
	struct osc2_hap_cmd cmd[OSC2_HAP_ACT_CMDS];   /* in the order they are queued */
	uint8_t n_cmd;
	struct {
		uint8_t channel, id;
		int8_t gain_db;
	} script[OSC2_HAP_ACT_CMDS];
	uint8_t n_script;
	struct {
		uint8_t channel;
		const uint8_t *data;
		uint8_t len;
	} write[2];
	uint8_t n_write;
};

/* What one decoded output does, in the order the real controller handles it: a two-channel target
 * takes the left channel first, except the stream reports (0x86, 0x87, 0x89), which take the right
 * one first; rumble 0x80 handles the right grip, then the left. `setting27` is the rumble gain
 * offset. A rumble the library itself stops (`timeout`: the host stopped repeating it, or the link
 * dropped) does nothing: the synthesiser's rumble lasts 500 ms by itself, as on the real one. */
void osc2_hap_actions(const struct opensc2_output *out, int16_t setting27,
		      struct osc2_hap_actions *a);

/* Clicks the library makes itself (lizard mode's pad ticks and presses: haptic events of report
 * ID 0, kind OPENSC2_HAPTIC_COMMAND with click A or B): each click on the pads of `channels` at its
 * gain as given (unlike report 0x82, whose click A plays 6 dB above the gain). Anything else in `h`
 * plays nothing. */
void osc2_hap_library_clicks(const struct opensc2_haptic *h, uint8_t n,
			     struct osc2_hap_actions *a);

/* The October rumble emulation (report 0x80): the level step's dB for `intensity` (16, 60, 100,
 * 200, 1000, 8000, 16000: +16, +12, +8, +4, -4, -8, -12; anything else 0), the right grip's gain,
 * the left grip's gain, the left grip's LFO rate and the right grip's burst rate. */
int8_t osc2_hap_rumble_level_db(uint16_t intensity);
int8_t osc2_hap_rumble_right_db(uint16_t right_speed);
int8_t osc2_hap_rumble_left_db(uint16_t left_speed, int16_t setting27, uint16_t intensity);
uint32_t osc2_hap_rumble_lfo_mhz(uint16_t left_speed);
uint32_t osc2_hap_rumble_burst_mhz(uint16_t left_speed);

#define OSC2_HAP_RUMBLE_MS 500
#define OSC2_HAP_RUMBLE_HZ 50

/* ---- built-in scripts (OpenSC2's own sounds, haptics_sounds.c) --------------------------------- */

enum osc2_hap_step_kind {
	OSC2_HAP_STEP_END = 0,         /* the script ends `delay_ms` after the step before */
	OSC2_HAP_STEP_TONE = 1,        /* a = Hz, b = ms, c = LFO in 0.1 Hz (0: none), d = depth % */
	OSC2_HAP_STEP_SWEEP = 2,       /* a = start Hz, b = end Hz, c = ms */
	OSC2_HAP_STEP_PULSE = 3,       /* a = on us, b = off us, c = periods (full scale) */
	OSC2_HAP_STEP_CLICK_A = 4,     /* the pads only */
	OSC2_HAP_STEP_CLICK_B = 5,
};

struct osc2_hap_step {
	uint16_t delay_ms;             /* after the step before (the first: after the request) */
	uint8_t kind;                  /* enum osc2_hap_step_kind */
	int8_t gain_db;                /* added to the script's gain */
	uint16_t a, b, c;
	uint8_t d;
};

#define OSC2_HAP_SCRIPT_FIRST 1
#define OSC2_HAP_SCRIPT_LAST 17        /* report 0x85 plays 1..17, as the October controller */
#define OSC2_HAP_SCRIPT_DROP_1 16
#define OSC2_HAP_SCRIPT_DROP_2 17

/* Script `id`'s steps (ending with an END step) and their number, or NULL for an unknown id. */
const struct osc2_hap_step *osc2_hap_script(uint8_t id, uint8_t *count);

/* The command a step plays on `channel` at the script's gain (the step's offset added without
 * wrapping round). */
void osc2_hap_step_cmd(const struct osc2_hap_step *step, uint8_t channel, int8_t gain_db,
		       struct osc2_hap_cmd *cmd);

/* One channel's script player: one script at a time, steps at their delays from the step before
 * (at least 1 ms). Times are milliseconds of any clock that wraps at 2^32. */
struct osc2_hap_player {
	const struct osc2_hap_step *steps;
	uint8_t count, next;
	uint8_t channel, id;
	int8_t gain_db;
	bool busy;
	uint32_t due_ms;
};

void osc2_hap_player_init(struct osc2_hap_player *p, uint8_t channel);
/* Start script `id` at `gain_db`; false (nothing changes) while a script runs or for an id
 * outside 1..17. */
bool osc2_hap_player_start(struct osc2_hap_player *p, uint8_t id, int8_t gain_db, uint32_t now_ms);

enum osc2_hap_poll {
	OSC2_HAP_POLL_NONE = 0,        /* nothing due yet (or no script) */
	OSC2_HAP_POLL_STEP = 1,        /* `*cmd` is the step due: queue it, then poll again */
	OSC2_HAP_POLL_DONE = 2,        /* the script ended: the channel is free */
};

enum osc2_hap_poll osc2_hap_player_poll(struct osc2_hap_player *p, uint32_t now_ms,
					struct osc2_hap_cmd *cmd);
/* Milliseconds until the player's next step or end is due (0: now), or -1 without a script. */
int32_t osc2_hap_player_wait(const struct osc2_hap_player *p, uint32_t now_ms);

/* ---- cues ------------------------------------------------------------------------------------- */

#define OSC2_HAP_CUE_COUNT 11

/* The script and gain of cue `cue` at chime level `level` (1 quiet -18 dB, 2 normal -12 dB,
 * anything else silent): cues 0, 1 and 3 script 1; 2 and 4 script 2; 5 script 5; 6 and 7 scripts
 * 8 and 9, 1 dB below the level; 8 script 10 at +2 dB whatever the level; 9 script 3; 10 script
 * 4. False: it plays nothing. */
bool osc2_hap_cue(uint8_t cue, uint8_t level, uint8_t *script, int8_t *gain_db);

/* ---- drop sounds ------------------------------------------------------------------------------ */

#define OSC2_HAP_DROP_DELAY_MS 250
#define OSC2_HAP_DROP_REARM_MS 5000

enum osc2_hap_drop_state {
	OSC2_HAP_DROP_ARMED = 0,
	OSC2_HAP_DROP_PENDING = 1,     /* a fall was seen: the sound is due at play_ms */
	OSC2_HAP_DROP_WAITING = 2,     /* until the re-arm at rearm_ms */
};

struct osc2_hap_drop {
	uint8_t state;
	uint32_t play_ms, rearm_ms;
};

enum osc2_hap_drop_poll {
	OSC2_HAP_DROP_NOTHING = 0,
	OSC2_HAP_DROP_PLAY = 1,        /* pick the sound now (osc2_hap_drop_pick()) */
	OSC2_HAP_DROP_REARMED = 2,
};

void osc2_hap_drop_init(struct osc2_hap_drop *d);
/* A free fall: taken (true) only while armed; the sound is due 250 ms later, the re-arm 5 s after
 * the fall. */
bool osc2_hap_drop_fall(struct osc2_hap_drop *d, uint32_t now_ms);
enum osc2_hap_drop_poll osc2_hap_drop_poll(struct osc2_hap_drop *d, uint32_t now_ms);
int32_t osc2_hap_drop_wait(const struct osc2_hap_drop *d, uint32_t now_ms);
/* The script a random byte picks: below 48 script 16, 48 to 127 script 17, otherwise none (0):
 * 3 in 16 and 5 in 16, about half of all drops. */
uint8_t osc2_hap_drop_pick(uint8_t random_byte);

#endif /* OPENSC2_HAPTICS_RULES_H_ */
