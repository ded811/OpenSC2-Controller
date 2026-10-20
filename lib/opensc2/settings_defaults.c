/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * settings_defaults.c: every controller setting's default and range, as the protocol documentation
 * gives them (not public yet; docs/protocol.md lists the pages): its two settings tables, the
 * settings the firmware reads and the 49 it only stores. Vendor command 0x87 clamps each write to
 * its setting's range; settings 48, 84 and 85 go to the IMU unclamped (their table entries are
 * never used), so they have no range here.
 */
#include <stdint.h>
#include <opensc2/settings.h>

#define NO_CLAMP INT16_MIN, INT16_MAX

static const struct {
	int16_t def, min, max;
} table[OPENSC2_SETTINGS_COUNT] = {
	[0] = { 0, 0, 10 },             /* MOUSE_SENSITIVITY: stored only */
	[1] = { 2, 0, 10 },             /* MOUSE_ACCELERATION */
	[2] = { 0, 0, 360 },            /* the pad rotation's angle */
	[3] = { 1200, 0, 25000 },
	[4] = { 0, 0, 1 },
	[5] = { 0, 0, 1 },
	[6] = { 0, 0, 255 },
	[7] = { 0, 0, 255 },
	[8] = { 0, 0, 1 },              /* Steam's {8, 7} is stored as 1 */
	[9] = { 1, 0, 1 },              /* lizard mode */
	[10] = { 7000, 0, 16384 },
	[11] = { 200, 0, 1000 },        /* lizard mode's glide threshold */
	[12] = { 100, 1, 1000 },        /* its glide length */
	[13] = { 50, 1, 500 },
	[14] = { 5500, 0, 32767 },      /* lizard mode's tick spacing */
	[15] = { 923, 0, 2000 },
	[16] = { 382, 0, 2000 },
	[17] = { 2, 1, 10 },
	[18] = { 8000, 0, 20000 },
	[19] = { 1770, 0, 4096 },
	[20] = { 1630, 0, 4096 },
	[21] = { 5, 0, 500 },
	[22] = { 2, 0, 8 },
	[23] = { 2, 0, 8 },
	[24] = { 20, 0, 30 },           /* the pad rotation on */
	[25] = { 40, 3, 99 },
	[26] = { 0, 0, 1 },
	[27] = { -10, -24, 12 },        /* the left grip's rumble gain, dB */
	/* 28 and 29: defaults above their range, as on the real controller; they read back as such
	 * until a write limits them */
	[28] = { 16500, 0, 4096 },
	[29] = { 15000, 0, 4096 },
	[30] = { 500, 100, 1000 },      /* stick touch on */
	[31] = { 400, 0, 800 },         /* stick touch off */
	[32] = { 800, 100, 1200 },
	[33] = { 0, 0, 1 },             /* no grip level in the trackpad chip */
	[34] = { 100, 25, 400 },        /* grip sensitivity, % */
	[35] = { 80, 25, 100 },         /* grip release ratio, % */
	[36] = { 0, 0, 100 },           /* IR LED brightness */
	[37] = { 0, 0, 1 },             /* LED colour override */
	[38] = { 1, 0, 1 },             /* LED brightness curve */
	[39] = { 0, 0, 1 },
	[40] = { 20, 1, 180 },
	[41] = { 3900, 0, 25000 },
	[42] = { 1, 0, 1 },
	[43] = { 0, 0, 1 },
	[44] = { 50, 0, 100 },
	[45] = { 50, 0, 100 },          /* status LED brightness */
	[46] = { 0, 0, 2 },             /* raw sticks: 1 mV, 2 counts */
	[47] = { 0, 0, 16 },
	[48] = { 0, NO_CLAMP },         /* the IMU stream: to the IMU, unclamped */
	[49] = { 2, 1, 2 },
	[50] = { 900, 0, 32767 },       /* idle timeout on battery, s */
	[51] = { 250, 0, 32767 },
	[52] = { 40, -1, 100 },         /* left click pressure, % (-1: 100) */
	[53] = { 40, -1, 100 },         /* right click pressure */
	[54] = { 100, 0, 100 },
	[55] = { 100, 0, 100 },
	[56] = { 10, 0, 100 },
	[57] = { 10, 0, 100 },
	[58] = { 0, 0, 100 },
	[59] = { 0, 0, 100 },
	[60] = { 0, 0, 1 },             /* raw pad pressure */
	[61] = { 0, 0, 15 },
	[62] = { 0, 0, 2 },             /* raw triggers: 1 mV, 2 counts */
	[63] = { 150, 0, 300 },
	[64] = { 4, 1, 16 },
	[65] = { 1, 0, 1 },             /* pad position filter */
	[66] = { 1, 0, 1 },             /* pad edge scaling */
	[67] = { 0, 0, 7 },
	[68] = { 90, 40, 99 },          /* trigger digital threshold, % */
	[69] = { 1, 0, 1 },
	[70] = { 1, 0, 2 },             /* lizard mode's ticks and clicks play only at 1 */
	[71] = { 1, 0, 1 },             /* the Steam watchdog */
	[72] = { 1300, 0, 16000 },      /* pad touch on */
	[73] = { 1000, 0, 16000 },      /* pad touch off */
	[74] = { 3, 0, 3 },
	[75] = { 0, 0, 1 },             /* amplifier held on */
	[76] = { -3, -24, 6 },          /* master haptic gain, dB */
	[77] = { 0, 0, 1 },             /* reboot instead of power-off */
	[78] = { 1, 0, 1 },
	[79] = { 2, 1, 4 },
	[80] = { 1, 0, 2 },             /* touch stabiliser: 1 lizard mode's right pad, 2 both pads */
	[81] = { 0, 0, 1 },
	[82] = { 3, 0, 3 },             /* ignore a pad while its stick is touched: bit 0 L, 1 R */
	[83] = { 1, 0, 1 },
	[84] = { 1, NO_CLAMP },         /* gyro bias subtraction: to the IMU, unclamped */
	[85] = { 100, NO_CLAMP },       /* a gyro threshold: to the IMU, unclamped */
};

int16_t opensc2_setting_default(uint8_t number)
{
	return number < OPENSC2_SETTINGS_COUNT ? table[number].def : 0;
}

int16_t opensc2_setting_clamp(uint8_t number, int16_t value)
{
	if (number >= OPENSC2_SETTINGS_COUNT) {
		return value;
	}
	return value < table[number].min ? table[number].min :
	       value > table[number].max ? table[number].max : value;
}
