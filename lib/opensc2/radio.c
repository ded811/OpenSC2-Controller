/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * radio.c: the controller's end of the puck's 2.4 GHz link, on Nordic's Enhanced ShockBurst.
 *
 * The controller is the receiver (PRX): it can only answer, and everything it says rides in the
 * acknowledgement of the puck's *next* packet, so it queues each reply as soon as it has handled
 * a packet. One thread runs a small state machine:
 *
 *   OFF        radio off
 *   NO_BOND    radio on, the selected bond slot is empty (10 s, then wireless off)
 *   PAIRING    answering a puck's pairing beacon (channel 2, address 91 A2 A7 93)
 *   LISTENING  waiting on channel 2, address "ibex", for an 0xE1 request carrying our bond's IDs
 *              (60 s, then wireless off)
 *   CONNECTED  on the puck's private pipe: keep-alives in, state reports and answers out
 *
 * Timings, packet formats, the order of replies and the rules of radio_rules.h follow the
 * protocol documentation's description of the controller's radio link (not public yet;
 * docs/protocol.md sums it up). Each connected turn is the real controller's: a packet is handled,
 * then whatever is pending ("disconnecting", the version answer, the identity) is queued at once,
 * and the next turn begins; a keep-alive's state report is built and queued at its slot, about
 * 2 ms later, while the thread waits for the next packet (radio_rules.h), as the real controller
 * builds it beside its radio.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/onoff.h>
#include <esb.h>
#include <hal/nrf_radio.h>
#include "internal.h"
#include "radio_rules.h"
#include "report_queue.h"
#include "system.h"
#include "test_hooks.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define LISTEN_POLL_MS        50     /* how often listening looks at control events */
#define PRIMARY_WINDOW_MS     10     /* waiting on the first channel of the list per turn */
#define ALTERNATE_WINDOW_MS   13     /* each other entry during an outage */
#define LINK_LOST_MS          500    /* from the start of a turn */
#define CHANNEL_SETTLE_US     500    /* 0xE4: wait this long before moving */
#define OFFER_LINGER_MS       30     /* pairing: stay on the private address after the offer */
/* The state report's lead before its slot until 65 reports have been timed: 81 ticks of
 * 32768 Hz, as the real controller starts (radio_rules.h). */
#define INITIAL_LEAD_US       2472

BUILD_ASSERT(OSC2_EV_START == OSC2_RCTL_START && OSC2_EV_STOP == OSC2_RCTL_STOP &&
	     OSC2_EV_PAIR == OSC2_RCTL_PAIR && OSC2_EV_CANCEL == OSC2_RCTL_CANCEL &&
	     OSC2_EV_BOND == OSC2_RCTL_BOND && OSC2_EV_SLOT == OSC2_RCTL_SLOT,
	     "radio_rules.h names the radio thread's events");

/* ---- queues -------------------------------------------------------------------------------- */

struct rx_item {
	int64_t ticks;   /* when the packet arrived */
	uint8_t len;
	int8_t rssi;     /* dB below 0 dBm */
	uint8_t data[CONFIG_ESB_MAX_PAYLOAD_LENGTH];
};

K_MSGQ_DEFINE(osc2_rx_q, sizeof(struct rx_item), CONFIG_OPENSC2_RX_QUEUE_DEPTH, 8);
static K_EVENT_DEFINE(radio_ev);

/* Extra input reports, riding behind the state reports (report_queue.h): up to
 * CONFIG_OPENSC2_REPORT_QUEUE_DEPTH (5, the real controller's queue, which drops a report that
 * finds it full), then CONFIG_OPENSC2_REPORT_WAIT_DEPTH more that wait for room (0 by default).
 * Any thread adds to it; only the radio thread takes from it. */
#define EXTRA_CAP (CONFIG_OPENSC2_REPORT_QUEUE_DEPTH + CONFIG_OPENSC2_REPORT_WAIT_DEPTH)
BUILD_ASSERT(EXTRA_CAP >= 1 && EXTRA_CAP <= 255, "the extra report queue holds 1 to 255 reports");
static struct osc2_rq_item extra_items[EXTRA_CAP];
static struct osc2_rq extra_q;
static struct k_spinlock extra_lock;

/* ---- link state ------------------------------------------------------------------------------ */

static volatile enum opensc2_link_state link_state = OPENSC2_LINK_OFF;
static enum opensc2_reason entry_reason;   /* why the next state is entered */
static uint16_t pair_seconds = CONFIG_OPENSC2_PAIRING_TIMEOUT_S;
static int8_t tx_power = CONFIG_OPENSC2_TX_POWER_DBM;
/* The give-ups (0: never), from Kconfig until the application sets them. */
static uint16_t listen_s = CONFIG_OPENSC2_LISTEN_TIMEOUT_S;
static uint16_t no_bond_s = CONFIG_OPENSC2_NO_BOND_TIMEOUT_S;
static bool radio_on;
static uint32_t rx_overflow;
static uint8_t run_slot;                   /* the bond slot NO_BOND and LISTENING run */

static struct k_spinlock info_lock;
static struct opensc2_link_info info;      /* readers copy it under info_lock */
static int64_t connected_at;

static struct opensc2_connect_request request;   /* the one that started this connection */

/* Per connection. The state report, lizard mode, the battery report, rumble and the Steam watchdog
 * are shared with the USB link (report.c, core.c). */
static struct {
	uint8_t channels[4];
	uint8_t slot;
	uint8_t protocol;
	bool version_pending;
	bool identity_pending;
	bool disconnecting;
	uint8_t reason;                    /* the first disconnect reason (radio_rules.h) */
	struct osc2_radio_answers answers;
	struct osc2_slot_lead lead;
	struct osc2_report_sched sched;    /* the state reports keep-alives asked for */
} conn;

/* ---- ESB ------------------------------------------------------------------------------------- */

static struct esb_payload isr_payload;
static struct rx_item isr_item;
/* The packet the radio thread works on, in whichever state: static, so that its 140 bytes stay off
 * the thread's stack, which a vendor command's way down (a test op of the hooks, a dock op, a log
 * line) needs. Only the radio thread uses it. */
static struct rx_item rx_it;

static void esb_handler(const struct esb_evt *evt)
{
	if (evt->evt_id != ESB_EVENT_RX_RECEIVED) {
		return;   /* transmit events do not matter to a receiver */
	}
	while (esb_read_rx_payload(&isr_payload) == 0) {
		isr_item.ticks = k_uptime_ticks();
		isr_item.len = isr_payload.length;
		isr_item.rssi = isr_payload.rssi;
		memcpy(isr_item.data, isr_payload.data, isr_payload.length);
		if (k_msgq_put(&osc2_rx_q, &isr_item, K_NO_WAIT) != 0) {
			rx_overflow++;
			osc2_test_count(OSC2_TEST_CNT_RADIO_RX_OVERFLOW);   /* atomic */
		}
	}
	k_event_post(&radio_ev, OSC2_EV_RX);
}

#if !defined(CONFIG_ESB_CLOCK_INIT) && defined(CONFIG_CLOCK_CONTROL_NRF)
static struct onoff_client clk_client;

/* The radio needs the 32 MHz crystal. The real controller waits for it without a limit; here a
 * crystal that has not started after 500 ms leaves the radio off (the same end: no link). */
static int hfclk_request(void)
{
	struct onoff_manager *mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	int err, result;

	sys_notify_init_spinwait(&clk_client.notify);
	err = onoff_request(mgr, &clk_client);
	if (err < 0) {
		return err;
	}
	for (int ms = 0; ms < 500; ms++) {
		if (sys_notify_fetch_result(&clk_client.notify, &result) == 0) {
			return result;
		}
		k_msleep(1);
	}
	return -ETIMEDOUT;
}

static void hfclk_release(void)
{
	(void)onoff_release(z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF));
}
#else
/* Either ESB starts the clock itself (CONFIG_ESB_CLOCK_INIT) or the platform has no such clock. */
static int hfclk_request(void) { return 0; }
static void hfclk_release(void) {}
#endif

/* Set while radio_enable() runs, before it looks at Bluetooth's claim: Bluetooth's idle check
 * (osc2_radio_idle()) then waits. Each side marks itself before it reads the other (Bluetooth sets
 * its claim before it polls), so one of them always sees the other: ESB never starts on top of a
 * SoftDevice Controller that was just enabled, also while the crystal starts (hfclk_request() can
 * sleep in 1 ms steps). */
static atomic_t esb_starting;

/* The real controller's ESB set-up: dynamic payloads of up to 128 bytes, 16-bit CRC, the 2 Mbit/s
 * BLE PHY, the receiver, +6 dBm (or the board's power), selective auto-acknowledgement, fast
 * ramp-up, no retransmissions. Done at every enable and again every time the link listens. */
static int radio_configure(void)
{
	struct esb_config cfg = ESB_DEFAULT_CONFIG;

	cfg.protocol = ESB_PROTOCOL_ESB_DPL;
	cfg.mode = ESB_MODE_PRX;
	cfg.event_handler = esb_handler;
	cfg.bitrate = ESB_BITRATE_2MBPS_BLE;
	cfg.crc = ESB_CRC_16BIT;
	cfg.tx_output_power = tx_power;
	cfg.retransmit_delay = 600;
	cfg.retransmit_count = 0;
	cfg.tx_mode = ESB_TXMODE_AUTO;
	cfg.payload_length = 128;
	cfg.selective_auto_ack = true;
	cfg.use_fast_ramp_up = true;
	return esb_init(&cfg);
}

static int radio_enable(void)
{
	int err;

	if (radio_on) {
		return 0;
	}
	atomic_set(&esb_starting, 1);
	if (osc2_ble_owns_radio()) {
		/* Only one wireless transport at a time: Bluetooth has the radio (wireless.c starts
		 * the radio again once it has given it back). */
		atomic_set(&esb_starting, 0);
		LOG_WRN("Bluetooth has the radio: the puck's link stays off");
		return -EBUSY;
	}
	err = hfclk_request();
	if (err) {
		atomic_set(&esb_starting, 0);
		LOG_ERR("The 32 MHz clock did not start (%d)", err);
		return err;
	}
	if (osc2_ble_owns_radio()) {
		/* Bluetooth asked for the radio while the crystal started: it waited, it has it. */
		hfclk_release();
		atomic_set(&esb_starting, 0);
		LOG_WRN("Bluetooth took the radio: the puck's link stays off");
		return -EBUSY;
	}
	err = radio_configure();
	if (err) {
		LOG_ERR("esb_init failed (%d)", err);
		hfclk_release();
		atomic_set(&esb_starting, 0);
		return err;
	}
	radio_on = true;   /* before the flag clears: an idle check that sees it clear sees this */
	atomic_set(&esb_starting, 0);
	return 0;
}

static void radio_disable(void)
{
	if (!radio_on) {
		return;
	}
	esb_disable();
	hfclk_release();
	radio_on = false;
	k_msgq_purge(&osc2_rx_q);
}

/* Listen on a fresh pipe: everything queued for the previous one is dropped. */
static int listen_on(uint8_t channel, const uint8_t base[4], uint8_t prefix)
{
	int err;

	(void)esb_stop_rx();
	(void)esb_flush_tx();
	(void)esb_flush_rx();
	k_msgq_purge(&osc2_rx_q);
	err = esb_set_rf_channel(channel);
	err = err ? err : esb_set_base_address_0(base);
	err = err ? err : esb_set_prefixes(&prefix, 1);   /* only pipe 0 */
	err = err ? err : esb_start_rx();
	if (err) {
		LOG_ERR("Could not listen on channel %u (%d)", channel, err);
	}
	info.channel = channel;
	return err;
}

/* Let an acknowledgement that is on the air finish: wait until the radio receives again. */
static void wait_ack_done(void)
{
	for (int i = 0; i < 40; i++) {
		nrf_radio_state_t s = nrf_radio_state_get(NRF_RADIO);

		if (s == NRF_RADIO_STATE_RX || s == NRF_RADIO_STATE_RXIDLE) {
			return;
		}
		k_busy_wait(50);
	}
}

/* Move to another channel and keep the queued acknowledgement payloads. */
static void retune(uint8_t channel)
{
	wait_ack_done();
	(void)esb_stop_rx();
	(void)esb_set_rf_channel(channel);
	(void)esb_start_rx();
	info.channel = channel;
}

static int write_payload(const struct opensc2_payload *p)
{
	struct esb_payload tx = {
		.pipe = 0,
		.length = p->len,
	};

	memcpy(tx.data, p->data, p->len);
	return esb_write_payload(&tx);
}

/* ---- control --------------------------------------------------------------------------------- */

void osc2_radio_post(uint32_t events)
{
	k_event_post(&radio_ev, events);
}

void osc2_radio_set_pair_seconds(uint16_t seconds)
{
	pair_seconds = seconds;
}

int osc2_radio_set_tx_power(int8_t dbm)
{
	if (dbm < -40 || dbm > 8) {
		return -EINVAL;
	}
	tx_power = dbm;   /* the next set-up: every time the link listens */
	return 0;
}

int8_t osc2_radio_tx_power(void)
{
	return tx_power;
}

void osc2_radio_set_give_ups(uint16_t listen_seconds, uint16_t no_bond_seconds)
{
	listen_s = listen_seconds;   /* from the next time the link listens or waits for a bond */
	no_bond_s = no_bond_seconds;
}

void osc2_radio_give_ups(uint16_t *listen_seconds, uint16_t *no_bond_seconds)
{
	*listen_seconds = listen_s;
	*no_bond_seconds = no_bond_s;
}

enum opensc2_link_state osc2_radio_state(void)
{
	return link_state;
}

bool osc2_radio_connected(void)
{
	return link_state == OPENSC2_LINK_CONNECTED;
}

bool osc2_radio_idle(void)
{
	/* The flag first: once it reads clear, a start that finished has set radio_on already. */
	return !atomic_get(&esb_starting) && !radio_on && link_state == OPENSC2_LINK_OFF;
}

void osc2_radio_info(struct opensc2_link_info *out)
{
	k_spinlock_key_t key = k_spin_lock(&info_lock);

	*out = info;
	out->connected_ms = info.state == OPENSC2_LINK_CONNECTED ?
			    (uint32_t)(k_uptime_get() - connected_at) : 0;
	k_spin_unlock(&info_lock, key);
}

int osc2_radio_queue_report(const uint8_t *report, size_t len, bool droppable)
{
	k_spinlock_key_t key;
	int err;

	if (len > OSC2_RQ_REPORT_MAX) {
		return -EINVAL;
	}
	key = k_spin_lock(&extra_lock);
	err = osc2_rq_put(&extra_q, report, (uint8_t)len, droppable);
	k_spin_unlock(&extra_lock, key);
	if (err == -ENOMEM) {
		osc2_test_count(OSC2_TEST_CNT_RADIO_EXTRA_DROPPED);
	}
	return err;
}

bool osc2_radio_backlog(void)
{
	k_spinlock_key_t key;
	bool backlog;

	if (CONFIG_OPENSC2_REPORT_WAIT_DEPTH == 0) {
		return false;   /* nothing waits: a report that finds the queue full is dropped */
	}
	key = k_spin_lock(&extra_lock);
	backlog = osc2_rq_backlog(&extra_q);
	k_spin_unlock(&extra_lock, key);
	return backlog;
}

static const char *state_name(enum opensc2_link_state s)
{
	static const char *const names[] = { "off", "no bond", "pairing", "listening", "connected" };

	return s <= OPENSC2_LINK_CONNECTED ? names[s] : "?";
}

static void enter(enum opensc2_link_state state)
{
	struct opensc2_link_info copy;
	enum opensc2_reason reason = entry_reason;
	k_spinlock_key_t key;
	bool changed = state != link_state || reason != OPENSC2_REASON_NONE;

	entry_reason = OPENSC2_REASON_NONE;
	key = k_spin_lock(&info_lock);
	info.state = state;
	info.reason = reason;
	info.slot = osc2.slot;
	info.transport = (uint8_t)(OSC2_TRANSPORT_PUCK_1 + osc2.slot);
	if (state == OPENSC2_LINK_CONNECTED) {
		connected_at = k_uptime_get();
		info.packets = info.reports = info.dropped = 0;
		info.protocol = 0;
	}
	copy = info;
	k_spin_unlock(&info_lock, key);
	link_state = state;
	if (state == OPENSC2_LINK_CONNECTED) {
		/* October saves the transport at every connection (wireless.c) */
		osc2_transport_link_up(copy.transport);
	}
	if (!changed) {
		return;
	}
	osc2_test_note_link((uint8_t)state, (uint8_t)reason, copy.slot, copy.channel, copy.protocol);
	osc2_sys_link_changed((uint8_t)state, (uint8_t)reason);
	LOG_INF("Link %s (reason %d, slot %u, channel %u)", state_name(state), reason, copy.slot + 1,
		copy.channel);
	if (osc2.cb && osc2.cb->link_changed) {
		osc2.cb->link_changed(&copy, osc2.user_data);
	}
}

/* Wait for a packet until `deadline_ms` (uptime). Returns 1 with a packet; 0 on timeout or when a
 * control event arrived (it is added to *ctrl). */
static int next_packet(struct rx_item *it, int64_t deadline_ms, uint32_t *ctrl)
{
	for (;;) {
		int64_t left;
		uint32_t ev;

		if (*ctrl == 0 && k_msgq_get(&osc2_rx_q, it, K_NO_WAIT) == 0) {
			return 1;
		}
		if (*ctrl) {
			return 0;
		}
		left = deadline_ms - k_uptime_get();
		ev = k_event_wait_safe(&radio_ev, UINT32_MAX, false,
				       left > 0 ? K_MSEC(left) : K_NO_WAIT);
		*ctrl |= ev & ~OSC2_EV_RX;
		if (ev == 0) {
			return k_msgq_get(&osc2_rx_q, it, K_NO_WAIT) == 0;
		}
	}
}

static enum opensc2_link_state with_bond_or_not(void)
{
	struct opensc2_bond bond;

	(void)opensc2_bond_get(osc2.slot, &bond);
	return opensc2_bond_valid(&bond) ? OPENSC2_LINK_LISTENING : OPENSC2_LINK_NO_BOND;
}

/* What the control events in *ctrl mean in `state` (radio_rules.h). Returns true when the state
 * must end (*next set, and the reason the next state is entered with). */
static bool control(uint32_t *ctrl, enum opensc2_link_state state, uint8_t slot_of_state,
		    enum opensc2_link_state *next)
{
	uint32_t c = *ctrl;
	uint8_t to, reason;

	*ctrl = 0;
	if (!osc2_radio_control((uint8_t)state, c, osc2.slot != slot_of_state, &to, &reason)) {
		return false;
	}
	entry_reason = (enum opensc2_reason)reason;
	*next = to == OSC2_RADIO_BOND_OR_NOT ? with_bond_or_not() : (enum opensc2_link_state)to;
	return true;
}

/* ---- OFF and NO_BOND ------------------------------------------------------------------------- */

static enum opensc2_link_state do_off(uint32_t *ctrl)
{
	radio_disable();
	enter(OPENSC2_LINK_OFF);
	for (;;) {
		uint32_t c = *ctrl;

		*ctrl = 0;
		if (c & (OSC2_EV_PAIR | OSC2_EV_START)) {
			if (radio_enable() == 0) {
				return (c & OSC2_EV_PAIR) ? OPENSC2_LINK_PAIRING : with_bond_or_not();
			}
		}
		*ctrl = k_event_wait_safe(&radio_ev, UINT32_MAX & ~OSC2_EV_RX, false, K_FOREVER);
	}
}

/* The real controller waits 10 s for a bond, then turns wireless off. A bond write of either slot
 * ends the wait, whatever it wrote. */
static enum opensc2_link_state do_no_bond(uint32_t *ctrl)
{
	enum opensc2_link_state next;
	uint16_t seconds = no_bond_s;
	int64_t deadline = seconds ? k_uptime_get() + (int64_t)seconds * 1000 : INT64_MAX;

	run_slot = osc2.slot;
	(void)esb_stop_rx();
	enter(OPENSC2_LINK_NO_BOND);
	for (;;) {
		if (*ctrl && control(ctrl, OPENSC2_LINK_NO_BOND, run_slot, &next)) {
			return next;
		}
		if (k_uptime_get() >= deadline) {
			LOG_INF("No bond within %u s: wireless off", seconds);
			entry_reason = OPENSC2_REASON_NO_BOND_TIMEOUT;
			return OPENSC2_LINK_OFF;
		}
		*ctrl = k_event_wait_safe(&radio_ev, UINT32_MAX & ~OSC2_EV_RX, false,
					  deadline == INT64_MAX ? K_FOREVER :
					  K_MSEC(MAX(deadline - k_uptime_get(), 0)));
	}
}

/* ---- PAIRING ----------------------------------------------------------------------------------
 * The puck's pairing window beacons a hello every 10 ms on channel 2 to address 91 A2 A7 93:
 *  1. we offer 0xF0 {T, unit serial} as the acknowledgement; T is random and becomes a temporary
 *     address, so it avoids the bytes 0x00, 0xAA and 0x55;
 *  2. the puck sends 0xE0 {puck ID, puck serial} to address T; we acknowledge with
 *     0xF4 {C, unit serial}, C being the controller ID both sides store;
 *  3. the bond {puck ID, C, puck serial} is stored and the puck starts connecting.
 * The real controller has this exchange but never starts it (Steam pairs over the cable); here it
 * is OpenSC2's own feature, reachable through opensc2_pair_start().
 */

static uint32_t random_address_word(void)
{
	uint8_t b[4];

	sys_rand_get(b, sizeof(b));
	for (int i = 0; i < 4; i++) {
		while (!opensc2_address_byte_ok(b[i])) {
			sys_rand_get(&b[i], 1);
		}
	}
	return opensc2_get_le32(b);
}

static uint32_t random_nonzero(void)
{
	uint32_t v;

	do {
		sys_rand_get(&v, sizeof(v));
	} while (v == 0);
	return v;
}

static int queue_pairing_packet(uint8_t type, uint32_t id, const char *serial)
{
	struct opensc2_payload p;

	p.len = (uint8_t)opensc2_build_pairing(type, id, serial, p.data);
	return write_payload(&p);
}

static enum opensc2_link_state do_pairing(uint32_t *ctrl)
{
	struct opensc2_identity id;
	struct rx_item *it = &rx_it;
	enum opensc2_link_state next;
	uint8_t temp_address[4];
	uint8_t slot = osc2.slot;
	uint32_t temp_id = random_address_word();
	uint32_t controller_id = random_nonzero();
	int64_t deadline = k_uptime_get() + (int64_t)pair_seconds * 1000;

	opensc2_identity_get(&id);
	opensc2_put_le32(temp_address, temp_id);
	enter(OPENSC2_LINK_PAIRING);

	while (k_uptime_get() < deadline) {
		bool beacon = false;
		uint32_t puck_id = 0;
		char puck_serial[17];

		/* 1. Wait for a beacon; our offer rides in its acknowledgement. */
		listen_on(OPENSC2_COMMON_CHANNEL, opensc2_pairing_address, OPENSC2_PIPE0_PREFIX);
		queue_pairing_packet(OPENSC2_PKT_PAIRING_REQUEST, temp_id, id.unit_serial);
		while (!beacon && k_uptime_get() < deadline) {
			int got = next_packet(it, MIN(deadline, k_uptime_get() + LISTEN_POLL_MS), ctrl);

			if (*ctrl) {
				if ((*ctrl & OSC2_EV_PAIR) && !(*ctrl & (OSC2_EV_STOP | OSC2_EV_CANCEL))) {
					*ctrl = 0;   /* asked again: a fresh window */
					deadline = k_uptime_get() + (int64_t)pair_seconds * 1000;
				} else if (control(ctrl, OPENSC2_LINK_PAIRING, slot, &next)) {
					return next;
				}
			}
			beacon = got && it->len >= 1 && it->data[0] == OPENSC2_PKT_HELLO;
		}
		if (!beacon) {
			break;
		}

		/* 2. The puck now talks to our temporary address. Let the acknowledgement that
		 * carries our offer finish first. */
		wait_ack_done();
		listen_on(OPENSC2_COMMON_CHANNEL, temp_address, OPENSC2_PIPE0_PREFIX);
		queue_pairing_packet(OPENSC2_PKT_PAIRING_CONFIRM, controller_id, id.unit_serial);
		int64_t until = k_uptime_get() + 50 + (sys_rand32_get() % 256);

		while (k_uptime_get() < until) {
			int got = next_packet(it, until, ctrl);

			if (*ctrl && control(ctrl, OPENSC2_LINK_PAIRING, slot, &next)) {
				return next;
			}
			if (got && opensc2_parse_pairing_offer(it->data, it->len, &puck_id, puck_serial)) {
				/* Our confirmation went with this acknowledgement. Keep a copy queued in
				 * case the puck missed it and repeats its offer. */
				queue_pairing_packet(OPENSC2_PKT_PAIRING_CONFIRM, controller_id,
						     id.unit_serial);
				until = k_uptime_get() + OFFER_LINGER_MS;
			}
		}
		if (puck_id) {
			struct opensc2_bond bond = {
				.puck_id = puck_id,
				.controller_id = controller_id,
			};
			k_spinlock_key_t key;

			memcpy(bond.puck_serial, puck_serial, sizeof(bond.puck_serial));
			key = k_spin_lock(&osc2.lock);
			osc2.bonds[osc2.slot] = bond;
			k_spin_unlock(&osc2.lock, key);
			(void)esb_stop_rx();   /* a flash write stalls the CPU */
			if (osc2_storage_save_bond(osc2.slot)) {
				LOG_ERR("Paired, but the bond could not be stored");
			}
			LOG_INF("Paired with puck %.16s in slot %u", puck_serial, osc2.slot + 1);
			entry_reason = OPENSC2_REASON_PAIRED;
			return OPENSC2_LINK_LISTENING;
		}
		/* No offer in time: offer ourselves to the beacon again. */
	}
	LOG_INF("No pairing: no puck answered");
	entry_reason = OPENSC2_REASON_PAIRING_FAILED;
	return with_bond_or_not();
}

/* ---- LISTENING ------------------------------------------------------------------------------- */

/* Listening, with the selected slot's bond as it is now (an empty one included: the real
 * controller then listens for IDs 0 and 0). ESB is set up again first, as at every listen on the
 * real controller. 60 s, then wireless off; every entry starts a fresh 60 s. */
static enum opensc2_link_state do_listen(uint32_t *ctrl)
{
	struct opensc2_bond bond;
	struct rx_item *it = &rx_it;
	enum opensc2_link_state next;
	uint16_t seconds = listen_s;
	int64_t deadline = seconds ? k_uptime_get() + (int64_t)seconds * 1000 : INT64_MAX;
	int err;

	run_slot = osc2.slot;
	(void)opensc2_bond_get(run_slot, &bond);
	(void)esb_stop_rx();
	err = radio_configure();
	if (err) {
		LOG_ERR("esb_init failed (%d)", err);
	}
	listen_on(OPENSC2_COMMON_CHANNEL, opensc2_common_address, OPENSC2_PIPE0_PREFIX);
	enter(OPENSC2_LINK_LISTENING);
	if (IS_ENABLED(CONFIG_OPENSC2_LOG_LEVEL_DBG)) {
		char serial[sizeof(bond.puck_serial) + 1] = { 0 };

		memcpy(serial, bond.puck_serial, sizeof(bond.puck_serial));
		LOG_DBG("Waiting for puck %s (IDs %08x/%08x)", serial, bond.puck_id,
			bond.controller_id);
	}

	for (;;) {
		int got = next_packet(it, MIN(deadline, k_uptime_get() + LISTEN_POLL_MS), ctrl);

		if (*ctrl && control(ctrl, OPENSC2_LINK_LISTENING, run_slot, &next)) {
			return next;
		}
		if (got && osc2_radio_request_matches(it->data, it->len, bond.puck_id,
						      bond.controller_id, &request)) {
			conn.slot = run_slot;
			return OPENSC2_LINK_CONNECTED;
		}
		if (k_uptime_get() >= deadline) {
			LOG_INF("No puck within %u s: wireless off", seconds);
			entry_reason = OPENSC2_REASON_LISTEN_TIMEOUT;
			return OPENSC2_LINK_OFF;
		}
	}
}

/* ---- CONNECTED ------------------------------------------------------------------------------- */

static void handle_records(const struct rx_item *it)
{
	struct opensc2_record rec;
	size_t pos = 1;
	int r;

	while ((r = opensc2_record_next(it->data, it->len, &pos, &rec)) == 1) {
		switch (rec.type) {
		case OPENSC2_REC_SET_FEATURE: {
			/* The record's bytes, zero padded to a whole command, as on USB. */
			uint8_t cmd[63] = { 0 };
			int32_t rc;

			memcpy(cmd, rec.data, MIN(rec.len, sizeof(cmd)));
			rc = osc2_command_set(OSC2_LINK_RADIO, cmd, sizeof(cmd));
			if (!osc2_radio_answers_put_set(&conn.answers, rc)) {
				osc2_test_count(OSC2_TEST_CNT_RADIO_ANSWERS_DROPPED);
			}
			break;
		}
		case OPENSC2_REC_GET_FEATURE: {
			/* Answers the last SET, like a GET on USB. The record's byte is not the
			 * opcode: Valve's puck leaves it unset, and drops an answer whose opcode is
			 * not its SET's. */
			uint8_t answer[63];
			uint8_t n = osc2_command_get(OSC2_LINK_RADIO, answer);

			if (!osc2_radio_answers_put_get(&conn.answers, answer, n)) {
				osc2_test_count(OSC2_TEST_CNT_RADIO_ANSWERS_DROPPED);
			}
			break;
		}
		case OPENSC2_REC_OUTPUT: {
			/* The record's length goes with it: the output reports see one byte more
			 * than on USB (radio_rules.h). */
			uint8_t report[OPENSC2_PAYLOAD_MAX + 1];
			size_t n = osc2_radio_output_record(it->data, it->len, &rec, report);

			if (n) {
				(void)osc2_output_dispatch(OSC2_LINK_RADIO, report, n);
			}
			break;
		}
		default:
			LOG_DBG("Unknown record type %u", rec.type);
			break;
		}
	}
	if (r < 0) {
		LOG_DBG("Malformed records in a keep-alive");
	}
}

/* Queue what is pending, each as its own acknowledgement payload, in this order: "disconnecting"
 * (every turn once the puck turned us off or spoke an unknown version), the version answer and
 * the identity (each retried with the next packet while the two-payload queue is full). */
static void queue_pending(void)
{
	struct opensc2_payload p;

	if (conn.disconnecting) {
		opensc2_payload_start(&p, OPENSC2_PKT_DISCONNECTING);
		(void)write_payload(&p);
	}
	if (conn.version_pending) {
		opensc2_payload_start(&p, OPENSC2_PKT_VERSION_ANSWER);
		(void)opensc2_payload_add_bytes(&p, &conn.protocol, 1);
		conn.version_pending = write_payload(&p) != 0;
	}
	if (conn.identity_pending) {
		uint8_t record[OPENSC2_IDENTITY_RECORD_LEN];
		struct opensc2_identity id;

		opensc2_identity_get(&id);
		opensc2_encode_identity(&id, record);
		opensc2_payload_start(&p, OPENSC2_PKT_IDENTITY);
		(void)opensc2_payload_add_bytes(&p, record, sizeof(record));
		conn.identity_pending = write_payload(&p) != 0;
		if (!conn.identity_pending) {
			osc2_test_note_identity(&id);   /* the numbers and the git revision, no serials */
		}
	}
}

/* The state report whose time has come (conn.sched): built now, early by the lead so that it is
 * ready at its slot, then queued with the waiting answer and as many extra reports as fit. Lizard
 * mode's reports of this frame ride with the next one, as the real controller's do. */
static void state_report(void)
{
	struct opensc2_payload p;
	struct opensc2_state s;
	const struct osc2_rq_item *extra;
	k_spinlock_key_t key;
	uint8_t report[OPENSC2_STATE_REPORT_LEN];
	uint8_t answer[63];
	uint8_t answer_len, answer_type;
	uint8_t extras = 0;
	int64_t start = k_uptime_ticks();

	osc2_report_state(report, &s);
	if (esb_tx_full()) {
		/* Two payloads wait already: this report is dropped (its answer and the extra
		 * reports stay for the next one). */
		info.dropped++;
		osc2_test_count(OSC2_TEST_CNT_RADIO_DROPPED);
	} else {
		opensc2_payload_start(&p, OPENSC2_PKT_APP_DATA);
		answer_type = osc2_radio_answers_next(&conn.answers, answer, &answer_len);
		if (answer_type) {
			(void)opensc2_payload_add_record(&p, answer_type, answer, answer_len);
		}
		(void)opensc2_payload_add_record(&p, OPENSC2_REC_INPUT, report, sizeof(report));
		/* As many extra reports as fit, oldest first; they leave the queue once the payload
		 * is written (only this thread takes from it, others only add at its end). */
		key = k_spin_lock(&extra_lock);
		while ((extra = osc2_rq_peek(&extra_q, extras)) != NULL &&
		       opensc2_payload_add_record(&p, OPENSC2_REC_INPUT, extra->data,
						  extra->len) == 0) {
			extras++;
		}
		k_spin_unlock(&extra_lock, key);
		if (write_payload(&p) != 0) {
			info.dropped++;
			osc2_test_count(OSC2_TEST_CNT_RADIO_DROPPED);
		} else {
			info.reports++;
			osc2_test_count(OSC2_TEST_CNT_RADIO_SENT);
			osc2_radio_answers_sent(&conn.answers, answer_type);
			key = k_spin_lock(&extra_lock);
			osc2_rq_take(&extra_q, extras);
			k_spin_unlock(&extra_lock, key);
		}
	}
	osc2_report_lizard(&s);
	osc2_slot_lead_done(&conn.lead, k_uptime_ticks() - start);
	osc2_report_sched_done(&conn.sched, &conn.lead);
}

/* Handle one packet received on `*channel` (moved by 0xE4). A keep-alive asks for a state report
 * at its slot (conn.sched). */
static void handle_packet(const struct rx_item *it, uint8_t *channel)
{
	uint8_t channels[4];

	info.packets++;
	info.rssi_dbm = (int8_t)-it->rssi;
	osc2_test_count(OSC2_TEST_CNT_RADIO_PACKETS);
	if (it->len == 0) {
		return;
	}

	switch (it->data[0]) {
	case OPENSC2_PKT_HELLO:
		break;   /* nothing to do: the acknowledgement carries whatever is queued */
	case OPENSC2_PKT_KEEPALIVE:
		/* The slot counts from now, before the records run. */
		osc2_report_sched_add(&conn.sched, &conn.lead, k_uptime_ticks() +
				      k_us_to_ticks_near64(CONFIG_OPENSC2_REPORT_DELAY_US));
		osc2_test_count(OSC2_TEST_CNT_RADIO_KEEPALIVES);
		handle_records(it);
		break;
	case OPENSC2_PKT_CHANNEL_CHANGE:
		if (opensc2_parse_channel_change(it->data, it->len, channels) > 0) {
			osc2_test_note_channels(channels);
			memcpy(conn.channels, channels, sizeof(conn.channels));
			memcpy(info.channels, channels, sizeof(info.channels));
			LOG_DBG("Channels %u %u %u %u", channels[0], channels[1], channels[2],
				channels[3]);
			if (channels[0] != *channel) {
				/* after our acknowledgement has gone out */
				k_usleep(CHANNEL_SETTLE_US);
				retune(channels[0]);
				*channel = channels[0];
			}
		}
		break;
	case OPENSC2_PKT_TURN_OFF:
		LOG_INF("The puck asks us to turn off");
		conn.reason = osc2_radio_reason_first(conn.reason, OPENSC2_REASON_PUCK_TURN_OFF);
		if (!conn.disconnecting) {
			osc2_schedule_power(OPENSC2_POWER_TURN_OFF_PUCK, 0, "puck: turn off");
		}
		conn.disconnecting = true;
		break;
	case OPENSC2_PKT_HOST_ASLEEP:
		LOG_INF("The puck says the PC is asleep");
		osc2_test_note_host_asleep();
		if (osc2.cb && osc2.cb->host_asleep) {
			osc2.cb->host_asleep(osc2.user_data);
		}
		osc2_sys_host_asleep();   /* its cue, once per boot */
		break;
	case OPENSC2_PKT_PROTOCOL_VERSION: {
		int v = opensc2_protocol_version(it->data, it->len);

		if (v < 0) {
			LOG_WRN("The puck speaks protocol version %u.%u, unknown here",
				it->len > 1 ? it->data[1] : 0, it->len > 2 ? it->data[2] : 0);
			conn.reason = osc2_radio_reason_first(conn.reason,
							      OPENSC2_REASON_BAD_VERSION);
			conn.disconnecting = true;
		} else {
			conn.protocol = (uint8_t)v;
			conn.version_pending = true;
			info.protocol = (uint8_t)v;
			osc2_test_note_version((uint8_t)v);
		}
		break;
	}
	default:
		LOG_DBG("Unknown packet type 0x%02x (%u bytes)", it->data[0], it->len);
		break;
	}
}

/* One wait of a turn, on the radio's current channel until `until` (uptime ticks). A state report
 * whose time comes meanwhile is built then and the wait goes on; one whose time has come already
 * is built before a packet is taken (the real controller's input loop runs ahead of its radio).
 * Control events do not cut the wait short (they are looked at when the next turn starts). */
static bool wait_packet(struct rx_item *it, int64_t until)
{
	for (;;) {
		int64_t at = until;
		bool report = osc2_report_sched_first(&conn.sched, &at);
		int64_t now = k_uptime_ticks();

		if (report && at <= now) {
			state_report();
			continue;
		}
		if (k_msgq_get(&osc2_rx_q, it, at <= now ? K_NO_WAIT : K_TIMEOUT_ABS_TICKS(at)) == 0) {
			return true;
		}
		if (!report) {
			return false;
		}
		state_report();
	}
}

static enum opensc2_link_state do_connected(uint32_t *ctrl)
{
	enum opensc2_link_state next = OPENSC2_LINK_LISTENING;
	enum opensc2_reason link_reason = OPENSC2_REASON_NONE;
	struct rx_item *it = &rx_it;
	uint8_t prev, channel;
	k_spinlock_key_t key;

	memset(&conn.channels, 0xFF, sizeof(conn.channels));
	conn.channels[0] = request.channel;
	memcpy(info.channels, conn.channels, sizeof(info.channels));
	conn.protocol = 0;
	conn.version_pending = false;
	conn.identity_pending = true;   /* the identity goes out once per connection */
	conn.disconnecting = false;
	conn.reason = OPENSC2_REASON_NONE;
	osc2_radio_answers_clear(&conn.answers);
	osc2_slot_lead_init(&conn.lead, k_us_to_ticks_near64(INITIAL_LEAD_US));
	osc2_report_sched_clear(&conn.sched);
	key = k_spin_lock(&extra_lock);
	osc2_rq_purge(&extra_q);   /* the puck released whatever was held when the link dropped */
	k_spin_unlock(&extra_lock, key);
	osc2_commands_reset(OSC2_LINK_RADIO);

	/* The puck sends its hello on the private pipe 1 ms after the request: be there. */
	listen_on(request.channel, request.address, request.prefix);
	enter(OPENSC2_LINK_CONNECTED);
	prev = conn.channels[0];
	(void)osc2_test_take_deaf();   /* one asked for on a link that has gone since */

	for (;;) {
		uint16_t deaf_ms = osc2_test_take_deaf();
		int64_t lost_at;
		bool got;

		/* The control events, once per turn. */
		*ctrl |= k_event_wait_safe(&radio_ev, UINT32_MAX & ~OSC2_EV_RX, false, K_NO_WAIT);
		if (*ctrl) {
			bool slot_change = (*ctrl & OSC2_EV_SLOT) && osc2.slot != conn.slot;

			if (control(ctrl, OPENSC2_LINK_CONNECTED, conn.slot, &next)) {
				LOG_INF("Leaving the puck (%s)", slot_change ? "another slot" :
					next == OPENSC2_LINK_PAIRING ? "pairing" : "stopped");
				conn.reason = osc2_radio_reason_first(conn.reason,
								      OPENSC2_REASON_DISABLED);
				link_reason = slot_change ? OPENSC2_REASON_BOND_CHANGED :
							    (enum opensc2_reason)conn.reason;
				break;
			}
		}
		if (deaf_ms) {
			/* The hardware test hooks' RADIO_DEAF: hear nothing for a while, so the link
			 * times out through its normal path. What arrived before is not news after. */
			(void)esb_stop_rx();
			k_sleep(K_MSEC(deaf_ms));
			(void)esb_flush_rx();
			k_msgq_purge(&osc2_rx_q);
			(void)esb_start_rx();
		}

		lost_at = k_uptime_ticks() + k_ms_to_ticks_ceil64(LINK_LOST_MS);
		channel = conn.channels[0];
		if (prev != channel) {
			retune(channel);   /* back to the first channel after a packet elsewhere */
		}
		got = wait_packet(it, k_uptime_ticks() + k_ms_to_ticks_ceil64(PRIMARY_WINDOW_MS));
		if (!got) {
			/* Silence: the other entries of the list, 13 ms each, until the turn's 500 ms
			 * are over. */
			uint8_t on = channel;
			int index = 0;

			while (k_uptime_ticks() < lost_at) {
				index = osc2_radio_walk_next(conn.channels, index);
				if (index < 0) {
					index = 0;   /* (the first entry is always a channel) */
				}
				channel = conn.channels[index];
				if (on != channel) {
					retune(channel);
				}
				if (wait_packet(it, k_uptime_ticks() +
						    k_ms_to_ticks_ceil64(ALTERNATE_WINDOW_MS))) {
					got = true;
					break;
				}
				on = channel;
			}
		}
		if (!got) {
			LOG_INF("Link lost: 500 ms without a packet from the puck");
			conn.reason = osc2_radio_reason_first(conn.reason, OPENSC2_REASON_LINK_LOST);
			link_reason = (enum opensc2_reason)conn.reason;
			next = OPENSC2_LINK_LISTENING;
			break;
		}

		handle_packet(it, &channel);
		queue_pending();
		prev = channel;
	}

	(void)esb_stop_rx();
	if (conn.disconnecting) {
		/* The puck turned us off (or we refused its version): wireless off, not listening. */
		next = OPENSC2_LINK_OFF;
		link_reason = (enum opensc2_reason)conn.reason;
	}
	osc2_output_link_down();
	osc2.last_disconnect_reason = conn.reason;
	entry_reason = link_reason;
	return next;
}

/* ---- the thread ------------------------------------------------------------------------------ */

static void radio_thread(void *start, void *p2, void *p3)
{
	enum opensc2_link_state next = OPENSC2_LINK_OFF;
	uint32_t ctrl = (uintptr_t)start ? OSC2_EV_START : 0;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		switch (next) {
		case OPENSC2_LINK_OFF:
			next = do_off(&ctrl);
			break;
		case OPENSC2_LINK_NO_BOND:
			next = do_no_bond(&ctrl);
			break;
		case OPENSC2_LINK_PAIRING:
			next = do_pairing(&ctrl);
			break;
		case OPENSC2_LINK_LISTENING:
			next = do_listen(&ctrl);
			break;
		case OPENSC2_LINK_CONNECTED:
			next = do_connected(&ctrl);
			break;
		}
		if (rx_overflow) {
			LOG_WRN("%u packets dropped: the radio thread fell behind", rx_overflow);
			rx_overflow = 0;
		}
	}
}

static K_THREAD_STACK_DEFINE(radio_stack, CONFIG_OPENSC2_THREAD_STACK_SIZE);
static struct k_thread radio_thread_data;

int osc2_radio_init(bool start)
{
	memset(info.channels, 0xFF, sizeof(info.channels));
	osc2_rq_init(&extra_q, extra_items, EXTRA_CAP, CONFIG_OPENSC2_REPORT_QUEUE_DEPTH);
	k_thread_create(&radio_thread_data, radio_stack, K_THREAD_STACK_SIZEOF(radio_stack),
			radio_thread, (void *)(uintptr_t)start, NULL, NULL,
			CONFIG_OPENSC2_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&radio_thread_data, "opensc2_radio");
	return 0;
}
