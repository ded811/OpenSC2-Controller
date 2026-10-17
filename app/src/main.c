/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * OpenSC2 blank canvas: a Steam Controller 2 for the official puck on a SuperMini / Pro Micro
 * nRF52840. Every input and every optional output is set up in the board overlay; this file joins
 * them to the library.
 *
 * Identity: the library's development identity ("OSC"/"OSB" serials made from the chip's ID, not
 * a real controller's). Change it in prj.conf (CONFIG_OPENSC2_*) or on the board with the shell:
 * `opensc2 id set unit ABC0123456789`.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif
#include "app.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* The Adafruit UF2 bootloader opens its USB drive when it finds this in GPREGRET. */
#define UF2_BOOTLOADER_MAGIC 0x57

static void reboot_to_uf2(void)
{
	nrf_power_gpregret_set(NRF_POWER, 0, UF2_BOOTLOADER_MAGIC);
	sys_reboot(SYS_REBOOT_COLD);
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

static void on_power(enum opensc2_power_request request, void *user_data)
{
	ARG_UNUSED(user_data);
	switch (request) {
	case OPENSC2_POWER_TURN_OFF_PUCK:
		/* The library answers "disconnecting" and goes OFF once the puck stops. */
		break;
	case OPENSC2_POWER_TURN_OFF:
		opensc2_stop();
		break;
	case OPENSC2_POWER_REBOOT:
	case OPENSC2_POWER_REBOOT_KEEP_LINK:
	case OPENSC2_POWER_FACTORY_RESET:
		sys_reboot(SYS_REBOOT_COLD);
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
};

/* ---- buttons that control the link ----------------------------------------------------------- */

void app_pair_button(void)
{
	LOG_INF("Pairing: Steam must have the puck's pairing window open");
	opensc2_pair_start(0);
}

void app_button_pressed(void)
{
	/* After a turn-off any button brings the controller back, as STEAM does on the real one. */
	if (opensc2_link_state() == OPENSC2_LINK_OFF) {
		opensc2_start();
	}
}

int main(void)
{
	static const struct opensc2_config config = {
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

	err = opensc2_init(&config);
	if (err) {
		LOG_ERR("opensc2_init failed (%d)", err);
		return err;
	}
	return 0;
}

#if defined(CONFIG_SHELL)
static int cmd_uf2(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "Rebooting into the UF2 bootloader");
	k_msleep(100);
	reboot_to_uf2();
	return 0;
}
SHELL_CMD_REGISTER(uf2, NULL, "Reboot into the UF2 bootloader (to flash a new .uf2)", cmd_uf2);
#endif
