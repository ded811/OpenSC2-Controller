/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * trackpad.h: the trackpads processed by the library the way the real controller processes them,
 * frame by frame at the sensor's own rate (CONFIG_OPENSC2_CAL), following the protocol
 * documentation (not public yet; docs/protocol.md lists the pages).
 *
 * The real controller's trackpad chip reports, for each pad and about 250 times a second, a touch,
 * a position, a raw pressure word and the capacitive words of the stick touch and the grip on that
 * side. For every such frame its firmware works out:
 *   - the position: the pad scale ((p - 0.5) x 2 x 16383), the speed-adaptive filter while setting
 *     65 is on, the rotation of settings 2 and 24 (the left pad the other way), doubled to the
 *     report's scale, the touch stabiliser while setting 80 is 2, Y turned up; 0, 0 and the filters
 *     and stabiliser started over without a touch;
 *   - the stick touch: the mean of the last two stick-touch words against settings 30 (on) and 31
 *     (off), or the stick itself deflected beyond 12000 (the calibrated stick; not with raw sticks,
 *     setting 46);
 *   - the grip: the mean of the last two grip words against the levels of settings 34 and 35;
 *   - the pressure and the click: the pressure calibration (cal/prs_l, cal/prs_r), settings 52 and
 *     53, 0 and no click without a touch; setting 60 sends the raw word instead;
 *   - setting 82: a touched stick on that side blanks the pad (no touch, no click, 0, 0, pressure
 *     0); the stick-touch and grip bits stay.
 * A change of setting 30, 31, 33, 34, 35, 52, 53, 66, 72 or 73, and a load of the pad's pressure key,
 * starts the pads over (the means, the hysteresis, the click, the filter, the stabiliser); the
 * pressure filter is kept. The factory pressure recording (0xC3) takes each frame's raw pressure.
 *
 * An application with a trackpad sensor calls opensc2_pad_frame() for every frame its sensor gives
 * (from any thread, its input callback for example): the side's pad, its touch and click bits, its
 * stick-touch and grip bits are then the library's from that frame on, in every state report, until
 * opensc2_pad_frame_release(). A frame may leave out the words the board has no sensor for (the
 * `has` bits): without a pressure word the pad reads pressure 0 and never clicks by itself (the
 * application's click bit stays, for a board with a click switch); without a stick-touch word the
 * application's own stick-touch bit counts as the sensor (a CAP12xx key, for example), with the
 * deflection rule added; without a grip word the application's grip bit stays.
 */
#ifndef OPENSC2_TRACKPAD_H_
#define OPENSC2_TRACKPAD_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The optional words of a frame. */
#define OPENSC2_PAD_HAS_PRESSURE     OPENSC2_BIT(0)
#define OPENSC2_PAD_HAS_STICK_TOUCH  OPENSC2_BIT(1)
#define OPENSC2_PAD_HAS_GRIP         OPENSC2_BIT(2)

/* One frame of a pad's sensor. The position is an absolute input event's value, millionths of the
 * pad: 0 at the left (X) and the top (Y), 1000000 at the right and the bottom, the centre 500000
 * (values beyond -1000000..1000000 are limited); it counts only while touched. The words are in the
 * units of the real controller's trackpad chip (docs/api.md, Trackpads frame by frame): scale your
 * sensor's readings to them. */
struct opensc2_pad_frame {
	bool touched;
	int32_t x, y;
	uint8_t has;            /* OPENSC2_PAD_HAS_* */
	int16_t pressure;       /* the raw pressure word */
	int16_t stick_touch;    /* this side's stick-touch word (the stick touches above setting 30) */
	int16_t grip;           /* this side's grip word */
};

/* One frame of `side`'s pad, processed at once as the real controller does (see above); its results
 * go out with the next state report. Thread safe; cheap (a few microseconds with an FPU).
 * -EINVAL for a bad side or NULL, -ENOTSUP without CONFIG_OPENSC2_CAL. */
int opensc2_pad_frame(enum opensc2_side side, const struct opensc2_pad_frame *frame);

/* Give `side`'s pad back to the finished values of opensc2_set_trackpad() and the button bits. */
int opensc2_pad_frame_release(enum opensc2_side side);

/* What the last frame of `side` gave (all zero before the first frame, and after a release). */
struct opensc2_pad_result {
	bool frames;            /* the side is fed by frames */
	bool touched, clicked;  /* as reported (after setting 82) */
	bool stick_touched, grip;
	int16_t x, y;           /* the report's scale, Y up */
	uint16_t pressure;
};

int opensc2_pad_result(enum opensc2_side side, struct opensc2_pad_result *result);

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_TRACKPAD_H_ */
