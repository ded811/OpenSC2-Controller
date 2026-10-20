/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_core.c: the logic of the hardware test hooks (test_core.h). Pure C: no kernel, no
 * allocation, nothing that waits. Every library action goes through the environment table, and
 * the core's own state is only touched with the environment's lock held, which is never held
 * across a call into the library.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "retained.h"   /* osc2_crc32() */
#include "test_core.h"

#define MIN_OF(a, b) ((a) < (b) ? (a) : (b))

_Static_assert(OSC2_TEST_RING_SIZE >= 2 * OSC2_TEST_ENTRY_MAX, "the ring must hold two entries");
_Static_assert(OSC2_TEST_SEQ_DEPTH >= 1 && OSC2_TEST_SEQ_DEPTH <= 32,
	       "a sequence has 1 to 32 steps (one bit each in `loaded`)");
_Static_assert(9 + OSC2_TEST_READ_MAX <= OSC2_TEST_RESULT_MAX,
	       "a RING_READ answer must fit in one test answer");

const char *const osc2_test_counter_names[OSC2_TEST_CNT_COUNT] = {
	"frames", "usb_sent", "usb_dropped", "radio_sent", "radio_dropped", "radio_packets",
	"radio_keepalives", "battery_sent", "battery_dropped", "lizard_made", "lizard_diverted",
	"app_reports_sent", "out_reports", "out_short", "vendor_set_ok", "vendor_set_err",
	"vendor_get_ok", "vendor_get_err", "usb_frame_err", "ring_entries", "ring_evicted",
	"radio_rx_overflow", "radio_answers_dropped", "radio_extra_dropped", "test_requests",
	"seq_steps", "usb_skipped",
};

/* ---- the ring -------------------------------------------------------------------------------- */

static uint32_t ring_len_at(const struct osc2_test_ring *r, uint32_t off)
{
	return OSC2_TEST_ENTRY_HEADER + r->buf[(off + 6) % OSC2_TEST_RING_SIZE];
}

static void ring_reset(struct osc2_test_ring *r, uint32_t first)
{
	r->head = 0;
	r->used = 0;
	r->first = first;
	r->next = first;
	r->cache_ok = false;
}

/* Append a whole entry, dropping the oldest ones until it fits. Returns how many were dropped. */
static uint32_t ring_put(struct osc2_test_ring *r, const uint8_t *entry, uint32_t len)
{
	uint32_t evicted = 0;
	uint32_t at;

	while (OSC2_TEST_RING_SIZE - r->used < len) {
		uint32_t n = ring_len_at(r, r->head);

		r->head = (r->head + n) % OSC2_TEST_RING_SIZE;
		r->used -= n;
		r->first++;
		evicted++;
	}
	at = (r->head + r->used) % OSC2_TEST_RING_SIZE;
	for (uint32_t i = 0; i < len; i++) {
		r->buf[(at + i) % OSC2_TEST_RING_SIZE] = entry[i];
	}
	r->used += len;
	r->next++;
	return evicted;
}

/* Whole entries from max(from, first), at most `max` of them (0: no limit) and `room` bytes. */
static uint8_t ring_read(struct osc2_test_ring *r, uint32_t from, uint8_t max, uint8_t *out,
			 uint32_t room, uint32_t *bytes)
{
	uint32_t seq, off, n = 0;
	uint8_t count = 0;

	if (from < r->first) {
		from = r->first;
	}
	/* Offsets of entries in the ring never change, so a read can go on where the last ended. */
	if (r->cache_ok && r->cache_seq >= r->first && r->cache_seq <= from) {
		seq = r->cache_seq;
		off = r->cache_off;
	} else {
		seq = r->first;
		off = r->head;
	}
	while (seq < from) {
		off = (off + ring_len_at(r, off)) % OSC2_TEST_RING_SIZE;
		seq++;
	}
	while (seq < r->next && (max == 0 || count < max)) {
		uint32_t len = ring_len_at(r, off);

		if (n + len > room) {
			break;
		}
		for (uint32_t i = 0; i < len; i++) {
			out[n + i] = r->buf[(off + i) % OSC2_TEST_RING_SIZE];
		}
		n += len;
		count++;
		off = (off + len) % OSC2_TEST_RING_SIZE;
		seq++;
	}
	r->cache_seq = seq;
	r->cache_off = off;
	r->cache_ok = true;
	*bytes = n;
	return count;
}

/* ---- entries --------------------------------------------------------------------------------- */

uint8_t osc2_test_entry(uint8_t *out, uint32_t time_us, uint8_t kind, uint8_t link,
			const uint8_t *data, uint8_t len)
{
	if (len > OSC2_TEST_ENTRY_DATA_MAX) {
		len = OSC2_TEST_ENTRY_DATA_MAX;
	}
	opensc2_put_le32(&out[0], time_us);
	out[4] = kind;
	out[5] = link;
	out[6] = len;
	if (len) {
		memcpy(&out[OSC2_TEST_ENTRY_HEADER], data, len);
	}
	return (uint8_t)(OSC2_TEST_ENTRY_HEADER + len);
}

/* With the lock held and armed. */
static void note_locked(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t kind,
			uint8_t link, const uint8_t *data, uint8_t len)
{
	uint8_t e[OSC2_TEST_ENTRY_MAX];
	uint8_t n = osc2_test_entry(e, env->now_us() - c->arm_us, kind, link, data, len);
	uint32_t evicted = ring_put(&c->ring, e, n);

	env->cnt_add(OSC2_TEST_CNT_RING_ENTRIES, 1);
	if (evicted) {
		env->cnt_add(OSC2_TEST_CNT_RING_EVICTED, evicted);
	}
}

void osc2_test_core_note(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t kind,
			 uint8_t link, const uint8_t *data, uint8_t len)
{
	if (!c->armed) {
		return;
	}
	if (link == OSC2_TEST_LINK_CTX) {
		link = env->ctx_link();
	}
	env->lock();
	if (c->armed) {
		note_locked(c, env, kind, link, data, len);
	}
	env->unlock();
}

/* ---- pure helpers ---------------------------------------------------------------------------- */

bool osc2_test_is_request(const uint8_t *payload, uint8_t n)
{
	return n >= OSC2_TEST_HEADER_LEN && n <= OSC2_TEST_REQUEST_MAX &&
	       opensc2_get_le32(payload) == OSC2_TEST_KEY && payload[4] == OSC2_TEST_VERSION;
}

bool osc2_test_claims(const uint8_t *payload, uint8_t n, bool armed)
{
	return osc2_test_is_request(payload, n) && (armed || payload[6] == OSC2_TEST_OP_ARM);
}

void osc2_test_battery_args(const uint8_t args[OSC2_TEST_BATTERY_ARGS], struct opensc2_battery *b,
			    uint8_t *flags)
{
	memset(b, 0, sizeof(*b));
	b->charge_state = args[0];
	b->level = args[1];
	b->battery_mv = opensc2_get_le16(&args[2]);
	b->system_mv = opensc2_get_le16(&args[4]);
	b->input_mv = opensc2_get_le16(&args[6]);
	b->current_ma = (int16_t)opensc2_get_le16(&args[8]);
	b->input_current_ma = opensc2_get_le16(&args[10]);
	b->temperature = opensc2_get_le16(&args[12]);
	b->gauge_flags = args[14];
	*flags = args[15];
}

void osc2_test_pack_caps(uint8_t out[OSC2_TEST_CAPS_LEN], const struct osc2_test_lib_caps *lib,
			 uint16_t lease_s)
{
	size_t name_len = 0;

	memset(out, 0, OSC2_TEST_CAPS_LEN);
	out[0] = OSC2_TEST_VERSION;
	out[1] = lib->mode;
	opensc2_put_le32(&out[2], lib->build_flags);
	opensc2_put_le16(&out[6], lib->battery_interval_ms);
	opensc2_put_le16(&out[8], lib->rumble_timeout_ms);
	opensc2_put_le16(&out[10], (uint16_t)OSC2_TEST_RING_SIZE);
	out[12] = (uint8_t)OSC2_TEST_SEQ_DEPTH;
	out[13] = (uint8_t)OSC2_TEST_CNT_COUNT;
	opensc2_put_le16(&out[14], lib->report_delay_us);
	opensc2_put_le16(&out[16], lease_s);
	out[18] = lib->policy;
	for (int i = 0; i < OPENSC2_GIT_REVISION_MAX && lib->git_revision[i] != '\0'; i++) {
		out[20 + i] = (uint8_t)lib->git_revision[i];
	}
	/* The product name does not fit: its CRC-32, which the PC checks the USB product string
	 * against. */
	while (name_len < OPENSC2_PRODUCT_NAME_MAX && lib->product_name[name_len] != '\0') {
		name_len++;
	}
	opensc2_put_le32(&out[32], osc2_crc32(0, lib->product_name, name_len));
}

bool osc2_test_host_set_allowed(uint8_t opcode, const uint8_t *payload, uint8_t n)
{
	switch (opcode) {
	case 0x81: case 0x83: case 0x85: case 0x87: case 0x89: case 0x8E: case 0x9F: case 0xA1:
	case 0xAE: case 0xBE: case 0xC5: case 0xDB: case 0xE9: case 0xED: case 0xF2:
		return true;
	case 0x95: {
		/* Only the turn-off key, read as the command channel reads it: the first four payload
		 * bytes of the record, zero padded. */
		uint8_t key[4] = { 0 };

		memcpy(key, payload, MIN_OF(n, (uint8_t)4));
		return opensc2_get_le32(key) == OSC2_TEST_TURN_OFF_KEY;
	}
	default:
		/* The bootloader, the factory reset, calibration, every flash or identity write, the
		 * test channel itself and everything the library does not answer itself. */
		return false;
	}
}

static const char *const key_names[] = {
	"esb/bond", "esb/bond_2", "user/wireless_transport", "user/haptic_boot_level",
	"opensc2/unit_serial", "opensc2/board_serial", "opensc2/git_revision",
	"opensc2/board_revision",
};

uint8_t osc2_test_key_id(const uint8_t *payload, size_t len, uint8_t *value_len)
{
	const uint8_t *nul = len ? memchr(payload, 0, len) : NULL;
	size_t name;

	*value_len = 0;
	if (!nul) {
		return 0;
	}
	name = (size_t)(nul - payload);
	*value_len = (uint8_t)(len - name - 1);
	for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
		if (strlen(key_names[i]) == name && memcmp(payload, key_names[i], name) == 0) {
			return (uint8_t)(i + 1);
		}
	}
	return 0;
}

bool osc2_test_host_get_masked(const uint8_t *stored, uint8_t stored_len, uint8_t n)
{
	const uint8_t *p = &stored[2];
	uint8_t len, sel, value_len;

	if (n == 0) {
		return false;
	}
	if (stored_len < 2) {
		return true;   /* nothing known about the request: mask */
	}
	len = MIN_OF(stored[1], (uint8_t)(stored_len - 2));
	sel = len ? p[0] : 0;   /* as the GET reads its selector */
	switch (stored[0]) {
	case 0xED: {
		uint8_t id = osc2_test_key_id(p, len, &value_len);

		return id == 1 || id == 2;   /* the bonds */
	}
	case 0xAE:
		return sel == 0 || sel == 1 || sel == 4;   /* the serials */
	case 0xF2:
		return sel == 0;   /* holds the unit serial */
	case 0xA1:
		return sel == 1;   /* the chip ID */
	default:
		return false;
	}
}

/* ---- entry data ------------------------------------------------------------------------------ */

uint8_t osc2_test_enc_rumble(uint8_t out[10], const struct opensc2_rumble *r)
{
	opensc2_put_le16(&out[0], r->left_speed);
	opensc2_put_le16(&out[2], r->right_speed);
	opensc2_put_le16(&out[4], r->intensity);
	out[6] = (uint8_t)r->left_gain_db;
	out[7] = (uint8_t)r->right_gain_db;
	out[8] = r->type;
	out[9] = r->timeout ? 1 : 0;
	return 10;
}

uint8_t osc2_test_enc_haptic(uint8_t out[12], const struct opensc2_haptic *h)
{
	out[0] = (uint8_t)h->kind;
	out[1] = h->report_id;
	out[2] = h->target;
	out[3] = h->channels;
	switch (h->kind) {
	case OPENSC2_HAPTIC_PULSE:
		opensc2_put_le16(&out[4], h->pulse.on_us);
		opensc2_put_le16(&out[6], h->pulse.off_us);
		opensc2_put_le16(&out[8], h->pulse.count);
		return 10;
	case OPENSC2_HAPTIC_COMMAND:
		out[4] = h->command.command;
		out[5] = (uint8_t)h->command.gain_db;
		return 6;
	case OPENSC2_HAPTIC_TONE:
		out[4] = (uint8_t)h->tone.gain_db;
		opensc2_put_le16(&out[5], h->tone.freq_hz);
		opensc2_put_le16(&out[7], h->tone.duration_ms);
		opensc2_put_le16(&out[9], h->tone.lfo_hz);
		out[11] = h->tone.lfo_depth;
		return 12;
	case OPENSC2_HAPTIC_SWEEP:
		out[4] = (uint8_t)h->sweep.gain_db;
		opensc2_put_le16(&out[5], h->sweep.duration_ms);
		opensc2_put_le16(&out[7], h->sweep.start_hz);
		opensc2_put_le16(&out[9], h->sweep.end_hz);
		out[11] = h->sweep.antiphase ? 1 : 0;
		return 12;
	case OPENSC2_HAPTIC_SCRIPT:
		out[4] = h->script.id;
		out[5] = (uint8_t)h->script.gain_db;
		return 6;
	case OPENSC2_HAPTIC_PCM_CONTROL:
		out[4] = h->pcm_control.operation;
		out[5] = h->pcm_control.format;
		out[6] = h->pcm_control.encoding;
		opensc2_put_le16(&out[7], h->pcm_control.rate_hz);
		return 9;
	case OPENSC2_HAPTIC_PCM_DATA:
		out[4] = h->pcm.len;
		opensc2_put_le32(&out[5], osc2_crc32(0, h->pcm.data, h->pcm.data ? h->pcm.len : 0));
		return 9;
	default:
		return 4;
	}
}

uint8_t osc2_test_enc_led(uint8_t out[6], const struct opensc2_led *led)
{
	out[0] = led->red;
	out[1] = led->green;
	out[2] = led->blue;
	out[3] = led->white;
	out[4] = led->override ? 1 : 0;
	out[5] = led->brightness;
	return 6;
}

uint8_t osc2_test_enc_setting(uint8_t out[3], uint8_t number, int16_t value)
{
	out[0] = number;
	opensc2_put_le16(&out[1], (uint16_t)value);
	return 3;
}

uint8_t osc2_test_enc_output(uint8_t out[19], const uint8_t *report, size_t len, int decoded)
{
	size_t k = len > 1 ? MIN_OF(len - 1, (size_t)16) : 0;

	out[0] = len ? report[0] : 0;
	out[1] = (uint8_t)MIN_OF(len, (size_t)255);
	out[2] = (uint8_t)(int8_t)decoded;
	if (k) {
		memcpy(&out[3], &report[1], k);
	}
	return (uint8_t)(3 + k);
}

uint8_t osc2_test_enc_power(uint8_t out[1 + OSC2_TEST_POWER_TEXT_MAX], uint8_t request,
			    const char *why)
{
	size_t n = 0;

	out[0] = request;
	while (why && n < OSC2_TEST_POWER_TEXT_MAX && why[n] != '\0') {
		out[1 + n] = (uint8_t)why[n];
		n++;
	}
	return (uint8_t)(1 + n);
}

/* `size` bytes from the first `have` of `p`, zero padded: what the command reads. */
static void fixed_bytes(uint8_t *out, const uint8_t *p, size_t have, size_t size)
{
	memset(out, 0, size);
	if (have) {
		memcpy(out, p, MIN_OF(have, size));
	}
}

uint8_t osc2_test_enc_vendor(uint8_t out[19], bool get, uint8_t opcode, uint8_t n, int32_t result,
			     const uint8_t *payload, size_t len, size_t avail)
{
	uint8_t *d = &out[7];
	uint8_t k = 0, value_len;

	len = MIN_OF(len, avail);   /* never more than are there */

	out[0] = get ? 1 : 0;
	out[1] = opcode;
	out[2] = n;
	opensc2_put_le32(&out[3], (uint32_t)result);
	switch (opcode) {
	case 0x87:   /* whole triplets, at most four */
		k = (uint8_t)(MIN_OF(len, (size_t)12) / 3 * 3);
		if (k) {
			memcpy(d, payload, k);
		}
		break;
	case 0x89: case 0xAE: case 0xF2: case 0xA1:   /* the selector or tag */
		if (len >= 1) {
			d[0] = payload[0];
			k = 1;
		}
		break;
	case 0xC5:
		fixed_bytes(d, payload, len, 4);
		k = 4;
		break;
	case 0xDC: case 0xE2:
		fixed_bytes(d, payload, len, 2);
		k = 2;
		break;
	case 0x86: case 0x95:   /* the key, read from the bytes there whatever n says */
		fixed_bytes(d, payload, avail, 4);
		k = 4;
		break;
	case 0xED: case 0xEE: case 0xEF: case 0xF0:   /* the key's name, never its value */
		d[0] = osc2_test_key_id(payload, len, &value_len);
		d[1] = value_len;
		k = 2;
		break;
	default:
		break;
	}
	return (uint8_t)(7 + k);
}

uint8_t osc2_test_enc_identity(uint8_t out[26], const struct opensc2_identity *id)
{
	opensc2_put_le16(&out[0], id->product_id);
	opensc2_put_le32(&out[2], id->build_time);
	opensc2_put_le32(&out[6], id->bootloader_build_time);
	opensc2_put_le32(&out[10], id->board_revision);
	memset(&out[14], 0, OPENSC2_GIT_REVISION_MAX);
	for (int i = 0; i < OPENSC2_GIT_REVISION_MAX && id->git_revision[i] != '\0'; i++) {
		out[14 + i] = (uint8_t)id->git_revision[i];
	}
	return 26;
}

/* ---- state ------------------------------------------------------------------------------------ */

void osc2_test_core_init(struct osc2_test_core *c)
{
	memset(c, 0, sizeof(*c));
	ring_reset(&c->ring, 0);
}

static void refresh_locked(struct osc2_test_core *c, const struct osc2_test_env *env)
{
	if (c->armed) {
		c->lease_ms = env->now_ms() + (uint32_t)c->lease_s * 1000u;
	}
}

/* Injection, the battery and the sequence stop (ARM and the end of arming). */
static void stop_all_locked(struct osc2_test_core *c)
{
	c->static_on = false;
	c->static_lizard = false;
	c->battery_on = false;
	c->seq_on = false;
	c->seq_gen++;
	c->loaded = 0;
	c->frame_seq = false;
	c->skip_lizard = 0;
}

static void seq_end_locked(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t how)
{
	uint8_t d[5];

	c->seq_on = false;
	c->seq_gen++;
	opensc2_put_le32(d, c->frame_index);
	d[4] = how;
	note_locked(c, env, OSC2_TEST_KIND_SEQ_END, env->mode_link(), d, sizeof(d));
}

static uint8_t popcount32(uint32_t v)
{
	uint8_t n = 0;

	for (; v; v &= v - 1) {
		n++;
	}
	return n;
}

/* The 32-byte STATUS result; returns the policy (the console prints it). */
static uint8_t make_status(struct osc2_test_core *c, const struct osc2_test_env *env,
			   uint8_t out[OSC2_TEST_STATUS_LEN])
{
	struct osc2_test_lib_status ls;
	uint8_t flags = 0;

	memset(&ls, 0, sizeof(ls));
	env->status(&ls);
	env->lock();
	flags |= c->armed ? OSC2_TEST_ST_ARMED : 0;
	flags |= c->guard ? OSC2_TEST_ST_GUARD : 0;
	flags |= c->static_on ? OSC2_TEST_ST_INJECT : 0;
	flags |= c->battery_on ? OSC2_TEST_ST_BATTERY : 0;
	flags |= c->seq_on ? OSC2_TEST_ST_SEQ : 0;
	flags |= ls.lizard_on ? OSC2_TEST_ST_LIZARD : 0;
	flags |= c->completing ? OSC2_TEST_ST_COMPLETING : 0;
	flags |= c->snap_on ? OSC2_TEST_ST_SNAPSHOT : 0;
	out[0] = flags;
	out[1] = ls.mode;
	out[2] = ls.mode_reason;
	out[3] = ls.link_state;
	out[4] = ls.usb_state;
	out[5] = ls.slot;
	out[6] = ls.last_reason;
	out[7] = c->last_seq;
	opensc2_put_le32(&out[8], c->nonce);
	opensc2_put_le32(&out[12], ls.uptime_ms);
	opensc2_put_le32(&out[16], c->ring.first);
	opensc2_put_le32(&out[20], c->ring.next);
	opensc2_put_le32(&out[24], c->frame_index);
	out[28] = c->seq_on ? c->seq_step : 0xFF;
	out[29] = popcount32(c->loaded);
	opensc2_put_le16(&out[30], c->seq_on ? c->seq_left : 0);
	env->unlock();
	return ls.policy;
}

/* The end of arming (DISARM, the lease): everything stops at once, the held answers are dropped
 * (a DISARM stores its own afterwards), the snapshot is written back with the guard still on, and
 * the guard stays on OSC2_TEST_COMPLETION_MS more. Called with req_lock held. */
static void end_arming(struct osc2_test_core *c, const struct osc2_test_env *env)
{
	const int16_t *snap = c->snap;
	uint8_t now_led[4] = { 0 };
	bool restore;

	env->lock();
	if (!c->armed) {
		env->unlock();
		return;
	}
	c->armed = 0;
	stop_all_locked(c);
	c->deaf_ms = 0;
	for (int l = 0; l < OSC2_TEST_LINKS; l++) {
		c->held[l].valid = false;
		c->just_claimed[l] = 0;
	}
	restore = c->snap_on;
	c->snap_on = false;
	ring_reset(&c->ring, 0);
	c->completing = true;
	c->completion_ms = env->now_ms() + OSC2_TEST_COMPLETION_MS;
	env->unlock();
	env->cnt_reset();

	/* Only requests use the snapshot, one at a time (req_lock): read in place, no copy on the
	 * stack of whichever thread ends arming. */
	if (restore) {
		/* All but lizard mode (9) and the Steam watchdog (71), then 71, then 9. */
		for (int n = 0; n < OPENSC2_SETTINGS_COUNT; n++) {
			if (n == OPENSC2_SETTING_LIZARD_MODE || n == OPENSC2_SETTING_STEAM_WATCHDOG) {
				continue;
			}
			if (env->setting_get((uint8_t)n) != snap[n]) {
				env->setting_write((uint8_t)n, snap[n]);
			}
		}
		if (env->setting_get(OPENSC2_SETTING_STEAM_WATCHDOG) !=
		    snap[OPENSC2_SETTING_STEAM_WATCHDOG]) {
			env->setting_write(OPENSC2_SETTING_STEAM_WATCHDOG,
					   snap[OPENSC2_SETTING_STEAM_WATCHDOG]);
		}
		if (env->setting_get(OPENSC2_SETTING_LIZARD_MODE) != snap[OPENSC2_SETTING_LIZARD_MODE]) {
			env->setting_write(OPENSC2_SETTING_LIZARD_MODE, snap[OPENSC2_SETTING_LIZARD_MODE]);
		}
		env->led_get(now_led);
		if (memcmp(now_led, c->snap_led, sizeof(now_led)) != 0) {
			env->led_set(c->snap_led);
		}
	}
	/* The 50 ms count from now: whatever lizard mode makes from the application's state is
	 * held back until then. */
	env->lock();
	if (c->completing) {
		c->completion_ms = env->now_ms() + OSC2_TEST_COMPLETION_MS;
	}
	env->unlock();
}

/* ---- ops -------------------------------------------------------------------------------------- */

struct op_req {
	struct osc2_test_core *c;
	const struct osc2_test_env *env;
	uint8_t link;            /* the transport: radio, USB, or test (the console) */
	const uint8_t *args;
	uint8_t alen;
	uint8_t *result;
	uint8_t rlen;
};

static uint8_t op_arm(struct op_req *r)
{
	struct osc2_test_core *c = r->c;
	const struct osc2_test_env *env = r->env;
	uint32_t nonce = opensc2_get_le32(&r->args[0]);
	uint16_t lease = opensc2_get_le16(&r->args[4]);
	uint8_t flags = r->args[6];
	struct osc2_test_lib_caps lib;
	uint8_t d[4];
	bool fresh, want;

	if (lease > OSC2_TEST_LEASE_MAX_S || (flags & ~OSC2_TEST_ARM_RESTORE) != 0) {
		return OSC2_TEST_BAD_ARG;
	}
	if (lease == 0) {
		lease = OSC2_TEST_LEASE_DEFAULT_S;
	}
	env->lock();
	/* Not armed and the guard off: lizard mode may hold a key or button towards the host. */
	fresh = !c->armed && !c->guard;
	want = (flags & OSC2_TEST_ARM_RESTORE) && !c->snap_on;
	env->unlock();
	if (fresh) {
		(void)env->release_held();   /* before the guard: these two reports go out */
	}
	if (want) {
		/* Only requests use the snapshot, one at a time (req_lock): written in place. */
		for (int n = 0; n < OPENSC2_SETTINGS_COUNT; n++) {
			c->snap[n] = env->setting_get((uint8_t)n);
		}
		env->led_get(c->snap_led);
	}

	env->lock();
	c->completing = false;
	ring_reset(&c->ring, 0);
	stop_all_locked(c);
	c->nonce = nonce;
	c->lease_s = lease;
	c->arm_us = env->now_us();
	c->frame_index = 0;
	if (want) {
		c->snap_on = true;
	}
	c->guard = 1;
	c->armed = 1;
	refresh_locked(c, env);
	env->cnt_reset();
	opensc2_put_le32(d, nonce);
	note_locked(c, env, OSC2_TEST_KIND_ARMED, r->link, d, sizeof(d));
	env->unlock();
	/* Lizard mode starts from nothing held (its entry is sequence 1). */
	env->lizard_resync();

	memset(&lib, 0, sizeof(lib));
	env->caps(&lib);
	osc2_test_pack_caps(r->result, &lib, lease);
	r->rlen = OSC2_TEST_CAPS_LEN;
	return OSC2_TEST_OK;
}

static uint8_t op_disarm(struct op_req *r)
{
	end_arming(r->c, r->env);
	return OSC2_TEST_OK;
}

static uint8_t op_status(struct op_req *r)
{
	(void)make_status(r->c, r->env, r->result);
	r->rlen = OSC2_TEST_STATUS_LEN;
	return OSC2_TEST_OK;
}

static uint8_t op_inject(struct op_req *r)
{
	struct osc2_test_core *c = r->c;
	uint8_t flags = r->args[0];
	uint32_t f;

	if (!(flags & OSC2_TEST_INJECT_LIZARD) && r->env->lizard_on()) {
		return OSC2_TEST_DENIED;
	}
	r->env->lock();
	memcpy(c->static_state, &r->args[1], OSC2_TEST_STATE52_LEN);
	c->static_on = true;
	c->static_lizard = (flags & OSC2_TEST_INJECT_LIZARD) != 0;
	if ((flags & OSC2_TEST_INJECT_STOP_SEQ) && c->seq_on) {
		seq_end_locked(c, r->env, OSC2_TEST_SEQ_END_STOP);
	}
	f = c->frame_index;
	r->env->unlock();
	opensc2_put_le32(r->result, f);
	r->rlen = 4;
	return OSC2_TEST_OK;
}

static uint8_t op_inject_off(struct op_req *r)
{
	struct osc2_test_core *c = r->c;
	uint32_t f;

	r->env->lock();
	c->static_on = false;
	if (c->seq_on) {
		seq_end_locked(c, r->env, OSC2_TEST_SEQ_END_STOP);
	}
	f = c->frame_index;
	r->env->unlock();
	opensc2_put_le32(r->result, f);
	r->rlen = 4;
	return OSC2_TEST_OK;
}

static uint8_t op_battery(struct op_req *r)
{
	struct opensc2_battery b;
	uint8_t flags;

	osc2_test_battery_args(r->args, &b, &flags);
	r->env->lock();
	r->c->battery = b;
	r->c->battery_on = true;
	r->env->unlock();
	if (flags & OSC2_TEST_BATTERY_SEND_NOW) {
		r->env->battery_now();
	}
	return OSC2_TEST_OK;
}

static uint8_t op_battery_off(struct op_req *r)
{
	r->env->lock();
	r->c->battery_on = false;
	r->env->unlock();
	return OSC2_TEST_OK;
}

static uint8_t op_seq_load(struct op_req *r)
{
	struct osc2_test_core *c = r->c;
	uint8_t step = r->args[0], hold = r->args[1];

	if (step >= OSC2_TEST_SEQ_DEPTH || hold == 0) {
		return OSC2_TEST_BAD_ARG;
	}
	r->env->lock();
	if (c->seq_on) {
		r->env->unlock();
		return OSC2_TEST_BUSY;
	}
	c->steps[step].hold = hold;
	memcpy(c->steps[step].state, &r->args[2], OSC2_TEST_STATE52_LEN);
	c->loaded |= 1u << step;
	r->env->unlock();
	return OSC2_TEST_OK;
}

static uint8_t op_seq_run(struct op_req *r)
{
	struct osc2_test_core *c = r->c;
	uint8_t count = r->args[0], flags = r->args[1];
	uint32_t mask, f;
	bool loaded, busy;

	if (count == 0 || count > OSC2_TEST_SEQ_DEPTH) {
		return OSC2_TEST_BAD_ARG;
	}
	mask = count >= 32 ? 0xFFFFFFFFu : (1u << count) - 1u;
	r->env->lock();
	loaded = (c->loaded & mask) == mask;
	busy = c->seq_on;
	r->env->unlock();
	if (!loaded) {
		return OSC2_TEST_BAD_ARG;
	}
	if (busy) {
		return OSC2_TEST_BUSY;
	}
	if (!(flags & OSC2_TEST_SEQ_LIZARD) && r->env->lizard_on()) {
		return OSC2_TEST_DENIED;
	}
	r->env->lock();
	c->seq_on = true;
	c->seq_started = false;
	c->seq_count = count;
	c->seq_flags = flags;
	c->seq_step = 0;
	c->seq_left = c->steps[0].hold;
	c->seq_gen++;
	f = c->frame_index;
	r->env->unlock();
	opensc2_put_le32(r->result, f);
	r->rlen = 4;
	return OSC2_TEST_OK;
}

static uint8_t op_seq_stop(struct op_req *r)
{
	r->env->lock();
	if (r->c->seq_on) {
		seq_end_locked(r->c, r->env, OSC2_TEST_SEQ_END_STOP);
	}
	r->env->unlock();
	return OSC2_TEST_OK;
}

static uint8_t op_ring_read(struct op_req *r)
{
	struct osc2_test_core *c = r->c;
	uint32_t from = opensc2_get_le32(&r->args[0]);
	uint8_t max = r->args[4];
	uint32_t bytes = 0;

	r->env->lock();
	if (from > c->ring.next) {
		r->env->unlock();
		return OSC2_TEST_BAD_ARG;
	}
	opensc2_put_le32(&r->result[0], c->ring.first);
	opensc2_put_le32(&r->result[4], c->ring.next);
	r->result[8] = ring_read(&c->ring, from, max, &r->result[9], OSC2_TEST_READ_MAX, &bytes);
	r->env->unlock();
	r->rlen = (uint8_t)(9 + bytes);
	return OSC2_TEST_OK;
}

static uint8_t op_ring_clear(struct op_req *r)
{
	r->env->lock();
	ring_reset(&r->c->ring, r->c->ring.next);   /* sequence numbers go on */
	opensc2_put_le32(r->result, r->c->ring.next);
	r->env->unlock();
	r->rlen = 4;
	return OSC2_TEST_OK;
}

static uint8_t op_cnt_read(struct op_req *r)
{
	uint8_t first = r->args[0], count = r->args[1], n;

	if (count == 0 || count > OSC2_TEST_CNT_READ_MAX) {
		return OSC2_TEST_BAD_ARG;
	}
	n = first >= OSC2_TEST_CNT_COUNT ? 0 :
	    (uint8_t)MIN_OF((unsigned int)count, (unsigned int)(OSC2_TEST_CNT_COUNT - first));
	r->result[0] = first;
	r->result[1] = n;
	for (uint8_t i = 0; i < n; i++) {
		opensc2_put_le32(&r->result[2 + 4 * i], r->env->cnt_get((uint8_t)(first + i)));
	}
	r->rlen = (uint8_t)(2 + 4 * n);
	return OSC2_TEST_OK;
}

static uint8_t op_cnt_reset(struct op_req *r)
{
	r->env->cnt_reset();
	return OSC2_TEST_OK;
}

static uint8_t op_mark(struct op_req *r)
{
	uint32_t seq;

	r->env->lock();
	seq = r->c->ring.next;
	note_locked(r->c, r->env, OSC2_TEST_KIND_MARK, r->link, r->args, 4);
	r->env->unlock();
	opensc2_put_le32(r->result, seq);
	r->rlen = 4;
	return OSC2_TEST_OK;
}

static uint8_t op_settings_read(struct op_req *r)
{
	uint8_t first = r->args[0], count = r->args[1], n;

	if (count == 0 || count > OSC2_TEST_SETTINGS_READ_MAX) {
		return OSC2_TEST_BAD_ARG;
	}
	n = first >= OPENSC2_SETTINGS_COUNT ? 0 :
	    (uint8_t)MIN_OF((unsigned int)count, (unsigned int)(OPENSC2_SETTINGS_COUNT - first));
	r->result[0] = first;
	r->result[1] = n;
	for (uint8_t i = 0; i < n; i++) {
		opensc2_put_le16(&r->result[2 + 2 * i],
				 (uint16_t)r->env->setting_get((uint8_t)(first + i)));
	}
	r->rlen = (uint8_t)(2 + 2 * n);
	return OSC2_TEST_OK;
}

static uint8_t op_host_out(struct op_req *r)
{
	int decoded = r->env->host_out(r->args, r->alen);

	r->result[0] = (uint8_t)(int8_t)decoded;
	r->rlen = 1;
	return OSC2_TEST_OK;
}

static uint8_t op_host_set(struct op_req *r)
{
	uint8_t n = r->args[1];
	int32_t rc;

	if (!osc2_test_host_set_allowed(r->args[0], &r->args[2], n)) {
		return OSC2_TEST_DENIED;
	}
	rc = r->env->host_set(r->args, (uint8_t)(2 + n));
	opensc2_put_le32(r->result, (uint32_t)rc);
	r->rlen = 4;
	return OSC2_TEST_OK;
}

static uint8_t op_host_get(struct op_req *r)
{
	uint8_t out[63] = { 0 }, stored[63] = { 0 };
	uint8_t stored_len = 0, n, k;

	(void)r->env->host_get(out, stored, &stored_len);
	n = MIN_OF(out[1], (uint8_t)OPENSC2_ANSWER_MAX);
	r->result[0] = out[0];
	r->result[1] = n;
	if (osc2_test_host_get_masked(stored, stored_len, n)) {
		r->result[2] = 0x01;   /* masked: the CRC-32 of the answer instead */
		opensc2_put_le32(&r->result[3], osc2_crc32(0, &out[2], n));
		r->rlen = 7;
	} else {
		k = MIN_OF(n, (uint8_t)OSC2_TEST_HOST_GET_MAX);
		r->result[2] = n > OSC2_TEST_HOST_GET_MAX ? 0x02 : 0;   /* cut */
		memcpy(&r->result[3], &out[2], k);
		r->rlen = (uint8_t)(3 + k);
	}
	return OSC2_TEST_OK;
}

static uint8_t op_app_send(struct op_req *r)
{
	int32_t rc;

	if (r->args[0] > 1) {
		return OSC2_TEST_BAD_ARG;
	}
	if (r->args[0] == 0) {
		/* State reports come only from the frame path. */
		if (r->args[1] == OPENSC2_RPT_STATE) {
			return OSC2_TEST_DENIED;
		}
		rc = r->env->app_send(&r->args[1], (uint8_t)(r->alen - 1));
	} else {
		rc = r->env->app_haptic(r->args[1], opensc2_get_le32(&r->args[2]));
	}
	opensc2_put_le32(r->result, (uint32_t)rc);
	r->rlen = 4;
	return OSC2_TEST_OK;
}

static uint8_t op_radio_deaf(struct op_req *r)
{
	uint16_t ms = opensc2_get_le16(r->args);
	bool busy;

	if (ms < OSC2_TEST_DEAF_MIN_MS || ms > OSC2_TEST_DEAF_MAX_MS) {
		return OSC2_TEST_BAD_ARG;
	}
	r->env->lock();
	busy = r->c->deaf_ms != 0;
	r->env->unlock();
	if (busy) {
		return OSC2_TEST_BUSY;
	}
	if (!r->env->radio_connected()) {
		return OSC2_TEST_NO_LINK;
	}
	r->env->lock();
	r->c->deaf_ms = ms;
	r->env->unlock();
	return OSC2_TEST_OK;
}

typedef uint8_t (*op_fn)(struct op_req *r);

static const struct {
	uint8_t op;
	uint8_t min, max;   /* argument bytes */
	bool unarmed;       /* runs while unarmed */
	op_fn fn;
} op_table[] = {
	{ OSC2_TEST_OP_ARM, 7, 7, true, op_arm },
	{ OSC2_TEST_OP_DISARM, 0, 0, false, op_disarm },
	{ OSC2_TEST_OP_STATUS, 0, 0, true, op_status },
	{ OSC2_TEST_OP_INJECT, 1 + OSC2_TEST_STATE52_LEN, 1 + OSC2_TEST_STATE52_LEN, false, op_inject },
	{ OSC2_TEST_OP_INJECT_OFF, 0, 0, false, op_inject_off },
	{ OSC2_TEST_OP_BATTERY, OSC2_TEST_BATTERY_ARGS, OSC2_TEST_BATTERY_ARGS, false, op_battery },
	{ OSC2_TEST_OP_BATTERY_OFF, 0, 0, false, op_battery_off },
	{ OSC2_TEST_OP_SEQ_LOAD, 2 + OSC2_TEST_STATE52_LEN, 2 + OSC2_TEST_STATE52_LEN, false,
	  op_seq_load },
	{ OSC2_TEST_OP_SEQ_RUN, 2, 2, false, op_seq_run },
	{ OSC2_TEST_OP_SEQ_STOP, 0, 0, false, op_seq_stop },
	{ OSC2_TEST_OP_RING_READ, 5, 5, false, op_ring_read },
	{ OSC2_TEST_OP_RING_CLEAR, 0, 0, false, op_ring_clear },
	{ OSC2_TEST_OP_CNT_READ, 2, 2, false, op_cnt_read },
	{ OSC2_TEST_OP_CNT_RESET, 0, 0, false, op_cnt_reset },
	{ OSC2_TEST_OP_MARK, 4, 4, false, op_mark },
	{ OSC2_TEST_OP_SETTINGS_READ, 2, 2, false, op_settings_read },
	{ OSC2_TEST_OP_HOST_OUT, 1, OSC2_TEST_ARGS_MAX, false, op_host_out },
	{ OSC2_TEST_OP_HOST_SET, 2, OSC2_TEST_ARGS_MAX, false, op_host_set },
	{ OSC2_TEST_OP_HOST_GET, 0, 0, false, op_host_get },
	{ OSC2_TEST_OP_APP_SEND, 1, OSC2_TEST_ARGS_MAX, false, op_app_send },
	{ OSC2_TEST_OP_RADIO_DEAF, 2, 2, false, op_radio_deaf },
};

/* Lengths that depend on the arguments themselves. */
static bool length_ok(uint8_t op, const uint8_t *args, uint8_t alen)
{
	switch (op) {
	case OSC2_TEST_OP_HOST_SET:
		return args[1] == alen - 2;   /* opcode, n, then exactly n bytes */
	case OSC2_TEST_OP_APP_SEND:
		if (args[0] == 0) {
			return alen >= 2;      /* a report of at least its ID */
		}
		if (args[0] == 1) {
			return alen == 6;      /* channel u8, flags u32 */
		}
		return true;               /* an unknown kind: BAD_ARG */
	default:
		return true;
	}
}

/* The first check that fails gives the status: op, length, armed (the console), then the op's own
 * checks of values, state and policy. */
static uint8_t dispatch(struct op_req *r, uint8_t op, bool console)
{
	for (size_t i = 0; i < sizeof(op_table) / sizeof(op_table[0]); i++) {
		if (op_table[i].op != op) {
			continue;
		}
		if (r->alen < op_table[i].min || r->alen > op_table[i].max ||
		    !length_ok(op, r->args, r->alen)) {
			return OSC2_TEST_BAD_LEN;
		}
		if (console && !op_table[i].unarmed && !r->c->armed) {
			return OSC2_TEST_NOT_ARMED;
		}
		return op_table[i].fn(r);
	}
	return OSC2_TEST_BAD_OP;
}

/* Run one request (already checked: key, version, length). Called with req_lock held. */
static uint8_t run(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t link,
		   const uint8_t *req, uint8_t n, uint8_t answer[OSC2_TEST_ANSWER_MAX], bool console)
{
	uint8_t result[OSC2_TEST_RESULT_MAX];
	struct op_req r = {
		.c = c,
		.env = env,
		.link = link,
		.args = &req[OSC2_TEST_HEADER_LEN],
		.alen = (uint8_t)(n - OSC2_TEST_HEADER_LEN),
		.result = result,
		.rlen = 0,
	};
	uint8_t status = dispatch(&r, req[6], console);

	if (status != OSC2_TEST_OK) {
		r.rlen = 0;
	}
	answer[0] = OSC2_TEST_VERSION;
	answer[1] = req[5];
	answer[2] = req[6];
	answer[3] = status;
	if (r.rlen) {
		memcpy(&answer[4], result, r.rlen);
	}
	return (uint8_t)(4 + r.rlen);
}

/* ---- the transports -------------------------------------------------------------------------- */

bool osc2_test_core_claim(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t link,
			  const uint8_t *payload, uint8_t n)
{
	uint8_t answer[OSC2_TEST_ANSWER_MAX];
	uint8_t m;

	if (link >= OSC2_TEST_LINKS || !osc2_test_is_request(payload, n)) {
		return false;
	}
	env->req_lock();
	if (!osc2_test_claims(payload, n, c->armed != 0)) {
		env->req_unlock();
		return false;   /* unarmed and not ARM: the library's usual path (a STALL) */
	}
	m = run(c, env, link, payload, n, answer, false);
	env->lock();
	memcpy(c->held[link].data, answer, m);
	c->held[link].len = m;
	c->held[link].valid = true;
	c->just_claimed[link] = 1;
	c->quiet[link] = 1;
	refresh_locked(c, env);
	if (c->armed) {
		env->cnt_add(OSC2_TEST_CNT_TEST_REQUESTS, 1);
	}
	env->unlock();
	env->req_unlock();
	return true;
}

bool osc2_test_core_answer(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t link,
			   uint8_t *out, uint8_t *out_len)
{
	bool held;

	if (link >= OSC2_TEST_LINKS) {
		return false;
	}
	env->lock();
	held = c->held[link].valid;
	if (held) {
		memcpy(out, c->held[link].data, c->held[link].len);
		*out_len = c->held[link].len;
		c->quiet[link] = 1;
	}
	env->unlock();
	return held;
}

void osc2_test_core_stored(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t link)
{
	if (link >= OSC2_TEST_LINKS) {
		return;
	}
	env->lock();
	if (c->just_claimed[link]) {
		c->just_claimed[link] = 0;   /* the claimed SET itself */
	} else {
		c->held[link].valid = false;   /* another SET replaced the stored request */
	}
	env->unlock();
}

bool osc2_test_core_quiet(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t link)
{
	bool quiet;

	if (link >= OSC2_TEST_LINKS) {
		return false;
	}
	env->lock();
	quiet = c->quiet[link] != 0;
	c->quiet[link] = 0;
	env->unlock();
	return quiet;
}

int osc2_test_core_raw(struct osc2_test_core *c, const struct osc2_test_env *env,
		       const uint8_t *req, uint8_t n, uint8_t answer[OSC2_TEST_ANSWER_MAX],
		       uint8_t *answer_len)
{
	if (!osc2_test_is_request(req, n)) {
		return OSC2_TEST_RC_BAD_ARGS;
	}
	env->req_lock();
	*answer_len = run(c, env, OSC2_TEST_LINK_TEST, req, n, answer, true);
	env->lock();
	if (c->armed) {
		env->cnt_add(OSC2_TEST_CNT_TEST_REQUESTS, 1);
	}
	env->unlock();
	env->req_unlock();
	return 0;
}

/* ---- time ------------------------------------------------------------------------------------- */

void osc2_test_core_poll(struct osc2_test_core *c, const struct osc2_test_env *env)
{
	bool expired, done;

	env->req_lock();
	env->lock();
	expired = c->armed && (int32_t)(env->now_ms() - c->lease_ms) >= 0;
	env->unlock();
	if (expired) {
		end_arming(c, env);
	}
	env->lock();
	done = c->completing && !c->armed && (int32_t)(env->now_ms() - c->completion_ms) >= 0;
	env->unlock();
	if (done) {
		/* Nothing of the armed session may produce a report later; then the guard comes off. */
		env->lizard_resync();
		env->lock();
		if (c->completing && !c->armed) {
			c->completing = false;
			c->guard = 0;
		}
		env->unlock();
	}
	env->req_unlock();
}

int32_t osc2_test_core_next(struct osc2_test_core *c, const struct osc2_test_env *env)
{
	int32_t left = 0;
	bool any = false;

	env->lock();
	if (c->armed) {
		left = (int32_t)(c->lease_ms - env->now_ms());
		any = true;
	} else if (c->completing) {
		left = (int32_t)(c->completion_ms - env->now_ms());
		any = true;
	}
	env->unlock();
	if (!any) {
		return -1;
	}
	return left < 0 ? 0 : left;
}

/* ---- frames ----------------------------------------------------------------------------------- */

bool osc2_test_core_input(struct osc2_test_core *c, const struct osc2_test_env *env, bool lizard_on,
			  struct opensc2_state *s)
{
	uint8_t buf[OPENSC2_STATE_REPORT_LEN];
	bool use = false, lizard_test = false;

	if (!c->armed) {
		c->skip_lizard = 0;
		return false;
	}
	env->lock();
	if (!c->armed) {
		c->skip_lizard = 0;
		c->frame_seq = false;
		env->unlock();
		return false;
	}
	if (lizard_on) {
		/* Lizard mode came on (the Steam watchdog, 0x8E, setting 9): an injection without
		 * the lizard test stops in this very frame, which is built from the application's
		 * state again. */
		uint8_t what = 0;

		if (c->seq_on && !(c->seq_flags & OSC2_TEST_SEQ_LIZARD)) {
			seq_end_locked(c, env, OSC2_TEST_SEQ_END_LIZARD);
			what |= 0x02;
		}
		if (c->static_on && !c->static_lizard) {
			c->static_on = false;
			what |= 0x01;
		}
		if (what) {
			uint8_t d[6];

			d[0] = 1;
			opensc2_put_le32(&d[1], c->frame_index);
			d[5] = what;
			note_locked(c, env, OSC2_TEST_KIND_INJECT_STOP, env->mode_link(), d, sizeof(d));
		}
	}
	c->frame_seq = false;
	if (c->seq_on) {
		memcpy(&buf[2], c->steps[c->seq_step].state, OSC2_TEST_STATE52_LEN);
		use = true;
		lizard_test = (c->seq_flags & OSC2_TEST_SEQ_LIZARD) != 0;
		c->frame_seq = true;
		c->frame_step = c->seq_step;
		c->frame_gen = c->seq_gen;
	} else if (c->static_on) {
		memcpy(&buf[2], c->static_state, OSC2_TEST_STATE52_LEN);
		use = true;
		lizard_test = c->static_lizard;
	}
	c->skip_lizard = use && !lizard_test;
	env->unlock();
	if (use) {
		buf[0] = OPENSC2_RPT_STATE;
		buf[1] = 0;
		opensc2_decode_state(buf, s, NULL);
	}
	return use;
}

/* The current step is done: the next, the first again (loop), or the end. */
static void advance_locked(struct osc2_test_core *c, const struct osc2_test_env *env)
{
	uint8_t next = (uint8_t)(c->seq_step + 1);

	if (next < c->seq_count || (c->seq_flags & OSC2_TEST_SEQ_LOOP)) {
		c->seq_step = next < c->seq_count ? next : 0;
		c->seq_left = c->steps[c->seq_step].hold;
		c->seq_started = false;
		return;
	}
	if (c->seq_flags & OSC2_TEST_SEQ_KEEP_LAST) {
		memcpy(c->static_state, c->steps[c->seq_step].state, OSC2_TEST_STATE52_LEN);
		c->static_on = true;
		c->static_lizard = (c->seq_flags & OSC2_TEST_SEQ_LIZARD) != 0;
		seq_end_locked(c, env, OSC2_TEST_SEQ_END_KEPT);
	} else {
		seq_end_locked(c, env, OSC2_TEST_SEQ_END_BACK);
	}
}

void osc2_test_core_frame(struct osc2_test_core *c, const struct osc2_test_env *env,
			  const uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	uint32_t idx;

	if (!c->armed) {
		return;
	}
	env->lock();
	if (!c->armed) {
		env->unlock();
		return;
	}
	idx = c->frame_index++;
	c->last_seq = report[1];
	env->cnt_add(OSC2_TEST_CNT_FRAMES, 1);
	/* Only a frame that was built from the current step counts for it: a request that arrives
	 * while a frame is half built starts with the next one. */
	if (c->seq_on && c->frame_seq && c->frame_gen == c->seq_gen &&
	    c->frame_step == c->seq_step) {
		if (!c->seq_started) {
			uint8_t d[6];

			c->seq_started = true;
			d[0] = c->seq_step;
			opensc2_put_le32(&d[1], idx);
			d[5] = report[1];
			note_locked(c, env, OSC2_TEST_KIND_SEQ_STEP, env->mode_link(), d, sizeof(d));
			env->cnt_add(OSC2_TEST_CNT_SEQ_STEPS, 1);
		}
		if (c->seq_left) {
			c->seq_left--;
		}
		if (c->seq_left == 0) {
			advance_locked(c, env);
		}
	}
	c->frame_seq = false;
	env->unlock();
}

bool osc2_test_core_battery(struct osc2_test_core *c, const struct osc2_test_env *env,
			    struct opensc2_battery *b)
{
	bool on;

	if (!c->armed) {
		return false;
	}
	env->lock();
	on = c->armed && c->battery_on;
	if (on) {
		*b = c->battery;
	}
	env->unlock();
	return on;
}

bool osc2_test_core_divert(struct osc2_test_core *c, const struct osc2_test_env *env,
			   const uint8_t *report, size_t len)
{
	uint8_t d[5 + OSC2_TEST_REPORT_MAX];
	uint8_t link;
	size_t k;

	if (!c->guard || len < 1 ||
	    (report[0] != OPENSC2_RPT_MOUSE && report[0] != OPENSC2_RPT_KEYBOARD)) {
		return false;
	}
	link = env->mode_link();
	env->lock();
	if (!c->guard) {
		env->unlock();
		return false;
	}
	if (c->armed) {
		k = MIN_OF(len, (size_t)OSC2_TEST_REPORT_MAX);
		d[0] = 0x01;   /* diverted */
		opensc2_put_le32(&d[1], c->frame_index ? c->frame_index - 1 : 0xFFFFFFFFu);
		memcpy(&d[5], report, k);
		note_locked(c, env, OSC2_TEST_KIND_LIZARD_RPT, link, d, (uint8_t)(5 + k));
		env->cnt_add(OSC2_TEST_CNT_LIZARD_DIVERTED, 1);
	}
	env->unlock();
	return true;
}

uint16_t osc2_test_core_take_deaf(struct osc2_test_core *c, const struct osc2_test_env *env)
{
	uint16_t ms;

	if (!c->armed) {
		return 0;
	}
	env->lock();
	ms = c->deaf_ms;
	c->deaf_ms = 0;
	env->unlock();
	return ms;
}

/* ---- console text ----------------------------------------------------------------------------- */

void osc2_test_hex(char *out, const uint8_t *data, size_t n)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[data[i] >> 4];
		out[2 * i + 1] = digits[data[i] & 0x0F];
	}
	out[2 * n] = '\0';
}

static int nibble(char ch)
{
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	return -1;
}

int osc2_test_unhex(const char *text, uint8_t *out, size_t max)
{
	size_t len = strlen(text);

	if (len % 2 != 0 || len / 2 > max) {
		return -1;
	}
	for (size_t i = 0; i < len / 2; i++) {
		int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);

		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return (int)(len / 2);
}

/* Append to a text buffer; never runs past its end. */
static void add(char *out, size_t size, size_t *at, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*at + 1 >= size) {
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(&out[*at], size - *at, fmt, ap);
	va_end(ap);
	if (n > 0) {
		*at = MIN_OF(*at + (size_t)n, size - 1);
	}
}

static const char *const link_state_names[] = { "off", "no-bond", "pairing", "listening",
						 "connected" };
static const char *const usb_state_names[] = { "off", "attached", "no-host", "configured" };

void osc2_test_fmt_status(char *out, size_t size, const uint8_t status[OSC2_TEST_STATUS_LEN],
			  uint8_t policy)
{
	uint8_t f = status[0];
	size_t at = 0;

	out[0] = '\0';
	add(out, size, &at, "armed=%u guard=%u inject=%u battery=%u seq=%s",
	    (unsigned int)(f & 1), (unsigned int)((f >> 1) & 1), (unsigned int)((f >> 2) & 1),
	    (unsigned int)((f >> 3) & 1), (f & OSC2_TEST_ST_SEQ) ? "run" : "idle");
	add(out, size, &at, " mode=%s reason=%u link=%s usb=%s slot=%u lizard=%u",
	    status[1] ? "wired" : "wireless", (unsigned int)status[2],
	    status[3] < 5 ? link_state_names[status[3]] : "?",
	    status[4] < 4 ? usb_state_names[status[4]] : "?", (unsigned int)status[5] + 1,
	    (unsigned int)((f >> 5) & 1));
	add(out, size, &at, " frames=%lu ring=%lu..%lu up=%lu nonce=%08lx",
	    (unsigned long)opensc2_get_le32(&status[24]), (unsigned long)opensc2_get_le32(&status[16]),
	    (unsigned long)opensc2_get_le32(&status[20]), (unsigned long)opensc2_get_le32(&status[12]),
	    (unsigned long)opensc2_get_le32(&status[8]));
	add(out, size, &at, " policy=%s snapshot=%u", policy == 1 ? "wireless" : "auto",
	    (unsigned int)((f >> 7) & 1));
}

static const char *kind_name(uint8_t kind)
{
	switch (kind) {
	case OSC2_TEST_KIND_ARMED:       return "ARMED";
	case OSC2_TEST_KIND_MARK:        return "MARK";
	case OSC2_TEST_KIND_SEQ_STEP:    return "SEQ_STEP";
	case OSC2_TEST_KIND_SEQ_END:     return "SEQ_END";
	case OSC2_TEST_KIND_INJECT_STOP: return "INJECT_STOP";
	case OSC2_TEST_KIND_OUTPUT:      return "OUTPUT";
	case OSC2_TEST_KIND_RUMBLE:      return "RUMBLE";
	case OSC2_TEST_KIND_HAPTIC:      return "HAPTIC";
	case OSC2_TEST_KIND_LED:         return "LED";
	case OSC2_TEST_KIND_SETTING:     return "SETTING";
	case OSC2_TEST_KIND_LIZARD:      return "LIZARD";
	case OSC2_TEST_KIND_LIZARD_RPT:  return "LIZARD_RPT";
	case OSC2_TEST_KIND_POWER:       return "POWER";
	case OSC2_TEST_KIND_VENDOR:      return "VENDOR";
	case OSC2_TEST_KIND_HOST_ASLEEP: return "HOST_ASLEEP";
	case OSC2_TEST_KIND_LINK:        return "LINK";
	case OSC2_TEST_KIND_USB:         return "USB";
	case OSC2_TEST_KIND_VERSION:     return "VERSION";
	case OSC2_TEST_KIND_IDENTITY:    return "IDENTITY";
	case OSC2_TEST_KIND_CHANNELS:    return "CHANNELS";
	case OSC2_TEST_KIND_STOP_STATUS: return "STOP_STATUS";
	default:                         return NULL;
	}
}

static void add_hex(char *out, size_t size, size_t *at, const uint8_t *data, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		add(out, size, at, "%02x", (unsigned int)data[i]);
	}
}

/* The fields of one entry's data, as key=value. */
static void add_fields(char *out, size_t size, size_t *at, uint8_t kind, const uint8_t *d,
		       uint8_t n)
{
	static const char *const lizard_what[] = { "leave", "enter", "down", "up", "resync" };

	switch (kind) {
	case OSC2_TEST_KIND_ARMED:
	case OSC2_TEST_KIND_MARK:
		if (n >= 4) {
			add(out, size, at, " %s=%08lx", kind == OSC2_TEST_KIND_ARMED ? "nonce" : "marker",
			    (unsigned long)opensc2_get_le32(d));
			return;
		}
		break;
	case OSC2_TEST_KIND_SEQ_STEP:
		if (n >= 6) {
			add(out, size, at, " step=%u frame=%lu seq=%u", (unsigned int)d[0],
			    (unsigned long)opensc2_get_le32(&d[1]), (unsigned int)d[5]);
			return;
		}
		break;
	case OSC2_TEST_KIND_SEQ_END:
		if (n >= 5) {
			add(out, size, at, " frame=%lu how=%u", (unsigned long)opensc2_get_le32(d),
			    (unsigned int)d[4]);
			return;
		}
		break;
	case OSC2_TEST_KIND_INJECT_STOP:
		if (n >= 6) {
			add(out, size, at, " reason=%u frame=%lu what=%u", (unsigned int)d[0],
			    (unsigned long)opensc2_get_le32(&d[1]), (unsigned int)d[5]);
			return;
		}
		break;
	case OSC2_TEST_KIND_OUTPUT:
		if (n >= 3) {
			add(out, size, at, " id=0x%02x len=%u decoded=%d data=", (unsigned int)d[0],
			    (unsigned int)d[1], (int)(int8_t)d[2]);
			add_hex(out, size, at, &d[3], (size_t)(n - 3));
			return;
		}
		break;
	case OSC2_TEST_KIND_RUMBLE:
		if (n >= 10) {
			add(out, size, at, " left=%u right=%u intensity=%u gains=%d,%d type=%u timeout=%u",
			    (unsigned int)opensc2_get_le16(&d[0]), (unsigned int)opensc2_get_le16(&d[2]),
			    (unsigned int)opensc2_get_le16(&d[4]), (int)(int8_t)d[6], (int)(int8_t)d[7],
			    (unsigned int)d[8], (unsigned int)d[9]);
			return;
		}
		break;
	case OSC2_TEST_KIND_HAPTIC:
		if (n >= 4) {
			add(out, size, at, " kind=%u id=0x%02x target=0x%02x channels=0x%x data=",
			    (unsigned int)d[0], (unsigned int)d[1], (unsigned int)d[2],
			    (unsigned int)d[3]);
			add_hex(out, size, at, &d[4], (size_t)(n - 4));
			return;
		}
		break;
	case OSC2_TEST_KIND_LED:
		if (n >= 6) {
			add(out, size, at, " rgbw=%u,%u,%u,%u override=%u brightness=%u",
			    (unsigned int)d[0], (unsigned int)d[1], (unsigned int)d[2],
			    (unsigned int)d[3], (unsigned int)d[4], (unsigned int)d[5]);
			return;
		}
		break;
	case OSC2_TEST_KIND_SETTING:
		if (n >= 3) {
			if (d[0] == OPENSC2_SETTINGS_ALL) {
				add(out, size, at, " number=all");
			} else {
				add(out, size, at, " number=%u value=%d", (unsigned int)d[0],
				    (int)(int16_t)opensc2_get_le16(&d[1]));
			}
			return;
		}
		break;
	case OSC2_TEST_KIND_LIZARD:
		if (n >= 1) {
			add(out, size, at, " what=%s", d[0] < 5 ? lizard_what[d[0]] : "?");
			return;
		}
		break;
	case OSC2_TEST_KIND_LIZARD_RPT:
		if (n >= 5) {
			uint32_t frame = opensc2_get_le32(&d[1]);

			add(out, size, at, " flags=%u frame=", (unsigned int)d[0]);
			if (frame == 0xFFFFFFFFu) {
				add(out, size, at, "none");
			} else {
				add(out, size, at, "%lu", (unsigned long)frame);
			}
			add(out, size, at, " report=");
			add_hex(out, size, at, &d[5], (size_t)(n - 5));
			return;
		}
		break;
	case OSC2_TEST_KIND_POWER:
		if (n >= 1) {
			add(out, size, at, " request=%u why=\"%.*s\"", (unsigned int)d[0], (int)(n - 1),
			    (const char *)&d[1]);
			return;
		}
		break;
	case OSC2_TEST_KIND_VENDOR:
		if (n >= 7) {
			add(out, size, at, " dir=%s op=0x%02x n=%u rc=%ld data=", d[0] ? "get" : "set",
			    (unsigned int)d[1], (unsigned int)d[2],
			    (long)(int32_t)opensc2_get_le32(&d[3]));
			add_hex(out, size, at, &d[7], (size_t)(n - 7));
			return;
		}
		break;
	case OSC2_TEST_KIND_HOST_ASLEEP:
		return;
	case OSC2_TEST_KIND_LINK:
		if (n >= 5) {
			add(out, size, at, " state=%s reason=%u slot=%u channel=%u protocol=%u",
			    d[0] < 5 ? link_state_names[d[0]] : "?", (unsigned int)d[1],
			    (unsigned int)d[2] + 1, (unsigned int)d[3], (unsigned int)d[4]);
			return;
		}
		break;
	case OSC2_TEST_KIND_USB:
		if (n >= 3) {
			add(out, size, at, " state=%s active=%u suspended=%u",
			    d[0] < 4 ? usb_state_names[d[0]] : "?", (unsigned int)d[1],
			    (unsigned int)d[2]);
			return;
		}
		break;
	case OSC2_TEST_KIND_VERSION:
		if (n >= 1) {
			add(out, size, at, " version=%u", (unsigned int)d[0]);
			return;
		}
		break;
	case OSC2_TEST_KIND_IDENTITY:
		if (n >= 26) {
			add(out, size, at, " pid=0x%04x build=0x%08lx blbuild=0x%08lx rev=0x%lx git=%.12s",
			    (unsigned int)opensc2_get_le16(&d[0]), (unsigned long)opensc2_get_le32(&d[2]),
			    (unsigned long)opensc2_get_le32(&d[6]),
			    (unsigned long)opensc2_get_le32(&d[10]), (const char *)&d[14]);
			return;
		}
		break;
	case OSC2_TEST_KIND_CHANNELS:
		if (n >= 4) {
			add(out, size, at, " channels=%u,%u,%u,%u", (unsigned int)d[0],
			    (unsigned int)d[1], (unsigned int)d[2], (unsigned int)d[3]);
			return;
		}
		break;
	case OSC2_TEST_KIND_STOP_STATUS:
		if (n >= 5) {
			add(out, size, at, " channel=%u flags=0x%lx", (unsigned int)d[0],
			    (unsigned long)opensc2_get_le32(&d[1]));
			return;
		}
		break;
	default:
		break;
	}
	add(out, size, at, " data=");
	add_hex(out, size, at, d, n);
}

void osc2_test_fmt_entry(char *out, size_t size, uint32_t seq, const uint8_t *entry, size_t len)
{
	static const char *const links[] = { "radio", "usb", "test", "none" };
	size_t at = 0;
	uint32_t t;
	uint8_t n;
	const char *name;

	out[0] = '\0';
	if (len < OSC2_TEST_ENTRY_HEADER) {
		return;
	}
	t = opensc2_get_le32(entry);
	n = (uint8_t)MIN_OF((size_t)entry[6], len - OSC2_TEST_ENTRY_HEADER);
	name = kind_name(entry[4]);
	add(out, size, &at, "%lu %lu.%03lu %s ", (unsigned long)seq, (unsigned long)(t / 1000u),
	    (unsigned long)(t % 1000u), entry[5] < 4 ? links[entry[5]] : "?");
	if (name) {
		add(out, size, &at, "%s", name);
	} else {
		add(out, size, &at, "KIND_%02X", (unsigned int)entry[4]);
	}
	add_fields(out, size, &at, entry[4], &entry[OSC2_TEST_ENTRY_HEADER], n);
}

/* ---- the console command ---------------------------------------------------------------------- */

struct console {
	struct osc2_test_core *c;
	const struct osc2_test_env *env;
	osc2_test_emit emit;
	void *ctx;
	char id[8];
};

static void out_line(struct console *k, const char *tag, const char *fmt, ...)
{
	char line[256];
	size_t at = 0;

	add(line, sizeof(line), &at, "HWT %s %s", k->id, tag);
	if (fmt && at + 2 < sizeof(line)) {
		va_list ap;

		line[at++] = ' ';
		line[at] = '\0';
		va_start(ap, fmt);
		(void)vsnprintf(&line[at], sizeof(line) - at, fmt, ap);
		va_end(ap);
	}
	k->emit(k->ctx, line);
}

/* The last line. A command that ends with 0 while armed refreshes the lease. */
static int finish(struct console *k, int rc)
{
	if (rc == 0) {
		k->env->lock();
		refresh_locked(k->c, k->env);
		k->env->unlock();
	}
	out_line(k, "END", "%d", rc);
	return rc;
}

static int fail(struct console *k, int rc, const char *text)
{
	out_line(k, "ERR", "%s", text);
	return finish(k, rc);
}

static const char *status_text(uint8_t status)
{
	static const char *const names[] = { "ok", "bad op", "bad length", "not armed",
					     "bad argument", "busy", "denied", "no link" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "?";
}

/* One op of the console's own verbs, as the console transport. */
static uint8_t console_op(struct console *k, uint8_t op, const uint8_t *args, uint8_t alen,
			  uint8_t *result, uint8_t *rlen)
{
	uint8_t req[OSC2_TEST_REQUEST_MAX], answer[OSC2_TEST_ANSWER_MAX];
	uint8_t m;

	opensc2_put_le32(req, OSC2_TEST_KEY);
	req[4] = OSC2_TEST_VERSION;
	req[5] = 0;
	req[6] = op;
	if (alen) {
		memcpy(&req[OSC2_TEST_HEADER_LEN], args, alen);
	}
	k->env->req_lock();
	m = run(k->c, k->env, OSC2_TEST_LINK_TEST, req, (uint8_t)(OSC2_TEST_HEADER_LEN + alen),
		answer, true);
	k->env->req_unlock();
	*rlen = (uint8_t)(m - 4);
	if (*rlen) {
		memcpy(result, &answer[4], *rlen);
	}
	return answer[3];
}

/* A decimal number of at most `digits` digits. */
static bool parse_decimal(const char *text, unsigned int digits, uint32_t *value)
{
	uint32_t v = 0;
	unsigned int n = 0;

	for (; text[n] != '\0'; n++) {
		if (text[n] < '0' || text[n] > '9' || n >= digits) {
			return false;
		}
		v = v * 10u + (uint32_t)(text[n] - '0');
	}
	*value = v;
	return n > 0;
}

static int verb_raw(struct console *k, int argc, const char *const *argv)
{
	uint8_t req[OSC2_TEST_REQUEST_MAX], answer[OSC2_TEST_ANSWER_MAX];
	char hex[2 * OSC2_TEST_ANSWER_MAX + 1];
	uint8_t m = 0;
	int n;

	if (argc != 1) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "raw <request as hex>");
	}
	n = osc2_test_unhex(argv[0], req, sizeof(req));
	if (n < 0) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "bad hex");
	}
	if (osc2_test_core_raw(k->c, k->env, req, (uint8_t)n, answer, &m) != 0) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "not a test request");
	}
	osc2_test_hex(hex, answer, m);
	out_line(k, "RAW", "%s", hex);
	return finish(k, 0);
}

static int verb_status(struct console *k, int argc)
{
	uint8_t status[OSC2_TEST_STATUS_LEN];
	char text[200];
	uint8_t policy;

	if (argc != 0) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "status takes no arguments");
	}
	policy = make_status(k->c, k->env, status);
	osc2_test_fmt_status(text, sizeof(text), status, policy);
	out_line(k, "STATUS", "%s", text);
	return finish(k, 0);
}

static int verb_arm(struct console *k, int argc, const char *const *argv)
{
	uint8_t key[4], args[7] = { 0 }, result[OSC2_TEST_RESULT_MAX], rlen = 0, status;
	uint32_t lease = 0;

	if (argc < 1 || argc > 2) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "arm <key hex8> [<lease s>]");
	}
	if (strlen(argv[0]) != 8 || osc2_test_unhex(argv[0], key, sizeof(key)) != 4 ||
	    (((uint32_t)key[0] << 24) | ((uint32_t)key[1] << 16) | ((uint32_t)key[2] << 8) |
	     key[3]) != OSC2_TEST_KEY) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "bad key");
	}
	if (argc == 2 && (!parse_decimal(argv[1], 3, &lease) || lease > OSC2_TEST_LEASE_MAX_S)) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "bad lease (0 to 600 s)");
	}
	/* Nonce 0, no restore at the end: a person at a terminal. */
	opensc2_put_le16(&args[4], (uint16_t)lease);
	status = console_op(k, OSC2_TEST_OP_ARM, args, sizeof(args), result, &rlen);
	if (status != OSC2_TEST_OK || rlen < OSC2_TEST_CAPS_LEN) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, status_text(status));
	}
	out_line(k, "ARMED", "nonce=00000000 lease=%u", (unsigned int)opensc2_get_le16(&result[16]));
	return finish(k, 0);
}

/* A verb that needs arming and runs one op without arguments. */
static int verb_simple(struct console *k, uint8_t op, const char *tag)
{
	uint8_t result[OSC2_TEST_RESULT_MAX], rlen = 0, status;

	if (!k->c->armed) {
		return fail(k, OSC2_TEST_RC_NOT_ARMED, "not armed");
	}
	status = console_op(k, op, NULL, 0, result, &rlen);
	if (status != OSC2_TEST_OK) {
		return fail(k, status == OSC2_TEST_NOT_ARMED ? OSC2_TEST_RC_NOT_ARMED :
			    OSC2_TEST_RC_BAD_ARGS, status_text(status));
	}
	out_line(k, tag, NULL);
	return finish(k, 0);
}

static int verb_ring(struct console *k, int argc, const char *const *argv)
{
	uint8_t result[OSC2_TEST_RESULT_MAX], args[5] = { 0 }, rlen = 0, status;
	uint32_t from = 0, first = 0, next = 0;
	char text[200];

	if (argc > 1 || (argc == 1 && !parse_decimal(argv[0], 10, &from))) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "ring [<from>]");
	}
	if (!k->c->armed) {
		return fail(k, OSC2_TEST_RC_NOT_ARMED, "not armed");
	}
	for (;;) {
		size_t at = 9;
		uint8_t count;

		opensc2_put_le32(args, from);
		status = console_op(k, OSC2_TEST_OP_RING_READ, args, sizeof(args), result, &rlen);
		if (status != OSC2_TEST_OK || rlen < 9) {
			return fail(k, status == OSC2_TEST_NOT_ARMED ? OSC2_TEST_RC_NOT_ARMED :
				    OSC2_TEST_RC_BAD_ARGS, status_text(status));
		}
		first = opensc2_get_le32(&result[0]);
		next = opensc2_get_le32(&result[4]);
		count = result[8];
		if (from < first) {
			from = first;
		}
		for (uint8_t i = 0; i < count && at + OSC2_TEST_ENTRY_HEADER <= rlen; i++) {
			size_t len = OSC2_TEST_ENTRY_HEADER + result[at + 6];

			if (at + len > rlen) {
				break;
			}
			osc2_test_fmt_entry(text, sizeof(text), from, &result[at], len);
			out_line(k, "EV", "%s", text);
			at += len;
			from++;
		}
		if (count == 0 || from >= next) {
			break;
		}
	}
	out_line(k, "RING", "first=%lu next=%lu", (unsigned long)first, (unsigned long)next);
	return finish(k, 0);
}

static int verb_cnt(struct console *k, int argc)
{
	uint8_t result[OSC2_TEST_RESULT_MAX], rlen = 0, status;

	if (argc != 0) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "cnt takes no arguments");
	}
	if (!k->c->armed) {
		return fail(k, OSC2_TEST_RC_NOT_ARMED, "not armed");
	}
	for (uint8_t first = 0; first < OSC2_TEST_CNT_COUNT; first += OSC2_TEST_CNT_READ_MAX) {
		uint8_t args[2] = { first, OSC2_TEST_CNT_READ_MAX };

		status = console_op(k, OSC2_TEST_OP_CNT_READ, args, sizeof(args), result, &rlen);
		if (status != OSC2_TEST_OK || rlen < 2) {
			return fail(k, status == OSC2_TEST_NOT_ARMED ? OSC2_TEST_RC_NOT_ARMED :
				    OSC2_TEST_RC_BAD_ARGS, status_text(status));
		}
		for (uint8_t i = 0; i < result[1] && 2 + 4 * i + 4 <= rlen; i++) {
			out_line(k, "CNT", "%s=%lu", osc2_test_counter_names[first + i],
				 (unsigned long)opensc2_get_le32(&result[2 + 4 * i]));
		}
	}
	return finish(k, 0);
}

static int verb_settings(struct console *k, int argc)
{
	if (argc != 0) {
		return fail(k, OSC2_TEST_RC_BAD_ARGS, "settings takes no arguments");
	}
	for (int first = 0; first < OPENSC2_SETTINGS_COUNT; first += 16) {
		char text[200];
		size_t at = 0;

		text[0] = '\0';
		add(text, sizeof(text), &at, "%d", first);
		for (int n = first; n < first + 16 && n < OPENSC2_SETTINGS_COUNT; n++) {
			add(text, sizeof(text), &at, " %d", (int)k->env->setting_get((uint8_t)n));
		}
		out_line(k, "SETTINGS", "%s", text);
	}
	return finish(k, 0);
}

int osc2_test_console(struct osc2_test_core *c, const struct osc2_test_env *env, int argc,
		      const char *const *argv, osc2_test_emit emit, void *ctx)
{
	struct console k = { .c = c, .env = env, .emit = emit, .ctx = ctx };
	const char *verb;
	int at = 0;

	strcpy(k.id, "-");
	if (argc > 0 && argv[0][0] >= '0' && argv[0][0] <= '9') {
		uint32_t id;

		if (!parse_decimal(argv[0], 5, &id) || id < 1 || id > 65535) {
			return fail(&k, OSC2_TEST_RC_BAD_ARGS, "the id is a number from 1 to 65535");
		}
		snprintf(k.id, sizeof(k.id), "%lu", (unsigned long)id);
		at = 1;
	}
	if (at >= argc) {
		return fail(&k, OSC2_TEST_RC_BAD_ARGS,
			    "test [<id>] sync|raw|status|arm|disarm|ring|cnt|settings|inject off|"
			    "battery off|seq stop");
	}
	verb = argv[at];
	argc -= at + 1;
	argv += at + 1;

	if (strcmp(verb, "sync") == 0) {
		return argc == 0 ? finish(&k, 0) :
				   fail(&k, OSC2_TEST_RC_BAD_ARGS, "sync takes no arguments");
	}
	if (strcmp(verb, "raw") == 0) {
		return verb_raw(&k, argc, argv);
	}
	if (strcmp(verb, "status") == 0) {
		return verb_status(&k, argc);
	}
	if (strcmp(verb, "arm") == 0) {
		return verb_arm(&k, argc, argv);
	}
	if (strcmp(verb, "disarm") == 0) {
		if (argc != 0) {
			return fail(&k, OSC2_TEST_RC_BAD_ARGS, "disarm takes no arguments");
		}
		return verb_simple(&k, OSC2_TEST_OP_DISARM, "DISARMED");
	}
	if (strcmp(verb, "ring") == 0) {
		return verb_ring(&k, argc, argv);
	}
	if (strcmp(verb, "cnt") == 0) {
		return verb_cnt(&k, argc);
	}
	if (strcmp(verb, "settings") == 0) {
		return verb_settings(&k, argc);
	}
	if (strcmp(verb, "inject") == 0 || strcmp(verb, "battery") == 0 ||
	    strcmp(verb, "seq") == 0) {
		bool seq = verb[0] == 's';
		const char *word = seq ? "stop" : "off";

		if (argc != 1 || strcmp(argv[0], word) != 0) {
			return fail(&k, OSC2_TEST_RC_BAD_ARGS,
				    seq ? "seq stop" : verb[0] == 'i' ? "inject off" : "battery off");
		}
		return verb_simple(&k, seq ? OSC2_TEST_OP_SEQ_STOP :
				   verb[0] == 'i' ? OSC2_TEST_OP_INJECT_OFF :
				   OSC2_TEST_OP_BATTERY_OFF, "OK");
	}
	return fail(&k, OSC2_TEST_RC_UNKNOWN, "unknown verb");
}
