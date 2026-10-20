/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * usblog.c: the retained RAM area (retained.h): the warm-restart mark and the USB session log. The
 * log is a ring of short text lines that a soft reset keeps, so a wired session (HID only, no
 * console) can be read afterwards in wireless mode with `opensc2 usb log`. Bond values and serials
 * never go into it, so it can be shared.
 *
 * Where the area lives: the devicetree region chosen as `opensc2,retained-ram` (a
 * "zephyr,memory-region" node taken out of the application's RAM), or else a buffer in the
 * application's no-init RAM. That fallback is only safe without a bootloader that uses RAM at
 * every reset: the Adafruit UF2 bootloader's own data and stack land in the application's RAM. So
 * the build warns about it (lib/opensc2/CMakeLists.txt), and on nRF52 the warm-restart mark then
 * defaults to GPREGRET2 (Kconfig), which keeps Steam's reboot into wireless mode working.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include "internal.h"
#include "crash.h"
#include "mode_rules.h"
#include "retained.h"
#if defined(CONFIG_OPENSC2_WARM_MARK_GPREGRET2)
#include <hal/nrf_power.h>
#endif

#if defined(CONFIG_OPENSC2_USB_LOG)
#define LOG_RING_SIZE CONFIG_OPENSC2_USB_LOG_SIZE
#else
#define LOG_RING_SIZE 0
#endif

#if DT_HAS_CHOSEN(opensc2_retained_ram)
#include <zephyr/linker/devicetree_regions.h>
#define AREA_NODE DT_CHOSEN(opensc2_retained_ram)
/* The region less the crash record's slot (crash.c), which lives in it too. */
#define AREA_SIZE (DT_REG_SIZE(AREA_NODE) - OSC2_CRASH_SLOT_SIZE)
static uint8_t area[AREA_SIZE] __aligned(4)
	__attribute__((__section__(LINKER_DT_NODE_REGION_NAME(AREA_NODE))));
#else
#define AREA_SIZE (sizeof(struct osc2_retained) + LOG_RING_SIZE)
static uint8_t area[AREA_SIZE] __aligned(4) __noinit;
#endif

BUILD_ASSERT(AREA_SIZE >= sizeof(struct osc2_retained), "The retained RAM region is too small");

#define RET ((struct osc2_retained *)area)
#define OSC2_LINE 120

static struct k_spinlock ring_lock;
static bool kept;                      /* the area was intact at this boot */
static int mark_at_boot = OSC2_MARK_NONE;
static int64_t time_base;              /* ticks: boot, then the attach */

/* ---- set-up at boot: read and clear the mark, check the area ---------------------------------- */

static int retained_init(void)
{
	struct osc2_retained *r = RET;

	/* The mark first: it carries its own check, and setting the area up would wipe it. */
#if defined(CONFIG_OPENSC2_WARM_MARK_GPREGRET2)
	mark_at_boot = osc2_mark_from_byte((uint8_t)nrf_power_gpregret_get(NRF_POWER, 1));
	nrf_power_gpregret_set(NRF_POWER, 1, 0);
#else
	mark_at_boot = osc2_mark_from_words(r->mark, r->mark_inv);
#endif
	kept = osc2_ret_valid(r, AREA_SIZE);
	if (!kept) {
		osc2_ret_format(r, AREA_SIZE);
	}
	r->mark = 0;
	r->mark_inv = ~0u;
	r->boots++;
	osc2_ret_seal(r);
	return 0;
}
SYS_INIT(retained_init, PRE_KERNEL_1, 0);

int osc2_retained_mark_taken(void)
{
	return mark_at_boot;
}

void osc2_retained_mark_set(int mark)
{
#if defined(CONFIG_OPENSC2_WARM_MARK_GPREGRET2)
	nrf_power_gpregret_set(NRF_POWER, 1, osc2_mark_byte((enum osc2_mark)mark));
#else
	RET->mark = osc2_mark_word((enum osc2_mark)mark);
	RET->mark_inv = ~RET->mark;
#endif
}

bool osc2_retained_was_kept(void)
{
	return kept;
}

/* ---- writing lines ------------------------------------------------------------------------------ */

/* Tenths of a millisecond since the time base (the attach while wired, else the boot). */
static uint32_t tenths_since_base(int64_t ticks)
{
	int64_t delta = ticks - time_base;

	return delta > 0 ? (uint32_t)(k_ticks_to_us_floor64((uint64_t)delta) / 100) : 0;
}

static size_t stamp(char *buf, size_t size)
{
	uint32_t tenths = tenths_since_base(k_uptime_ticks());
	int n = snprintf(buf, size, "%7u.%u ", (unsigned int)(tenths / 10),
			 (unsigned int)(tenths % 10));

	return n > 0 ? MIN((size_t)n, size - 1) : 0;
}

static void append_line(char *line, size_t n)
{
	k_spinlock_key_t key;

	line[n++] = '\n';
	key = k_spin_lock(&ring_lock);
	osc2_ret_append(RET, line, n);
	k_spin_unlock(&ring_lock, key);
}

/* Setup packets come from the USB driver's own thread, which has a small stack: it only copies
 * them here, and whoever logs next (or a work item) turns them into lines, in order. */
#define SETUP_FIFO 16
static struct {
	int64_t ticks;
	uint8_t pkt[8];
} setups[SETUP_FIFO];
static atomic_t setup_head, setup_tail, setup_lost;

static void drain_setups(void)
{
	for (;;) {
		char line[OSC2_LINE + 2];
		char text[72];
		atomic_val_t tail = atomic_get(&setup_tail);
		int64_t ticks;
		uint8_t pkt[8];
		uint32_t tenths;
		size_t n;

		if (tail == atomic_get(&setup_head)) {
			break;
		}
		ticks = setups[tail % SETUP_FIFO].ticks;
		memcpy(pkt, setups[tail % SETUP_FIFO].pkt, sizeof(pkt));
		if (!atomic_cas(&setup_tail, tail, tail + 1)) {
			continue;   /* someone else took this one */
		}
		opensc2_usb_setup_text(pkt, text, sizeof(text));
		tenths = tenths_since_base(ticks);
		n = (size_t)snprintf(line, OSC2_LINE, "%7u.%u SETUP %s", (unsigned int)(tenths / 10),
				     (unsigned int)(tenths % 10), text);
		append_line(line, MIN(n, (size_t)OSC2_LINE - 1));
	}
	if (atomic_get(&setup_lost)) {
		char line[OSC2_LINE + 2];
		size_t n = stamp(line, OSC2_LINE);

		n += (size_t)snprintf(&line[n], OSC2_LINE - n, "(%u setup packets not logged)",
				      (unsigned int)atomic_set(&setup_lost, 0));
		append_line(line, MIN(n, (size_t)OSC2_LINE - 1));
	}
}

static void setup_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	drain_setups();
}
static K_WORK_DEFINE(setup_work, setup_work_fn);

void osc2_usblog_setup(const uint8_t setup[8])
{
	atomic_val_t head = atomic_get(&setup_head);

	if (!IS_ENABLED(CONFIG_OPENSC2_USB_LOG)) {
		return;
	}
	if (head - atomic_get(&setup_tail) >= SETUP_FIFO) {
		atomic_inc(&setup_lost);
		return;
	}
	setups[head % SETUP_FIFO].ticks = k_uptime_ticks();
	memcpy(setups[head % SETUP_FIFO].pkt, setup, 8);
	atomic_set(&setup_head, head + 1);
	k_work_submit(&setup_work);
}

#if defined(CONFIG_OPENSC2_USB_LOG_ZEPHYR)
/* Zephyr log messages that the log core dropped while it fed the session log. The log thread only
 * counts them (its stack is small); the next osc2_usblog(), from any other thread, writes the line
 * into `line`, its own buffer. */
static atomic_t zlost;

static void write_zlost(char *line)
{
	atomic_val_t lost = atomic_set(&zlost, 0);
	size_t n;

	if (lost == 0) {
		return;
	}
	n = stamp(line, OSC2_LINE);
	n += (size_t)snprintf(&line[n], OSC2_LINE - n, "(%u log messages dropped)",
			      (unsigned int)lost);
	append_line(line, MIN(n, (size_t)OSC2_LINE - 1));
}
#endif

void osc2_usblog(const char *fmt, ...)
{
#if defined(CONFIG_OPENSC2_USB_LOG)
	char line[OSC2_LINE + 2];
	va_list ap;
	size_t n;
	int m;

	if (k_is_in_isr()) {
		return;
	}
	drain_setups();
#if defined(CONFIG_OPENSC2_USB_LOG_ZEPHYR)
	write_zlost(line);
#endif
	n = stamp(line, OSC2_LINE);
	va_start(ap, fmt);
	m = vsnprintf(&line[n], OSC2_LINE - n, fmt, ap);
	va_end(ap);
	if (m > 0) {
		n = MIN(n + (size_t)m, (size_t)OSC2_LINE - 1);
	}
	append_line(line, n);
#else
	ARG_UNUSED(fmt);
#endif
}

void osc2_usblog_set_time_base(void)
{
	time_base = k_uptime_ticks();
}

/* ---- the session summary ------------------------------------------------------------------------ */

void osc2_usblog_start_session(void)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	memset(&RET->session, 0, sizeof(RET->session));
	RET->session.boot = RET->boots;
	osc2_ret_seal(RET);
	k_spin_unlock(&ring_lock, key);
}

void osc2_usblog_end_session(const char *why)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	osc2_ret_set_end(RET, why ? why : "?");
	k_spin_unlock(&ring_lock, key);
}

void osc2_usblog_counts(uint32_t requests, uint32_t stalls, uint32_t outputs, uint32_t reports,
			uint32_t dropped)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	RET->session.requests = requests;
	RET->session.stalls = stalls;
	RET->session.outputs = outputs;
	RET->session.reports = reports;
	RET->session.dropped = dropped;
	osc2_ret_seal(RET);
	k_spin_unlock(&ring_lock, key);
}

/* ---- reading, for the shell --------------------------------------------------------------------- */

/* The shell shows the whole reason (Steam's "host: reboot into wireless" has 26 characters). */
BUILD_ASSERT(SIZEOF_FIELD(struct osc2_usblog_info, end) == SIZEOF_FIELD(struct osc2_session, end),
	     "osc2_usblog_info.end must hold what the retained session keeps");

void osc2_usblog_info(struct osc2_usblog_info *info)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	info->kept = kept;
	info->boots = RET->boots;
	info->size = RET->size;
	info->used = RET->used;
	info->session_boot = RET->session.boot;
	info->requests = RET->session.requests;
	info->stalls = RET->session.stalls;
	info->outputs = RET->session.outputs;
	info->reports = RET->session.reports;
	info->dropped = RET->session.dropped;
	memcpy(info->end, RET->session.end, sizeof(info->end));
	info->end[sizeof(info->end) - 1] = '\0';
	k_spin_unlock(&ring_lock, key);
}

void osc2_usblog_lines(void (*cb)(const char *line, size_t len, void *ctx), void *ctx)
{
	static char line[OSC2_LINE + 8];

	drain_setups();
	osc2_ret_lines(RET, line, sizeof(line), cb, ctx);
}

void osc2_usblog_clear(void)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	osc2_ret_clear(RET);
	k_spin_unlock(&ring_lock, key);
}

/* ---- the Zephyr log into the session log, while wired ------------------------------------------ */

#if defined(CONFIG_OPENSC2_USB_LOG_ZEPHYR)
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_output.h>

/* This backend runs in the log thread. Its own frames are kept small (static buffers, no session
 * log calls from there), but formatting a message nests deeply in the log core and the C library:
 * the default 768 bytes are not enough. */
#if defined(CONFIG_LOG_PROCESS_THREAD)
BUILD_ASSERT(CONFIG_LOG_PROCESS_THREAD_STACK_SIZE >= 1536,
	     "Set CONFIG_LOG_PROCESS_THREAD_STACK_SIZE=1536: while wired the log thread also writes "
	     "the Zephyr log into the USB session log (or set CONFIG_OPENSC2_USB_LOG_ZEPHYR=n)");
#endif

static char zline[OSC2_LINE];
static size_t zlen;

/* The session log is meant to be shareable: the board's serials never go into it. */
static void mask(char *text, size_t len, const char *secret)
{
	size_t n = strlen(secret);

	for (size_t i = 0; n && i + n <= len; i++) {
		if (memcmp(&text[i], secret, n) == 0) {
			memset(&text[i], '*', n);
			i += n - 1;
		}
	}
}

/* Static, as the identity: only the log thread commits lines. */
static void zcommit(void)
{
	static char line[OSC2_LINE + 2];
	static struct opensc2_identity id;
	size_t n = stamp(line, OSC2_LINE);
	size_t take = MIN(zlen, (size_t)OSC2_LINE - 1 - n);

	opensc2_identity_get(&id);
	mask(zline, zlen, id.unit_serial);
	mask(zline, zlen, id.board_serial);
	memcpy(&line[n], zline, take);
	append_line(line, n + take);
	zlen = 0;
}

static int zout(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	for (size_t i = 0; i < length; i++) {
		char c = (char)data[i];

		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			zcommit();
			continue;
		}
		if (zlen < sizeof(zline)) {
			zline[zlen++] = c;
		}
	}
	return (int)length;
}

static uint8_t zbuf[64];
LOG_OUTPUT_DEFINE(osc2_log_output, zout, zbuf, sizeof(zbuf));

static void zprocess(const struct log_backend *const backend, union log_msg_generic *msg)
{
	ARG_UNUSED(backend);
	log_output_msg_process(&osc2_log_output, &msg->log,
			       LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_CRLF_LFONLY);
}

static void zpanic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
}

static void zdropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);
	(void)atomic_add(&zlost, (atomic_val_t)cnt);   /* written out by the next osc2_usblog() */
}

static const struct log_backend_api zapi = {
	.process = zprocess,
	.panic = zpanic,
	.dropped = zdropped,
};
LOG_BACKEND_DEFINE(osc2_usblog_zephyr, zapi, false);

void osc2_usblog_backend(bool on)
{
	if (on) {
		log_backend_enable(&osc2_usblog_zephyr, NULL, LOG_LEVEL_INF);
	} else {
		log_backend_disable(&osc2_usblog_zephyr);
	}
}
#else
void osc2_usblog_backend(bool on)
{
	ARG_UNUSED(on);
}
#endif
