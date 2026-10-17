/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Bit numbers of the 32-bit button word of the Steam Controller 2 state report (input report 0x42).
 * Plain numbers, so the same header works in C and in devicetree overlays (for example as the
 * `zephyr,code` of a gpio-keys child: the application then sets BIT(code) in the button word).
 *
 * Names follow SDL's open-source driver (SDL_hidapi_steam_triton.c, TRITON_* button flags). One
 * naming disagreement: the controller's own firmware calls bit 6 "MENU" and bit 14 "Left view";
 * SDL calls them VIEW and MENU. These names are SDL's.
 */
#ifndef OPENSC2_DT_BINDINGS_INPUTS_H_
#define OPENSC2_DT_BINDINGS_INPUTS_H_

/* The 20 buttons. */
#define OPENSC2_KEY_A                   0   /* TRITON_LBUTTON_A */
#define OPENSC2_KEY_B                   1   /* TRITON_LBUTTON_B */
#define OPENSC2_KEY_X                   2   /* TRITON_LBUTTON_X */
#define OPENSC2_KEY_Y                   3   /* TRITON_LBUTTON_Y */
#define OPENSC2_KEY_QAM                 4   /* TRITON_HBUTTON_QAM, quick access ("...") */
#define OPENSC2_KEY_R3                  5   /* TRITON_LBUTTON_R3, right stick click */
#define OPENSC2_KEY_VIEW                6   /* TRITON_LBUTTON_VIEW (right-hand menu button) */
#define OPENSC2_KEY_R4                  7   /* TRITON_HBUTTON_R4, right upper grip */
#define OPENSC2_KEY_R5                  8   /* TRITON_LBUTTON_R5, right lower grip */
#define OPENSC2_KEY_R                   9   /* TRITON_LBUTTON_R, right bumper */
#define OPENSC2_KEY_DPAD_DOWN           10  /* TRITON_LBUTTON_DPAD_DOWN */
#define OPENSC2_KEY_DPAD_RIGHT          11  /* TRITON_LBUTTON_DPAD_RIGHT */
#define OPENSC2_KEY_DPAD_LEFT           12  /* TRITON_LBUTTON_DPAD_LEFT */
#define OPENSC2_KEY_DPAD_UP             13  /* TRITON_LBUTTON_DPAD_UP */
#define OPENSC2_KEY_MENU                14  /* TRITON_LBUTTON_MENU (left-hand view button) */
#define OPENSC2_KEY_L3                  15  /* TRITON_LBUTTON_L3, left stick click */
#define OPENSC2_KEY_STEAM               16  /* TRITON_LBUTTON_STEAM */
#define OPENSC2_KEY_L4                  17  /* TRITON_HBUTTON_L4, left upper grip */
#define OPENSC2_KEY_L5                  18  /* TRITON_LBUTTON_L5, left lower grip */
#define OPENSC2_KEY_L                   19  /* TRITON_LBUTTON_L, left bumper */

/* Bits set by the touch sensors, the trackpads and the triggers. */
#define OPENSC2_KEY_RIGHT_STICK_TOUCH   20  /* TRITON_RIGHT_JOYSTICK_TOUCH */
#define OPENSC2_KEY_RIGHT_PAD_TOUCH     21  /* TRITON_RIGHT_TOUCHPAD_TOUCH */
#define OPENSC2_KEY_RIGHT_PAD_CLICK     22  /* TRITON_RIGHT_TOUCHPAD_CLICK */
#define OPENSC2_KEY_RIGHT_TRIGGER_CLICK 23  /* TRITON_RIGHT_TRIGGER_CLICK: past its threshold */
#define OPENSC2_KEY_LEFT_STICK_TOUCH    24  /* TRITON_LEFT_JOYSTICK_TOUCH */
#define OPENSC2_KEY_LEFT_PAD_TOUCH      25  /* TRITON_LEFT_TOUCHPAD_TOUCH */
#define OPENSC2_KEY_LEFT_PAD_CLICK      26  /* TRITON_LEFT_TOUCHPAD_CLICK */
#define OPENSC2_KEY_LEFT_TRIGGER_CLICK  27  /* TRITON_LEFT_TRIGGER_CLICK */
#define OPENSC2_KEY_RIGHT_GRIP_TOUCH    28  /* TRITON_RIGHT_GRIP_TOUCH */
#define OPENSC2_KEY_LEFT_GRIP_TOUCH     29  /* TRITON_LEFT_GRIP_TOUCH */

/* Not a report bit: a code applications may give their pairing button (bit numbers stop at 29). */
#define OPENSC2_KEY_PAIR                64

#endif /* OPENSC2_DT_BINDINGS_INPUTS_H_ */
