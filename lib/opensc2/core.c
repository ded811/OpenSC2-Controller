/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * core.c: initialisation, the input state, identity, settings, the Steam watchdog, power requests
 * and the other public setters.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif
#if defined(CONFIG_POWEROFF)
#include <zephyr/sys/poweroff.h>
#endif
#include "internal.h"
#include "crash.h"
#include "hwdt.h"
#include "key_store.h"
#include "system.h"
#include "test_hooks.h"

LOG_MODULE_REGISTER(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

BUILD_ASSERT(CONFIG_ESB_MAX_PAYLOAD_LENGTH >= 128,
	     "OpenSC2 needs CONFIG_ESB_MAX_PAYLOAD_LENGTH of 128 or more: the puck sends packets "
	     "of up to 128 bytes");

/* The identity's Kconfig defaults, which opensc2_init() checks at boot (opensc2_identity_check())
 * and refuses when one is invalid. CMakeLists.txt checks the strings by the same rules at build
 * time, characters and lengths; the asserts here check the lengths and the product ID. */
BUILD_ASSERT(CONFIG_OPENSC2_PRODUCT_ID != 0,
	     "CONFIG_OPENSC2_PRODUCT_ID must not be 0 (the Steam Controller 2 is 0x1302)");
BUILD_ASSERT(sizeof(CONFIG_OPENSC2_UNIT_SERIAL) == OPENSC2_SERIAL_LEN + 1,
	     "CONFIG_OPENSC2_UNIT_SERIAL must be exactly 13 characters (0-9, A-Z)");
BUILD_ASSERT(sizeof(CONFIG_OPENSC2_BOARD_SERIAL) == OPENSC2_SERIAL_LEN + 1,
	     "CONFIG_OPENSC2_BOARD_SERIAL must be exactly 13 characters (0-9, A-Z)");
BUILD_ASSERT(sizeof(CONFIG_OPENSC2_GIT_REVISION) >= 2 &&
	     sizeof(CONFIG_OPENSC2_GIT_REVISION) <= OPENSC2_GIT_REVISION_MAX + 1,
	     "CONFIG_OPENSC2_GIT_REVISION must be 1 to 12 characters");

/* The identity's product name: the wired device's USB product string. A build without USB never
 * shows it, and keeps the real controller's. */
#if defined(CONFIG_OPENSC2_USB_PRODUCT)
#define DEFAULT_PRODUCT_NAME CONFIG_OPENSC2_USB_PRODUCT
#else
#define DEFAULT_PRODUCT_NAME "Steam Controller"
#endif
BUILD_ASSERT(sizeof(DEFAULT_PRODUCT_NAME) >= 2 &&
	     sizeof(DEFAULT_PRODUCT_NAME) <= OPENSC2_PRODUCT_NAME_MAX + 1,
	     "CONFIG_OPENSC2_USB_PRODUCT must be 1 to 31 characters");

#if CONFIG_OPENSC2_BUILD_TIME != 0
#define DEFAULT_BUILD_TIME ((uint32_t)CONFIG_OPENSC2_BUILD_TIME)
#else
#define DEFAULT_BUILD_TIME ((uint32_t)OPENSC2_CMAKE_BUILD_TIME)   /* when the build was configured */
#endif

/* A deliberate test below the floor stores its value at run time (`opensc2 id set build ...
 * force`); a default there would make every board built with it a forced-update case. */
BUILD_ASSERT(DEFAULT_BUILD_TIME > OSC2_BUILD_TIME_FLOOR,
	     "The build time (CONFIG_OPENSC2_BUILD_TIME, or with 0 this machine's clock) is at or "
	     "before 0x6A18D057: Steam would force an update into a bootloader this board does not "
	     "have");

#define WATCHDOG_MS 10000   /* Steam must repeat "lizard off" this often */

struct osc2_ctx osc2;

/* ---- identity -------------------------------------------------------------------------------- */

static void copy_string(char *dst, size_t size, const char *src)
{
	size_t n = 0;

	while (n + 1 < size && src[n] != '\0') {
		dst[n] = src[n];
		n++;
	}
	memset(&dst[n], 0, size - n);
}

static void kconfig_identity(struct opensc2_identity *id)
{
	memset(id, 0, sizeof(*id));
	copy_string(id->unit_serial, sizeof(id->unit_serial), CONFIG_OPENSC2_UNIT_SERIAL);
	copy_string(id->board_serial, sizeof(id->board_serial), CONFIG_OPENSC2_BOARD_SERIAL);
	copy_string(id->git_revision, sizeof(id->git_revision), CONFIG_OPENSC2_GIT_REVISION);
	copy_string(id->product_name, sizeof(id->product_name), DEFAULT_PRODUCT_NAME);
	id->product_id = CONFIG_OPENSC2_PRODUCT_ID;
	id->board_revision = CONFIG_OPENSC2_BOARD_REVISION;
	id->build_time = DEFAULT_BUILD_TIME;
	id->bootloader_build_time = CONFIG_OPENSC2_BOOTLOADER_BUILD_TIME;

#if defined(CONFIG_OPENSC2_SERIAL_FROM_CHIP)
	/* Development serials made from the chip's unique ID, so two boards never share one. "OSC"
	 * and "OSB" mark them as this library's, not a real controller's. */
	uint8_t chip[8] = { 0 };

	if (hwinfo_get_device_id(chip, sizeof(chip)) >= 5) {
		snprintf(id->unit_serial, sizeof(id->unit_serial), "OSC%02X%02X%02X%02X%02X",
			 chip[0], chip[1], chip[2], chip[3], chip[4]);
		snprintf(id->board_serial, sizeof(id->board_serial), "OSB%02X%02X%02X%02X%02X",
			 chip[7], chip[6], chip[5], chip[4], chip[3]);
	}
#endif
}

K_MUTEX_DEFINE(osc2_id_lock);

void osc2_identity_refresh(void)
{
	struct opensc2_identity id = osc2.base;
	k_spinlock_key_t key;

	osc2_id_apply(&id, &osc2.stored);
	if (opensc2_identity_check(&id) != 0) {
		LOG_ERR("The stored identity is not valid (0x%x): using the configured one",
			opensc2_identity_check(&id));
		id = osc2.base;
	}
	if (id.build_time <= OSC2_BUILD_TIME_FLOOR) {
		LOG_WRN("Build time 0x%08x: Steam forces an update on controllers at or below 0x%08x",
			id.build_time, OSC2_BUILD_TIME_FLOOR);
	}
	key = k_spin_lock(&osc2.lock);
	osc2.identity = id;
	k_spin_unlock(&osc2.lock, key);
}

void opensc2_identity_get(struct opensc2_identity *identity)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	*identity = osc2.identity;
	k_spin_unlock(&osc2.lock, key);
}

int osc2_identity_store_fields(uint8_t fields, const struct opensc2_identity *value)
{
	struct osc2_id_override next;
	struct opensc2_identity check;
	int err;

	(void)k_mutex_lock(&osc2_id_lock, K_FOREVER);
	next = osc2.stored;
	check = osc2.base;
	osc2_id_override_set(&next, fields, value);
	osc2_id_apply(&check, &next);
	if (opensc2_identity_check(&check) != 0) {
		(void)k_mutex_unlock(&osc2_id_lock);
		return -EINVAL;
	}
	osc2.stored = next;
	osc2_identity_refresh();
	err = osc2_storage_save_identity();
	(void)k_mutex_unlock(&osc2_id_lock);
	return err;
}

int opensc2_identity_store(const struct opensc2_identity *identity)
{
	/* Only the fields that are set: an empty or zero field keeps what is there. */
	return osc2_identity_store_fields(osc2_id_fields_set(identity), identity);
}

int opensc2_identity_clear(void)
{
	int err;

	(void)k_mutex_lock(&osc2_id_lock, K_FOREVER);
	memset(&osc2.stored, 0, sizeof(osc2.stored));
	osc2_identity_refresh();
	err = osc2_storage_save_identity();
	(void)k_mutex_unlock(&osc2_id_lock);
	return err;
}

/* ---- settings, LED, chime -------------------------------------------------------------------- */

void osc2_settings_restore_defaults(bool keep_imu)
{
	static const uint8_t imu[] = { OPENSC2_SETTING_IMU_MODE, OPENSC2_SETTING_IMU_BIAS,
				       OPENSC2_SETTING_IMU_THRESHOLD };
	int16_t kept[ARRAY_SIZE(imu)];
	k_spinlock_key_t key;

	for (size_t i = 0; i < ARRAY_SIZE(imu); i++) {
		kept[i] = osc2.settings[imu[i]];
	}
	for (int i = 0; i < OPENSC2_SETTINGS_COUNT; i++) {
		osc2.settings[i] = opensc2_setting_default((uint8_t)i);
	}
	if (keep_imu) {
		for (size_t i = 0; i < ARRAY_SIZE(imu); i++) {
			osc2.settings[imu[i]] = kept[i];
		}
	}
	key = k_spin_lock(&osc2.lock);
	osc2.led.override = false;
	osc2.led.brightness = (uint8_t)osc2.settings[OPENSC2_SETTING_LED_BRIGHTNESS];
	k_spin_unlock(&osc2.lock, key);
	osc2_sys_setting(OPENSC2_SETTINGS_ALL);
}

int16_t opensc2_setting_get(uint8_t number)
{
	return number < OPENSC2_SETTINGS_COUNT ? osc2.settings[number] : 0;
}

bool opensc2_lizard_mode(void)
{
	return osc2.settings[OPENSC2_SETTING_LIZARD_MODE] != 0;
}

void opensc2_led_get(struct opensc2_led *led)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	*led = osc2.led;
	k_spin_unlock(&osc2.lock, key);
}

void osc2_notify_led(void)
{
	struct opensc2_led led;

	opensc2_led_get(&led);
	osc2_test_note_led(&led);
	if (osc2.cb && osc2.cb->led) {
		osc2.cb->led(&led, osc2.user_data);
	}
}

uint8_t opensc2_chime_level(void)
{
	return osc2.chime;
}

/* ---- the Steam watchdog: a one-shot 10 s timer in the system work queue, whatever the link ---- */

static void watchdog_fn(struct k_work *work)
{
	ARG_UNUSED(work);
#if defined(CONFIG_OPENSC2_STEAM_WATCHDOG)
	if (osc2.settings[OPENSC2_SETTING_LIZARD_MODE] != 0) {
		return;   /* lizard mode is on: nothing to bring back */
	}
	LOG_INF("Steam went quiet: lizard mode on, settings back to their defaults");
	osc2_usblog("watchdog: lizard mode on, settings back to their defaults");
	osc2_lizard_switches(true);
	osc2_settings_restore_defaults(true);
	osc2_lizard_set(true);
	osc2_test_note_setting(OPENSC2_SETTINGS_ALL, 0);
	if (osc2.cb && osc2.cb->setting_changed) {
		osc2.cb->setting_changed(OPENSC2_SETTINGS_ALL, 0, osc2.user_data);
	}
	osc2_notify_led();
#endif
}
static K_WORK_DELAYABLE_DEFINE(watchdog_work, watchdog_fn);

void osc2_watchdog_arm(void)
{
	if (IS_ENABLED(CONFIG_OPENSC2_STEAM_WATCHDOG) &&
	    osc2.settings[OPENSC2_SETTING_STEAM_WATCHDOG] != 0) {
		k_work_reschedule(&watchdog_work, K_MSEC(WATCHDOG_MS));
	}
}

void osc2_watchdog_cancel(void)
{
	(void)k_work_cancel_delayable(&watchdog_work);
}

/* ---- power requests: acted on a little later, from the system work queue --------------------- */

static enum opensc2_power_request pending_power;
static const char *pending_why;
static const char *running_why;   /* the cause of the request being handled, else NULL */

/* What the library does itself when the application has no power_request callback. */
static void default_power(enum opensc2_power_request request)
{
	switch (request) {
	case OPENSC2_POWER_REBOOT:
	case OPENSC2_POWER_FACTORY_RESET:
	case OPENSC2_POWER_USB_PLUGGED:
	case OPENSC2_POWER_USB_UNPLUGGED:   /* without USB power the boot rule gives wireless */
		opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL);
		break;
	case OPENSC2_POWER_REBOOT_KEEP_LINK:
		opensc2_reboot(OPENSC2_REBOOT_WIRELESS, NULL);
		break;
	case OPENSC2_POWER_OFF:
#if defined(CONFIG_OPENSC2_BATTERY) && defined(CONFIG_POWEROFF)
		/* No wake-up pin of our own: a new USB supply (or a reset) wakes the chip. */
		sys_poweroff();
#else
		opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL);
#endif
		break;
	default:
		break;   /* turn-off and the bootloader: left to the application */
	}
}

void osc2_power_to_app(enum opensc2_power_request request, const char *why)
{
	running_why = why;
	if (osc2.cb && osc2.cb->power_request) {
		osc2.cb->power_request(request, osc2.user_data);
	} else {
		default_power(request);
	}
	running_why = NULL;
}

void osc2_power_now(enum opensc2_power_request request, const char *why)
{
	const char *text = why ? why : "";

	LOG_INF("Power request %d (%s)", request, text);
	osc2_usblog("power request %d: %s", request, text);
	osc2_test_note_power((uint8_t)request, why);
	if (osc2_sys_power_request(request, why)) {
		return;   /* the system state machine took it (it may pass it on later) */
	}
	osc2_power_to_app(request, why);
}

/* The pending request and its cause, written by several threads (the links' command threads, the
 * USB stack, the Bluetooth thread): always as a pair. */
static struct k_spinlock pending_lock;

static void power_work_fn(struct k_work *work)
{
	enum opensc2_power_request request;
	const char *why;
	k_spinlock_key_t key = k_spin_lock(&pending_lock);

	ARG_UNUSED(work);
	request = pending_power;
	why = pending_why;
	k_spin_unlock(&pending_lock, key);
	osc2_power_now(request, why);
}
static K_WORK_DELAYABLE_DEFINE(power_work, power_work_fn);

void osc2_schedule_power(enum opensc2_power_request request, uint32_t delay_ms, const char *why)
{
	k_spinlock_key_t key = k_spin_lock(&pending_lock);

	pending_power = request;
	pending_why = why;
	(void)k_work_reschedule(&power_work, K_MSEC(delay_ms));
	k_spin_unlock(&pending_lock, key);
}

const char *osc2_power_reason(void)
{
	return running_why;
}

/* ---- links ----------------------------------------------------------------------------------- */

bool osc2_link_active(enum osc2_link link)
{
#if defined(CONFIG_OPENSC2_TEST_HOOKS)
	if (link == OSC2_LINK_TEST) {
		/* The test hooks' host emulation stands in for the host of the mode's real link. */
		link = osc2_mode_link();
	}
#endif
	if (link == OSC2_LINK_USB) {
#if defined(CONFIG_OPENSC2_USB)
		return osc2_wired() && osc2_usb_ready();
#else
		return false;
#endif
	}
	if (link == OSC2_LINK_BLE) {
		return !osc2_wired() && osc2_ble_active();
	}
	if (link == OSC2_LINK_DOCK) {
		/* The dock carries no lizard mode, and the real controller's 0x81 and 0x85 do nothing
		 * from it (dock_link.h says when it is up). */
		return false;
	}
	return !osc2_wired() && osc2_radio_connected();
}

/* ---- input state ----------------------------------------------------------------------------- */

int opensc2_set_state(const struct opensc2_state *state)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);
	bool changed = osc2.state.buttons != state->buttons;

	osc2.state = *state;
	k_spin_unlock(&osc2.lock, key);
	if (changed) {
		osc2_sys_buttons_changed();
		osc2_lizard_buttons_changed();
	}
	return 0;
}

uint32_t osc2_sys_app_buttons(void)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);
	uint32_t b = osc2.state.buttons;

	k_spin_unlock(&osc2.lock, key);
	return b;
}

void opensc2_get_state(struct opensc2_state *state)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	*state = osc2.state;
	k_spin_unlock(&osc2.lock, key);
}

int opensc2_set_buttons(uint32_t mask, uint32_t value)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);
	uint32_t before = osc2.state.buttons;
	bool changed;

	osc2.state.buttons = (osc2.state.buttons & ~mask) | (value & mask);
	changed = osc2.state.buttons != before;
	k_spin_unlock(&osc2.lock, key);
	if (changed) {
		osc2_sys_buttons_changed();   /* the chords, STEAM and the idle monitor look again */
		osc2_lizard_buttons_changed();   /* lizard mode's keyboard hears of it now */
	}
	return 0;
}

int opensc2_set_stick(enum opensc2_side side, int16_t x, int16_t y)
{
	k_spinlock_key_t key;

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	osc2.state.stick[side].x = x;
	osc2.state.stick[side].y = y;
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_set_trigger(enum opensc2_side side, uint16_t value)
{
	k_spinlock_key_t key;

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	osc2.state.trigger[side] = MIN(value, OPENSC2_TRIGGER_MAX);
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_set_trackpad(enum opensc2_side side, const struct opensc2_trackpad *pad)
{
	k_spinlock_key_t key;

	if (side > OPENSC2_RIGHT) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	osc2.state.pad[side] = *pad;
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_set_imu(const struct opensc2_imu *imu)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	osc2.state.imu = *imu;
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

static bool battery_set;   /* the application has given its battery (opensc2_set_battery()) */

int opensc2_set_battery(const struct opensc2_battery *battery)
{
	k_spinlock_key_t key = k_spin_lock(&osc2.lock);

	osc2.battery = *battery;
	battery_set = true;
	k_spin_unlock(&osc2.lock, key);
#if defined(CONFIG_OPENSC2_USB)
	osc2_retained_level_set(battery->level);   /* kept over a warm reset, as the real gauge does */
#endif
	osc2_sys_battery_given();   /* from now on its level counts for the power rules */
	return 0;
}

uint8_t opensc2_battery_kept_level(void)
{
#if defined(CONFIG_OPENSC2_USB)
	return osc2_retained_level_at_boot();
#else
	return OPENSC2_BATTERY_NO_LEVEL;
#endif
}

/* The battery report before the application gives one. A battery build sends what the real
 * controller's first report after start-up carries, before its gauge has measured anything: the
 * level kept over a warm reset (else 255), charge state 0 and zeros. A board without a battery
 * reports a full one on USB power. */
static void battery_defaults(void)
{
	if (battery_set) {
		return;   /* given before opensc2_init(): kept */
	}
	if (IS_ENABLED(CONFIG_OPENSC2_BATTERY)) {
		osc2.battery = (struct opensc2_battery){
			.charge_state = OPENSC2_CHARGE_RESET,
			.level = opensc2_battery_kept_level(),
		};
		return;
	}
	osc2.battery = (struct opensc2_battery){
		.charge_state = OPENSC2_CHARGE_DONE,
		.level = 100,
		.battery_mv = 4100,
		.system_mv = 3300,
		.input_mv = 5000,
	};
}

int opensc2_send_report(const uint8_t *report, size_t len)
{
	int err;

	if (len < 1 || len > 64) {
		return -EINVAL;
	}
	err = osc2_input_send(report, len, false);
	if (err == 0) {
		osc2_test_count(OSC2_TEST_CNT_APP_REPORTS_SENT);
	}
	return err;
}

int opensc2_send_haptic_status(uint8_t channel_id, uint32_t flags)
{
	uint8_t report[OPENSC2_HAPTIC_STATUS_LEN];

	opensc2_encode_haptic_status(channel_id, flags, report);
	return opensc2_send_report(report, sizeof(report));
}

/* ---- bonds ----------------------------------------------------------------------------------- */

int opensc2_bond_get(uint8_t slot, struct opensc2_bond *bond)
{
	k_spinlock_key_t key;

	if (slot >= OPENSC2_BOND_SLOTS) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	*bond = osc2.bonds[slot];
	k_spin_unlock(&osc2.lock, key);
	return 0;
}

int opensc2_bond_set(uint8_t slot, const struct opensc2_bond *bond)
{
	k_spinlock_key_t key;
	int err;

	if (slot >= OPENSC2_BOND_SLOTS) {
		return -EINVAL;
	}
	key = k_spin_lock(&osc2.lock);
	if (opensc2_bond_valid(bond)) {
		osc2.bonds[slot] = *bond;
	} else {
		memset(&osc2.bonds[slot], 0, sizeof(osc2.bonds[slot]));
	}
	k_spin_unlock(&osc2.lock, key);
	err = osc2_storage_save_bond(slot);
	osc2_radio_post(OSC2_EV_BOND);
	return err;
}

int opensc2_select_slot(uint8_t slot)
{
	if (slot >= OPENSC2_BOND_SLOTS) {
		return -EINVAL;
	}
	return opensc2_select_transport((enum opensc2_transport)(OSC2_TRANSPORT_PUCK_1 + slot));
}

uint8_t opensc2_selected_slot(void)
{
	return osc2.slot;
}

/* ---- control --------------------------------------------------------------------------------- */

int opensc2_init(const struct opensc2_config *config)
{
	int err;

	if (osc2.initialised) {
		return -EALREADY;
	}
	if (!config) {
		LOG_ERR("opensc2_init() needs a configuration");
		osc2_mode_init_failed();   /* the console, so the error can be read */
		return -EINVAL;
	}

	kconfig_identity(&osc2.base);
	if (config->identity) {
		/* Empty and zero fields keep the Kconfig value. */
		osc2_id_copy(&osc2.base, config->identity, osc2_id_fields_set(config->identity));
	}
	if (opensc2_identity_check(&osc2.base) != 0) {
		LOG_ERR("Invalid identity (problems 0x%x): serials must be 13 characters 0-9/A-Z, "
			"the git revision 1 to 12 characters, the product name 1 to 31 printable "
			"characters", opensc2_identity_check(&osc2.base));
		osc2_mode_init_failed();
		return -EINVAL;
	}
	if (osc2.base.product_id != OPENSC2_PRODUCT_ID) {
		LOG_WRN("Product ID 0x%04x: Steam knows the Steam Controller 2 as 0x%04x",
			osc2.base.product_id, OPENSC2_PRODUCT_ID);
	}

	osc2.cb = config->callbacks;
	osc2.user_data = config->user_data;
	osc2.chime = 2;
	battery_defaults();
	osc2.policy = IS_ENABLED(CONFIG_OPENSC2_DEFAULT_POLICY_WIRELESS) ? OPENSC2_POLICY_WIRELESS :
									   OPENSC2_POLICY_AUTO;
	osc2.transport = OSC2_TRANSPORT_DEFAULT;
	osc2.transport_saved = OSC2_TRANSPORT_NONE;
	osc2_settings_restore_defaults(false);
	osc2_keys_boot();   /* the kept keys' defaults, before the stored ones load */

	err = osc2_storage_load();
	if (err) {
		LOG_WRN("Could not load stored bonds and identity (%d)", err);
	}
	osc2_crash_boot();   /* a record a fatal error left in RAM: stored now */
	(void)osc2_hwdt_start();   /* CONFIG_OPENSC2_WATCHDOG: the hardware watchdog from here on */
	if (config->bond_slot < OPENSC2_BOND_SLOTS) {
		osc2.slot = config->bond_slot;
	}
	osc2_transport_init();   /* the stored transport (a puck one uses the slot just set) */
	osc2_identity_refresh();
	for (int link = 0; link < OSC2_LINK_COUNT; link++) {
		osc2_commands_reset((enum osc2_link)link);
	}

	/* Wired or wireless: once, now, like the real controller at power-on. */
	osc2_mode_boot(config->boot_mode);
	osc2_report_init();
	osc2_watchdog_arm();   /* armed at boot; lizard mode is on, so its first expiry does nothing */

	LOG_INF("Controller %s (board %s, revision 0x%x, build 0x%08x), bond slot %u%s, %s, %s",
		osc2.identity.unit_serial, osc2.identity.board_serial, osc2.identity.board_revision,
		osc2.identity.build_time, osc2.slot + 1,
		opensc2_bond_valid(&osc2.bonds[osc2.slot]) ? "" : " (empty: pair first)",
		osc2_wired() ? "wired" : "wireless", osc2_transport_name(osc2_transport_running()));

	osc2.initialised = true;
	/* The radio starts the puck link at once for a puck transport; with the system state
	 * machine, the machine starts the link (either transport) when its rules say so. */
	err = osc2_radio_init(config->start && !osc2_wired() &&
			      osc2_transport_is_puck(osc2_transport_running()) &&
			      !IS_ENABLED(CONFIG_OPENSC2_SYSTEM));
	if (err) {
		LOG_ERR("The radio thread did not start (%d)", err);
	}
#if defined(CONFIG_OPENSC2_BLE)
	if (osc2_ble_init() == 0 && config->start && !osc2_wired() &&
	    !IS_ENABLED(CONFIG_OPENSC2_SYSTEM) && osc2_transport_is_ble(osc2_transport_running())) {
		(void)osc2_ble_start(osc2_transport_running(), false);
	}
#endif
	/* USB comes up whatever happened to the radio: the wired device, or the console. */
	osc2_mode_start(config->start && !IS_ENABLED(CONFIG_OPENSC2_SYSTEM));
	/* The power-on decision, once this boot's mode is final. */
	osc2_sys_init(config->start);
	return err;
}

int opensc2_start(void)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	if (osc2_wired()) {
		return -EBUSY;
	}
	return osc2_wireless_start();   /* the radio, or Bluetooth when that is the transport */
}

int opensc2_stop(void)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	return osc2_wireless_stop();
}

int opensc2_pair_start(uint16_t seconds)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	if (osc2_wired()) {
		return -EBUSY;
	}
#if defined(CONFIG_OPENSC2_BLE)
	if (osc2_transport_is_ble(osc2_transport_running())) {
		ARG_UNUSED(seconds);
		return osc2_ble_pair();   /* pairing mode: open to a new host */
	}
#endif
	osc2_radio_set_pair_seconds(seconds ? seconds : CONFIG_OPENSC2_PAIRING_TIMEOUT_S);
	osc2_radio_post(OSC2_EV_PAIR);
	return 0;
}

int opensc2_pair_cancel(void)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	osc2_radio_post(OSC2_EV_CANCEL);
	return 0;
}

enum opensc2_link_state opensc2_link_state(void)
{
#if defined(CONFIG_OPENSC2_BLE)
	if (osc2_transport_is_ble(osc2_transport_running())) {
		struct opensc2_link_info info;

		osc2_ble_info(&info);
		return info.state;
	}
#endif
	return osc2_radio_state();
}

int opensc2_link_info(struct opensc2_link_info *info)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
#if defined(CONFIG_OPENSC2_BLE)
	if (osc2_transport_is_ble(osc2_transport_running())) {
		osc2_ble_info(info);
		return 0;
	}
#endif
	osc2_radio_info(info);
	info->transport = osc2_transport_running();
	return 0;
}
