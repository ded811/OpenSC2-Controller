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
 *   NO_BOND    radio on, the selected bond slot is empty
 *   PAIRING    answering a puck's pairing beacon (channel 2, address 91 A2 A7 93)
 *   LISTENING  waiting on channel 2, address "ibex", for an 0xE1 request carrying our bond's IDs
 *   CONNECTED  on the puck's private pipe: keep-alives in, state reports and answers out
 *
 * Timings, packet formats and the order of replies follow the protocol documentation's description
 * of the controller's radio link (not public yet; docs/protocol.md sums it up).
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
#include "report_queue.h"
#include "system.h"
#include "test_hooks.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define LISTEN_POLL_MS        50     /* how often listening looks at control events */
#define PRIMARY_WINDOW_MS     10     /* waiting on the current channel per turn */
#define ALTERNATE_WINDOW_MS   13     /* each alternate channel during an outage */
#define LINK_LOST_MS          500
#define OFFER_LINGER_MS       30     /* pairing: stay on the private address after the offer */
#define ANSWER_DEPTH          4

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
 * CONFIG_OPENSC2_REPORT_QUEUE_DEPTH, then CONFIG_OPENSC2_REPORT_WAIT_DEPTH more that must not be
 * lost, waiting for room. Any thread adds to it; only the radio thread takes from it. */
#define EXTRA_CAP (CONFIG_OPENSC2_REPORT_QUEUE_DEPTH + CONFIG_OPENSC2_REPORT_WAIT_DEPTH)
BUILD_ASSERT(EXTRA_CAP >= 1 && EXTRA_CAP <= 255, "the extra report queue holds 1 to 255 reports");
static struct osc2_rq_item extra_items[EXTRA_CAP];
static struct osc2_rq extra_q;
static struct k_spinlock extra_lock;

/* ---- link state ------------------------------------------------------------------------------ */

static volatile enum opensc2_link_state link_state = OPENSC2_LINK_OFF;
static enum opensc2_reason entry_reason;   /* why the next state is entered */
static uint16_t pair_seconds = CONFIG_OPENSC2_PAIRING_TIMEOUT_S;
static bool radio_on;
static uint32_t rx_overflow;

static struct k_spinlock info_lock;
static struct opensc2_link_info info;      /* readers copy it under info_lock */
static int64_t connected_at;

static struct opensc2_connect_request request;   /* the one that started this connection */

struct answer {
	uint8_t type;
	uint8_t len;
	uint8_t data[63];
};

/* Per connection. The state report, lizard mode, the battery report, rumble and the Steam watchdog
 * are shared with the USB link (report.c, core.c). */
static struct {
	uint8_t channels[4];
	uint8_t slot;
	uint8_t protocol;
	bool version_pending;
	bool identity_pending;
	bool disconnecting;
	enum opensc2_reason disconnect_reason;
	struct answer answers[ANSWER_DEPTH];
	uint8_t answer_head, answer_count;
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

/* The radio needs the 32 MHz crystal. */
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

static int radio_enable(void)
{
	struct esb_config cfg = ESB_DEFAULT_CONFIG;
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
	cfg.protocol = ESB_PROTOCOL_ESB_DPL;
	cfg.mode = ESB_MODE_PRX;
	cfg.event_handler = esb_handler;
	cfg.bitrate = ESB_BITRATE_2MBPS_BLE;
	cfg.crc = ESB_CRC_16BIT;
	cfg.tx_output_power = CONFIG_OPENSC2_TX_POWER_DBM;
	cfg.retransmit_delay = 600;
	cfg.retransmit_count = 0;
	cfg.tx_mode = ESB_TXMODE_AUTO;
	cfg.payload_length = 128;
	cfg.selective_auto_ack = true;
	cfg.use_fast_ramp_up = true;
	err = esb_init(&cfg);
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

/* Change channel and keep the queued acknowledgement payloads. */
static void move_channel(uint8_t channel)
{
	if (channel == info.channel || channel > 100) {
		return;
	}
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
	k_spinlock_key_t key = k_spin_lock(&extra_lock);
	bool backlog = osc2_rq_backlog(&extra_q);

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
	if (!changed) {
		return;
	}
	if (state == OPENSC2_LINK_CONNECTED) {
		osc2_transport_link_up(copy.transport);   /* October saves the choice now */
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

static bool bond_of_slot(uint8_t slot, struct opensc2_bond *bond)
{
	(void)opensc2_bond_get(slot, bond);
	return opensc2_bond_valid(bond);
}

static enum opensc2_link_state with_bond_or_not(void)
{
	struct opensc2_bond bond;

	return bond_of_slot(osc2.slot, &bond) ? OPENSC2_LINK_LISTENING : OPENSC2_LINK_NO_BOND;
}

/* What a control event means in `state`. Returns true when the state must end (*next set). */
static bool control(uint32_t *ctrl, enum opensc2_link_state state, enum opensc2_link_state *next)
{
	uint32_t c = *ctrl;

	*ctrl = 0;
	if (c & OSC2_EV_STOP) {
		entry_reason = OPENSC2_REASON_DISABLED;
		*next = OPENSC2_LINK_OFF;
		return true;
	}
	if (c & OSC2_EV_PAIR) {
		if (state == OPENSC2_LINK_CONNECTED) {
			entry_reason = OPENSC2_REASON_DISABLED;
		}
		*next = OPENSC2_LINK_PAIRING;
		return true;
	}
	if ((c & OSC2_EV_CANCEL) && state == OPENSC2_LINK_PAIRING) {
		entry_reason = OPENSC2_REASON_PAIRING_FAILED;
		*next = with_bond_or_not();
		return true;
	}
	if (c & OSC2_EV_BOND) {
		if (state == OPENSC2_LINK_LISTENING || state == OPENSC2_LINK_NO_BOND) {
			entry_reason = OPENSC2_REASON_BOND_CHANGED;
			*next = with_bond_or_not();
			return true;
		}
		if (state == OPENSC2_LINK_CONNECTED && osc2.slot != conn.slot) {
			/* Another slot was chosen: leave this puck. A rewritten bond of the same
			 * slot only counts from the next connection. */
			entry_reason = OPENSC2_REASON_BOND_CHANGED;
			*next = with_bond_or_not();
			return true;
		}
	}
	return false;
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

static enum opensc2_link_state do_no_bond(uint32_t *ctrl)
{
	enum opensc2_link_state next;
	/* The real controller waits 10 s for a bond, then turns wireless off. */
	k_timeout_t wait = CONFIG_OPENSC2_NO_BOND_TIMEOUT_S ?
			   K_SECONDS(CONFIG_OPENSC2_NO_BOND_TIMEOUT_S) : K_FOREVER;
	int64_t deadline = k_uptime_get() + (int64_t)CONFIG_OPENSC2_NO_BOND_TIMEOUT_S * 1000;

	(void)esb_stop_rx();
	enter(OPENSC2_LINK_NO_BOND);
	for (;;) {
		if (*ctrl && control(ctrl, OPENSC2_LINK_NO_BOND, &next)) {
			if (next != OPENSC2_LINK_NO_BOND) {
				return next;
			}
		}
		if (CONFIG_OPENSC2_NO_BOND_TIMEOUT_S && k_uptime_get() >= deadline) {
			entry_reason = OPENSC2_REASON_NO_BOND_TIMEOUT;
			return OPENSC2_LINK_OFF;
		}
		if (CONFIG_OPENSC2_NO_BOND_TIMEOUT_S) {
			wait = K_MSEC(MAX(deadline - k_uptime_get(), 0));
		}
		*ctrl = k_event_wait_safe(&radio_ev, UINT32_MAX & ~OSC2_EV_RX, false, wait);
	}
}

/* ---- PAIRING ----------------------------------------------------------------------------------
 * The puck's pairing window beacons a hello every 10 ms on channel 2 to address 91 A2 A7 93:
 *  1. we offer 0xF0 {T, unit serial} as the acknowledgement; T is random and becomes a temporary
 *     address, so it avoids the bytes 0x00, 0xAA and 0x55;
 *  2. the puck sends 0xE0 {puck ID, puck serial} to address T; we acknowledge with
 *     0xF4 {C, unit serial}, C being the controller ID both sides store;
 *  3. the bond {puck ID, C, puck serial} is stored and the puck starts connecting.
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
				} else if (control(ctrl, OPENSC2_LINK_PAIRING, &next)) {
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

			if (*ctrl && control(ctrl, OPENSC2_LINK_PAIRING, &next)) {
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

static enum opensc2_link_state do_listen(uint32_t *ctrl)
{
	struct opensc2_bond bond;
	struct rx_item *it = &rx_it;
	enum opensc2_link_state next;
	int64_t deadline = CONFIG_OPENSC2_LISTEN_TIMEOUT_S ?
			   k_uptime_get() + (int64_t)CONFIG_OPENSC2_LISTEN_TIMEOUT_S * 1000 : INT64_MAX;

	if (!bond_of_slot(osc2.slot, &bond)) {
		return OPENSC2_LINK_NO_BOND;
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

		if (*ctrl && control(ctrl, OPENSC2_LINK_LISTENING, &next)) {
			return next;
		}
		if (got && opensc2_parse_connect_request(it->data, it->len, &request) &&
		    request.puck_id == bond.puck_id && request.controller_id == bond.controller_id) {
			conn.slot = osc2.slot;
			return OPENSC2_LINK_CONNECTED;
		}
		if (k_uptime_get() >= deadline) {
			entry_reason = OPENSC2_REASON_LISTEN_TIMEOUT;
			return OPENSC2_LINK_OFF;
		}
	}
}

/* ---- CONNECTED ------------------------------------------------------------------------------- */

static void push_answer(uint8_t type, const uint8_t *data, uint8_t len)
{
	uint8_t at;

	if (conn.answer_count == ANSWER_DEPTH) {
		osc2_test_count(OSC2_TEST_CNT_RADIO_ANSWERS_DROPPED);
		LOG_WRN("Too many feature reports at once: one answer dropped");
		conn.answer_head = (conn.answer_head + 1) % ANSWER_DEPTH;
		conn.answer_count--;
	}
	at = (conn.answer_head + conn.answer_count) % ANSWER_DEPTH;
	conn.answers[at].type = type;
	conn.answers[at].len = len;
	memcpy(conn.answers[at].data, data, len);
	conn.answer_count++;
}

/* An output report from a record. Its handlers on the real controller get the record's length,
 * which counts the report ID too, one more than the bytes after it: the PCM reports, the only
 * ones that read their length, take one byte from beyond the report (0x87 one more sample byte,
 * 0x89 an n one larger), the next record's length byte or whatever the receive buffer still holds
 * there. Every other report has its own length, as on USB. */
static void output_record(const struct rx_item *it, const struct opensc2_record *rec)
{
	uint8_t report[sizeof(it->data) + 1u];
	size_t at = (size_t)(rec->data - it->data) + rec->len;

	if (rec->data[0] != 0x87 && rec->data[0] != 0x89) {
		(void)osc2_output_dispatch(OSC2_LINK_RADIO, rec->data, rec->len);
		return;
	}
	memcpy(report, rec->data, rec->len);
	report[rec->len] = at < sizeof(it->data) ? it->data[at] : 0u;
	(void)osc2_output_dispatch(OSC2_LINK_RADIO, report, (size_t)rec->len + 1u);
}

static void handle_records(const struct rx_item *it)
{
	struct opensc2_record rec;
	size_t pos = 1;
	int r;

	while ((r = opensc2_record_next(it->data, it->len, &pos, &rec)) == 1) {
		switch (rec.type) {
		case OPENSC2_REC_SET_FEATURE: {
			uint8_t code[4];

			opensc2_put_le32(code, (uint32_t)osc2_command_set(OSC2_LINK_RADIO, rec.data,
									  rec.len));
			push_answer(OPENSC2_REC_SET_RESULT, code, sizeof(code));
			break;
		}
		case OPENSC2_REC_GET_FEATURE: {
			/* Answers the last SET, like a GET on USB. The record's byte is not the
			 * opcode: Valve's puck leaves it unset, and drops an answer whose opcode is
			 * not its SET's. */
			uint8_t answer[63];
			uint8_t n = osc2_command_get(OSC2_LINK_RADIO, answer);

			push_answer(OPENSC2_REC_GET_ANSWER, answer, n);
			break;
		}
		case OPENSC2_REC_OUTPUT:
			if (rec.len >= 1) {
				output_record(it, &rec);
			}
			break;
		default:
			break;
		}
	}
	if (r < 0) {
		LOG_DBG("Malformed records in a keep-alive");
	}
}

/* Build the state report the next acknowledgement carries; lizard reports ride behind it (they go
 * into the queue of extra reports, which this link is connected for). */
static void build_state_report(uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	struct opensc2_state s;

	osc2_report_state(report, &s);
	osc2_report_lizard(&s);
}

/* Queue the reply the puck gets with its next packet. */
static void queue_reply(int64_t report_at_ticks)
{
	struct opensc2_payload p;
	const struct osc2_rq_item *extra;
	k_spinlock_key_t key;
	uint8_t report[OPENSC2_STATE_REPORT_LEN];
	uint8_t extras = 0;
	bool with_answer = false;

	if (conn.disconnecting) {
		/* Every packet after "turn off" (or a version we refuse) is answered with 0xF2. */
		opensc2_payload_start(&p, OPENSC2_PKT_DISCONNECTING);
		(void)write_payload(&p);
		return;
	}
	if (conn.version_pending) {
		opensc2_payload_start(&p, OPENSC2_PKT_VERSION_ANSWER);
		(void)opensc2_payload_add_bytes(&p, &conn.protocol, 1);
		if (write_payload(&p) == 0) {
			conn.version_pending = false;
		}
		return;
	}
	if (conn.identity_pending) {
		uint8_t record[OPENSC2_IDENTITY_RECORD_LEN];
		struct opensc2_identity id;

		opensc2_identity_get(&id);
		opensc2_encode_identity(&id, record);
		opensc2_payload_start(&p, OPENSC2_PKT_IDENTITY);
		(void)opensc2_payload_add_bytes(&p, record, sizeof(record));
		if (write_payload(&p) == 0) {
			conn.identity_pending = false;
			osc2_test_note_identity(&id);   /* the numbers and the git revision, no serials */
		}
		return;
	}

	/* A state report, sampled as late as possible: just before the next keep-alive is due. */
	if (report_at_ticks > k_uptime_ticks()) {
		k_sleep(K_TIMEOUT_ABS_TICKS(report_at_ticks));
	}
	build_state_report(report);

	opensc2_payload_start(&p, OPENSC2_PKT_APP_DATA);
	if (conn.answer_count) {
		/* One GET answer or SET result per payload, ahead of the state report. */
		struct answer *a = &conn.answers[conn.answer_head];

		with_answer = opensc2_payload_add_record(&p, a->type, a->data, a->len) == 0;
	}
	(void)opensc2_payload_add_record(&p, OPENSC2_REC_INPUT, report, sizeof(report));
	/* As many extra reports as fit, oldest first; they leave the queue once the payload is
	 * written (only this thread takes from it, others only add at its end). */
	key = k_spin_lock(&extra_lock);
	while ((extra = osc2_rq_peek(&extra_q, extras)) != NULL &&
	       opensc2_payload_add_record(&p, OPENSC2_REC_INPUT, extra->data, extra->len) == 0) {
		extras++;
	}
	k_spin_unlock(&extra_lock, key);

	if (write_payload(&p) != 0) {
		info.dropped++;   /* two payloads are already waiting: this report is dropped */
		osc2_test_count(OSC2_TEST_CNT_RADIO_DROPPED);
		return;
	}
	info.reports++;
	osc2_test_count(OSC2_TEST_CNT_RADIO_SENT);
	if (with_answer) {
		conn.answer_head = (conn.answer_head + 1) % ANSWER_DEPTH;
		conn.answer_count--;
	}
	key = k_spin_lock(&extra_lock);
	osc2_rq_take(&extra_q, extras);
	k_spin_unlock(&extra_lock, key);
}

static void handle_packet(const struct rx_item *it)
{
	int64_t report_at = k_uptime_ticks();
	uint8_t channels[4];
	int n;

	info.packets++;
	info.rssi_dbm = (int8_t)-it->rssi;
	osc2_test_count(OSC2_TEST_CNT_RADIO_PACKETS);
	if (it->len == 0) {
		return;
	}

	switch (it->data[0]) {
	case OPENSC2_PKT_HELLO:
		break;
	case OPENSC2_PKT_KEEPALIVE:
		osc2_test_count(OSC2_TEST_CNT_RADIO_KEEPALIVES);
		handle_records(it);
		report_at = it->ticks + k_us_to_ticks_ceil64(CONFIG_OPENSC2_REPORT_DELAY_US);
		break;
	case OPENSC2_PKT_CHANNEL_CHANGE:
		n = opensc2_parse_channel_change(it->data, it->len, channels);
		if (n > 0) {
			osc2_test_note_channels(channels);
			memcpy(conn.channels, channels, sizeof(conn.channels));
			memcpy(info.channels, channels, sizeof(info.channels));
			LOG_DBG("Channels %u %u %u %u", channels[0], channels[1], channels[2],
				channels[3]);
			move_channel(channels[0]);   /* after our acknowledgement has gone out */
		}
		break;
	case OPENSC2_PKT_TURN_OFF:
		if (!conn.disconnecting) {
			LOG_INF("The puck asks us to turn off");
			conn.disconnecting = true;
			conn.disconnect_reason = OPENSC2_REASON_PUCK_TURN_OFF;
			osc2_schedule_power(OPENSC2_POWER_TURN_OFF_PUCK, 0, "puck: turn off");
		}
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
			if (!conn.disconnecting) {
				conn.disconnecting = true;
				conn.disconnect_reason = OPENSC2_REASON_BAD_VERSION;
			}
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
	queue_reply(report_at);
}

static enum opensc2_link_state do_connected(uint32_t *ctrl)
{
	enum opensc2_link_state next = OPENSC2_LINK_LISTENING;
	enum opensc2_reason reason = OPENSC2_REASON_NONE;
	struct rx_item *it = &rx_it;
	int index = 0;   /* which entry of the channel list we listen on */
	int64_t last_rx;
	k_spinlock_key_t key;

	memset(&conn.channels, 0xFF, sizeof(conn.channels));
	conn.channels[0] = request.channel;
	memcpy(info.channels, conn.channels, sizeof(info.channels));
	conn.protocol = 0;
	conn.version_pending = false;
	conn.identity_pending = true;   /* the identity goes out once per connection */
	conn.disconnecting = false;
	conn.disconnect_reason = OPENSC2_REASON_NONE;
	conn.answer_head = conn.answer_count = 0;
	key = k_spin_lock(&extra_lock);
	osc2_rq_purge(&extra_q);   /* the puck released whatever was held when the link dropped */
	k_spin_unlock(&extra_lock, key);
	osc2_commands_reset(OSC2_LINK_RADIO);

	/* The puck sends its hello on the private pipe 1 ms after the request: be there. */
	listen_on(request.channel, request.address, request.prefix);
	enter(OPENSC2_LINK_CONNECTED);
	last_rx = k_uptime_get();
	(void)osc2_test_take_deaf();   /* one asked for on a link that has gone since */

	for (;;) {
		uint16_t deaf_ms = osc2_test_take_deaf();
		int64_t now;

		if (deaf_ms) {
			/* The hardware test hooks' RADIO_DEAF: hear nothing for a while, so the link
			 * times out through its normal path. What arrived before is not news after. */
			(void)esb_stop_rx();
			k_sleep(K_MSEC(deaf_ms));
			(void)esb_flush_rx();
			k_msgq_purge(&osc2_rx_q);
			(void)esb_start_rx();
		}
		now = k_uptime_get();
		int64_t window = now + (index == 0 ? PRIMARY_WINDOW_MS : ALTERNATE_WINDOW_MS);
		int64_t lost_at = last_rx + LINK_LOST_MS;
		int got = next_packet(it, MIN(window, lost_at), ctrl);

		if (*ctrl) {
			if (control(ctrl, OPENSC2_LINK_CONNECTED, &next)) {
				reason = OPENSC2_REASON_DISABLED;
				break;
			}
			continue;   /* an event that does not concern this link: keep listening */
		}
		if (got) {
			last_rx = k_uptime_get();
			if (index != 0) {
				index = 0;   /* back to the current channel after any packet */
				move_channel(conn.channels[0]);
			}
			handle_packet(it);
		} else if (k_uptime_get() >= lost_at) {
			if (conn.disconnecting) {
				/* The puck stopped after our 0xF2: turned off (or refused). */
				reason = conn.disconnect_reason;
				next = OPENSC2_LINK_OFF;
			} else {
				LOG_INF("Link lost: 500 ms without a packet from the puck");
				reason = OPENSC2_REASON_LINK_LOST;
				next = OPENSC2_LINK_LISTENING;
			}
			break;
		} else {
			/* Nothing on this channel: try the next entry of the list. */
			for (int i = 1; i <= 4; i++) {
				int candidate = (index + i) % 4;

				if (conn.channels[candidate] != 0xFF) {
					index = candidate;
					break;
				}
			}
			move_channel(conn.channels[index]);
		}
	}

	osc2_output_link_down();
	osc2.last_disconnect_reason = (uint8_t)reason;
	if (entry_reason == OPENSC2_REASON_NONE) {
		entry_reason = reason;
	}
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
