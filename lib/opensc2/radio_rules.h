/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * radio_rules.h: the rules of the puck's radio link that need no radio, as pure functions the host
 * tests build (radio.c runs them): what a control event does in each state, the connection
 * request a bond answers, the disconnect reason, the walk over the channel list, the two answers
 * that wait for the next state report, the state report's slot and its lead, and the length an
 * output record hands on. They follow the protocol documentation's description of the real
 * controller's radio link (docs/protocol.md sums it up). No kernel.
 */
#ifndef OPENSC2_RADIO_RULES_H_
#define OPENSC2_RADIO_RULES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <opensc2/opensc2.h>
#include <opensc2/codec.h>

/* ---- control events -------------------------------------------------------------------------
 * The events radio.c's thread waits for (internal.h names them OSC2_EV_*, with these values). */
#define OSC2_RCTL_START   (1u << 0)   /* wireless on (the selected slot) */
#define OSC2_RCTL_STOP    (1u << 1)   /* wireless off */
#define OSC2_RCTL_PAIR    (1u << 2)   /* over-the-air pairing (OpenSC2's) */
#define OSC2_RCTL_CANCEL  (1u << 3)   /* ... cancelled */
#define OSC2_RCTL_BOND    (1u << 4)   /* a bond was written (either slot), as the real one hears it */
#define OSC2_RCTL_SLOT    (1u << 6)   /* another bond slot was chosen at run time (OpenSC2's) */

/* What a control event does: the state to go to, or OSC2_RADIO_BOND_OR_NOT for "listening when
 * the selected slot holds a valid bond, else waiting for one". */
#define OSC2_RADIO_BOND_OR_NOT 0xFE

/* `ev` (OSC2_RCTL_*) in `state` (enum opensc2_link_state; not OFF, which only waits for START or
 * PAIR). Returns true when the state ends, with the next state and the reason it is entered with.
 * `other_slot`: the selected slot is not the one this state runs (OSC2_RCTL_SLOT). As on the real
 * controller:
 *   NO_BOND    a stop ends it (OFF); any bond write goes to LISTENING, also one that leaves the
 *              slot empty (it then listens for a puck with IDs 0 and 0, which none sends);
 *   LISTENING  a stop ends it; any bond write, of either slot, starts listening again (with the
 *              slot's bond as it is now, and a fresh give-up timer);
 *   CONNECTED  a stop ends it (nothing is sent: the puck notices the silence); a bond write is
 *              ignored (the new bond counts from the next time it listens).
 * OpenSC2's own: PAIR goes to PAIRING (CONNECTED leaves the puck first); another slot chosen
 * leaves the state for that slot; in PAIRING a stop ends it and CANCEL gives up (PAIR, a fresh
 * window, is radio.c's). START and CANCEL elsewhere do nothing. */
bool osc2_radio_control(uint8_t state, uint32_t ev, bool other_slot, uint8_t *next,
			uint8_t *reason);

/* ---- connecting ------------------------------------------------------------------------------ */

/* The connection request a listening controller takes: exactly 18 bytes of 0xE1 (the codec's
 * format) whose puck and controller IDs equal the bond's, as they are (a bond that was left
 * empty listens for IDs 0 and 0). */
bool osc2_radio_request_matches(const uint8_t *pkt, size_t len, uint32_t puck_id,
				uint32_t controller_id, struct opensc2_connect_request *req);

/* ---- the disconnect reason (vendor command 0xF2, selector 2) --------------------------------
 * The first reason of a connection stays: a local stop or a lost link after the puck's turn-off
 * (3) or a refused version (4) leaves 3 or 4. */
static inline uint8_t osc2_radio_reason_first(uint8_t now, uint8_t reason)
{
	return now != 0 ? now : reason;
}

/* ---- the channel walk -------------------------------------------------------------------------
 * After 10 ms of silence on the first channel of the list, the controller listens 13 ms on each
 * other entry in turn, skipping 0xFF and coming round to the first one again (13 ms there too),
 * until 500 ms have passed since the turn began. The entry after `index`, or -1 when the list has
 * no channel at all. */
int osc2_radio_walk_next(const uint8_t channels[4], int index);

/* ---- answers waiting for the next state report ---------------------------------------------
 * One GET answer and one SET result wait, each alone: a newer one is dropped while one waits. The
 * next state report carries the GET answer, else the SET result, never both. */
struct osc2_radio_answers {
	bool get_waiting;
	bool set_waiting;
	uint8_t get_len;
	uint8_t get[63];      /* {opcode, length, data} */
	int32_t set_rc;
};

void osc2_radio_answers_clear(struct osc2_radio_answers *a);
/* false: one waits already, this one is dropped. */
bool osc2_radio_answers_put_get(struct osc2_radio_answers *a, const uint8_t *answer, uint8_t len);
bool osc2_radio_answers_put_set(struct osc2_radio_answers *a, int32_t rc);
/* The record the next state report carries: its type (OPENSC2_REC_GET_ANSWER or
 * OPENSC2_REC_SET_RESULT; 0 for none) and its bytes (a SET result as a little-endian s32). */
uint8_t osc2_radio_answers_next(const struct osc2_radio_answers *a, uint8_t data[63],
				uint8_t *len);
/* The record of `type` went out. */
void osc2_radio_answers_sent(struct osc2_radio_answers *a, uint8_t type);

/* ---- the state report's slot -------------------------------------------------------------------
 * A keep-alive sets the slot of the next state report: the moment it is handled plus the slot
 * delay (CONFIG_OPENSC2_REPORT_DELAY_US, 2075 µs on the real controller: 68 ticks of 32768 Hz).
 * The report is built early by the longest time building one took over the last 65 reports
 * (before the first 65: an initial lead of 2472 µs, 81 such ticks, which starts it at once), so
 * it is ready at the slot. Times in any unit, the same for all. */
#define OSC2_SLOT_LEAD_FRAMES 65

struct osc2_slot_lead {
	int64_t peak;     /* the longest build of the current 65 */
	int64_t worst;    /* of the last complete 65, or the initial lead */
	uint8_t frames;
};

void osc2_slot_lead_init(struct osc2_slot_lead *l, int64_t initial);
/* How long to wait from `now` before building the report for `slot` (0 or less: at once). */
int64_t osc2_slot_lead_wait(const struct osc2_slot_lead *l, int64_t slot, int64_t now);
/* A report took `took` to build and send. */
void osc2_slot_lead_done(struct osc2_slot_lead *l, int64_t took);

/* ---- records ----------------------------------------------------------------------------------
 * The length an output record hands to the output reports: the record's length, which counts the
 * report ID, and the byte after the report with it (the real controller's handlers are told the
 * record's length as the length of the bytes after the ID, one more than there are). `out` gets
 * the report and that byte: the byte that follows it in the packet, or 0 past the packet's end.
 * Returns the length to hand on (0 for an empty record). */
size_t osc2_radio_output_record(const uint8_t *pkt, size_t pkt_len, const struct opensc2_record *rec,
				uint8_t out[OPENSC2_PAYLOAD_MAX + 1]);

#endif /* OPENSC2_RADIO_RULES_H_ */
