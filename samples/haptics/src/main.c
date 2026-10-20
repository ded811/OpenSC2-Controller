/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * The haptics synthesiser with real outputs, wired or wireless (built; not yet tested on
 * hardware). With CONFIG_OPENSC2_HAPTICS_SYNTH the library itself renders everything Steam plays
 * on the controller's haptics (rumble, pulses, clicks, tones, sweeps, scripts, PCM streams) into
 * sample blocks, and the outputs set up from the overlay play them: the grips on an I2S amplifier,
 * the pads on a PWM "DAC" (or, with drv2605.conf, one actuator on a DRV2605). This file only starts
 * the library, plays a start-up cue of OpenSC2's own, and adds a shell command to try the sounds
 * by hand:
 *
 *   hap cue <0..10> [level]           a cue on both grips (the level: 1 quiet, 2 normal)
 *   hap script <1..17> [chan] [dB]    a built-in script (chan: OPENSC2_HAPTIC_* bits, hex)
 *   hap click <a|b> [chan] [dB]       a click on the pads
 *   hap stop [chan]                   stop everything
 *   hap fall                          a free fall: a drop sound, half of the time
 *   hap info                          what plays
 *
 * There is no power_request callback: the library does the reboots Steam asks for itself, and it
 * ignores Steam's request to enter a bootloader for a firmware update.
 */
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <opensc2/opensc2.h>
#include <opensc2/haptics.h>

LOG_MODULE_REGISTER(haptics_sample, LOG_LEVEL_INF);

/* ---- The pairing button: the same rules in every sample ------------------------------------- */

static const struct gpio_dt_spec pair_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(opensc2_pair), gpios);
static bool pair_held_at_boot;   /* the press that chose wireless: ignored until it is released */
static bool pair_down;

/* Is the button held while the board starts? gpio-keys has already set the pin up with its
 * pull-up. Read twice, 5 ms apart: a bouncing contact counts as released. */
static bool pair_held(void)
{
	if (!gpio_is_ready_dt(&pair_pin) || gpio_pin_get_dt(&pair_pin) != 1) {
		return false;
	}
	k_msleep(5);
	return gpio_pin_get_dt(&pair_pin) == 1;
}

/* Wired: pressed and released, reboot into wireless mode (on the release, so that the button is
 * up again at the next boot). Wireless: pressed, answer a puck's open pairing window (untested
 * with Steam, which pairs controllers over the cable). */
static void pair_button(bool pressed)
{
	if (pair_held_at_boot) {
		pair_held_at_boot = pressed;   /* released: a normal button from now on */
		return;
	}
	if (opensc2_mode() == OPENSC2_MODE_WIRED) {
		if (pair_down && !pressed) {
			opensc2_reboot_later(OPENSC2_REBOOT_WIRELESS, "pair button", 0);
		}
	} else if (pressed) {
		opensc2_pair_start(0);   /* 0: the default window, 60 s */
	}
	pair_down = pressed;
}

static void on_button(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type == INPUT_EV_KEY && evt->code == OPENSC2_KEY_PAIR) {
		pair_button(evt->value != 0);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(opensc2_buttons)), on_button, NULL);

/* ---- Trying the sounds by hand ---------------------------------------------------------------- */

static uint8_t arg_u8(size_t argc, char **argv, size_t i, uint8_t fallback, int base)
{
	return i < argc ? (uint8_t)strtol(argv[i], NULL, base) : fallback;
}

static int cmd_cue(const struct shell *sh, size_t argc, char **argv)
{
	int err = opensc2_haptics_play_cue(arg_u8(argc, argv, 1, 0, 10),
					   arg_u8(argc, argv, 2, opensc2_chime_level(), 10));

	shell_print(sh, "cue: %d", err);
	return 0;
}

static int cmd_script(const struct shell *sh, size_t argc, char **argv)
{
	int n = opensc2_haptics_play_script(arg_u8(argc, argv, 2, 0x0C, 16),
					    arg_u8(argc, argv, 1, 1, 10),
					    (int8_t)(argc > 3 ? atoi(argv[3]) : -12));

	shell_print(sh, "script: %d channel(s)", n);
	return 0;
}

static int cmd_click(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t click = argc > 1 && argv[1][0] == 'b' ? OPENSC2_HAPTIC_CMD_CLICK_B :
			OPENSC2_HAPTIC_CMD_CLICK_A;
	int n = opensc2_haptics_click(arg_u8(argc, argv, 2, 0x03, 16), click,
				      (int8_t)(argc > 3 ? atoi(argv[3]) : 0));

	shell_print(sh, "click: %d channel(s)", n);
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "stop: %d", opensc2_haptics_stop(arg_u8(argc, argv, 1, 0x0F, 16)));
	return 0;
}

static int cmd_fall(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	opensc2_haptics_free_fall();
	shell_print(sh, "free fall reported");
	return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	struct opensc2_haptics_info info;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	(void)opensc2_haptics_info(&info);
	for (int p = 0; p < 2; p++) {
		shell_print(sh, "%s: %s, output %s, %u blocks, %u underruns", p ? "grips" : "pads",
			    info.busy[p] ? "playing" : "idle", info.output[p] ? "yes" : "none",
			    info.blocks[p], info.underruns[p]);
	}
	shell_print(sh, "scripts 0x%x, lost commands %u, stream bytes %u %u %u %u", info.scripts,
		    info.lost, info.stream_fill[0], info.stream_fill[1], info.stream_fill[2],
		    info.stream_fill[3]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(hap_cmds,
	SHELL_CMD_ARG(cue, NULL, "Play a cue: <0..10> [level]", cmd_cue, 2, 1),
	SHELL_CMD_ARG(script, NULL, "Play a script: <1..17> [channels, hex] [dB]", cmd_script, 2, 2),
	SHELL_CMD_ARG(click, NULL, "Click: <a|b> [channels, hex] [dB]", cmd_click, 2, 2),
	SHELL_CMD_ARG(stop, NULL, "Stop: [channels, hex]", cmd_stop, 1, 1),
	SHELL_CMD(fall, NULL, "Report a free fall", cmd_fall),
	SHELL_CMD(info, NULL, "What plays", cmd_info),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(hap, &hap_cmds, "The haptics synthesiser's sounds", NULL);

int main(void)
{
	struct opensc2_config config = { .start = true };
	int err;

	if (pair_held()) {
		pair_held_at_boot = true;
		config.boot_mode = OPENSC2_BOOT_WIRELESS;   /* held while plugging in: wireless */
	}
	err = opensc2_init(&config);
	if (err) {
		/* The console still starts (in wireless mode), so this can be read there. */
		LOG_ERR("opensc2_init failed (%d)", err);
		return 0;
	}
	/* A start-up cue of OpenSC2's own at the stored chime level, as the real controller plays one:
	 * cue 0 when wired, the puck slot's cue (3 or 4) when wireless. */
	(void)opensc2_haptics_play_cue(opensc2_mode() == OPENSC2_MODE_WIRED ?
				       OPENSC2_HAPTICS_CUE_USB_START :
				       opensc2_selected_slot() ? OPENSC2_HAPTICS_CUE_PUCK_2 :
				       OPENSC2_HAPTICS_CUE_PUCK, opensc2_chime_level());
	return 0;
}
