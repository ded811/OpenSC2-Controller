/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The haptics synthesiser (CONFIG_OPENSC2_HAPTICS_SYNTH, off by default): the library turns the
 * output reports (rumble 0x80, pulses, clicks, tones, sweeps, scripts and PCM streams 0x81..0x89)
 * into sample blocks for the controller's four haptic channels, as the real controller's two
 * sequencers do, and hands the blocks to outputs the application provides (an I2S amplifier, a
 * PWM "DAC", a motor driver). It also plays OpenSC2's own built-in sounds: the scripts of report
 * 0x85, the cues (chimes) and the drop sounds, and sends the PCM streams' flow control, input
 * report 0x44.
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
 * One output per path. A sequencer idles until something plays; then it renders a first block
 * (4 ms of silence first), calls enable(true), write(first block) and start(), and keeps writing
 * one block after the other, staying one block ahead of what plays: write() must block until the
 * output can take the next block (an output with two buffers returns at once for the first two
 * and then when one has played). Once nothing plays any more, it calls stop(false) (drain: return
 * when everything written has played) and enable(false). Controller setting 75 = 1 holds the
 * enable on between sounds. Every call comes from the sequencer's own thread. Without an output a
 * path renders anyway, paced by a timer, so the test hooks can check what it renders.
 */
struct opensc2_haptics_output;

struct opensc2_haptics_output_api {
	/* Start playing what was written (once per playback, after the first block). Optional. */
	int (*start)(const struct opensc2_haptics_output *out);
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
 * -18 dB; 2 normal, -12 dB; anything else silent; cues 6 and 7 play 1 dB lower). The sounds are
 * OpenSC2's own. 0 when it started on at least one grip (a grip whose script still runs keeps
 * it), -ENODATA when the cue plays nothing at that level, -EBUSY when both grips are busy,
 * -ENOTSUP without the synthesiser. */
int opensc2_haptics_play_cue(uint8_t cue, uint8_t level);

/* Play built-in script `id` (1..17, OpenSC2's own sounds) on the channels in `channels`
 * (OPENSC2_HAPTIC_* bits) at `gain_db`, as report 0x85 does. Returns how many channels it started
 * on (a channel whose script still runs refuses it), or a negative errno. */
int opensc2_haptics_play_script(uint8_t channels, uint8_t id, int8_t gain_db);

/* Click A or B (OPENSC2_HAPTIC_CMD_CLICK_A, _B) on the pads in `channels` at `gain_db`; the grips
 * refuse clicks, as on the real controller. Lizard mode's pad ticks are click A at +8 dB, its pad
 * presses click B at +6 dB. Returns how many channels it queued, or a negative errno. */
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
 * (3 in 16 and 5 in 16) or none, on the left grip; further falls count again 5 s after this one.
 * Every other opensc2_haptics_* call takes the synthesiser's mutex: from threads only, never from
 * an interrupt. */
void opensc2_haptics_free_fall(void);

/* The IMU's stream went off (<opensc2/imu.h> calls it): a drop sound still waiting its 250 ms is
 * not played, and after a fall further falls never count again until the next boot, as on the real
 * controller. From a thread. */
void opensc2_haptics_free_fall_cancel(void);

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
