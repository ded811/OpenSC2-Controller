/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * cal_shell.c: "opensc2 cal", the calibration on the console (wireless mode), so a board's sticks,
 * triggers and pads can be calibrated without a host tool. The steps are those of vendor commands
 * 0xD8, 0xC0 and 0xC3 and run through the same code (osc2_cal_command()); the recordings take
 * their samples from the raw inputs while a link is up.
 *
 *   opensc2 cal                         what is stored and in use
 *   opensc2 cal sticks|triggers|pressure <0-3>
 *   opensc2 cal save <0|1> <mask>       vendor command 0xE2: one side's RAM values to flash
 *   opensc2 cal erase c6674885          vendor command 0xFE: every cal/ key deleted from flash
 */
#include <stdlib.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <opensc2/cal.h>
#include "internal.h"
#include "cal_store.h"

static const char *const step_text[4] = { "end (0)", "1", "2", "save (3)" };

static void show(const struct shell *sh)
{
	static const char *const names[] = { "left stick", "right stick", "left trigger",
					     "right trigger", "left pressure", "right pressure" };
	uint8_t st[16];

	osc2_cal_status(st);
	shell_print(sh, "generation %u, raw inputs in use 0x%02x", (unsigned int)opensc2_get_le32(st),
		    st[5]);
	for (int i = 0; i < 6; i++) {
		shell_print(sh, "  %-14s %s", names[i], (st[4] & BIT(i)) ? "calibrated" :
							     "not calibrated (reads 0 when raw)");
	}
	shell_print(sh, "recordings: sticks %s, triggers %s, pressure %s",
		    (st[8] & 0x80) ? step_text[st[8] & 3] : "idle",
		    (st[9] & 0x80) ? step_text[st[9] & 3] : "idle",
		    (st[10] & 0x80) ? step_text[st[10] & 3] : "idle");
	shell_print(sh, "LED trims %u %u %u %u %%, battery offset %d mV", opensc2_cal_led_trim(0),
		    opensc2_cal_led_trim(1), opensc2_cal_led_trim(2), opensc2_cal_led_trim(3),
		    opensc2_cal_volt_offset());
}

static bool parse_byte(const char *text, unsigned int max, uint8_t *out)
{
	char *end;
	unsigned long v = strtoul(text, &end, 0);

	if (*text == '\0' || *end != '\0' || v > max) {
		return false;
	}
	*out = (uint8_t)v;
	return true;
}

int osc2_cal_shell(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t p[4] = { 0 };
	uint8_t opcode;
	int rc;

	if (argc == 1) {
		show(sh);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "erase") == 0) {
		char *end;
		unsigned long key = strtoul(argv[2], &end, 16);

		if (*end != '\0') {
			shell_error(sh, "erase needs the key in hex");
			return -EINVAL;
		}
		opensc2_put_le32(p, (uint32_t)key);
		rc = osc2_cal_command(0xFE, p, 4, 4);
		shell_print(sh, rc ? "Refused: wrong key" :
			    "Every cal/ key is deleted from flash; RAM keeps them until the next boot");
		return rc;
	}
	if (argc == 4 && strcmp(argv[1], "save") == 0) {
		if (!parse_byte(argv[2], 1, &p[0]) || !parse_byte(argv[3], 0xFF, &p[1])) {
			shell_error(sh, "save <0|1> <mask: 1 trigger, 2 stick, 4 pressure>");
			return -EINVAL;
		}
		rc = osc2_cal_command(0xE2, p, 2, 2);
		if (rc) {
			shell_error(sh, "Refused (%d)", rc);
		} else {
			shell_print(sh, "Saving");
		}
		return rc;
	}
	if (argc == 3) {
		uint8_t step;

		if (!parse_byte(argv[2], 3, &step)) {
			shell_error(sh, "the step is 0 to 3");
			return -EINVAL;
		}
		if (strcmp(argv[1], "sticks") == 0) {
			opcode = 0xD8;
			p[1] = step;   /* 0xD8 reads its step from the second byte */
		} else if (strcmp(argv[1], "triggers") == 0) {
			opcode = 0xC0;
			p[0] = step;
		} else if (strcmp(argv[1], "pressure") == 0) {
			opcode = 0xC3;
			p[0] = step;
		} else {
			shell_error(sh, "sticks, triggers or pressure");
			return -EINVAL;
		}
		rc = osc2_cal_command(opcode, p, 2, 2);
		if (step == 1) {
			shell_print(sh, "%s", opcode == 0xD8 ? "Leave the sticks centred, then step 2 and "
				    "circle them fully, then step 3" : opcode == 0xC0 ?
				    "Leave the triggers released, then step 2 and pull them fully, then "
				    "step 3" : "Press both pads firmly after step 2, then step 3");
		}
		return rc;
	}
	shell_error(sh, "cal | cal sticks|triggers|pressure <0-3> | cal save <0|1> <mask> | "
			"cal erase <key>");
	return -EINVAL;
}
