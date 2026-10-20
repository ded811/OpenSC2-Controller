/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * fwemu: the library's command channel and hardware test hooks on a PC, driven over stdin/stdout,
 * so the hardware test suite's runner (tools/hwtest, "--dry-run --fw-emu <this program>") can run
 * its groups against the firmware's own code instead of a model of it. It is the hooks binary of
 * the host tests (commands.c, test_core.c, test_notes.c, codec.c, lizard.c, ... with stubs.c and
 * hooks_stub.c) with this main() instead of the test cases.
 *
 * The firmware's own code here: the vendor command channel on USB (the 64-byte feature reports,
 * their framing and STALLs) and on the radio (SET/GET records), the test channel (key, claiming
 * rule, every op, held answers, the lease and the end of arming), the console's "opensc2 test"
 * lines, the ring and the counters, the settings and the LED, input injection, the state report's
 * rules and encoding, lizard mode (lizard.c), the battery report, output report decoding. Done
 * here the way report.c, core.c and radio.c do it, in a few lines each: the order of lizard mode's
 * reports, the frame and battery cadence, the Steam watchdog's expiry, the rumble timeout, the
 * radio link (connected, or off after "opensc2 stop" or a turn-off), reboots, and the reports the
 * application sends. Not here: USB descriptors, the radio protocol, the other shell commands.
 *
 * One command per line; every reply ends with a line ".":
 *   BOOT W|R            boot wired or wireless (the hooks unarmed, every setting at its default)
 *   TIME                "TIME <µs>"
 *   ADVANCE <ms>        the clock moves on (the lease, the end of a disarm, timers)
 *   USBSET <hex>        a SET_REPORT(Feature 1) data stage: "OK" or "STALL <errno>"
 *   USBGET [<wLength>]  a GET_REPORT(Feature 1): "ANS <hex>" or "STALL <errno>"
 *   RADIOSET <hex>      a SET record {opcode, n, payload} from the puck: "RC <rc>"
 *   RADIOGET            a GET record: "ANS <hex>" ({opcode, n, data})
 *   OUT <hex>           an output report from the host on the mode's link: "N <decoded>"
 *   CONSOLE <words>     "opensc2 test <words>": the console's lines as they are printed
 *   FRAMES <n>          n state reports (4028 µs apart): "RPT <hex>" for each, with the battery
 *                       reports that fall due and the reports the application sent, in order
 *   RADIO OFF|ON        "opensc2 stop" / "opensc2 start" (wireless)
 *   STATE               "MODE W|R", "LINK <state>", "ARMED <0|1>", "GUARD <0|1>"
 *   QUIT
 * Reports sent outside FRAMES come as "RPT <hex>" lines in that command's reply. A reboot (a power
 * request falling due) prints "EVENT BOOT W|R" where it happens.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include "cal_store.h"
#include "cal_host.h"
#include "lizard.h"
#include "report_rules.h"
#include "stubs.h"
#include "hooks_stub.h"

#define FRAME_US       4028u     /* the USB report interval, 4.0283 ms */
#define BATTERY_US     3500000u  /* CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS */
#define WATCHDOG_US    10000000u /* the Steam watchdog */
#define RUMBLE_US      500000u   /* CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS */
#define QUEUE_MAX      64

void osc2_stub_lizard_note(char what);
static int output_hook(uint8_t link, const uint8_t *report, uint8_t len);

static uint64_t now_us;
static uint64_t boot_us;             /* the uptime counts from the last boot */
static uint64_t next_battery_us;
static uint8_t sequence;
static bool trigger_bit[2];
static bool wireless;
static int last_power_requests;
static uint64_t power_due_us;
static bool power_pending;
static int last_battery_now;
static struct opensc2_lizard lizard;
static int last_wd_armed, last_wd_cancelled;
static bool watchdog_pending;
static uint64_t watchdog_due_us;
static bool rumble_active;
static uint64_t rumble_due_us;

/* Reports waiting to be read with the next FRAMES, in the order they were sent. */
static struct {
	uint8_t len;
	uint8_t data[64];
} queue[QUEUE_MAX];
static int queued;

static void put_hex(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		printf("%02x", p[i]);
	}
}

static void enqueue(const uint8_t *report, size_t len)
{
	if (queued < QUEUE_MAX && len <= sizeof(queue[0].data)) {
		queue[queued].len = (uint8_t)len;
		memcpy(queue[queued].data, report, len);
		queued++;
	}
}

static void flush_queue(void)
{
	for (int i = 0; i < queued; i++) {
		printf("RPT ");
		put_hex(queue[i].data, queue[i].len);
		printf("\n");
	}
	queued = 0;
}

/* osc2_input_send(): the lizard guard first, then the link of this mode. */
static int input_send(const uint8_t *report, size_t len)
{
	if (osc2_test_divert(report, len)) {
		return 0;
	}
	if (wireless && !hk.radio_connected) {
		return -ENOTCONN;
	}
	enqueue(report, len);
	return 0;
}

/* ---- lizard mode, in report.c's order ------------------------------------------------------- */

void osc2_lizard_set(bool on)
{
	uint8_t keyboard[9], mouse[6];
	uint32_t made = on ? opensc2_lizard_enter(&lizard, keyboard, mouse) :
			     opensc2_lizard_leave(&lizard, keyboard, mouse);

	osc2_stub_lizard_note(on ? 'E' : 'L');
	if (made) {
		osc2_test_note_lizard(on ? OSC2_TEST_LIZARD_ENTER : OSC2_TEST_LIZARD_LEAVE);
		osc2_test_count_n(OSC2_TEST_CNT_LIZARD_MADE, 2);
	}
	if (made && on) {
		(void)input_send(mouse, sizeof(mouse));
		(void)input_send(keyboard, sizeof(keyboard));
	} else if (made) {
		(void)input_send(keyboard, sizeof(keyboard));
		(void)input_send(mouse, sizeof(mouse));
	}
}

void osc2_lizard_switches(bool up)
{
	osc2_stub_lizard_note(up ? 'U' : 'D');
	opensc2_lizard_switches(&lizard, up);
	osc2_test_note_lizard(up ? OSC2_TEST_LIZARD_UP : OSC2_TEST_LIZARD_DOWN);
}

static bool lizard_release(void)
{
	uint8_t keyboard[9], mouse[6];
	bool sent = opensc2_lizard_release(&lizard, keyboard, mouse) != 0;

	if (sent) {
		(void)input_send(keyboard, sizeof(keyboard));
		(void)input_send(mouse, sizeof(mouse));
	}
	return sent;
}

static void lizard_resync(void)
{
	opensc2_lizard_resync(&lizard);
}

static void lizard_frame(const struct opensc2_state *s)
{
	uint8_t keyboard[9], mouse[6];
	uint32_t made;

	if (osc2_test_skip_lizard() || osc2.settings[OPENSC2_SETTING_LIZARD_MODE] == 0) {
		return;
	}
	made = opensc2_lizard_step(&lizard, s,
				   (uint8_t)osc2.settings[OPENSC2_SETTING_PAD_IGNORE_ON_STICK],
				   keyboard, mouse);
	if (made & OPENSC2_LIZARD_KEYBOARD) {
		osc2_test_count(OSC2_TEST_CNT_LIZARD_MADE);
		(void)input_send(keyboard, sizeof(keyboard));
	}
	if (made & OPENSC2_LIZARD_MOUSE) {
		osc2_test_count(OSC2_TEST_CNT_LIZARD_MADE);
		(void)input_send(mouse, sizeof(mouse));
	}
}

/* ---- the clock and the timers ----------------------------------------------------------------- */

static void set_clock(void)
{
	stub.now_ms = (int64_t)((now_us - boot_us) / 1000u);
	hk.extra_us = (uint32_t)((now_us - boot_us) % 1000u);
}

static void boot(bool to_wireless);

static void battery_report(void)
{
	uint8_t report[OPENSC2_BATTERY_REPORT_LEN];
	struct opensc2_battery b = osc2.battery;

	osc2_test_battery(&b);
	opensc2_encode_battery(&b, report);
	osc2_test_count(input_send(report, sizeof(report)) == 0 ? OSC2_TEST_CNT_BATTERY_SENT :
			OSC2_TEST_CNT_BATTERY_DROPPED);
}

/* core.c's watchdog_fn(). */
static void watchdog_expiry(void)
{
	if (osc2.settings[OPENSC2_SETTING_LIZARD_MODE] != 0) {
		return;
	}
	osc2_lizard_switches(true);
	osc2_settings_restore_defaults(true);
	osc2_lizard_set(true);
	osc2_test_note_setting(OPENSC2_SETTINGS_ALL, 0);
	osc2_notify_led();
}

/* report.c's rumble_off(). */
static void rumble_off(void)
{
	struct opensc2_rumble off = { .timeout = true };

	rumble_active = false;
	osc2_test_note_rumble(OSC2_TEST_LINK_NONE, &off);
}

static void radio_off(void)
{
	hk.radio_connected = false;
	stub.link_active[OSC2_LINK_RADIO] = false;
	hk.link_state = OPENSC2_LINK_OFF;
	osc2.last_disconnect_reason = OPENSC2_REASON_DISABLED;
	osc2_test_note_link(OPENSC2_LINK_OFF, OPENSC2_REASON_DISABLED, osc2.slot, 0, 0);
	if (rumble_active) {
		rumble_off();   /* osc2_output_link_down() */
	}
}

/* A power request falls due: noted as core.c does, then what the blank canvas does with it. */
static void power_now(void)
{
	enum opensc2_power_request request = stub.power;

	power_pending = false;
	osc2_test_note_power((uint8_t)request, stub.power_why);
	switch (request) {
	case OPENSC2_POWER_TURN_OFF:
		if (wireless && hk.radio_connected) {
			radio_off();   /* the blank canvas: opensc2_stop() */
		}
		break;
	case OPENSC2_POWER_REBOOT:
	case OPENSC2_POWER_FACTORY_RESET:
	case OPENSC2_POWER_REBOOT_KEEP_LINK: {
		bool to_r = request == OPENSC2_POWER_REBOOT_KEEP_LINK ||
			    osc2.policy == OPENSC2_POLICY_WIRELESS;

		if (!wireless) {
			/* osc2_usb_detach(): one all-zero state report, sequence 0 too */
			static const uint8_t neutral[OPENSC2_STATE_REPORT_LEN] = { OPENSC2_RPT_STATE };

			printf("RPT ");
			put_hex(neutral, sizeof(neutral));
			printf("\n");
		}
		printf("EVENT BOOT %c\n", to_r ? 'R' : 'W');
		boot(to_r);
		break;
	}
	default:
		break;   /* the bootloader request: ignored by the blank canvas */
	}
}

/* What a request may have started: a power request, the watchdog, a battery report now, a report
 * the application sent. */
static void after_request(void)
{
	if (stub.power_requests != last_power_requests) {
		last_power_requests = stub.power_requests;
		power_pending = true;
		power_due_us = now_us + (uint64_t)stub.power_delay_ms * 1000u;
	}
	if (stub.watchdog_armed != last_wd_armed) {
		last_wd_armed = stub.watchdog_armed;
		watchdog_pending = true;
		watchdog_due_us = now_us + WATCHDOG_US;
	}
	if (stub.watchdog_cancelled != last_wd_cancelled) {
		last_wd_cancelled = stub.watchdog_cancelled;
		watchdog_pending = false;
	}
	if (hk.battery_now_calls != last_battery_now) {
		last_battery_now = hk.battery_now_calls;
		battery_report();
		next_battery_us = now_us + BATTERY_US;
	}
	if (hk.app_send_len) {
		enqueue(hk.app_send_last, hk.app_send_len);
		hk.app_send_len = 0;
	}
	if (hk.app_haptic_calls) {
		uint8_t report[OPENSC2_HAPTIC_STATUS_LEN];

		/* opensc2_send_haptic_status(): one 0x44, sent like any report of the application */
		hk.app_haptic_calls = 0;
		opensc2_encode_haptic_status(hk.app_haptic_channel, hk.app_haptic_flags, report);
		if (input_send(report, sizeof(report)) == 0) {
			osc2_test_count(OSC2_TEST_CNT_APP_REPORTS_SENT);
		}
	}
}

/* The deadlines of the hooks (the lease, the end of a disarm) as the work item runs them. */
static void deadlines(void)
{
	for (int i = 0; i < 4; i++) {
		int32_t ms = osc2_test_core_next(&osc2_test_core_obj, &osc2_test_env_obj);

		if (ms != 0) {
			break;
		}
		osc2_test_core_poll(&osc2_test_core_obj, &osc2_test_env_obj);
	}
}

static void advance_us(uint64_t us)
{
	uint64_t end = now_us + us;

	while (now_us < end) {
		uint64_t step = end - now_us < 1000u ? end - now_us : 1000u;

		now_us += step;
		set_clock();
		deadlines();
		if (power_pending && now_us >= power_due_us) {
			power_now();
		}
		if (watchdog_pending && now_us >= watchdog_due_us) {
			watchdog_pending = false;
			watchdog_expiry();
			after_request();
		}
		if (rumble_active && now_us >= rumble_due_us) {
			rumble_off();
		}
		if (now_us >= next_battery_us) {
			next_battery_us += BATTERY_US;
			battery_report();
		}
	}
}

/* ---- boot --------------------------------------------------------------------------------------- */

static void boot(bool to_wireless)
{
	struct opensc2_bond bond = { .puck_id = 0x11111111u, .controller_id = 0x22222222u };
	struct cal_host_record flash = cal_host;   /* the stored calibration survives a reboot */

	stub_reset();
	cal_host = flash;
	cal_host_boot();
	memcpy(bond.puck_serial, "FAKEPUCK00001", 13);
	osc2.bonds[0] = bond;
	osc2.chime = 2;
	osc2.battery = (struct opensc2_battery){
		.charge_state = OPENSC2_CHARGE_DONE,
		.level = 100,
		.battery_mv = 4100,
		.system_mv = 3300,
		.input_mv = 5000,
	};
	osc2.policy = OPENSC2_POLICY_AUTO;
	wireless = to_wireless;
	if (wireless) {
		osc2.mode = OPENSC2_MODE_WIRELESS;
		osc2.mode_reason = OPENSC2_MODE_WARM_RESTART;
		stub.link_active[OSC2_LINK_USB] = false;
		stub.link_active[OSC2_LINK_RADIO] = true;
		hk.radio_connected = true;
		hk.link_state = OPENSC2_LINK_CONNECTED;
	} else {
		osc2.mode = OPENSC2_MODE_WIRED;
		osc2.mode_reason = OPENSC2_MODE_USB_POWER;
		hk.radio_connected = false;
		hk.link_state = OPENSC2_LINK_OFF;
	}
	hk.release_hook = lizard_release;
	hk.resync_hook = lizard_resync;
	hk.output_hook = output_hook;
	boot_us = now_us;
	opensc2_lizard_reset(&lizard);
	sequence = 0;
	trigger_bit[0] = trigger_bit[1] = false;
	queued = 0;
	power_pending = false;
	rumble_active = false;
	last_power_requests = stub.power_requests;
	last_battery_now = hk.battery_now_calls;
	last_wd_armed = stub.watchdog_armed;
	last_wd_cancelled = stub.watchdog_cancelled;
	watchdog_pending = true;                 /* armed at boot; lizard mode is on: harmless */
	watchdog_due_us = now_us + WATCHDOG_US;
	next_battery_us = now_us + BATTERY_US;
	set_clock();
}

/* ---- the frame path: report.c's osc2_report_state() and osc2_report_lizard() ------------------ */

static void frame(void)
{
	uint8_t report[OPENSC2_STATE_REPORT_LEN];
	struct opensc2_state s;
	static const uint32_t bit[2] = { OPENSC2_CLICK_LEFT_TRIGGER, OPENSC2_CLICK_RIGHT_TRIGGER };

	if (wireless && !hk.radio_connected) {
		return;   /* no keep-alives from the puck, no reports */
	}
	memset(&s, 0, sizeof(s));   /* nothing wired: the application's state is all zero */
	osc2_cal_frame(&s);
	osc2_test_input(&s);
	osc2_cal_rules(&s);
	for (int side = 0; side < 2; side++) {
		trigger_bit[side] = osc2_trigger_click(s.trigger[side],
						       osc2.settings[OPENSC2_SETTING_TRIGGER_THRESHOLD],
						       trigger_bit[side]);
		s.buttons = trigger_bit[side] ? (s.buttons | bit[side]) : (s.buttons & ~bit[side]);
	}
	opensc2_encode_state(&s, sequence++, report);
	osc2_test_frame(report);
	enqueue(report, sizeof(report));
	osc2_test_count(wireless ? OSC2_TEST_CNT_RADIO_SENT : OSC2_TEST_CNT_USB_SENT);
	lizard_frame(&s);
}

/* ---- output reports: report.c's osc2_output_dispatch() without the callbacks ------------------ */

static int output(enum osc2_link link, const uint8_t *report, size_t len)
{
	struct opensc2_output out[2];
	int n;

	if (len < 1) {
		return -1;
	}
	n = opensc2_decode_output(report, len, out);
	osc2_test_note_output(link, report, len, n);
	osc2_test_count(OSC2_TEST_CNT_OUT_REPORTS);
	if (n < 0) {
		osc2_test_count(OSC2_TEST_CNT_OUT_SHORT);
		return n;
	}
	for (int i = 0; i < n; i++) {
		if (out[i].type == OPENSC2_OUTPUT_RUMBLE) {
			rumble_active = out[i].rumble.left_speed || out[i].rumble.right_speed;
			rumble_due_us = now_us + RUMBLE_US;
			osc2_test_note_rumble(link, &out[i].rumble);
		} else if (out[i].type == OPENSC2_OUTPUT_HAPTIC) {
			osc2_test_note_haptic(link, &out[i].haptic);
		}
	}
	return n;
}

static int output_hook(uint8_t link, const uint8_t *report, uint8_t len)
{
	return output((enum osc2_link)link, report, len);
}

/* ---- the console ---------------------------------------------------------------------------- */

static void emit(void *ctx, const char *line)
{
	(void)ctx;
	printf("%s\n", line);
}

static void console(char *words)
{
	char *argv[16];
	int argc = 0;

	for (char *t = strtok(words, " \t"); t && argc < 16; t = strtok(NULL, " \t")) {
		argv[argc++] = t;
	}
	(void)osc2_test_console_run(argc, (const char *const *)argv, emit, NULL);
}

/* ---- main ------------------------------------------------------------------------------------- */

static void radio(bool on)
{
	if (!wireless) {
		return;
	}
	if (!on && hk.radio_connected) {
		radio_off();
	} else if (on && !hk.radio_connected) {
		/* radio.c: listening, then the puck's connection; the identity record and the
		 * protocol version are written at every connection */
		osc2_test_note_link(OPENSC2_LINK_LISTENING, OPENSC2_REASON_NONE, osc2.slot, 0, 0);
		hk.radio_connected = true;
		stub.link_active[OSC2_LINK_RADIO] = true;
		hk.link_state = OPENSC2_LINK_CONNECTED;
		osc2_test_note_link(OPENSC2_LINK_CONNECTED, OPENSC2_REASON_NONE, osc2.slot, 27, 2);
		osc2_test_note_identity(&osc2.identity);
		osc2_test_note_version(1);
	}
}

int main(void)
{
	char line[2048];

	setvbuf(stdout, NULL, _IOFBF, 1 << 16);
	now_us = 1000000u;   /* a board up for a second */
	boot(false);
	while (fgets(line, sizeof(line), stdin)) {
		char *nl = strpbrk(line, "\r\n");
		char *arg;
		uint8_t buf[256];
		int n;

		if (nl) {
			*nl = '\0';
		}
		arg = strchr(line, ' ');
		if (arg) {
			*arg++ = '\0';
		} else {
			arg = line + strlen(line);
		}
		hk.app_send_len = 0;
		if (strcmp(line, "QUIT") == 0) {
			break;
		} else if (strcmp(line, "BOOT") == 0) {
			boot(arg[0] == 'R');
		} else if (strcmp(line, "TIME") == 0) {
			printf("TIME %llu\n", (unsigned long long)now_us);
		} else if (strcmp(line, "ADVANCE") == 0) {
			advance_us((uint64_t)strtoul(arg, NULL, 10) * 1000u);
		} else if (strcmp(line, "USBSET") == 0) {
			n = osc2_test_unhex(arg, buf, sizeof(buf));
			if (n < 0 || wireless) {
				printf("STALL %d\n", n < 0 ? -EINVAL : -ENODEV);
			} else {
				int rc = osc2_usb_feature_set(buf, (uint16_t)n);

				if (rc) {
					printf("STALL %d\n", rc);
				} else {
					printf("OK\n");
				}
				after_request();
			}
		} else if (strcmp(line, "USBGET") == 0) {
			uint8_t op = 0, nn = 0;
			unsigned long wlength = *arg ? strtoul(arg, NULL, 10) : 64;
			int rc = wireless ? -ENODEV :
				 osc2_usb_feature_get(buf, (uint16_t)wlength, 64, &op, &nn);

			if (rc <= 0) {
				printf("STALL %d\n", rc < 0 ? rc : -EIO);
			} else {
				printf("ANS ");
				put_hex(buf, (size_t)rc);
				printf("\n");
			}
			after_request();
		} else if (strcmp(line, "RADIOSET") == 0) {
			n = osc2_test_unhex(arg, buf, sizeof(buf));
			if (n < 2) {
				printf("ERR bad record\n");
			} else {
				printf("RC %ld\n", (long)osc2_command_set(OSC2_LINK_RADIO, buf, (uint8_t)n));
				after_request();
			}
		} else if (strcmp(line, "RADIOGET") == 0) {
			uint8_t out[63];
			uint8_t len = osc2_command_get(OSC2_LINK_RADIO, out);

			printf("ANS ");
			put_hex(out, len);
			printf("\n");
			after_request();
		} else if (strcmp(line, "OUT") == 0) {
			n = osc2_test_unhex(arg, buf, sizeof(buf));
			if (n < 0) {
				printf("ERR bad hex\n");
			} else {
				printf("N %d\n", output(wireless ? OSC2_LINK_RADIO : OSC2_LINK_USB, buf,
							(size_t)n));
				after_request();
			}
		} else if (strcmp(line, "CONSOLE") == 0) {
			console(arg);
			after_request();
		} else if (strcmp(line, "FRAMES") == 0) {
			long count = strtol(arg, NULL, 10);

			flush_queue();
			for (long i = 0; i < count; i++) {
				advance_us(FRAME_US);
				frame();
				flush_queue();
			}
		} else if (strcmp(line, "RADIO") == 0) {
			radio(strcmp(arg, "ON") == 0);
		} else if (strcmp(line, "STATE") == 0) {
			printf("MODE %c\nLINK %u\nARMED %d\nGUARD %d\n", wireless ? 'R' : 'W',
			       (unsigned int)hk.link_state, osc2_test_core_armed(&osc2_test_core_obj),
			       osc2_test_core_guard(&osc2_test_core_obj));
		} else if (line[0] != '\0') {
			printf("ERR unknown command %s\n", line);
		}
		flush_queue();
		printf(".\n");
		fflush(stdout);
	}
	return 0;
}
