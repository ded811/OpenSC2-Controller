/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_core.h: the logic of OpenSC2's hardware test hooks (CONFIG_OPENSC2_TEST_HOOKS), as pure C:
 *   - the test channel, vendor command 0x7E guarded by a 32-bit key: the parser, the claiming
 *     rule, the ops and their answers;
 *   - the output capture ring: what the library decoded or called back, entry by entry;
 *   - input injection: a static state and frame-locked sequences, and the rule that keeps lizard
 *     mode off injected inputs;
 *   - the allowlist of the host emulation (HOST_SET) and the masking of its answers (HOST_GET);
 *   - the console's lines ("opensc2 test ...").
 * No kernel. It reaches the rest of the library only through `struct osc2_test_env`, which
 * test_hooks.c implements with library calls and the host tests fake (tests/host).
 *
 * The byte formats (requests, answers, caps, status, ring entries, counters) are in
 * docs/test-protocol.md.
 */
#ifndef OPENSC2_TEST_CORE_H_
#define OPENSC2_TEST_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <opensc2/types.h>
#include <opensc2/codec.h>
#include <opensc2/settings.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the test channel ------------------------------------------------------------------------ */

#define OSC2_TEST_OPCODE           0x7E
#define OSC2_TEST_KEY              0x7E57AB1Eu   /* bytes 1E AB 57 7E */
#define OSC2_TEST_VERSION          1
#define OSC2_TEST_HEADER_LEN       7      /* key u32, version, request ID, op */
#define OSC2_TEST_REQUEST_MAX      61     /* a vendor payload */
#define OSC2_TEST_ARGS_MAX         (OSC2_TEST_REQUEST_MAX - OSC2_TEST_HEADER_LEN)
#define OSC2_TEST_ANSWER_MAX       61     /* version, request ID, op, status, result */
#define OSC2_TEST_RESULT_MAX       (OSC2_TEST_ANSWER_MAX - 4)
#define OSC2_TEST_STATE52_LEN      52     /* report 0x42 without its ID and sequence byte */
#define OSC2_TEST_CAPS_LEN         36
#define OSC2_TEST_STATUS_LEN       32
#define OSC2_TEST_BATTERY_ARGS     16
#define OSC2_TEST_LEASE_DEFAULT_S  15
#define OSC2_TEST_LEASE_MAX_S      600
#define OSC2_TEST_COMPLETION_MS    50     /* the guard stays on this long after arming ends */
#define OSC2_TEST_DEAF_MIN_MS      100
#define OSC2_TEST_DEAF_MAX_MS      5000
#define OSC2_TEST_CNT_READ_MAX     13
#define OSC2_TEST_SETTINGS_READ_MAX 27
#define OSC2_TEST_HOST_GET_MAX     54     /* answer bytes HOST_GET passes on */
#define OSC2_TEST_TURN_OFF_KEY     0xABDF9710u   /* the only 0x95 key HOST_SET lets through */

#if defined(CONFIG_OPENSC2_TEST_RING_SIZE)
#define OSC2_TEST_RING_SIZE CONFIG_OPENSC2_TEST_RING_SIZE
#else
#define OSC2_TEST_RING_SIZE 2048
#endif
#if defined(CONFIG_OPENSC2_TEST_SEQ_DEPTH)
#define OSC2_TEST_SEQ_DEPTH CONFIG_OPENSC2_TEST_SEQ_DEPTH
#else
#define OSC2_TEST_SEQ_DEPTH 8
#endif

/* Answer status. */
enum osc2_test_status {
	OSC2_TEST_OK = 0,
	OSC2_TEST_BAD_OP = 1,
	OSC2_TEST_BAD_LEN = 2,
	OSC2_TEST_NOT_ARMED = 3,   /* the console only: USB and the radio do not claim such a request */
	OSC2_TEST_BAD_ARG = 4,
	OSC2_TEST_BUSY = 5,
	OSC2_TEST_DENIED = 6,
	OSC2_TEST_NO_LINK = 7,
};

enum osc2_test_op {
	OSC2_TEST_OP_ARM = 0x01,
	OSC2_TEST_OP_DISARM = 0x02,
	OSC2_TEST_OP_STATUS = 0x03,
	OSC2_TEST_OP_INJECT = 0x10,
	OSC2_TEST_OP_INJECT_OFF = 0x11,
	OSC2_TEST_OP_BATTERY = 0x12,
	OSC2_TEST_OP_BATTERY_OFF = 0x13,
	OSC2_TEST_OP_SEQ_LOAD = 0x14,
	OSC2_TEST_OP_SEQ_RUN = 0x15,
	OSC2_TEST_OP_SEQ_STOP = 0x16,
	OSC2_TEST_OP_RULES = 0x17,        /* the rule word: in effect, the build's, the known bits */
	OSC2_TEST_OP_RULES_SET = 0x18,    /* another rule word until the end of arming */
	OSC2_TEST_OP_RING_READ = 0x20,
	OSC2_TEST_OP_RING_CLEAR = 0x21,
	OSC2_TEST_OP_CNT_READ = 0x22,
	OSC2_TEST_OP_CNT_RESET = 0x23,
	OSC2_TEST_OP_MARK = 0x24,
	OSC2_TEST_OP_SETTINGS_READ = 0x30,
	OSC2_TEST_OP_HOST_OUT = 0x40,
	OSC2_TEST_OP_HOST_SET = 0x41,
	OSC2_TEST_OP_HOST_GET = 0x42,
	OSC2_TEST_OP_APP_SEND = 0x43,
	OSC2_TEST_OP_RADIO_DEAF = 0x44,
};

/* Flags of ARM, INJECT, SEQ_RUN and BATTERY. */
#define OSC2_TEST_ARM_RESTORE        0x01   /* restore the settings and the LED colour at the end */
#define OSC2_TEST_INJECT_STOP_SEQ    0x01   /* stop a running sequence first */
#define OSC2_TEST_INJECT_LIZARD      0x02   /* the lizard test: allowed with lizard mode on */
#define OSC2_TEST_SEQ_LOOP           0x01
#define OSC2_TEST_SEQ_KEEP_LAST      0x02
#define OSC2_TEST_SEQ_LIZARD         0x04
#define OSC2_TEST_BATTERY_SEND_NOW   0x01

/* Links, as the ring and the held answers number them (the library's enum osc2_link, plus none). */
enum osc2_test_link {
	OSC2_TEST_LINK_RADIO = 0,
	OSC2_TEST_LINK_USB = 1,
	OSC2_TEST_LINK_TEST = 2,    /* the host emulation, and everything the console causes */
	OSC2_TEST_LINK_NONE = 3,    /* work queue events: the rumble timeout, the Steam watchdog */
};
#define OSC2_TEST_LINKS 3          /* links with a stored request and a held answer */

/* ---- the ring ---------------------------------------------------------------------------------
 * Entry: time u32 (µs since ARM), kind u8, link u8, data length u8, data. */

#define OSC2_TEST_ENTRY_HEADER    7
#define OSC2_TEST_ENTRY_DATA_MAX  41
#define OSC2_TEST_ENTRY_MAX       (OSC2_TEST_ENTRY_HEADER + OSC2_TEST_ENTRY_DATA_MAX)
#define OSC2_TEST_READ_MAX        48    /* bytes of entries in one RING_READ answer */
#define OSC2_TEST_POWER_TEXT_MAX  32
#define OSC2_TEST_REPORT_MAX      36    /* bytes of a held-back report a LIZARD_RPT entry keeps */

enum osc2_test_kind {
	OSC2_TEST_KIND_ARMED = 0x01,
	OSC2_TEST_KIND_MARK = 0x02,
	OSC2_TEST_KIND_SEQ_STEP = 0x03,
	OSC2_TEST_KIND_SEQ_END = 0x04,
	OSC2_TEST_KIND_INJECT_STOP = 0x05,
	OSC2_TEST_KIND_OUTPUT = 0x10,
	OSC2_TEST_KIND_RUMBLE = 0x11,
	OSC2_TEST_KIND_HAPTIC = 0x12,
	OSC2_TEST_KIND_LED = 0x13,
	OSC2_TEST_KIND_SETTING = 0x14,
	OSC2_TEST_KIND_LIZARD = 0x15,
	OSC2_TEST_KIND_LIZARD_RPT = 0x16,
	OSC2_TEST_KIND_POWER = 0x17,
	OSC2_TEST_KIND_VENDOR = 0x18,
	OSC2_TEST_KIND_HOST_ASLEEP = 0x19,
	OSC2_TEST_KIND_LINK = 0x20,
	OSC2_TEST_KIND_USB = 0x21,
	OSC2_TEST_KIND_VERSION = 0x22,
	OSC2_TEST_KIND_IDENTITY = 0x23,
	OSC2_TEST_KIND_CHANNELS = 0x24,
};

/* LIZARD entries. */
enum osc2_test_lizard {
	OSC2_TEST_LIZARD_LEAVE = 0,
	OSC2_TEST_LIZARD_ENTER = 1,
	OSC2_TEST_LIZARD_DOWN = 2,
	OSC2_TEST_LIZARD_UP = 3,
	OSC2_TEST_LIZARD_RESYNC = 4,
};

/* SEQ_END: how the sequence ended. */
enum osc2_test_seq_end {
	OSC2_TEST_SEQ_END_BACK = 0,     /* back to the static state or the application's */
	OSC2_TEST_SEQ_END_KEPT = 1,     /* the last step became the static state */
	OSC2_TEST_SEQ_END_STOP = 2,     /* SEQ_STOP, INJECT_OFF or INJECT with bit 0 */
	OSC2_TEST_SEQ_END_LIZARD = 3,   /* lizard mode came on */
};

/* ---- counters -------------------------------------------------------------------------------- */

enum osc2_test_counter {
	OSC2_TEST_CNT_FRAMES,
	OSC2_TEST_CNT_USB_SENT,
	OSC2_TEST_CNT_USB_DROPPED,
	OSC2_TEST_CNT_RADIO_SENT,
	OSC2_TEST_CNT_RADIO_DROPPED,
	OSC2_TEST_CNT_RADIO_PACKETS,
	OSC2_TEST_CNT_RADIO_KEEPALIVES,
	OSC2_TEST_CNT_BATTERY_SENT,
	OSC2_TEST_CNT_BATTERY_DROPPED,
	OSC2_TEST_CNT_LIZARD_MADE,
	OSC2_TEST_CNT_LIZARD_DIVERTED,
	OSC2_TEST_CNT_APP_REPORTS_SENT,
	OSC2_TEST_CNT_OUT_REPORTS,
	OSC2_TEST_CNT_OUT_SHORT,
	OSC2_TEST_CNT_VENDOR_SET_OK,
	OSC2_TEST_CNT_VENDOR_SET_ERR,
	OSC2_TEST_CNT_VENDOR_GET_OK,
	OSC2_TEST_CNT_VENDOR_GET_ERR,
	OSC2_TEST_CNT_USB_FRAME_ERR,
	OSC2_TEST_CNT_RING_ENTRIES,
	OSC2_TEST_CNT_RING_EVICTED,
	OSC2_TEST_CNT_RADIO_RX_OVERFLOW,
	OSC2_TEST_CNT_RADIO_ANSWERS_DROPPED,
	OSC2_TEST_CNT_RADIO_EXTRA_DROPPED,
	OSC2_TEST_CNT_TEST_REQUESTS,
	OSC2_TEST_CNT_SEQ_STEPS,
	OSC2_TEST_CNT_USB_SKIPPED,
	OSC2_TEST_CNT_COUNT,
};

extern const char *const osc2_test_counter_names[OSC2_TEST_CNT_COUNT];

/* ---- caps: build flags (each a CONFIG_OPENSC2_* option) --------------------------------------- */

#define OSC2_TEST_FLAG_TRIGGER_CLICK_AUTO     (1u << 0)
#define OSC2_TEST_FLAG_LIZARD_MODE            (1u << 1)
#define OSC2_TEST_FLAG_STEAM_WATCHDOG         (1u << 2)
#define OSC2_TEST_FLAG_USB                    (1u << 3)
#define OSC2_TEST_FLAG_USB_PROFILE_OCTOBER    (1u << 4)
#define OSC2_TEST_FLAG_USB_CONSOLE            (1u << 5)
#define OSC2_TEST_FLAG_USB_WIRED_CONSOLE      (1u << 6)
#define OSC2_TEST_FLAG_IDENTITY_VENDOR_WRITE  (1u << 7)
#define OSC2_TEST_FLAG_STORAGE                (1u << 8)
#define OSC2_TEST_FLAG_USB_LOG                (1u << 9)
#define OSC2_TEST_FLAG_USB_REMOTE_WAKEUP      (1u << 10)
#define OSC2_TEST_FLAG_USB_PLUG_IN_REBOOT     (1u << 11)
#define OSC2_TEST_FLAG_SHELL                  (1u << 12)
#define OSC2_TEST_FLAG_FUSION                 (1u << 13)
/* CONFIG_OPENSC2_NO_HOST_WIRELESS_MS is not 0: the charger test build (a testing aid that is not
 * the real controller's behaviour). */
#define OSC2_TEST_FLAG_NO_HOST_WIRELESS       (1u << 14)
/* The input rules and lizard mode's processing switch with a rule word (input_rules.h), which the
 * ops RULES and RULES_SET read and change. Firmware without it has the rules of OpenSC2 0.2.0:
 * the trigger bits only, and none of lizard mode's processing. */
#define OSC2_TEST_FLAG_RULES                  (1u << 15)

/* STATUS flags (byte 0). */
#define OSC2_TEST_ST_ARMED       (1u << 0)
#define OSC2_TEST_ST_GUARD       (1u << 1)
#define OSC2_TEST_ST_INJECT      (1u << 2)
#define OSC2_TEST_ST_BATTERY     (1u << 3)
#define OSC2_TEST_ST_SEQ         (1u << 4)
#define OSC2_TEST_ST_LIZARD      (1u << 5)
#define OSC2_TEST_ST_COMPLETING  (1u << 6)
#define OSC2_TEST_ST_SNAPSHOT    (1u << 7)

/* ---- what the core needs from the library ---------------------------------------------------- */

/* The library's part of the caps. */
struct osc2_test_lib_caps {
	uint8_t mode;                   /* this boot: 0 wireless, 1 wired */
	uint32_t build_flags;           /* OSC2_TEST_FLAG_* */
	uint16_t battery_interval_ms;
	uint16_t rumble_timeout_ms;
	uint16_t report_delay_us;
	uint8_t policy;                 /* enum opensc2_policy, as stored */
	char git_revision[OPENSC2_GIT_REVISION_MAX + 1];
	char product_name[OPENSC2_PRODUCT_NAME_MAX + 1];   /* the identity's; the caps carry its
							     * CRC-32 */
};

/* The library's part of the status. */
struct osc2_test_lib_status {
	uint8_t mode;                   /* 0 wireless, 1 wired */
	uint8_t mode_reason;            /* enum opensc2_mode_reason */
	uint8_t link_state;             /* enum opensc2_link_state */
	uint8_t usb_state;              /* enum opensc2_usb_state */
	uint8_t slot;                   /* the selected bond slot, 0 or 1 */
	uint8_t last_reason;            /* the last radio disconnect reason */
	uint8_t policy;                 /* enum opensc2_policy */
	bool lizard_on;                 /* setting 9 is not 0 */
	uint32_t uptime_ms;
};

/* Every function may be called from any thread. `lock` guards the core's state: the core takes it
 * only to copy state in and out, and calls nothing in this table but the clock, the counters,
 * ctx_link and mode_link while it holds it. `req_lock` lets one request (or the end of arming) run
 * at a time; nothing the core calls under it takes it again. */
struct osc2_test_env {
	uint32_t (*now_ms)(void);                 /* uptime, ms (wrapping) */
	uint32_t (*now_us)(void);                 /* µs (wrapping) */
	void (*lock)(void);
	void (*unlock)(void);
	void (*req_lock)(void);
	void (*req_unlock)(void);
	void (*cnt_add)(uint8_t counter, uint32_t n);
	uint32_t (*cnt_get)(uint8_t counter);
	void (*cnt_reset)(void);
	uint8_t (*ctx_link)(void);                /* the link of the command this thread runs, or none */
	uint8_t (*mode_link)(void);               /* where reports go: USB while wired, else radio */
	void (*caps)(struct osc2_test_lib_caps *caps);
	void (*status)(struct osc2_test_lib_status *status);
	bool (*lizard_on)(void);                  /* setting 9 is not 0 */
	int16_t (*setting_get)(uint8_t number);
	void (*setting_write)(uint8_t number, int16_t value);   /* what one 0x87 triplet does */
	void (*led_get)(uint8_t rgbw[4]);
	void (*led_set)(const uint8_t rgbw[4]);                 /* what 0xC5 does */
	int (*host_out)(const uint8_t *report, uint8_t len);    /* an output report on the test link */
	int32_t (*host_set)(const uint8_t *rec, uint8_t rec_len);   /* a vendor SET on the test link */
	/* A vendor GET on the test link: {opcode, n, data} into `out`, the bytes written; the test
	 * link's stored request {opcode, n, payload} into `stored`. */
	uint8_t (*host_get)(uint8_t out[63], uint8_t stored[63], uint8_t *stored_len);
	int32_t (*app_send)(const uint8_t *report, uint8_t len);   /* opensc2_send_report() */
	int32_t (*app_haptic)(uint8_t channel, uint32_t flags);   /* opensc2_send_haptic_status() */
	bool (*radio_connected)(void);            /* wireless and connected */
	void (*battery_now)(void);                /* a battery report now; the period restarts */
	bool (*release_held)(void);               /* osc2_lizard_release_held() */
	void (*lizard_resync)(void);              /* osc2_lizard_resync() (writes a LIZARD entry) */
	uint32_t (*rules_get)(void);              /* the rule word in effect (input_rules.h) */
	uint32_t (*rules_build)(void);            /* the build's */
	void (*rules_set)(uint32_t rules);
};

/* ---- the core's state ------------------------------------------------------------------------ */

struct osc2_test_ring {
	uint8_t buf[OSC2_TEST_RING_SIZE];
	uint32_t head;          /* offset of the oldest entry */
	uint32_t used;          /* bytes in use */
	uint32_t first;         /* sequence number of the oldest entry */
	uint32_t next;          /* sequence number the next entry gets */
	uint32_t cache_seq;     /* where the last read ended (an entry still in the ring, or `next`) */
	uint32_t cache_off;
	bool cache_ok;
};

struct osc2_test_step {
	uint8_t hold;           /* frames, 1..255 */
	uint8_t state[OSC2_TEST_STATE52_LEN];
};

struct osc2_test_held {
	bool valid;
	uint8_t len;
	uint8_t data[OSC2_TEST_ANSWER_MAX];
};

struct osc2_test_core {
	volatile uint8_t armed;     /* read without the lock for the fast paths */
	volatile uint8_t guard;
	bool completing;            /* arming ended; the guard stays on until completion_ms */
	uint32_t completion_ms;
	uint32_t nonce;
	uint16_t lease_s;
	uint32_t lease_ms;          /* arming ends at this uptime */
	uint32_t arm_us;            /* the ring's time base */
	uint32_t frame_index;       /* state reports built since ARM: the index of the next one */
	uint8_t last_seq;           /* the sequence byte of the last state report built */
	/* injection */
	bool static_on, static_lizard;
	uint8_t static_state[OSC2_TEST_STATE52_LEN];
	bool battery_on;
	struct opensc2_battery battery;
	/* the frame-locked sequence */
	struct osc2_test_step steps[OSC2_TEST_SEQ_DEPTH];
	uint32_t loaded;            /* one bit per step loaded since ARM */
	bool seq_on, seq_started;
	uint8_t seq_count, seq_flags, seq_step;
	uint16_t seq_left;          /* frames left in the current step */
	uint32_t seq_gen;           /* changes whenever a sequence starts or stops */
	/* the frame being built */
	bool frame_seq;             /* built from a sequence step ... */
	uint8_t frame_step;         /* ... this one ... */
	uint32_t frame_gen;         /* ... of this sequence */
	volatile uint8_t skip_lizard;   /* built from an injected state without the lizard test */
	uint16_t deaf_ms;           /* RADIO_DEAF, waiting for the radio thread */
	/* the settings snapshot written back at the end of arming */
	bool snap_on;
	int16_t snap[OPENSC2_SETTINGS_COUNT];
	uint8_t snap_led[4];
	/* RULES_SET changed the rule word: the build's comes back at the end of arming */
	bool rules_changed;
	/* per link */
	struct osc2_test_held held[OSC2_TEST_LINKS];
	volatile uint8_t just_claimed[OSC2_TEST_LINKS];
	volatile uint8_t quiet[OSC2_TEST_LINKS];
	struct osc2_test_ring ring;
};

/* ---- the core's entry points (they take the env's locks themselves) --------------------------- */

void osc2_test_core_init(struct osc2_test_core *c);

/* USB and the radio: a SET of opcode 0x7E on `link`. Claimed when n >= 7, the key matches, the
 * version is 1 and the board is armed or the op is ARM: then it runs at once, its answer is held
 * for the link's GETs, and true is returned. Otherwise false: the library's usual path follows. */
bool osc2_test_core_claim(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t link,
			  const uint8_t *payload, uint8_t n);
/* A GET of opcode 0x7E on `link`: the held answer (never re-run), or false when none is held. */
bool osc2_test_core_answer(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t link,
			   uint8_t *out, uint8_t *out_len);
/* A SET on `link` succeeded and replaced its stored request: unless that SET was the claim, the
 * held answer is dropped. */
void osc2_test_core_stored(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t link);
/* Was the last request on `link` a claimed SET or a GET the hooks answered? Reading clears it. */
bool osc2_test_core_quiet(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t link);

/* The console's "raw": any request that passes the key and version checks runs (an op other than
 * ARM or STATUS answers NOT_ARMED while unarmed). -22 for anything else. */
int osc2_test_core_raw(struct osc2_test_core *c, const struct osc2_test_env *env,
		       const uint8_t *req, uint8_t n, uint8_t answer[OSC2_TEST_ANSWER_MAX],
		       uint8_t *answer_len);

/* The console command "opensc2 test [<id>] <verb> [<arguments>]": `argv` holds the words after
 * "test". Every line goes to `emit` (without a line end); the last one is "HWT <id> END <rc>".
 * Returns rc. */
typedef void (*osc2_test_emit)(void *ctx, const char *line);
int osc2_test_console(struct osc2_test_core *c, const struct osc2_test_env *env, int argc,
		      const char *const *argv, osc2_test_emit emit, void *ctx);

/* The clock: the lease running out, and the end of a disarm. Call at the deadline. */
void osc2_test_core_poll(struct osc2_test_core *c, const struct osc2_test_env *env);
/* Milliseconds to the next deadline (0 when due), or -1 when there is none. */
int32_t osc2_test_core_next(struct osc2_test_core *c, const struct osc2_test_env *env);

/* Every state report: the injected state replaces the application's (true when it did). */
bool osc2_test_core_input(struct osc2_test_core *c, const struct osc2_test_env *env, bool lizard_on,
			  struct opensc2_state *s);
/* After each state report is encoded. */
void osc2_test_core_frame(struct osc2_test_core *c, const struct osc2_test_env *env,
			  const uint8_t report[OPENSC2_STATE_REPORT_LEN]);
/* Was this frame built from an injected state without the lizard test? */
static inline bool osc2_test_core_skip_lizard(const struct osc2_test_core *c)
{
	return c->skip_lizard != 0;
}
/* The injected battery replaces `*b` (true when one is injected). */
bool osc2_test_core_battery(struct osc2_test_core *c, const struct osc2_test_env *env,
			    struct opensc2_battery *b);
/* The lizard guard: a mouse or keyboard report is held back (true) while the guard is on. */
bool osc2_test_core_divert(struct osc2_test_core *c, const struct osc2_test_env *env,
			   const uint8_t *report, size_t len);
/* RADIO_DEAF: the milliseconds to stay deaf, once (0: none). */
uint16_t osc2_test_core_take_deaf(struct osc2_test_core *c, const struct osc2_test_env *env);
/* An entry for the ring, while armed. `link` OSC2_TEST_LINK_*, or OSC2_TEST_LINK_CTX for the link
 * of the command this thread runs. */
#define OSC2_TEST_LINK_CTX 0xFE
void osc2_test_core_note(struct osc2_test_core *c, const struct osc2_test_env *env, uint8_t kind,
			 uint8_t link, const uint8_t *data, uint8_t len);

static inline bool osc2_test_core_armed(const struct osc2_test_core *c)
{
	return c->armed != 0;
}

static inline bool osc2_test_core_guard(const struct osc2_test_core *c)
{
	return c->guard != 0;
}

/* ---- pure helpers (no state; the host tests check each) --------------------------------------- */

/* Key, version and length: a request of the test channel at all. */
bool osc2_test_is_request(const uint8_t *payload, uint8_t n);
/* The claiming rule of USB and the radio. */
bool osc2_test_claims(const uint8_t *payload, uint8_t n, bool armed);
/* BATTERY's 16 argument bytes. */
void osc2_test_battery_args(const uint8_t args[OSC2_TEST_BATTERY_ARGS], struct opensc2_battery *b,
			    uint8_t *flags);
/* The 36-byte caps from the library's part, the lease in effect. */
void osc2_test_pack_caps(uint8_t out[OSC2_TEST_CAPS_LEN], const struct osc2_test_lib_caps *lib,
			 uint16_t lease_s);
/* HOST_SET's allowlist: may this vendor SET run on the test link? */
bool osc2_test_host_set_allowed(uint8_t opcode, const uint8_t *payload, uint8_t n);
/* HOST_GET: is an answer of `n` bytes to the stored request {opcode, len, payload} masked (bonds,
 * serials, the chip ID)? */
bool osc2_test_host_get_masked(const uint8_t *stored, uint8_t stored_len, uint8_t n);
/* The settings-by-name keys VENDOR entries name: 1 esb/bond, 2 esb/bond_2,
 * 3 user/wireless_transport, 4 user/haptic_boot_level, 5 opensc2/unit_serial,
 * 6 opensc2/board_serial, 7 opensc2/git_revision, 8 opensc2/board_revision, 0 anything else or no
 * NUL. `*value_len`: the bytes after the NUL. */
uint8_t osc2_test_key_id(const uint8_t *payload, size_t len, uint8_t *value_len);

/* Entry data (each returns its length). */
uint8_t osc2_test_enc_rumble(uint8_t out[10], const struct opensc2_rumble *r);
uint8_t osc2_test_enc_haptic(uint8_t out[12], const struct opensc2_haptic *h);
uint8_t osc2_test_enc_led(uint8_t out[6], const struct opensc2_led *led);
uint8_t osc2_test_enc_setting(uint8_t out[3], uint8_t number, int16_t value);
uint8_t osc2_test_enc_output(uint8_t out[19], const uint8_t *report, size_t len, int decoded);
uint8_t osc2_test_enc_power(uint8_t out[1 + OSC2_TEST_POWER_TEXT_MAX], uint8_t request,
			    const char *why);
/* VENDOR: `n` is the request's length byte (SET) or the answer's length (GET); `payload` holds
 * the request's `len` payload bytes, and `avail` bytes exist there (on USB a SET has 61 whatever
 * its length byte says: the 0x86 and 0x95 keys are read from them, as the command does). Only the
 * safe parts are kept: setting triplets, selectors, colours, keys, the names of settings by name
 * and their value lengths; never bond values, serials or answers. */
uint8_t osc2_test_enc_vendor(uint8_t out[19], bool get, uint8_t opcode, uint8_t n, int32_t result,
			     const uint8_t *payload, size_t len, size_t avail);
uint8_t osc2_test_enc_identity(uint8_t out[26], const struct opensc2_identity *id);
/* A whole entry into `out` (at most OSC2_TEST_ENTRY_MAX bytes); returns its length. */
uint8_t osc2_test_entry(uint8_t *out, uint32_t time_us, uint8_t kind, uint8_t link,
			const uint8_t *data, uint8_t len);

/* Console text. */
void osc2_test_hex(char *out, const uint8_t *data, size_t n);   /* lower case, NUL ended */
int osc2_test_unhex(const char *text, uint8_t *out, size_t max);   /* bytes, or -1 */
/* "armed=1 guard=1 ..." from a STATUS result and the policy. */
void osc2_test_fmt_status(char *out, size_t size, const uint8_t status[OSC2_TEST_STATUS_LEN],
			  uint8_t policy);
/* "<seq> <t ms>.<µs> <link> <KIND> <field=value ...>" for one entry. */
void osc2_test_fmt_entry(char *out, size_t size, uint32_t seq, const uint8_t *entry, size_t len);

/* Console return codes. */
#define OSC2_TEST_RC_BAD_ARGS    (-22)
#define OSC2_TEST_RC_NOT_ARMED   (-13)
#define OSC2_TEST_RC_UNKNOWN     (-2)

#ifdef __cplusplus
}
#endif

#endif /* OPENSC2_TEST_CORE_H_ */
