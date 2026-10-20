/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * OpenSC2 blank canvas: a Steam Controller 2 for the official puck on a SuperMini / Pro Micro
 * nRF52840, wireless (through the puck) or wired (on its USB cable). Every input and every
 * optional output is set up in the board overlay; this file joins them to the library.
 *
 * Modes, as on the real controller: a boot with USB power is wired; Steam pairs the board with
 * its puck over the cable and reboots it into wireless mode, and it connects through the puck.
 * The pairing button chooses too: held while powering up, this boot is wireless; while wired,
 * pressed before a host configures the board, or held for 3 s (the LED goes dark), it reboots
 * into wireless mode when it is released. Acting on the release keeps the button up at the next
 * boot, so the warm-restart mark brings the board back, not a button still held. Only in the
 * charger test build (test-charger.conf, a testing aid for a board without buttons, not the real
 * controller's behaviour) does the library also ask for that reboot by itself, when no host has
 * talked to the wired board within 5 s; on_power() below handles it like STEAM's.
 *
 * Identity: the library's development identity ("OSC"/"OSB" serials made from the chip's ID, not
 * a real controller's). Change it in prj.conf (CONFIG_OPENSC2_*) or on the board with the shell:
 * `opensc2 id set unit OSC2MYBOARD01` (keep the OSC prefix: the hardware test suite needs it).
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#if defined(CONFIG_POWEROFF)
#include <zephyr/sys/poweroff.h>
#endif
#include <hal/nrf_power.h>
#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif
#include "app.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define USER DT_PATH(zephyr_user)
#define PAIR_HOLD_MS 3000   /* wired: hold the pairing button this long, then release, to go wireless */

/* The Adafruit UF2 bootloader opens its USB drive when it finds this in GPREGRET. */
#define UF2_BOOTLOADER_MAGIC 0x57

static void reboot_to_uf2(void)
{
	nrf_power_gpregret_set(NRF_POWER, 0, UF2_BOOTLOADER_MAGIC);
	opensc2_reboot(OPENSC2_REBOOT_PLAIN, "UF2 bootloader");
}

/* ---- the mode of this boot ------------------------------------------------------------------- */

#if DT_NODE_EXISTS(DT_NODELABEL(opensc2_pair))
static const struct gpio_dt_spec pair_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(opensc2_pair), gpios);
#endif
#if DT_NODE_HAS_PROP(USER, mode_wireless_gpios)
static const struct gpio_dt_spec mode_pin = GPIO_DT_SPEC_GET(USER, mode_wireless_gpios);
#endif

/* The pairing button was held at power-up: ignore it until it is released, so the press that
 * chose the mode does not also start pairing. */
static bool pair_held_at_boot;

static bool pin_active(const struct gpio_dt_spec *pin)
{
	/* Read twice, 5 ms apart: a bouncing contact counts as released. */
	if (!gpio_is_ready_dt(pin) || gpio_pin_get_dt(pin) != 1) {
		return false;
	}
	k_msleep(5);
	return gpio_pin_get_dt(pin) == 1;
}

static enum opensc2_boot_mode boot_mode(void)
{
	enum opensc2_boot_mode mode = OPENSC2_BOOT_DEFAULT;

#if DT_NODE_HAS_PROP(USER, mode_wireless_gpios)
	if (gpio_is_ready_dt(&mode_pin) && gpio_pin_configure_dt(&mode_pin, GPIO_INPUT) == 0 &&
	    pin_active(&mode_pin)) {
		LOG_INF("Mode switch: wireless");
		mode = OPENSC2_BOOT_WIRELESS;
	}
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(opensc2_pair))
	/* gpio-keys has already set the pin up as an input with its pull-up. */
	if (pin_active(&pair_pin)) {
		LOG_INF("Pairing button held at power-up: wireless this time");
		pair_held_at_boot = true;
		mode = OPENSC2_BOOT_WIRELESS;
	}
#endif
	return mode;
}

/* ---- library callbacks ----------------------------------------------------------------------- */

static void on_link(const struct opensc2_link_info *info, void *user_data)
{
	ARG_UNUSED(user_data);
	status_link(info->state);
	if (info->reason == OPENSC2_REASON_PAIRED) {
		LOG_INF("Paired (slot %u)", info->slot + 1);
	} else if (info->reason == OPENSC2_REASON_PAIRING_FAILED) {
		LOG_INF("Pairing failed: open the puck's pairing window in Steam, then press pair");
	}
	if (info->state == OPENSC2_LINK_OFF && info->reason == OPENSC2_REASON_PUCK_TURN_OFF) {
		LOG_INF("Turned off by the puck (the PC sleeps). Press any button to come back.");
	}
}

static void on_usb(const struct opensc2_usb_info *info, void *user_data)
{
	ARG_UNUSED(user_data);
	status_usb(info->state);
}

static void on_rumble(const struct opensc2_rumble *rumble, void *user_data)
{
	ARG_UNUSED(user_data);
	rumble_set(rumble);
}

static void on_haptic(const struct opensc2_haptic *haptic, void *user_data)
{
	ARG_UNUSED(user_data);
	rumble_haptic(haptic);
}

static void on_led(const struct opensc2_led *led, void *user_data)
{
	ARG_UNUSED(user_data);
	status_led(led);
}

static void on_setting(uint8_t number, int16_t value, void *user_data)
{
	ARG_UNUSED(user_data);
	status_setting(number, value);
	if (number == OPENSC2_SETTING_LIZARD_MODE) {
		LOG_DBG("Lizard mode %s", value ? "on" : "off");
	} else if (number == OPENSC2_SETTING_IMU_MODE) {
		LOG_INF("Steam set the IMU mode to 0x%02x", (unsigned int)value);
	}
}

static void on_host_asleep(void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_INF("The PC is asleep");
}

/* A cue (the power-on chime, a transport's cue, the power-off chime, ...). This board has no
 * actuator of its own for them: a haptics renderer would start OpenSC2's sound for the cue here. */
static void on_cue(const struct opensc2_cue_event *cue, void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_INF("Cue %u (the real controller's script %u, %d dB)", cue->cue, cue->script,
		cue->gain_db);
}

/* The system state machine's shutdown has run (the link dropped, the power-off cue, STEAM let go):
 * off, the way Zephyr's nordic system_off sample does it, with STEAM as the pin that wakes the
 * chip (a new USB supply wakes the nRF52840 too). Only a battery build gets here: without a
 * battery (CONFIG_OPENSC2_BATTERY off) the library asks for a reboot instead. */
static void power_off(void)
{
#if defined(CONFIG_POWEROFF)
	int pins = inputs_wake_pins();

	LOG_INF("Power off: %d wake-up pin%s, or a new USB supply", pins, pins == 1 ? "" : "s");
	status_off();
	k_msleep(20);   /* the log */
	sys_poweroff();
#else
	opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL);
#endif
}

static void on_power(enum opensc2_power_request request, void *user_data)
{
	ARG_UNUSED(user_data);
	switch (request) {
	case OPENSC2_POWER_TURN_OFF_PUCK:
		/* The library answers "disconnecting" and goes OFF once the puck stops. */
		break;
	case OPENSC2_POWER_TURN_OFF:
		/* Without the system state machine only (it handles the turn-off itself). */
		opensc2_stop();
		break;
	case OPENSC2_POWER_OFF:
		power_off();
		break;
	case OPENSC2_POWER_REBOOT:
	case OPENSC2_POWER_FACTORY_RESET:
	case OPENSC2_POWER_USB_PLUGGED:
		opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL);
		break;
	case OPENSC2_POWER_REBOOT_KEEP_LINK:
		/* Steam's "reboot into wireless mode", after it paired the board over the cable; STEAM
		 * pressed while no host set the board up; in the charger test build only, no host
		 * within 5 s. */
		opensc2_reboot(OPENSC2_REBOOT_WIRELESS, NULL);
		break;
	case OPENSC2_POWER_USB_UNPLUGGED:
		/* Without the system state machine only (it shuts down itself, then asks for
		 * OPENSC2_POWER_OFF). The cable was pulled while wired and the board still runs, on a
		 * battery or another supply: come back wireless on it. A board on USB power alone has
		 * already gone dark. */
		opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL);
		break;
	case OPENSC2_POWER_BOOTLOADER:
		/* Steam's firmware update. The UF2 bootloader does not speak Steam's update protocol,
		 * so by default the request is ignored. */
		if (IS_ENABLED(CONFIG_APP_OBEY_BOOTLOADER_REQUEST)) {
			reboot_to_uf2();
		} else {
			LOG_WRN("Ignoring Steam's request to enter the bootloader");
		}
		break;
	}
}

static int on_vendor(const struct opensc2_vendor_cmd *cmd, uint8_t *answer, uint8_t *answer_len,
		     void *user_data)
{
	ARG_UNUSED(answer);
	ARG_UNUSED(answer_len);
	ARG_UNUSED(user_data);
	LOG_INF("Vendor command 0x%02x (%s, %u bytes) not handled", cmd->opcode,
		cmd->is_get ? "get" : "set", cmd->len);
	return -ENOTSUP;
}

static const struct opensc2_callbacks callbacks = {
	.link_changed = on_link,
	.rumble = on_rumble,
	.haptic = on_haptic,
	.led = on_led,
	.setting_changed = on_setting,
	.host_asleep = on_host_asleep,
	.power_request = on_power,
	.vendor_command = on_vendor,
	.usb_changed = on_usb,
	.cue = on_cue,
};

/* ---- buttons that control the link and the mode ---------------------------------------------- */

/* Wired: a press of the pairing button, acted on when it is released. */
static bool pair_pressed;          /* a press was seen (a release alone does nothing) */
static bool pair_no_host;          /* it began before a host configured the board */
static int64_t pair_pressed_ms;

/* Held 3 s: the LED goes dark, so the user knows the release now reboots. */
static void hold_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	status_hold_cue(true);
}
static K_WORK_DELAYABLE_DEFINE(hold_work, hold_fn);

/* Back to wireless, from the system work queue (rebooting leaves the bus first). */
static const char *to_wireless_why;

static void to_wireless_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	opensc2_reboot(OPENSC2_REBOOT_WIRELESS, to_wireless_why);
}
static K_WORK_DEFINE(to_wireless_work, to_wireless_fn);

static void wired_pair_button(bool pressed)
{
	if (pressed) {
		pair_pressed = true;
		pair_pressed_ms = k_uptime_get();
		pair_no_host = opensc2_usb_state() != OPENSC2_USB_CONFIGURED;
		if (!pair_no_host) {
			k_work_reschedule(&hold_work, K_MSEC(PAIR_HOLD_MS));
		}
		return;
	}
	if (!pair_pressed) {
		return;
	}
	pair_pressed = false;
	(void)k_work_cancel_delayable(&hold_work);
	if (pair_no_host) {
		/* No host (a charger, a power bank): back to wireless, like STEAM. */
		to_wireless_why = "pair button, no host";
	} else if (k_uptime_get() - pair_pressed_ms >= PAIR_HOLD_MS) {
		to_wireless_why = "pair button held 3 s";
	} else {
		status_hold_cue(false);   /* a short press while configured: nothing */
		return;
	}
	k_work_submit(&to_wireless_work);
}

void app_pair_button(bool pressed)
{
	if (pair_held_at_boot) {
		pair_held_at_boot = pressed;   /* released: from now on a normal button */
		return;
	}
	if (opensc2_mode() == OPENSC2_MODE_WIRED) {
		wired_pair_button(pressed);
		return;
	}
	if (pressed) {
		LOG_INF("Pairing: Steam must have the puck's pairing window open");
		opensc2_pair_start(0);
	}
}

void app_button_pressed(void)
{
#if !defined(CONFIG_OPENSC2_SYSTEM)
	/* After a turn-off any button brings the controller back. With the system state machine
	 * the real controller's rules decide instead: on USB power with wireless off, STEAM
	 * reboots (the next boot is wired); on battery STEAM wakes the board from power-off. */
	if (opensc2_mode() == OPENSC2_MODE_WIRELESS && opensc2_link_state() == OPENSC2_LINK_OFF) {
		opensc2_start();
	}
#endif
}

int main(void)
{
	struct opensc2_config config = {
		.callbacks = &callbacks,
		.bond_slot = OPENSC2_SLOT_STORED,
		.start = true,
	};
	int err;

	status_init();
	inputs_init();
	rumble_init();
	trackpad_init();
	imu_init();
	battery_init();

	config.boot_mode = boot_mode();
	err = opensc2_init(&config);
	if (err) {
		LOG_ERR("opensc2_init failed (%d)", err);
		return err;
	}
	status_mode(opensc2_mode());
	return 0;
}

#if defined(CONFIG_SHELL)
static int cmd_uf2(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	/* The reboot does not wait for the message: with the terminal gone the print can block. */
	nrf_power_gpregret_set(NRF_POWER, 0, UF2_BOOTLOADER_MAGIC);
	opensc2_reboot_later(OPENSC2_REBOOT_PLAIN, "UF2 bootloader", 100);
	shell_print(sh, "Rebooting into the UF2 bootloader");
	return 0;
}
SHELL_CMD_REGISTER(uf2, NULL, "Reboot into the UF2 bootloader (to flash a new .uf2)", cmd_uf2);
#endif
