/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * mode.c: wired or wireless, decided once at boot from the chip's own USB power detection, the
 * warm-restart mark, the stored policy and the application's choice (mode_rules.c); the reboot
 * that leaves USB the way the real controller does and sets the mark; and, in wireless mode, the
 * real controller's rule for a cable plugged in later, while the policy is "auto": with the link
 * down, or STEAM held, the board reboots to come back wired; with the link up it stays wireless
 * and only charges. A wired boot whose USB device cannot be set up falls back to wireless. With
 * the system state machine the wired device attaches when the machine's wired start says so
 * (once STEAM is up), and USB power in its battery wait makes a battery build's boot wired in
 * place, as on the real controller.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include "internal.h"
#include "mode_rules.h"
#include "system.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define VBUS_POLL_MS 100

static bool boot_warm;   /* the warm-restart mark brought this boot back (a chord did not use it) */

static const char *mode_name(enum opensc2_mode mode)
{
	return mode == OPENSC2_MODE_WIRED ? "wired" : "wireless";
}

static const char *reason_name(enum opensc2_mode_reason reason)
{
	switch (reason) {
	case OPENSC2_MODE_NO_USB_POWER:  return "no USB power";
	case OPENSC2_MODE_USB_POWER:     return "USB power";
	case OPENSC2_MODE_WARM_RESTART:  return "warm restart";
	case OPENSC2_MODE_WIRED_RESTART: return "restart into wired";
	case OPENSC2_MODE_POLICY:        return "policy wireless";
	case OPENSC2_MODE_APP:           return "chosen by the application";
	case OPENSC2_MODE_NO_USB:        return "no USB support";
	case OPENSC2_MODE_USB_FAILED:    return "USB set-up failed";
	default:                         return "?";
	}
}

bool osc2_usb_power(void)
{
#if defined(NRF_POWER) && defined(NRF_POWER_HAS_USBREG) && NRF_POWER_HAS_USBREG
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
#else
	return false;
#endif
}

void osc2_mode_boot(enum opensc2_boot_mode app)
{
	struct osc2_mode_inputs in = {
		.usb_supported = IS_ENABLED(CONFIG_OPENSC2_USB),
		/* VBUS, unless the power source in effect is a battery or the dock (the system
		 * state machine's; the test hooks' boot-time injection among them). */
		.usb_power = osc2_sys_wired_power(),
		.mark = OSC2_MARK_NONE,
		.policy = osc2.policy,
		.app = app,
	};

#if defined(CONFIG_OPENSC2_USB)
	struct osc2_usblog_info log;

	in.mark = osc2_mark_after_chord((enum osc2_mark)osc2_retained_mark_taken(),
					osc2_sys_transport_chord_held());
	boot_warm = in.mark == OSC2_MARK_WIRELESS;
	osc2_usblog_info(&log);
#endif
	osc2.mode = osc2_mode_decide(&in, &osc2.mode_reason);
	LOG_INF("Mode %s (%s)", mode_name(osc2.mode), reason_name(osc2.mode_reason));
#if defined(CONFIG_OPENSC2_USB)
	osc2_usblog("boot %u: %s (%s); policy %s; USB power %s; retained area %s",
		    (unsigned int)log.boots, mode_name(osc2.mode), reason_name(osc2.mode_reason),
		    osc2.policy == OPENSC2_POLICY_AUTO ? "auto" : "wireless",
		    in.usb_power ? "yes" : "no", log.kept ? "kept" : "was empty");
#endif
}

/* USB power that appears while wireless. A battery build of the system state machine
 * (CONFIG_OPENSC2_BATTERY) applies this rule itself, from its own look at the power source.
 * Without a battery the machine takes a board that runs without USB power to be on an external
 * supply, so this poll keeps the rule (as v0.2.0) for a board that has a battery all the same. */
#if defined(CONFIG_OPENSC2_USB) && defined(CONFIG_OPENSC2_USB_PLUG_IN_REBOOT) && \
	!defined(CONFIG_OPENSC2_BATTERY)
#define PLUG_IN_POLL 1
static bool vbus_last;

static void vbus_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(vbus_work, vbus_fn);

/* The policy is read when USB power appears, so a policy changed at run time counts at once. */
static void vbus_fn(struct k_work *work)
{
	bool now = osc2_usb_power();

	ARG_UNUSED(work);
	/* With the machine: not while the application has named another source (it decides). */
	if (now && !vbus_last &&
	    (!IS_ENABLED(CONFIG_OPENSC2_SYSTEM) ||
	     opensc2_power_source() == OPENSC2_POWER_SOURCE_USB)) {
		enum osc2_plug_in rule;

		/* The link counts on either transport: the puck's radio or Bluetooth. */
		rule = osc2_plug_in_rule(osc2.policy, osc2_wireless_up(),
					 (osc2_sys_buttons() & OPENSC2_BTN_STEAM) != 0);
		if (rule == OSC2_PLUG_IN_LINK_DOWN || rule == OSC2_PLUG_IN_STEAM) {
			const char *why = rule == OSC2_PLUG_IN_LINK_DOWN ? "USB power, link down" :
									    "USB power, STEAM held";

			LOG_INF("%s: back to wired", why);
			osc2_usblog("%s: back to wired", why);
			osc2_schedule_power(OPENSC2_POWER_USB_PLUGGED, 0, why);
			return;
		}
		if (rule == OSC2_PLUG_IN_CHARGE) {
			LOG_INF("USB power: charging, the link stays up");
		}
	}
	vbus_last = now;
	k_work_schedule(&vbus_work, K_MSEC(VBUS_POLL_MS));
}
#endif

bool osc2_mode_warm(void)
{
	return boot_warm;
}

#if defined(CONFIG_OPENSC2_USB)
static bool wired_attached;

/* The wired device; 0 when it is up. Otherwise this boot goes on wireless. */
static int wired_attach(bool start_radio)
{
	int err = osc2_usb_wired_start();

	if (err == 0) {
		wired_attached = true;
		return 0;
	}
	/* No USB device, and the radio was left off for the wired boot: rather than neither link,
	 * this boot goes on wireless (the session log has the failure). */
	osc2.mode = OPENSC2_MODE_WIRELESS;
	osc2.mode_reason = OPENSC2_MODE_USB_FAILED;
	LOG_WRN("Mode wireless (%s, %d)", reason_name(osc2.mode_reason), err);
	osc2_usblog("wired start failed (%d): wireless this boot", err);
	if (start_radio) {
		(void)osc2_wireless_start();   /* this boot's transport: the radio or Bluetooth */
	}
	return err;
}

void osc2_mode_usb_on(void)
{
	if (osc2_wired() && !wired_attached && wired_attach(false) != 0) {
		(void)osc2_usb_console_start();
	}
}

void osc2_mode_go_wired(void)
{
	if (osc2_wired()) {
		return;
	}
	/* Nothing wireless runs yet (the battery wait starts its link only at the end): the
	 * console gives the USB controller back, and this boot is wired from now on. */
	osc2_usb_console_stop();
	osc2.mode = OPENSC2_MODE_WIRED;
	osc2.mode_reason = OPENSC2_MODE_USB_POWER;
	LOG_INF("Mode wired (USB power in the battery wait)");
	osc2_usblog("USB power in the battery wait: wired in place");
}
#endif

void osc2_mode_start(bool start_radio)
{
#if defined(CONFIG_OPENSC2_USB)
	if (osc2_wired()) {
		if (IS_ENABLED(CONFIG_OPENSC2_SYSTEM) && (osc2_sys_buttons() & OPENSC2_BTN_STEAM)) {
			return;   /* the machine's wired start attaches it, once STEAM is up */
		}
		/* STEAM up: the device now, as the machine's wired start would at once (a device
		 * that cannot be set up still falls back to wireless before the machine starts). */
		if (wired_attach(start_radio) == 0) {
			return;
		}
	}
	(void)osc2_usb_console_start();
#if defined(PLUG_IN_POLL)
	/* Every wireless boot whose mode the application did not choose; the policy counts when USB
	 * power appears. Not after a failed wired start: coming back wired would fail again. */
	if (osc2.mode_reason != OPENSC2_MODE_APP && osc2.mode_reason != OPENSC2_MODE_USB_FAILED) {
		vbus_last = osc2_usb_power();
		k_work_schedule(&vbus_work, K_MSEC(VBUS_POLL_MS));
	}
#endif
#else
	ARG_UNUSED(start_radio);
#endif
}

void osc2_mode_init_failed(void)
{
#if defined(CONFIG_OPENSC2_USB)
	(void)osc2_usb_console_start();
#endif
}

/* ---- public ---------------------------------------------------------------------------------- */

enum opensc2_mode opensc2_mode(void)
{
	return osc2.mode;
}

enum opensc2_mode_reason opensc2_mode_reason(void)
{
	return osc2.mode_reason;
}

enum opensc2_policy opensc2_policy(void)
{
	return osc2.policy;
}

int opensc2_policy_set(enum opensc2_policy policy)
{
	if (policy != OPENSC2_POLICY_AUTO && policy != OPENSC2_POLICY_WIRELESS) {
		return -EINVAL;
	}
	osc2.policy = policy;
	return osc2_storage_save_policy();
}

void opensc2_reboot(enum opensc2_reboot how, const char *why)
{
	static const char *const how_names[] = { "plain", "into wireless", "into wired" };
	const char *reason = why ? why : osc2_power_reason();

	if (!reason) {
		reason = "application";
	}
	LOG_INF("Reboot %s: %s", how <= OPENSC2_REBOOT_WIRED ? how_names[how] : "?", reason);
#if defined(CONFIG_OPENSC2_USB)
	osc2_usblog("reboot %s: %s", how <= OPENSC2_REBOOT_WIRED ? how_names[how] : "?", reason);
	if (osc2_wired()) {
		osc2_usb_detach(reason);   /* the zero report, 10 ms, USB off */
	}
#endif
#if defined(CONFIG_OPENSC2_BLE)
	if (!osc2_wired() && osc2_ble_active()) {
		/* Bluetooth is left as the real controller leaves it before every reboot: an
		 * all-zero state report, 100 ms, then off (the Bluetooth thread does it). */
		(void)osc2_ble_stop(false, false);
		k_msleep(110);
	}
#endif
	/* Bond and setting writes are synchronous: nothing is pending. Let the log catch up. */
	if (IS_ENABLED(CONFIG_LOG)) {
		k_msleep(20);
	}
#if defined(CONFIG_OPENSC2_USB)
	/* The mark last, right before the reset, so it never outlives a reboot that did not happen. A
	 * plain reboot leaves a mark set earlier as it is: the real controller's warm-restart flag,
	 * once set by Steam's request, brings back any later reboot of that boot wireless. */
	if (how != OPENSC2_REBOOT_PLAIN) {
		osc2_retained_mark_set(how == OPENSC2_REBOOT_WIRELESS ? OSC2_MARK_WIRELESS :
				       OSC2_MARK_WIRED);
	}
#endif
	sys_reboot(SYS_REBOOT_COLD);
}

static struct {
	enum opensc2_reboot how;
	const char *why;
} reboot_later_args;

static void reboot_later_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	opensc2_reboot(reboot_later_args.how, reboot_later_args.why);
}

static K_WORK_DELAYABLE_DEFINE(reboot_later_work, reboot_later_fn);

void opensc2_reboot_later(enum opensc2_reboot how, const char *why, uint32_t delay_ms)
{
	reboot_later_args.how = how;
	reboot_later_args.why = why;
	(void)k_work_reschedule(&reboot_later_work, K_MSEC(delay_ms));
}

enum opensc2_usb_state opensc2_usb_state(void)
{
#if defined(CONFIG_OPENSC2_USB)
	if (osc2_wired()) {
		struct opensc2_usb_info info;

		osc2_usb_get_info(&info);
		return info.state;
	}
#endif
	return OPENSC2_USB_OFF;
}

int opensc2_usb_info(struct opensc2_usb_info *info)
{
	if (!osc2.initialised) {
		return -EACCES;
	}
	memset(info, 0, sizeof(*info));
#if defined(CONFIG_OPENSC2_USB)
	if (osc2_wired()) {
		osc2_usb_get_info(info);
	}
#endif
	return 0;
}
