/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * internal.h: what the parts of the library share. Not a public interface.
 */
#ifndef OPENSC2_INTERNAL_H_
#define OPENSC2_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <opensc2/opensc2.h>

/* Report interval of this link, attribute 11 of vendor command 0x83 (as the puck link reports). */
#define OSC2_REPORT_INTERVAL_US 4000

/* Fields of a stored identity override. */
#define OSC2_ID_UNIT      BIT(0)
#define OSC2_ID_BOARD     BIT(1)
#define OSC2_ID_GIT       BIT(2)
#define OSC2_ID_PID       BIT(3)
#define OSC2_ID_REV       BIT(4)
#define OSC2_ID_BUILD     BIT(5)
#define OSC2_ID_BL_BUILD  BIT(6)
#define OSC2_ID_ALL       0x7F

struct osc2_id_override {
	uint8_t mask;
	struct opensc2_identity id;
};

struct osc2_ctx {
	bool initialised;
	const struct opensc2_callbacks *cb;
	void *user_data;

	struct k_spinlock lock;       /* guards everything below that the application can change */
	struct opensc2_state state;
	struct opensc2_battery battery;
	struct opensc2_identity base;       /* Kconfig and opensc2_init() */
	struct osc2_id_override stored;     /* from the settings store */
	struct opensc2_identity identity;   /* what is in use: base + stored */
	struct opensc2_bond bonds[OPENSC2_BOND_SLOTS];
	uint8_t slot;
	int16_t settings[OPENSC2_SETTINGS_COUNT];
	struct opensc2_led led;
	uint8_t chime;
	uint8_t last_disconnect_reason;
	int64_t watchdog_kick_ms;
};

extern struct osc2_ctx osc2;

/* core.c */
void osc2_identity_refresh(void);
void osc2_settings_restore_defaults(void);
void osc2_watchdog_kick(void);
void osc2_notify_led(void);
void osc2_schedule_power(enum opensc2_power_request request, uint32_t delay_ms);

/* storage.c (no-ops without CONFIG_OPENSC2_STORAGE) */
int osc2_storage_load(void);
int osc2_storage_save_bond(uint8_t slot);
int osc2_storage_save_slot(void);
int osc2_storage_save_chime(void);
int osc2_storage_save_identity(void);
int osc2_storage_factory_reset(void);

/* radio.c */
#define OSC2_EV_START   BIT(0)
#define OSC2_EV_STOP    BIT(1)
#define OSC2_EV_PAIR    BIT(2)
#define OSC2_EV_CANCEL  BIT(3)
#define OSC2_EV_BOND    BIT(4)    /* a bond or the slot selection changed */
#define OSC2_EV_RX      BIT(5)    /* packets are waiting */

int osc2_radio_init(bool start);
void osc2_radio_post(uint32_t events);
void osc2_radio_set_pair_seconds(uint16_t seconds);
enum opensc2_link_state osc2_radio_state(void);
void osc2_radio_info(struct opensc2_link_info *info);
bool osc2_radio_connected(void);
/* Extra input reports waiting for the next state reports. */
int osc2_radio_queue_report(const uint8_t *report, size_t len);

/* commands.c: the vendor command channel of the radio link. */
void osc2_commands_reset(void);
/* `rec` is a SET record's data: {opcode, length, payload}. Returns the code for the SET result. */
int32_t osc2_command_set(const uint8_t *rec, uint8_t rec_len);
/* Answer a GET for `opcode` into `out` ({opcode, length, data}); returns the bytes written. */
uint8_t osc2_command_get(uint8_t opcode, uint8_t out[63]);

#endif /* OPENSC2_INTERNAL_H_ */
