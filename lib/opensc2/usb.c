/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * usb.c: wired mode. The board becomes one full-speed USB device, 28DE:1302, with one HID
 * interface (Zephyr's HID class, on the devicetree node chosen as `opensc2,usb-hid`):
 *   - the report descriptor of usb_desc.c (mouse, keyboard, then the vendor collection);
 *   - feature report 1 for the vendor commands (commands.c): a SET runs a command and stores it,
 *     a GET answers the stored one; anything refused is a STALL;
 *   - output reports on the interrupt OUT endpoint, decoded as on the radio link;
 *   - on interrupt IN the state report 0x42 every 132 ticks of the 32,768 Hz clock (a 4000 µs
 *     timer) from the moment the host configures the device, plus the battery and lizard-mode
 *     reports. Eight IN buffers; a state report that finds none free is dropped, so nothing ever
 *     waits for the host (a suspended bus included).
 * Leaving the bus is always on the way to a reboot: one all-zero state report, 10 ms, USB off.
 * USB power gone for 0.2 s (the cable pulled; only a board with a battery is still running) is
 * handed to the application as OPENSC2_POWER_USB_UNPLUGGED: the real controller shuts down, and
 * without a callback the library reboots, which comes back wireless.
 * The product string is the identity's product name ("Steam Controller" unless one is stored).
 * The charger test build (CONFIG_OPENSC2_NO_HOST_WIRELESS_MS, a testing aid that is not the real
 * controller's behaviour and off by default) asks for the reboot into wireless mode when no host
 * has talked to the board within that time (usb_rules.h).
 *
 * In wireless mode the same USB controller can carry a debug console instead (a CDC-ACM serial
 * port with non-Valve IDs), or nothing at all, like the real controller.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/udc.h>
#include "internal.h"
#include "test_hooks.h"
#include "usb_rules.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define HID_NODE DT_CHOSEN(opensc2_usb_hid)
#define UDC_NODE DT_NODELABEL(zephyr_udc0)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(UDC_NODE), "OpenSC2's USB support needs the zephyr_udc0 node");
BUILD_ASSERT(DT_PROP(HID_NODE, in_report_size) == 64,
	     "The opensc2,usb-hid node needs in-report-size = <64>");
BUILD_ASSERT(DT_PROP_OR(HID_NODE, out_report_size, 0) == 64,
	     "The opensc2,usb-hid node needs out-report-size = <64>: output reports come on "
	     "interrupt OUT");
BUILD_ASSERT(!IS_ENABLED(CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT),
	     "Set CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n: OpenSC2's USB support runs the USB "
	     "device (CONFIG_OPENSC2_USB_CONSOLE replaces the board's serial port in wireless mode)");
BUILD_ASSERT(CONFIG_USBD_THREAD_STACK_SIZE >= 2048,
	     "Set CONFIG_USBD_THREAD_STACK_SIZE=2048: vendor commands write flash in the USB "
	     "stack's thread");

#define IN_BUFS        MIN(8, CONFIG_USBD_HID_IN_BUF_COUNT)
#define IN_BUF_SIZE    64
#define REPORT_PERIOD  K_USEC(4000)   /* 132 ticks of 32,768 Hz, as on the real controller */
#define NO_HOST_MS     500            /* then the "no host" LED, still waiting */
#define WAIT_POLL_MS   10
#define STEAM_POLLS    3              /* STEAM held about 20 ms */
#define OUT_LOG_PER_S  8              /* output reports logged per second, the rest counted */
#define UNPLUG_MS      200            /* USB power gone this long: the cable is out */

BUILD_ASSERT(OSC2_NO_HOST_WIRELESS_MS == 0 || OSC2_NO_HOST_WIRELESS_MS >= 1000,
	     "CONFIG_OPENSC2_NO_HOST_WIRELESS_MS is 0 (off, as the real controller) or at least 1000: "
	     "a PC needs that long to reset the bus and send its first request");
/* Setup packets are watched for the session log, and by the charger test build, which tells a
 * host (it sends requests) from a charger (it sends none) by them. */
#if defined(CONFIG_OPENSC2_USB_LOG_SETUP) || OSC2_NO_HOST_WIRELESS_MS > 0
#define WATCH_SETUP 1
#else
#define WATCH_SETUP 0
#endif

static const struct device *const hid_dev = DEVICE_DT_GET(HID_NODE);

/* ---- the wired device ------------------------------------------------------------------------ */

USBD_DEVICE_DEFINE(osc2_usbd, DEVICE_DT_GET(UDC_NODE), OPENSC2_USB_VID, OPENSC2_USB_PID);
USBD_DESC_LANG_DEFINE(osc2_lang);
USBD_DESC_MANUFACTURER_DEFINE(osc2_mfr, CONFIG_OPENSC2_USB_MANUFACTURER);

/* The product string is the identity's product name (CONFIG_OPENSC2_USB_PRODUCT, "Steam
 * Controller", unless one is stored), taken when the device attaches: its text lives in a buffer
 * of ours, and its length is set then. */
static uint8_t product_text[OPENSC2_PRODUCT_NAME_MAX + 1];
static struct usbd_desc_node osc2_product = {
	.str = {
		.utype = USBD_DUT_STRING_PRODUCT,
		.ascii7 = true,
	},
	.ptr = product_text,
	.bLength = USB_STRING_DESCRIPTOR_LENGTH(CONFIG_OPENSC2_USB_PRODUCT),
	.bDescriptorType = USB_DESC_STRING,
};

/* The serial is the identity's unit serial (13 characters), the same string vendor command 0xAE
 * answers, so its text lives in a buffer of ours. */
static uint8_t serial_text[OPENSC2_SERIAL_LEN + 1];
static struct usbd_desc_node osc2_sn = {
	.str = {
		.utype = USBD_DUT_STRING_SERIAL_NUMBER,
		.ascii7 = true,
	},
	.ptr = serial_text,
	.bLength = USB_STRING_DESCRIPTOR_LENGTH("0000000000000"),
	.bDescriptorType = USB_DESC_STRING,
};

/* Bus powered, remote wakeup advertised, 500 mA, no string. */
USBD_CONFIGURATION_DEFINE(osc2_config, USB_SCD_REMOTE_WAKEUP, 250, NULL);

#if defined(CONFIG_OPENSC2_USB_PROFILE_OCTOBER)
static const struct usb_bos_capability_lpm usb20_ext = {
	.bLength = sizeof(struct usb_bos_capability_lpm),
	.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
	.bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
	.bmAttributes = 0,   /* no link power management */
};
USBD_DESC_BOS_DEFINE(osc2_bos, sizeof(usb20_ext), &usb20_ext);
#endif

#if defined(CONFIG_OPENSC2_USB_CONSOLE) || defined(CONFIG_OPENSC2_USB_WIRED_CONSOLE)
#define CDC_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_cdc_acm_uart)
static const struct device *const cdc_dev = DEVICE_DT_GET(CDC_NODE);
#endif

/* ---- state ----------------------------------------------------------------------------------- */

static const struct opensc2_usb_profile *profile = &opensc2_usb_profile_july;
static volatile bool started;          /* attached at least once in this boot */
static volatile bool detaching;        /* on the way to a reboot */
static volatile bool configured;       /* the interface is configured: USB is a transport */
static volatile bool configured_once;  /* "waiting for the host" is over */
#if WATCH_SETUP
static volatile bool request_seen;     /* a host sent a control request */
#endif
static volatile bool bus_reset_seen;   /* a bus reset came (for the session log) */
static volatile enum opensc2_usb_state usb_state = OPENSC2_USB_OFF;
static int64_t attach_ms, configured_at_ms;
static uint32_t n_reports, n_dropped, n_requests, n_stalls, n_outputs;
static uint32_t out_logged, out_unlogged;

static void notify_fn(struct k_work *work)
{
	struct opensc2_usb_info info;

	ARG_UNUSED(work);
	osc2_usb_get_info(&info);
	osc2_test_note_usb(&info);
	if (osc2.cb && osc2.cb->usb_changed) {
		osc2.cb->usb_changed(&info, osc2.user_data);
	}
}
static K_WORK_DEFINE(notify_work, notify_fn);

static void notify(void)
{
	k_work_submit(&notify_work);
}

bool osc2_usb_ready(void)
{
	return configured && !detaching;
}

/* Whether the bus is suspended: the USB stack's own state, which it keeps in its thread as the
 * events arrive. No copy of it is kept here: a bus reset ends a suspend without a RESUME message,
 * and a message is dropped when the stack's message pool is full, so a copy could stay
 * "suspended" on a bus that runs again. */
static bool bus_suspended(void)
{
	return usbd_is_suspended(&osc2_usbd);
}

void osc2_usb_get_info(struct opensc2_usb_info *info)
{
	memset(info, 0, sizeof(*info));
	info->state = usb_state;
	info->active = configured;
	info->suspended = bus_suspended();
	info->reports = n_reports;
	info->dropped = n_dropped;
	info->requests = n_requests;
	info->stalls = n_stalls;
	info->outputs = n_outputs;
	info->configured_ms = configured ? (uint32_t)(k_uptime_get() - configured_at_ms) : 0;
}

/* ---- IN reports: eight buffers, never waiting for the host ---------------------------------- */

UDC_STATIC_BUF_DEFINE(in_pool, IN_BUFS * IN_BUF_SIZE);
static atomic_t in_busy;               /* one bit per buffer on its way to the host */
static K_MUTEX_DEFINE(send_lock);      /* keeps the order of submissions */

struct pending_report {
	uint8_t len;
	uint8_t data[IN_BUF_SIZE];
};
/* Reports that must not be lost (lizard mode, the application's) wait here for a buffer. */
K_MSGQ_DEFINE(osc2_usb_pending, sizeof(struct pending_report), 8, 1);

static bool buffers_full(void)
{
	atomic_val_t all = (atomic_val_t)BIT_MASK(IN_BUFS);

	return (atomic_get(&in_busy) & all) == all;
}

static struct osc2_usb_view link_view(void)
{
	return (struct osc2_usb_view){
		.configured = configured,
		.detaching = detaching,
		.suspended = bus_suspended(),
		.buffers_full = buffers_full(),
	};
}

/* Copy `report` into a free buffer and hand it to the HID class. Call with send_lock held. */
static int submit_locked(const uint8_t *report, size_t len)
{
	for (int i = 0; i < IN_BUFS; i++) {
		if (!atomic_test_and_set_bit(&in_busy, i)) {
			uint8_t *buf = &in_pool[i * IN_BUF_SIZE];
			int err;

			memcpy(buf, report, len);
			err = hid_device_submit_report(hid_dev, (uint16_t)len, buf);
			if (err) {
				atomic_clear_bit(&in_busy, i);
			}
			return err;
		}
	}
	return -ENOBUFS;
}

static int queue_locked(const uint8_t *report, size_t len)
{
	struct pending_report p = { .len = (uint8_t)len };

	memcpy(p.data, report, len);
	return k_msgq_put(&osc2_usb_pending, &p, K_NO_WAIT) ? -ENOMEM : 0;
}

int osc2_usb_send(const uint8_t *report, size_t len, bool droppable)
{
	struct osc2_usb_view v;
	int err;

	if (len < 1 || len > IN_BUF_SIZE) {
		return -EINVAL;
	}
	v = link_view();
	err = osc2_usb_send_gate(&v);   /* not configured, leaving, or suspended (dropped) */
	if (err) {
		return err;
	}
	k_mutex_lock(&send_lock, K_FOREVER);
	if (!droppable && k_msgq_num_used_get(&osc2_usb_pending)) {
		err = queue_locked(report, len);   /* behind the ones already waiting */
	} else {
		err = submit_locked(report, len);
		if (err == -ENOBUFS && !droppable) {
			err = queue_locked(report, len);
		}
	}
	k_mutex_unlock(&send_lock);
	return err;
}

static void pending_fn(struct k_work *work)
{
	struct pending_report p;

	ARG_UNUSED(work);
	k_mutex_lock(&send_lock, K_FOREVER);
	while (k_msgq_peek(&osc2_usb_pending, &p) == 0) {
		if (!configured || detaching) {
			k_msgq_purge(&osc2_usb_pending);
			break;
		}
		if (submit_locked(p.data, p.len) == -ENOBUFS) {
			break;   /* the next "report done" tries again */
		}
		(void)k_msgq_get(&osc2_usb_pending, &p, K_NO_WAIT);
	}
	k_mutex_unlock(&send_lock);
}
static K_WORK_DEFINE(pending_work, pending_fn);

static void on_input_report_done(const struct device *dev, const uint8_t *const report)
{
	ptrdiff_t at = report - in_pool;

	ARG_UNUSED(dev);
	if (at >= 0 && at < IN_BUFS * IN_BUF_SIZE) {
		atomic_clear_bit(&in_busy, (int)(at / IN_BUF_SIZE));
	}
	if (k_msgq_num_used_get(&osc2_usb_pending)) {
		k_work_submit(&pending_work);
	}
}

/* ---- the report timer and thread ------------------------------------------------------------- */

static K_SEM_DEFINE(tick_sem, 0, 1);

static void tick_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_sem_give(&tick_sem);
}
static K_TIMER_DEFINE(report_timer, tick_fn, NULL);

static int64_t next_count_ms;
static uint32_t count_lines, last_reports, last_dropped;

/* A line of counters every second for the first ten seconds, then every ten seconds. */
static void counters_tick(void)
{
	int64_t now = k_uptime_get();

	if (now < next_count_ms) {
		return;
	}
	osc2_usblog("0x42: %u sent, %u dropped; %u requests, %u stalls, %u outputs (%u not logged)",
		    n_reports - last_reports, n_dropped - last_dropped, n_requests, n_stalls,
		    n_outputs, out_unlogged);
	osc2_usblog_counts(n_requests, n_stalls, n_outputs, n_reports, n_dropped);
	last_reports = n_reports;
	last_dropped = n_dropped;
	out_logged = 0;
	count_lines++;
	next_count_ms = now + (count_lines < 10 ? 1000 : 10000);
}

#if defined(CONFIG_OPENSC2_USB_REMOTE_WAKEUP)
/* July 2026 behaviour, off by default: STEAM held about 20 ms on a suspended bus wakes the PC. */
static void wakeup_check(void)
{
	static uint8_t frames;
	static int64_t last_ms = -3000;
	struct opensc2_state s;

	opensc2_get_state(&s);
	if (!(s.buttons & OPENSC2_BTN_STEAM)) {
		frames = 0;
		return;
	}
	if (++frames == 5 && k_uptime_get() - last_ms >= 3000) {
		int err = usbd_wakeup_request(&osc2_usbd);

		last_ms = k_uptime_get();
		osc2_usblog("STEAM: remote wakeup %s (%d)", err ? "refused" : "requested", err);
	}
}
#endif

static void report_thread_fn(void *p1, void *p2, void *p3)
{
	uint8_t state_report[OPENSC2_STATE_REPORT_LEN];
	struct opensc2_state s;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		struct osc2_usb_view v;
		enum osc2_usb_tick action;

		k_sem_take(&tick_sem, K_FOREVER);
		v = link_view();
		action = osc2_usb_tick_action(&v);
		if (action == OSC2_USB_TICK_IDLE) {
			continue;
		}
		if (action != OSC2_USB_TICK_SEND) {
			/* Suspended, or no buffer free: skipped without using a sequence number. */
			n_dropped++;
			osc2_test_count(OSC2_TEST_CNT_USB_SKIPPED);
#if defined(CONFIG_OPENSC2_USB_REMOTE_WAKEUP)
			if (action == OSC2_USB_TICK_SUSPENDED) {
				wakeup_check();
			}
#endif
			counters_tick();
			continue;
		}
		osc2_report_state(state_report, &s);
		if (osc2_usb_send(state_report, sizeof(state_report), true) == 0) {
			n_reports++;
			osc2_test_count(OSC2_TEST_CNT_USB_SENT);
		} else {
			n_dropped++;
			osc2_test_count(OSC2_TEST_CNT_USB_DROPPED);   /* its sequence number is lost */
		}
		/* The frame's lizard reports, sent before lizard mode can turn off (0x81 runs in the
		 * USB stack's thread): never after its release reports. */
		osc2_report_lizard(&s);
		counters_tick();
	}
}
K_THREAD_DEFINE(osc2_usb_tid, CONFIG_OPENSC2_USB_THREAD_STACK_SIZE, report_thread_fn, NULL, NULL,
		NULL, CONFIG_OPENSC2_USB_THREAD_PRIORITY, 0, SYS_FOREVER_MS);

/* ---- HID class callbacks (the USB stack's thread) -------------------------------------------- */

static void on_iface_ready(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);
	if (ready) {
		k_mutex_lock(&send_lock, K_FOREVER);
		atomic_set(&in_busy, 0);   /* a report in flight at a bus reset may never be "done" */
		k_msgq_purge(&osc2_usb_pending);
		k_mutex_unlock(&send_lock);
		configured_at_ms = k_uptime_get();
		configured = true;
		configured_once = true;
		usb_state = OPENSC2_USB_CONFIGURED;
		count_lines = 0;
		next_count_ms = configured_at_ms + 1000;
		last_reports = n_reports;
		last_dropped = n_dropped;
		k_timer_start(&report_timer, REPORT_PERIOD, REPORT_PERIOD);
		osc2_usblog("configured: state reports every 132 ticks (4.03 ms)");
	} else {
		configured = false;
		k_timer_stop(&report_timer);
		if (!detaching) {
			/* On the bus, not configured (a bus reset, or the host let go): for the LED and
			 * the application's "no host" rules, until the host configures it again. */
			usb_state = OPENSC2_USB_ATTACHED;
		}
		osc2_usblog("deconfigured");
		osc2_output_link_down();
	}
	notify();
}

static const char *stall_reason(int rc)
{
	switch (rc) {
	case -ENOTSUP:  return "unknown opcode or no answer";
	case -EINVAL:   return "refused";
	case -ENOENT:   return "no such key, or no value";
	case -EMSGSIZE: return "not 64 bytes";
	case -EBADMSG:  return "report ID";
	case -E2BIG:    return "length over 61";
	default:        return "error";
	}
}

static int on_set_report(const struct device *dev, const uint8_t type, const uint8_t id,
			 const uint16_t len, const uint8_t *const buf)
{
	uint32_t start = k_cycle_get_32();
	char detail[56] = "";
	uint32_t hold_us;
	int rc;

	ARG_UNUSED(dev);
	n_requests++;
	if (type != HID_REPORT_TYPE_FEATURE || id != OPENSC2_USB_FEATURE_ID) {
		/* Only feature report 1 is a command; output reports come on interrupt OUT. */
		n_stalls++;
		osc2_usblog("SET_REPORT type %u id %u (%u bytes) STALL: only feature report 1", type,
			    id, len);
		return -ENOTSUP;
	}
	rc = osc2_usb_feature_set(buf, len);
	if (osc2_test_quiet(OSC2_LINK_USB)) {
		/* A claimed request of the hardware test channel: counted, not logged line by line (a
		 * test run sends thousands; they would push every real session out of the log). */
		if (rc) {
			n_stalls++;
		}
		return rc;
	}
	if (len >= 3) {
		osc2_command_describe(&buf[1], (uint8_t)MIN(len - 1, OPENSC2_USB_FEATURE_LEN - 1),
				      detail, sizeof(detail));
	}
	hold_us = k_cyc_to_us_floor32(k_cycle_get_32() - start);
	if (rc) {
		n_stalls++;
	}
	osc2_usblog("SET %02X n=%u %s %s%s %u.%u ms", len >= 2 ? buf[1] : 0, len >= 3 ? buf[2] : 0,
		    detail, rc ? "STALL: " : "ok", rc ? stall_reason(rc) : "", hold_us / 1000,
		    (hold_us % 1000) / 100);
	return rc;
}

static int on_get_report(const struct device *dev, const uint8_t type, const uint8_t id,
			 const uint16_t len, uint8_t *const buf)
{
	uint8_t opcode = 0, n = 0;
	bool quiet;
	int rc;

	ARG_UNUSED(dev);
	n_requests++;
	if (type != HID_REPORT_TYPE_FEATURE || id != OPENSC2_USB_FEATURE_ID) {
		n_stalls++;
		osc2_usblog("GET_REPORT type %u id %u STALL: only feature report 1", type, id);
		return -ENOTSUP;
	}
	rc = osc2_usb_feature_get(buf, len, profile->get_reply_len, &opcode, &n);
	quiet = osc2_test_quiet(OSC2_LINK_USB);   /* the test channel's answer: counted only */
	if (rc <= 0) {
		n_stalls++;
		osc2_usblog("GET %02X STALL: %s (wLength %u)", opcode, stall_reason(rc), len);
		return rc < 0 ? rc : -EIO;
	}
	/* Answers are counted, never shown: they hold serials and bonds. */
	if (!quiet) {
		osc2_usblog("GET %02X n=%u len=%d (wLength %u) ok", opcode, n, rc, len);
	}
	/* The USB stack stalls a request whose errno is set after the class handler, and the HID
	 * class sets errno only when a GET fails: clear what building the answer (the application's
	 * vendor_command among it) may have left there. */
	errno = 0;
	return rc;
}

static void on_output_report(const struct device *dev, const uint16_t len,
			     const uint8_t *const buf)
{
	ARG_UNUSED(dev);
	if (len == 0) {
		return;
	}
	n_outputs++;
	if (out_logged < OUT_LOG_PER_S) {
		out_logged++;
		osc2_usblog("OUT %02X %u bytes", buf[0], len);
	} else {
		out_unlogged++;
	}
	(void)osc2_output_dispatch(OSC2_LINK_USB, buf, len);
}

/* No idle or protocol callbacks: SET_IDLE is stored for report 0 and then stalled, and the boot
 * protocol requests are stalled, as on the real controller. */
static const struct hid_device_ops hid_ops = {
	.iface_ready = on_iface_ready,
	.get_report = on_get_report,
	.set_report = on_set_report,
	.output_report = on_output_report,
	.input_report_done = on_input_report_done,
};

/* ---- bus events (the system work queue) -------------------------------------------------------
 * Only logged and passed on: whether the bus is suspended is always read from the stack itself
 * (bus_suspended()), which has already changed its state when a message arrives here.
 */

/* USB power went away 0.2 s ago. The stack only reports it (the classes stay configured), so a
 * board that is still running (a battery) would sit with neither link: no host, and no radio while
 * wired. A short dip (a hub resetting its port) changes nothing. Its own work item, not
 * osc2_schedule_power(): a pending request (Steam's 0x95, 20 ms) must not be replaced. */
static void unplug_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!started || detaching) {
		return;
	}
	if (osc2_usb_power()) {
		osc2_usblog("USB power back after a short dip: no change");
		return;
	}
	configured = false;
	k_timer_stop(&report_timer);
	usb_state = OPENSC2_USB_OFF;
	osc2_usblog("USB power gone for %d ms: unplugged", UNPLUG_MS);
	osc2_output_link_down();
	notify();
	osc2_power_now(OPENSC2_POWER_USB_UNPLUGGED, "USB power gone");
}
static K_WORK_DELAYABLE_DEFINE(unplug_work, unplug_fn);

static void on_usb_msg(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);
	switch (msg->type) {
	case USBD_MSG_SUSPEND:
		/* The October 2026 controller changes nothing on a suspend; reports are dropped. */
		osc2_usblog("bus suspended");
		notify();
		break;
	case USBD_MSG_RESUME:
		osc2_usblog("bus resumed");
		notify();
		break;
	case USBD_MSG_RESET:
		/* A reset also ends a suspend (no RESUME comes), and deconfigures. */
		bus_reset_seen = true;
		osc2_usblog("bus reset");
		notify();
		break;
	case USBD_MSG_CONFIGURATION:
		osc2_usblog("configuration %d", msg->status);
		break;
	case USBD_MSG_VBUS_READY:
		osc2_usblog("USB power");
		break;
	case USBD_MSG_VBUS_REMOVED:
		osc2_usblog("USB power gone");
		k_work_reschedule(&unplug_work, K_MSEC(UNPLUG_MS));   /* looks again then */
		break;
	case USBD_MSG_UDC_ERROR:
	case USBD_MSG_STACK_ERROR:
		osc2_usblog("USB %s %d", usbd_msg_type_string(msg->type), msg->status);
		break;
	default:
		break;
	}
}

/* ---- waiting for the host: the 0.5 s timer and STEAM ----------------------------------------- */

static bool steam_seen_up;
static uint8_t steam_polls;

static void wait_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(wait_work, wait_fn);

#if OSC2_NO_HOST_WIRELESS_MS > 0
/* The charger test build: a boot wired by the boot rule that no host has talked to within the time
 * goes back to wireless, the way STEAM takes the real controller there, so that a board without
 * buttons connects through the puck on a USB charger. True once the reboot is asked for. */
static bool no_host_check(void)
{
	static bool host_logged;
	struct osc2_no_host_view v = {
		.timeout_ms = OSC2_NO_HOST_WIRELESS_MS,
		.reason = osc2.mode_reason,
		.waited_ms = (uint32_t)(k_uptime_get() - attach_ms),
		.host_seen = request_seen || configured_once,
		.leaving = detaching,
	};

	switch (osc2_no_host_rule(&v)) {
	case OSC2_NO_HOST_HOST:
		if (!host_logged) {
			host_logged = true;
			osc2_usblog("test build: a host talked after %u ms: no fallback, waiting for it",
				    (unsigned int)v.waited_ms);
		}
		return false;
	case OSC2_NO_HOST_WIRELESS:
		osc2_usblog("test build: no request from a host in %u ms (%s): to wireless",
			    (unsigned int)OSC2_NO_HOST_WIRELESS_MS,
			    bus_reset_seen ? "a bus reset, no request" : "no bus reset");
		LOG_WRN("No USB host within %u ms: rebooting into wireless mode (charger test build)",
			(unsigned int)OSC2_NO_HOST_WIRELESS_MS);
		osc2_schedule_power(OPENSC2_POWER_REBOOT_KEEP_LINK, 0, "no host (charger test build)");
		return true;
	default:
		return false;
	}
}
#endif

static void wait_fn(struct k_work *work)
{
	struct opensc2_state s;

	ARG_UNUSED(work);
	if (!started || detaching || configured_once) {
		return;
	}
	if (usb_state == OPENSC2_USB_ATTACHED && k_uptime_get() - attach_ms >= NO_HOST_MS) {
		usb_state = OPENSC2_USB_NO_HOST;
		osc2_usblog("no host after 0.5 s, still waiting");
		notify();
	}
#if OSC2_NO_HOST_WIRELESS_MS > 0
	if (no_host_check()) {
		return;
	}
#endif
	/* STEAM pressed while no host has configured the device: back to wireless, as on the real
	 * controller (for example on a USB charger). */
	opensc2_get_state(&s);
	if (s.buttons & OPENSC2_BTN_STEAM) {
		if (steam_seen_up && ++steam_polls >= STEAM_POLLS) {
			osc2_usblog("STEAM pressed while waiting for a host");
			osc2_schedule_power(OPENSC2_POWER_REBOOT_KEEP_LINK, 0, "STEAM, no host");
			return;
		}
	} else {
		steam_seen_up = true;
		steam_polls = 0;
	}
	k_work_reschedule(&wait_work, K_MSEC(WAIT_POLL_MS));
}

/* ---- set-up ---------------------------------------------------------------------------------- */

static struct usbd_class_data *class_of(const struct device *dev)
{
	STRUCT_SECTION_FOREACH_ALTERNATE(usbd_class_fs, usbd_class_node, c_nd) {
		if (usbd_class_get_private(c_nd->c_data) == dev) {
			return c_nd->c_data;
		}
	}
	return NULL;
}

static int register_class_of(struct usbd_context *ctx, const struct device *dev)
{
	struct usbd_class_data *c_data = class_of(dev);

	return c_data ? usbd_register_class(ctx, c_data->name, USBD_SPEED_FS, 1) : -ENODEV;
}

/* Compare what the stack built with the descriptors the real device has. */
static void self_check(void)
{
	uint8_t expect[41], got[48];
	struct usbd_class_data *c_data = class_of(hid_dev);
	struct usb_desc_header **dh;
	size_t n = sizeof(struct usb_cfg_descriptor);

	opensc2_usb_device_descriptor(profile, expect);
	if (memcmp(osc2_usbd.fs_desc, expect, 18) != 0) {
		osc2_usblog("check: the device descriptor differs from the expected one");
		LOG_WRN("USB device descriptor differs from the expected one");
	}
	if (IS_ENABLED(CONFIG_OPENSC2_USB_WIRED_CONSOLE) || c_data == NULL) {
		return;   /* a composite device has more interfaces */
	}
	memcpy(got, &cfg_desc_osc2_config, n);
	for (dh = c_data->api->get_desc(c_data, USBD_SPEED_FS); *dh && (*dh)->bLength; dh++) {
		if (n + (*dh)->bLength > sizeof(got)) {
			break;
		}
		memcpy(&got[n], *dh, (*dh)->bLength);
		n += (*dh)->bLength;
	}
	opensc2_usb_config_descriptor(expect);
	if (n != sizeof(expect) || memcmp(got, expect, sizeof(expect)) != 0) {
		osc2_usblog("check: the configuration (%u bytes) differs from the expected 41",
			    (unsigned int)n);
		LOG_WRN("USB configuration descriptor differs from the expected one");
	} else {
		osc2_usblog("check: device and configuration descriptors as expected");
	}
}

#if WATCH_SETUP
/* Every control request goes into the session log, also the standard ones the stack answers
 * itself: the UDC driver's event callback is wrapped to copy each setup packet on its way. The
 * charger test build counts the first one as a host. */
static udc_event_cb_t stack_event_cb;

static int setup_tap(const struct device *dev, const struct udc_event *const event)
{
	if (event->type == UDC_EVT_EP_REQUEST && event->buf != NULL) {
		struct udc_buf_info *bi = udc_get_buf_info(event->buf);

		if (bi->setup && bi->ep == USB_CONTROL_EP_OUT && event->buf->len == 8) {
			request_seen = true;
#if defined(CONFIG_OPENSC2_USB_LOG_SETUP)
			osc2_usblog_setup(event->buf->data);
#endif
		}
	}
	return stack_event_cb(dev, event);
}

static void setup_tap_install(void)
{
	struct udc_data *data = DEVICE_DT_GET(UDC_NODE)->data;

	stack_event_cb = data->event_cb;
	data->event_cb = setup_tap;
}
#endif

/* The wired device did not come up: close the session in the log and, with `shut_down`, release
 * what this device holds (the USB controller, its classes), so that the wireless boot that follows
 * can show the console (mode.c falls back to wireless). */
static int wired_start_failed(int err, bool shut_down)
{
	if (shut_down) {
		(void)usbd_shutdown(&osc2_usbd);
	}
	started = false;
	usb_state = OPENSC2_USB_OFF;
	osc2_usblog_end_session("USB set-up failed");
	osc2_usblog_backend(false);
	return err;
}

int osc2_usb_wired_start(void)
{
	struct opensc2_identity id;
	size_t product_len = 0;
	int err;

	profile = IS_ENABLED(CONFIG_OPENSC2_USB_PROFILE_OCTOBER) ? &opensc2_usb_profile_october :
								   &opensc2_usb_profile_july;
	opensc2_identity_get(&id);
	memcpy(serial_text, id.unit_serial, OPENSC2_SERIAL_LEN);
	/* The identity in use was checked: 1 to 31 printable characters. */
	memset(product_text, 0, sizeof(product_text));
	while (product_len < OPENSC2_PRODUCT_NAME_MAX && id.product_name[product_len] != '\0') {
		product_text[product_len] = (uint8_t)id.product_name[product_len];
		product_len++;
	}
	osc2_product.bLength = (uint8_t)(2 + 2 * product_len);

	osc2_usblog_set_time_base();
	osc2_usblog_start_session();
	osc2_usblog_backend(true);

	err = hid_device_register(hid_dev, opensc2_hid_report_desc,
				  sizeof(opensc2_hid_report_desc), &hid_ops);
	err = err ? err : usbd_add_descriptor(&osc2_usbd, &osc2_lang);
	err = err ? err : usbd_add_descriptor(&osc2_usbd, &osc2_mfr);
	err = err ? err : usbd_add_descriptor(&osc2_usbd, &osc2_product);
	err = err ? err : usbd_add_descriptor(&osc2_usbd, &osc2_sn);
	err = err ? err : usbd_add_configuration(&osc2_usbd, USBD_SPEED_FS, &osc2_config);
	err = err ? err : register_class_of(&osc2_usbd, hid_dev);   /* interface 0 */
#if defined(CONFIG_OPENSC2_USB_WIRED_CONSOLE)
	/* Debug only: a console as interfaces 1 and 2 makes the device composite (Windows paths gain
	 * MI_00); never use it to compare with the real controller. */
	err = err ? err : register_class_of(&osc2_usbd, cdc_dev);
	err = err ? err : usbd_device_set_code_triple(&osc2_usbd, USBD_SPEED_FS,
						      USB_BCC_MISCELLANEOUS, 0x02, 0x01);
#else
	err = err ? err : usbd_device_set_code_triple(&osc2_usbd, USBD_SPEED_FS,
						      profile->device_class,
						      profile->device_subclass,
						      profile->device_protocol);
#endif
	err = err ? err : usbd_device_set_bcd_usb(&osc2_usbd, USBD_SPEED_FS, profile->bcd_usb);
	err = err ? err : usbd_device_set_bcd_device(&osc2_usbd, profile->bcd_device);
#if defined(CONFIG_OPENSC2_USB_PROFILE_OCTOBER)
	err = err ? err : usbd_add_descriptor(&osc2_usbd, &osc2_bos);
#endif
	err = err ? err : usbd_msg_register_cb(&osc2_usbd, on_usb_msg);
	err = err ? err : usbd_init(&osc2_usbd);
	if (err) {
		/* The USB controller is as it was (a failed usbd_init() leaves it so). Only the debug
		 * console registered in this device must be released for the wireless console. */
		LOG_ERR("Setting up the USB device failed (%d)", err);
		osc2_usblog("USB set-up failed (%d)", err);
		return wired_start_failed(err, IS_ENABLED(CONFIG_OPENSC2_USB_WIRED_CONSOLE));
	}
	self_check();
#if WATCH_SETUP
	setup_tap_install();
#endif
	k_thread_name_set(osc2_usb_tid, "opensc2_usb");
	k_thread_start(osc2_usb_tid);

	started = true;
	usb_state = OPENSC2_USB_ATTACHED;
	attach_ms = k_uptime_get();
	osc2_usblog_set_time_base();
	err = usbd_enable(&osc2_usbd);
	if (err) {
		LOG_ERR("Enabling the USB device failed (%d)", err);
		osc2_usblog("USB enable failed (%d)", err);
		return wired_start_failed(err, true);
	}
	/* The name itself stays out of the log: it may be personal. */
	osc2_usblog("attach: %04X:%04X, %s profile, bcdDevice %04X, %s product string",
		    OPENSC2_USB_VID, OPENSC2_USB_PID,
		    profile == &opensc2_usb_profile_july ? "July" : "October", profile->bcd_device,
		    strcmp(id.product_name, CONFIG_OPENSC2_USB_PRODUCT) == 0 ? "the default" :
									       "a custom");
	LOG_INF("Wired: USB device %04x:%04x attached", OPENSC2_USB_VID, OPENSC2_USB_PID);
#if OSC2_NO_HOST_WIRELESS_MS > 0
	if (osc2.mode_reason == OPENSC2_MODE_USB_POWER) {
		osc2_usblog("test build: wireless after %u ms if no host talks (not the real "
			    "controller)", (unsigned int)OSC2_NO_HOST_WIRELESS_MS);
		LOG_WRN("Charger test build: no USB host within %u ms and the board goes wireless "
			"(the real controller waits)", (unsigned int)OSC2_NO_HOST_WIRELESS_MS);
	}
#endif
	k_work_schedule(&wait_work, K_MSEC(WAIT_POLL_MS));
	notify();
	return 0;
}

void osc2_usb_detach(const char *why)
{
	if (!started || detaching) {
		return;
	}
	detaching = true;
	k_timer_stop(&report_timer);
	(void)k_work_cancel_delayable(&wait_work);
	(void)k_work_cancel_delayable(&unplug_work);
	if (configured) {
		/* One all-zero state report (sequence 0) so the host releases everything. */
		static const uint8_t neutral[OPENSC2_STATE_REPORT_LEN] = { OPENSC2_RPT_STATE };
		int err = -ENOBUFS;

		for (int i = 0; i < 5 && err == -ENOBUFS; i++) {
			k_mutex_lock(&send_lock, K_FOREVER);
			k_msgq_purge(&osc2_usb_pending);
			err = submit_locked(neutral, sizeof(neutral));
			k_mutex_unlock(&send_lock);
			if (err == -ENOBUFS) {
				k_msleep(1);
			}
		}
		osc2_usblog("neutral state report %s", err ? "not sent" : "sent");
	}
	k_msleep(10);
	(void)usbd_disable(&osc2_usbd);
	configured = false;
	usb_state = OPENSC2_USB_OFF;
	osc2_usblog("detach: %s", why ? why : "?");
	osc2_usblog_counts(n_requests, n_stalls, n_outputs, n_reports, n_dropped);
	osc2_usblog_end_session(why);
	osc2_usblog_backend(false);
}

/* ---- the debug console in wireless mode ------------------------------------------------------ */

#if defined(CONFIG_OPENSC2_USB_CONSOLE)
USBD_DEVICE_DEFINE(osc2_console, DEVICE_DT_GET(UDC_NODE), CONFIG_OPENSC2_USB_CONSOLE_VID,
		   CONFIG_OPENSC2_USB_CONSOLE_PID);
USBD_DESC_LANG_DEFINE(console_lang);
USBD_DESC_MANUFACTURER_DEFINE(console_mfr, CONFIG_OPENSC2_USB_CONSOLE_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(console_product, CONFIG_OPENSC2_USB_CONSOLE_PRODUCT);
#if defined(CONFIG_HWINFO)
USBD_DESC_SERIAL_NUMBER_DEFINE(console_sn);
#endif
USBD_CONFIGURATION_DEFINE(console_config, 0, 125, NULL);
#endif

int osc2_usb_console_start(void)
{
#if defined(CONFIG_OPENSC2_USB_CONSOLE)
	static bool console_started;
	int err;

	if (console_started) {
		return 0;   /* also asked for after an error in opensc2_init() */
	}
	console_started = true;
	err = usbd_add_descriptor(&osc2_console, &console_lang);
	err = err ? err : usbd_add_descriptor(&osc2_console, &console_mfr);
	err = err ? err : usbd_add_descriptor(&osc2_console, &console_product);
#if defined(CONFIG_HWINFO)
	err = err ? err : usbd_add_descriptor(&osc2_console, &console_sn);
#endif
	err = err ? err : usbd_add_configuration(&osc2_console, USBD_SPEED_FS, &console_config);
	err = err ? err : register_class_of(&osc2_console, cdc_dev);
	err = err ? err : usbd_device_set_code_triple(&osc2_console, USBD_SPEED_FS,
						      USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	err = err ? err : usbd_init(&osc2_console);
	err = err ? err : usbd_enable(&osc2_console);
	if (err) {
		LOG_ERR("The USB console did not start (%d)", err);
	}
	return err;
#else
	return 0;
#endif
}
