/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * radio_rules.c: the puck link's rules that need no radio (radio_rules.h). Pure logic.
 */
#include <string.h>
#include "radio_rules.h"

#define CONNECT_REQUEST_LEN 18

bool osc2_radio_control(uint8_t state, uint32_t ev, bool other_slot, uint8_t *next,
			uint8_t *reason)
{
	if (ev & OSC2_RCTL_STOP) {
		*next = OPENSC2_LINK_OFF;
		*reason = OPENSC2_REASON_DISABLED;
		return true;
	}
	switch (state) {
	case OPENSC2_LINK_NO_BOND:
	case OPENSC2_LINK_LISTENING:
		if ((ev & OSC2_RCTL_SLOT) && other_slot) {
			*next = OSC2_RADIO_BOND_OR_NOT;
			*reason = OPENSC2_REASON_BOND_CHANGED;
			return true;
		}
		if (ev & OSC2_RCTL_BOND) {
			/* Listening (again) whatever the bond now is. */
			*next = OPENSC2_LINK_LISTENING;
			*reason = OPENSC2_REASON_BOND_CHANGED;
			return true;
		}
		break;
	case OPENSC2_LINK_CONNECTED:
		if ((ev & OSC2_RCTL_SLOT) && other_slot) {
			*next = OSC2_RADIO_BOND_OR_NOT;
			*reason = OPENSC2_REASON_DISABLED;   /* the puck sees a local stop */
			return true;
		}
		break;   /* a bond write counts from the next time the link listens */
	case OPENSC2_LINK_PAIRING:
		if (ev & OSC2_RCTL_CANCEL) {
			*next = OSC2_RADIO_BOND_OR_NOT;
			*reason = OPENSC2_REASON_PAIRING_FAILED;
			return true;
		}
		return false;   /* a repeated PAIR (a fresh window) is the pairing's own */
	default:
		return false;
	}
	if (ev & OSC2_RCTL_PAIR) {
		*next = OPENSC2_LINK_PAIRING;
		*reason = state == OPENSC2_LINK_CONNECTED ? OPENSC2_REASON_DISABLED :
							     OPENSC2_REASON_NONE;
		return true;
	}
	return false;
}

bool osc2_radio_request_matches(const uint8_t *pkt, size_t len, uint32_t puck_id,
				uint32_t controller_id, struct opensc2_connect_request *req)
{
	if (len != CONNECT_REQUEST_LEN || !opensc2_parse_connect_request(pkt, len, req)) {
		return false;
	}
	return req->puck_id == puck_id && req->controller_id == controller_id;
}

int osc2_radio_walk_next(const uint8_t channels[4], int index)
{
	for (int i = 1; i <= 4; i++) {
		int at = (index + i) % 4;

		if (channels[at] != 0xFF) {
			return at;
		}
	}
	return -1;
}

void osc2_radio_answers_clear(struct osc2_radio_answers *a)
{
	memset(a, 0, sizeof(*a));
}

bool osc2_radio_answers_put_get(struct osc2_radio_answers *a, const uint8_t *answer, uint8_t len)
{
	if (a->get_waiting || len > sizeof(a->get)) {
		return false;
	}
	memcpy(a->get, answer, len);
	a->get_len = len;
	a->get_waiting = true;
	return true;
}

bool osc2_radio_answers_put_set(struct osc2_radio_answers *a, int32_t rc)
{
	if (a->set_waiting) {
		return false;
	}
	a->set_rc = rc;
	a->set_waiting = true;
	return true;
}

uint8_t osc2_radio_answers_next(const struct osc2_radio_answers *a, uint8_t data[63],
				uint8_t *len)
{
	if (a->get_waiting) {
		memcpy(data, a->get, a->get_len);
		*len = a->get_len;
		return OPENSC2_REC_GET_ANSWER;
	}
	if (a->set_waiting) {
		opensc2_put_le32(data, (uint32_t)a->set_rc);
		*len = 4;
		return OPENSC2_REC_SET_RESULT;
	}
	*len = 0;
	return 0;
}

void osc2_radio_answers_sent(struct osc2_radio_answers *a, uint8_t type)
{
	if (type == OPENSC2_REC_GET_ANSWER) {
		a->get_waiting = false;
	} else if (type == OPENSC2_REC_SET_RESULT) {
		a->set_waiting = false;
	}
}

void osc2_slot_lead_init(struct osc2_slot_lead *l, int64_t initial)
{
	l->peak = 0;
	l->worst = initial;
	l->frames = 0;
}

int64_t osc2_slot_lead_wait(const struct osc2_slot_lead *l, int64_t slot, int64_t now)
{
	int64_t lead = l->peak > l->worst ? l->peak : l->worst;

	return slot - now - lead;
}

void osc2_slot_lead_done(struct osc2_slot_lead *l, int64_t took)
{
	if (took > l->peak) {
		l->peak = took;
	}
	if (++l->frames == OSC2_SLOT_LEAD_FRAMES) {
		l->worst = l->peak;
		l->peak = 0;
		l->frames = 0;
	}
}

void osc2_report_sched_clear(struct osc2_report_sched *s)
{
	memset(s, 0, sizeof(*s));
}

void osc2_report_sched_add(struct osc2_report_sched *s, const struct osc2_slot_lead *l,
			   int64_t slot)
{
	if (s->due) {
		s->follows = true;
		s->next_slot = slot;
		return;
	}
	s->due = true;
	s->build_at = osc2_slot_lead_wait(l, slot, 0);   /* the slot less the lead */
}

bool osc2_report_sched_first(const struct osc2_report_sched *s, int64_t *at)
{
	if (!s->due || s->build_at > *at) {
		return false;
	}
	*at = s->build_at;
	return true;
}

void osc2_report_sched_done(struct osc2_report_sched *s, const struct osc2_slot_lead *l)
{
	s->due = s->follows;
	s->follows = false;
	if (s->due) {
		s->build_at = osc2_slot_lead_wait(l, s->next_slot, 0);
	}
}

size_t osc2_radio_output_record(const uint8_t *pkt, size_t pkt_len, const struct opensc2_record *rec,
				uint8_t out[OPENSC2_PAYLOAD_MAX + 1])
{
	size_t after = (size_t)(rec->data - pkt) + rec->len;

	if (rec->len == 0) {
		return 0;
	}
	memcpy(out, rec->data, rec->len);
	out[rec->len] = after < pkt_len ? pkt[after] : 0;
	return (size_t)rec->len + 1;
}
