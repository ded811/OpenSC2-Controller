/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The haptics synthesiser (CONFIG_OPENSC2_HAPTICS_SYNTH, on in the app): the library turns the
 * output reports (rumble 0x80, pulses, clicks, tones, sweeps, scripts and PCM streams 0x81..0x89)
 * into sample blocks for the controller's four haptic channels, as the real controller's two
 * sequencers do, arithmetic quirks included, and hands the blocks to outputs the application
 * provides (an I2S amplifier, a PWM "DAC", a motor driver). It also plays the built-in sounds:
 * the scripts of report 0x85, the cues (chimes) and the drop sounds (OpenSC2's own, or the
 * application's: opensc2_haptics_set_script(), opensc2_haptics_set_click()), and sends input
 * report 0x44: the PCM streams' flow control and the replies to stops.
 *
 *     static const struct opensc2_haptics_output_api amp_api = { .write = amp_write };
 *     static const struct opensc2_haptics_output amp = { .api = &amp_api };
 *
 *     opensc2_haptics_set_output(OPENSC2_HAPTICS_GRIPS, &amp);
 *     opensc2_haptics_play_script(OPENSC2_HAPTIC_LEFT_GRIP | OPENSC2_HAPTIC_RIGHT_GRIP, 4, 0);
 *
 * With the system state machine (CONFIG_OPENSC2_SYSTEM, on by default) the library plays the cues
 * itself; opensc2_haptics_play_cue() is for an application without it. The decoded outputs still
 * reach the application's rumble and haptic callbacks as before. Every
 * function and option: docs/api.md, Haptics synthesiser.
 */
#ifndef OPENSC2_HAPTICS_H_
#define OPENSC2_HAPTICS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the two paths (the real controller's two sequencers) -------------------------------------
 * A block is `frames` frames of two samples, the left channel's first:
 *   pads:  4000 Hz, 32 frames (8 ms), uint8_t, 128 = silence (0..255)
 *   grips: 8000 Hz, 64 frames (8 ms), int16_t (-32767..32767)
 * As on the real controller, the left pad and the right grip are rendered with the master gain
 * negated (the two actuators of a pair driven with opposite signs, which suits how the real ones
 * are mounted; invert one of them in your output if yours are not).
 */
enum opensc2_haptics_path {
	OPENSC2_HAPTICS_PADS = 0,     /* channels 0 (left pad) and 1 (right pad) */
	OPENSC2_HAPTICS_GRIPS = 1,    /* channels 2 (left grip) and 3 (right grip) */
};

#define OPENSC2_HAPTICS_PAD_RATE_HZ   4000
#define OPENSC2_HAPTICS_PAD_FRAMES    32
#define OPENSC2_HAPTICS_GRIP_RATE_HZ  8000
#define OPENSC2_HAPTICS_GRIP_FRAMES   64

/* ---- outputs ----------------------------------------------------------------------------------
 * One output per path, run as the real controller runs its two outputs. A sequencer idles until a
 * command makes something play; then it renders a first block (4 ms of silence first), calls
 * write(first block), enable(true) and start(), and keeps writing one block after the other,
 * staying one block ahead of what plays: before each block it applies the commands that arrived,
 * calls ready() (wait until the output can take a block without waiting: an output with two
 * buffers returns at once for the second and then when one has played), renders the block and
 * writes it. Once nothing plays any more, it calls stop(false) (drain: return when everything
 * written has played) and enable(false); commands that arrive meanwhile wait for the next start.
 * Amplifier mode 1 (controller setting 75) calls enable(true) at once and holds it; mode 0 calls
 * enable(false) at once, even in the middle of a sound. Every call comes from the sequencer's own
 * thread. Without an output a path renders anyway, paced by a timer, so the test hooks can check
 * what it renders.
 */
struct opensc2_haptics_output;

struct opensc2_haptics_output_api {
	/* Start playing what was written (once per playback, after the first block). Optional. */
	int (*start)(const struct opensc2_haptics_output *out);
	/* Wait until write() would take a block without waiting (one of the two buffers has
	 * played). Optional: without it the sequencer renders a block before the wait, a block's
	 * time earlier than the real controller does, so a PCM stream's bytes are read and its 0x44
	 * flags sent that much sooner. */
	int (*ready)(const struct opensc2_haptics_output *out);
	/* One block, valid during the call only. 0, or -EPIPE when the output ran out of blocks (an
	 * underrun): the sequencer then stops it (stop(true)), writes the block again and starts it
	 * again. Required. */
	int (*write)(const struct opensc2_haptics_output *out, const void *block, size_t frames);
	/* Stop: `drop` false lets what was written play out first. Optional. */
	int (*stop)(const struct opensc2_haptics_output *out, bool drop);
	/* The amplifier enable. Optional. */
	void (*enable)(const struct opensc2_haptics_output *out, bool on);
};

struct opensc2_haptics_output {
	const struct opensc2_haptics_output_api *api;
	void *ctx;
};

/* Use `out` for `path` from its next playback on (NULL: no output, blocks paced by a timer). The
 * built-in outputs (CONFIG_OPENSC2_HAPTICS_OUT_*) register themselves at start-up. -EINVAL for a
 * bad path or an output without write(). */
int opensc2_haptics_set_output(enum opensc2_haptics_path path,
			       const struct opensc2_haptics_output *out);

/* For an actuator that has only an amplitude (an ERM motor's duty cycle, a DRV2605 in real-time
 * playback): `fn` gets the level of every block of `path` (opensc2_haptics_block_level(), 0..255)
 * as it is rendered, every 8 ms while something plays, and 0 once nothing plays any more. The
 * blocks are paced by a timer, as without an output; an output set with
 * opensc2_haptics_set_output() takes precedence. NULL removes it. Called from the path's thread. */
typedef void (*opensc2_haptics_level_fn)(enum opensc2_haptics_path path, uint8_t level,
					 void *user_data);
int opensc2_haptics_set_level_output(enum opensc2_haptics_path path, opensc2_haptics_level_fn fn,
				     void *user_data);

/* ---- sounds of the board's own ------------------------------------------------------------------ */

/* The cues the real controller plays, by number (the chimes; docs/api.md has when each plays). */
#define OPENSC2_HAPTICS_CUE_USB_START       0    /* USB start-up, wired mode */
#define OPENSC2_HAPTICS_CUE_BT_SLOT_1       1
#define OPENSC2_HAPTICS_CUE_BT_SLOT_2       2
#define OPENSC2_HAPTICS_CUE_PUCK            3    /* the main puck chosen */
#define OPENSC2_HAPTICS_CUE_PUCK_2          4    /* the second puck chosen */
#define OPENSC2_HAPTICS_CUE_POWER_OFF       5
#define OPENSC2_HAPTICS_CUE_BT_PAIRING      6
#define OPENSC2_HAPTICS_CUE_BT_PAIRED       7
#define OPENSC2_HAPTICS_CUE_CHARGE_WINDOW   8    /* plays at +2 dB whatever the level */
#define OPENSC2_HAPTICS_CUE_SPARE           9    /* nothing plays it on the real controller */
#define OPENSC2_HAPTICS_CUE_HOST_ASLEEP     10

/* Play cue `cue` on both grips at chime level `level` (pass opensc2_chime_level(): 1 quiet,
 * -18 dB; 2 normal, -12 dB; anything else silent; cues 6 and 7 play 1 dB lower; cue 8 at +2 dB
 * at any level). The sounds are the scripts': OpenSC2's own unless the application gave its own
 * (opensc2_haptics_set_script()). 0 when it started on at least one grip (a grip whose script
 * still runs keeps it), -ENODATA when the cue plays nothing at that level, -EBUSY when both grips
 * are busy, -ENOTSUP without the synthesiser. */
int opensc2_haptics_play_cue(uint8_t cue, uint8_t level);

/* Play script `id` (1..17: OpenSC2's own sounds, or the application's) on the channels in
 * `channels` (OPENSC2_HAPTIC_* bits) at `gain_db`, as report 0x85 does. Returns how many channels
 * it started on (a channel whose script still runs refuses it), or a negative errno. */
int opensc2_haptics_play_script(uint8_t channels, uint8_t id, int8_t gain_db);

/* Click A or B (OPENSC2_HAPTIC_CMD_CLICK_A, _B) on the pads in `channels` at `gain_db` as given
 * (report 0x82 plays click A 6 dB above its gain byte); the grips refuse clicks, as on the real
 * controller. Lizard mode's pad ticks are click A at +8 dB, its pad presses click B at +6 dB.
 * Returns how many channels it queued, or a negative errno. */
int opensc2_haptics_click(uint8_t channels, uint8_t click, int8_t gain_db);

/* Stop everything on the channels in `channels` (a stop-all, as report 0x82 command 0): how many
 * channels it queued, or a negative errno. */
int opensc2_haptics_stop(uint8_t channels);

/* Play a recording on `channel` (0..3): `samples` samples in stream format `format` (0..11, as
 * report 0x86 numbers them: format / 4 is s16le, s8 or mu-law, format % 4 the rate 8, 4, 2 or
 * 1 kHz) at `gain_db`. `data` must stay valid until it has played. */
int opensc2_haptics_play_clip(uint8_t channel, const uint8_t *data, uint16_t samples,
			      uint8_t format, int8_t gain_db);

/* A free fall (CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS): from any context, an IMU's free-fall interrupt
 * included (it only notes the time; the rest runs in the system work queue), or from a thread on
 * opensc2_freefall_update(). 250 ms after the fall a random byte picks one of the two drop sounds
 * (scripts 16 and 17: 3 in 16 and 5 in 16) or none, on the left grip; further falls count again
 * 5 s after this one. As on the real controller, report falls only while the IMU streams. Every
 * other opensc2_haptics_* call takes the synthesiser's mutex: from threads only, never from an
 * interrupt. */
void opensc2_haptics_free_fall(void);

/* The IMU stopped streaming (its mode off, the controller going to sleep): as on the real
 * controller, a drop sound still due and the 5 s re-arm are cancelled, so after a fall in the last
 * 5 s the drop sounds stay off until the next boot. Call it whenever the IMU stops (it does
 * nothing otherwise). From a thread. */
void opensc2_haptics_imu_stopped(void);

/* ---- sounds the application supplies --------------------------------------------------------------
 * The real controller's clicks, scripts and drop sounds are Valve's recordings and step data, which
 * OpenSC2 does not carry: it plays sounds of its own in their places. A board can give its own
 * instead, at any time; they take effect from the next click or script on. Everything given must
 * stay valid (static const) while it may play.
 */

/* A recording: `samples` samples in stream format `format` (0..11, as report 0x86 numbers them:
 * format / 4 is s16le, s8 or mu-law, format % 4 the rate 8, 4, 2 or 1 kHz). */
struct opensc2_haptics_sound {
	const uint8_t *data;
	uint16_t samples;
	uint8_t format;
};

/* Click A (OPENSC2_HAPTIC_CMD_CLICK_A) or B (_CLICK_B) from now on, NULL for OpenSC2's own (70 and
 * 93 samples of s8 at 4 kHz, the real clicks' lengths). As on the real controller a click plays
 * one sample per output sample whatever its format's rate says (the pads run at 4 kHz), and the
 * grips refuse clicks. -EINVAL for another click, data missing or a format above 11. */
int opensc2_haptics_set_click(uint8_t click, const struct opensc2_haptics_sound *sound);

/* A script step (opensc2_haptics_set_script()). Each step runs `delay_ms` after the step before it
 * ran (at least 1 ms; the first after the request) and starts one sound without waiting for it to
 * end; an END step ends the script `delay_ms` after the step before, and frees the channel. */
enum opensc2_haptics_step_kind {
	OPENSC2_HAPTICS_STEP_END = 0,
	OPENSC2_HAPTICS_STEP_TONE = 1,     /* a Hz, b ms, c LFO in 0.1 Hz (0: none), d LFO depth % */
	OPENSC2_HAPTICS_STEP_SWEEP = 2,    /* a start Hz, b end Hz, c ms */
	OPENSC2_HAPTICS_STEP_PULSE = 3,    /* a on us, b off us, c periods (full scale: no gain) */
	OPENSC2_HAPTICS_STEP_CLICK_A = 4,  /* the pads only, as every click */
	OPENSC2_HAPTICS_STEP_CLICK_B = 5,
	OPENSC2_HAPTICS_STEP_CLIP = 6,     /* `clip`, a recording */
};

#define OPENSC2_HAPTICS_STEP_FIXED_GAIN 0x01   /* `gain_db` as it is, not added to the script's */
#define OPENSC2_HAPTICS_STEP_ON_CHANNEL 0x02   /* always on `channel`, whatever runs the script */

struct opensc2_haptics_step {
	uint16_t delay_ms;
	uint8_t kind;                      /* enum opensc2_haptics_step_kind */
	int8_t gain_db;                    /* added to the script's gain in 8 bits (wrapping round) */
	uint16_t a, b, c;
	uint8_t d;
	uint8_t flags;                     /* OPENSC2_HAPTICS_STEP_* flags */
	uint8_t channel;                   /* with OPENSC2_HAPTICS_STEP_ON_CHANNEL: 0..3 */
	const struct opensc2_haptics_sound *clip;   /* OPENSC2_HAPTICS_STEP_CLIP */
};

/* Script `id` (1..17: report 0x85, the cues' scripts 1, 2, 3, 4, 5, 8, 9 and 10, the drop sounds'
 * 16 and 17) as `count` steps of the application's, from its next start on (a script that runs
 * goes on with the steps it has); NULL for OpenSC2's own. A script without an END step ends 1 ms
 * after its last step. -EINVAL for an id outside 1..17, no steps, or a step that cannot play (an
 * unknown kind, a channel above 3, a clip without data or in a format above 11); -ENOTSUP without
 * the synthesiser. */
int opensc2_haptics_set_script(uint8_t id, const struct opensc2_haptics_step *steps,
			       uint8_t count);

/* What the synthesiser does now. */
struct opensc2_haptics_info {
	bool busy[2];                 /* [path]: something plays */
	bool output[2];               /* [path]: an output of the application's is set */
	uint32_t blocks[2];           /* [path]: blocks rendered since boot */
	uint32_t underruns[2];        /* [path]: outputs restarted after running out of blocks */
	uint32_t lost;                /* commands lost to a full queue */
	uint16_t stream_fill[4];      /* [channel]: bytes in the PCM ring */
	uint8_t scripts;              /* bit per channel: a script runs */
};

int opensc2_haptics_info(struct opensc2_haptics_info *info);

/* ---- pure helpers (no kernel; for outputs and IMUs of your own) --------------------------------- */

/* The loudness of a block for an output that has only an amplitude (an ERM motor's duty cycle, a
 * DRV2605 in real-time playback): the larger of the two channels' peaks, 0..255. */
uint8_t opensc2_haptics_block_level(enum opensc2_haptics_path path, const void *block,
				    size_t frames);

/* A free-fall detector for IMUs without one: the acceleration's magnitude below 0.35 g for
 * 150 ms is a fall; the next one counts once it was above 0.6 g again. OpenSC2's own thresholds:
 * the real controller uses its IMU's built-in detector. */
struct opensc2_freefall {
	bool falling;                 /* below the threshold since `since_ms` */
	bool reported;                /* this fall was reported; wait for > 0.6 g */
	uint32_t since_ms;
};

void opensc2_freefall_init(struct opensc2_freefall *ff);
/* One accelerometer sample (16384 per g, report 0x42's units) at `now_ms`: true once per fall. */
bool opensc2_freefall_update(struct opensc2_freefall *ff, const int16_t accel[3], uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_HAPTICS_H_ */
