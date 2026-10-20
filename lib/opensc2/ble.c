/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * ble.c: Bluetooth LE (CONFIG_OPENSC2_BLE): the board as a Steam Controller 2 over Bluetooth, a
 * HID-over-GATT peripheral, on Nordic's SoftDevice Controller and Zephyr's host. What a host
 * sees follows the protocol documentation's Bluetooth page (not public yet; docs/protocol.md lists
 * the pages; its gap list says where this differs):
 *
 *   - the name "Steam Ctrl (BT) " and the unit serial, legacy advertising every 100 to 150 ms:
 *     open to any host (pairing mode) while the slot's identity has no bond, else only for its
 *     bonded host (allow-listed); Valve's company ID in the manufacturer data;
 *   - Just Works bonding with LE Secure Connections; every HID value needs an encrypted link;
 *   - OpenSC2's own HID service with the USB device's 372-byte report map: inputs 0x40 to 0x45,
 *     outputs 0x80 to 0x89, feature report 1 (the vendor commands, refusals as ATT errors); its
 *     own Device Information Service (PnP ID 28DE:1303) and Zephyr's Battery Service;
 *   - reports only once the link is encrypted and its ATT MTU is 64 or more (October's gate);
 *     then Bluetooth is the active transport, and the choice is stored;
 *   - one state report per connection event: the SoftDevice Controller's event-start trigger
 *     fires an EGU interrupt, and the report thread builds the report one interval later, less
 *     500 µs; 0x42 goes out as 0x45, cut after the angular rate.
 *
 * One thread does all Bluetooth work in order (start, stop, advertising, the link's events): the
 * host's callbacks only post messages to its queue, as on the real controller. A second thread
 * sends the reports. The radio is shared with the puck's ESB, one at a time: Bluetooth starts only
 * once ESB is off, and gives the radio back by disabling its stack. MPSL, Nordic's radio scheduler
 * and (in this build) the clock driver for everything, the USB console's crystal included, stays
 * initialised all the time; ESB takes the radio interrupt while it runs, and this file hands the
 * interrupt back to MPSL before the stack starts again.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/bas.h>
#include <bluetooth/hci_vs_sdc.h>
#include <hal/nrf_egu.h>
#include <hal/nrf_radio.h>
#include <mpsl.h>
#include "internal.h"
#include "ble_rules.h"
#include "test_hooks.h"

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

BUILD_ASSERT(CONFIG_BT_L2CAP_TX_MTU >= OSC2_BLE_GATE_MTU &&
	     CONFIG_BT_BUF_ACL_RX_SIZE >= OSC2_BLE_GATE_MTU + 4,
	     "OpenSC2's Bluetooth needs an ATT MTU of 64 or more (a 45-byte state report in one "
	     "notification): set CONFIG_BT_L2CAP_TX_MTU=67 and CONFIG_BT_BUF_ACL_RX_SIZE=71");
BUILD_ASSERT(CONFIG_BT_DEVICE_NAME_MAX >= OSC2_BLE_NAME_MAX,
	     "Set CONFIG_BT_DEVICE_NAME_MAX to 29 or more: the name is 29 characters");
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_DEVICE_NAME_DYNAMIC),
	     "Set CONFIG_BT_DEVICE_NAME_DYNAMIC=y: the name carries the unit serial");
BUILD_ASSERT(CONFIG_BT_ID_MAX >= 5,
	     "Set CONFIG_BT_ID_MAX=5: identities 1 and 2 are slot 1, 3 and 4 slot 2");
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_SETTINGS),
	     "Set CONFIG_BT_SETTINGS=y: bonds and identities must survive a reboot");
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_BAS), "Set CONFIG_BT_BAS=y: the battery service");
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_UNINIT_MPSL_ON_DISABLE),
	     "Leave CONFIG_BT_UNINIT_MPSL_ON_DISABLE off: OpenSC2 keeps MPSL initialised, whose clock "
	     "driver also serves the puck's radio and the USB console");

/* The EGU whose interrupt the connection-event trigger raises: egu3 (SWI3) unless the devicetree
 * chooses another as opensc2,ble-egu. ESB uses egu0, MPSL egu5, the PWM workaround egu4. */
#if DT_HAS_CHOSEN(opensc2_ble_egu)
#define EGU_NODE DT_CHOSEN(opensc2_ble_egu)
#else
#define EGU_NODE DT_NODELABEL(egu3)
#endif
#define EGU      ((NRF_EGU_Type *)DT_REG_ADDR(EGU_NODE))
#define EGU_IRQN DT_IRQN(EGU_NODE)
#define EGU_PRIO 4

#define READVERTISE_MS     500    /* after a lost link */
#define ZERO_REPORT_MS     100    /* the zero report, then the disconnect */
#define RADIO_WAIT_MS      1000   /* for ESB to let go of the radio */
#define DISCONNECT_WAIT_MS 1000
#define POST_WAIT_MS       200    /* a poster waits this long for room in the queue */
#define TX_LIMIT           MAX(1, CONFIG_BT_ATT_TX_COUNT - 2)   /* notifications in flight */
#define FEATURE_CMD_LEN    63     /* {opcode, length, payload}: the value's first 63 bytes */

/* ---- state (the Bluetooth thread writes it; others read the flags) ---------------------------- */

static struct {
	bool enabled;              /* the stack is enabled */
	bool want;                 /* advertise whenever no host is connected */
	bool pair_request;         /* the next advertising is open, for a new host */
	bool advertising;
	uint8_t slot;              /* the Bluetooth slot, 0 or 1 */
	uint8_t id;                /* the identity in use */
	enum osc2_ble_adv_form form;
	int8_t tx_power;
	struct bt_conn *conn;      /* the connected host (a reference) */
	uint8_t security;
	uint16_t mtu;
	bool trigger;              /* the connection-event trigger is set */
	int64_t connected_at;
	enum opensc2_link_state state;
	enum opensc2_reason reason;
	uint32_t reports, dropped;
	char name[OSC2_BLE_NAME_MAX + 1];
} ble;

static atomic_t owns_radio;            /* the stack is (being) enabled: ESB must not start */
static volatile bool gate;              /* past the gate: the active transport */
static volatile uint32_t interval_us;
static struct k_spinlock conn_lock;     /* ble.conn for the other threads */

/* The device information strings set at every start. */
static char dis_serial[OPENSC2_SERIAL_LEN + 1];
static char dis_revision[9];

/* ---- the queue of the Bluetooth thread ------------------------------------------------------- */

enum msg_type {
	MSG_START,          /* a: the transport */
	MSG_STOP,           /* a: release the radio, b: start the puck's radio after */
	MSG_PAIR,
	MSG_CONNECTED,      /* conn (a reference), a: the HCI error */
	MSG_DISCONNECTED,   /* conn (no reference), a: the reason */
	MSG_SECURITY,       /* conn, a: the level, b: the error */
	MSG_MTU,            /* conn, d: the MTU */
	MSG_PARAMS,         /* conn */
	MSG_PAIRED,         /* conn, a: bonded */
	MSG_READVERTISE,
};

struct ble_msg {
	uint8_t type;
	uint8_t a;
	uint8_t b;
	uint8_t pad;
	uint32_t d;
	struct bt_conn *conn;
};

K_MSGQ_DEFINE(osc2_ble_q, sizeof(struct ble_msg), 10, 4);

static void post(uint8_t type, struct bt_conn *conn, uint8_t a, uint8_t b, uint32_t d)
{
	struct ble_msg m = { .type = type, .a = a, .b = b, .d = d, .conn = conn };

	if (k_msgq_put(&osc2_ble_q, &m, K_MSEC(POST_WAIT_MS)) != 0) {
		LOG_WRN("Bluetooth queue full: event %u lost", type);
		if (type == MSG_CONNECTED && conn) {
			bt_conn_unref(conn);
		}
	}
}

/* ---- the reports' storage, per connection ---------------------------------------------------- */

static struct k_spinlock value_lock;
static uint8_t input_last[OSC2_BLE_INPUTS][53];   /* the last value sent on each input */
static uint8_t output_value[OSC2_BLE_OUTPUTS][63];
static uint8_t feature_value[OSC2_BLE_FEATURE_LEN];
static uint8_t feature_answer[FEATURE_CMD_LEN];
static uint8_t protocol_mode = 1;                  /* report protocol */
static uint8_t bas_level = 100;

static void values_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&value_lock);

	memset(input_last, 0, sizeof(input_last));
	k_spin_unlock(&value_lock, key);
	memset(output_value, 0, sizeof(output_value));
	memset(feature_value, 0, sizeof(feature_value));
	memset(feature_answer, 0, sizeof(feature_answer));
	protocol_mode = 1;
	osc2_commands_reset(OSC2_LINK_BLE);
}

static const struct osc2_hid_report *report_of(const struct bt_gatt_attr *attr)
{
	return (const struct osc2_hid_report *)attr->user_data;
}

static int index_of(const struct bt_gatt_attr *attr)
{
	return (int)(report_of(attr) - osc2_ble_reports);
}

/* ---- the HID service ------------------------------------------------------------------------ */

static ssize_t read_protocol_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				  uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &protocol_mode, 1);
}

static ssize_t write_protocol_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *v = buf;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	/* 0 or 1 is kept and read back; nothing acts on it (the service has no boot reports). */
	if (offset == 0 && len == 1 && v[0] <= 1) {
		protocol_mode = v[0];
	}
	return len;
}

static ssize_t read_input(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			  uint16_t len, uint16_t offset)
{
	int i = index_of(attr);
	uint8_t value[53];
	k_spinlock_key_t key = k_spin_lock(&value_lock);

	memcpy(value, input_last[i], sizeof(value));
	k_spin_unlock(&value_lock, key);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, report_of(attr)->size);
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_DBG("Notifications %s", value & BT_GATT_CCC_NOTIFY ? "on" : "off");
}

static ssize_t read_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			       uint16_t len, uint16_t offset)
{
	uint8_t ref[2] = { report_of(attr)->id, report_of(attr)->type };

	return bt_gatt_attr_read(conn, attr, buf, len, offset, ref, sizeof(ref));
}

static ssize_t read_output(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	int j = index_of(attr) - OSC2_BLE_INPUTS;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, output_value[j],
				 report_of(attr)->size);
}

/* An output report: written into its value (a shorter write keeps the bytes after it), then the
 * whole report to the dispatcher every link uses. */
static ssize_t write_output(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			    uint16_t len, uint16_t offset, uint8_t flags)
{
	const struct osc2_hid_report *r = report_of(attr);
	int j = index_of(attr) - OSC2_BLE_INPUTS;
	uint8_t report[1 + 63];
	uint8_t err = osc2_ble_write_check(offset, len, r->size);

	ARG_UNUSED(conn);
	ARG_UNUSED(flags);
	if (err) {
		return BT_GATT_ERR(err);   /* dropped by the stack for a write without response */
	}
	memcpy(&output_value[j][offset], buf, len);
	report[0] = r->id;
	memcpy(&report[1], output_value[j], r->size);
	(void)osc2_output_dispatch(OSC2_LINK_BLE, report, 1u + r->size);
	return len;
}

/* Feature report 1: a write is a SET of the whole value, a read the GET of the stored request.
 * Refusals are ATT error 0x04, the counterpart of a STALL on USB. */
static ssize_t write_feature(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t err = osc2_ble_write_check(offset, len, OSC2_BLE_FEATURE_LEN);

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	if (err) {
		return BT_GATT_ERR(err);
	}
	if (flags & BT_GATT_WRITE_FLAG_CMD) {
		return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);   /* not offered: dropped */
	}
	memcpy(&feature_value[offset], buf, len);
	if (osc2_command_set(OSC2_LINK_BLE, feature_value, FEATURE_CMD_LEN) != 0) {
		return BT_GATT_ERR(OSC2_ATT_ERR_INVALID_PDU);
	}
	return len;
}

static ssize_t read_feature(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	/* The GET runs for a read from the start; a read blob of the rest gets the same answer. */
	if (offset == 0 && osc2_command_get_strict(OSC2_LINK_BLE, feature_answer) < 0) {
		return BT_GATT_ERR(OSC2_ATT_ERR_INVALID_PDU);
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, feature_answer,
				 sizeof(feature_answer));
}

static ssize_t read_map(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, opensc2_hid_report_desc,
				 OPENSC2_HID_REPORT_DESC_LEN);
}

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	/* bcdHID 1.01, country 0, remote wake and normally connectable */
	static const uint8_t info[4] = { 0x01, 0x01, 0x00, 0x03 };

	return bt_gatt_attr_read(conn, attr, buf, len, offset, info, sizeof(info));
}

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(buf);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
	return len;   /* suspend and exit suspend: taken, nothing changes */
}

#define PERM_R  BT_GATT_PERM_READ_ENCRYPT
#define PERM_RW (BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)

/* Each input: the value, its configuration (notifications) and its report reference. */
#define HID_INPUT(i)                                                                              \
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,       \
			       PERM_R, read_input, NULL, (void *)&osc2_ble_reports[i]),            \
	BT_GATT_CCC(ccc_changed, PERM_RW),                                                        \
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, PERM_R, read_report_ref, NULL,                 \
			   (void *)&osc2_ble_reports[i])

/* Each output: the value (read back, written with or without response) and its reference. */
#define HID_OUTPUT(i)                                                                             \
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,                                                \
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |                            \
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,                                    \
			       PERM_RW, read_output, write_output, (void *)&osc2_ble_reports[i]),  \
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, PERM_R, read_report_ref, NULL,                 \
			   (void *)&osc2_ble_reports[i])

BT_GATT_SERVICE_DEFINE(osc2_hid_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP, PERM_RW,
			       read_protocol_mode, write_protocol_mode, NULL),
	HID_INPUT(0), HID_INPUT(1), HID_INPUT(2), HID_INPUT(3), HID_INPUT(4), HID_INPUT(5),
	HID_OUTPUT(6), HID_OUTPUT(7), HID_OUTPUT(8), HID_OUTPUT(9), HID_OUTPUT(10),
	HID_OUTPUT(11), HID_OUTPUT(12), HID_OUTPUT(13), HID_OUTPUT(14), HID_OUTPUT(15),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE, PERM_RW,
			       read_feature, write_feature, (void *)&osc2_ble_reports[16]),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, PERM_R, read_report_ref, NULL,
			   (void *)&osc2_ble_reports[16]),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, PERM_R, read_map, NULL,
			       NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, PERM_R, read_info, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_ctrl_point, NULL),
);

/* The service, as the real controller's: 66 attributes (the service, protocol mode 2, six inputs
 * of 4, ten outputs of 3, the feature report 3, the map 2, the information 2, the control point
 * 2). */
BUILD_ASSERT(ARRAY_SIZE(attr_osc2_hid_svc) == 66, "the HID service has 66 attributes");
BUILD_ASSERT(OSC2_BLE_INPUTS == 6 && OSC2_BLE_OUTPUTS == 10 && OSC2_BLE_REPORTS == 17,
	     "the service lists six inputs, ten outputs and one feature report");

/* The value attribute of input i (after the service, protocol mode's two, four per input). */
#define INPUT_ATTR(i) (&attr_osc2_hid_svc[4 + 4 * (i)])

/* ---- device information ------------------------------------------------------------------------ */

static ssize_t read_string(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	const char *s = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, s, strlen(s));
}

static ssize_t read_pnp(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			uint16_t len, uint16_t offset)
{
	/* vendor ID source 2: the USB-IF */
	static const uint8_t pnp[7] = {
		0x02,
		(uint8_t)CONFIG_OPENSC2_BLE_PNP_VENDOR, (uint8_t)(CONFIG_OPENSC2_BLE_PNP_VENDOR >> 8),
		(uint8_t)CONFIG_OPENSC2_BLE_PNP_PRODUCT, (uint8_t)(CONFIG_OPENSC2_BLE_PNP_PRODUCT >> 8),
		(uint8_t)CONFIG_OPENSC2_BLE_PNP_VERSION, (uint8_t)(CONFIG_OPENSC2_BLE_PNP_VERSION >> 8),
	};

	return bt_gatt_attr_read(conn, attr, buf, len, offset, pnp, sizeof(pnp));
}

/* In the real controller's order; read-only, no encryption needed. */
BT_GATT_SERVICE_DEFINE(osc2_dis_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DIS),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_MODEL_NUMBER, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_string, NULL, (void *)CONFIG_OPENSC2_BLE_MODEL),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_MANUFACTURER_NAME, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_string, NULL, (void *)CONFIG_OPENSC2_BLE_MANUFACTURER),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_PNP_ID, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_pnp,
			       NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_SERIAL_NUMBER, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_string, NULL, dis_serial),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_FIRMWARE_REVISION, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_string, NULL, dis_revision),
	BT_GATT_CHARACTERISTIC(BT_UUID_DIS_SOFTWARE_REVISION, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_string, NULL, dis_revision),
);

/* ---- the link's state, for the application and the ring --------------------------------------- */

static void link_info_locked(struct opensc2_link_info *info)
{
	memset(info, 0, sizeof(*info));
	info->state = ble.state;
	info->reason = ble.reason;
	info->slot = ble.slot;
	info->transport = (uint8_t)(OSC2_TRANSPORT_BLE_1 + ble.slot);
	memset(info->channels, 0xFF, sizeof(info->channels));
	info->reports = ble.reports;
	info->dropped = ble.dropped;
	info->connected_ms = ble.conn ? (uint32_t)(k_uptime_get() - ble.connected_at) : 0;
}

void osc2_ble_info(struct opensc2_link_info *info)
{
	link_info_locked(info);
}

void osc2_ble_status(struct osc2_ble_status *st)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);

	memset(st, 0, sizeof(*st));
	st->enabled = ble.enabled;
	st->advertising = ble.advertising;
	st->open = ble.form == OSC2_BLE_ADV_PAIRING;
	st->connected = ble.conn != NULL;
	st->active = gate;
	st->trigger = ble.trigger;
	st->slot = ble.slot;
	st->id = ble.id;
	st->security = ble.security;
	st->mtu = ble.mtu;
	st->interval_us = interval_us;
	st->reports = ble.reports;
	st->dropped = ble.dropped;
	memcpy(st->name, ble.name, sizeof(ble.name));
	if (ble.enabled) {
		bt_id_get(addrs, &count);
		if (ble.id < count) {
			bt_addr_le_to_str(&addrs[ble.id], st->address, sizeof(st->address));
		}
	}
}

static void set_state(enum opensc2_link_state state, enum opensc2_reason reason)
{
	static const char *const names[] = { "off", "no bond", "pairing mode", "advertising",
					     "connected" };
	struct opensc2_link_info info;

	if (state == ble.state && reason == OPENSC2_REASON_NONE) {
		return;
	}
	ble.state = state;
	ble.reason = reason;
	osc2_test_note_ble(OSC2_TEST_BLE_STATE, (uint8_t)state, (uint16_t)reason,
			   ble.slot | ((uint32_t)ble.id << 8) | ((uint32_t)ble.form << 16));
	LOG_INF("Bluetooth %s (reason %d, slot %u, identity %u)",
		state <= OPENSC2_LINK_CONNECTED ? names[state] : "?", reason, ble.slot + 1, ble.id);
	link_info_locked(&info);
	if (osc2.cb && osc2.cb->link_changed) {
		osc2.cb->link_changed(&info, osc2.user_data);
	}
}

/* ---- the radio: ESB lets go, MPSL takes the interrupt back -------------------------------------- */

static void mpsl_radio_isr(const void *arg)
{
	ARG_UNUSED(arg);
	MPSL_IRQ_RADIO_Handler();
}

/* While ESB ran, its handler had the radio interrupt (both take theirs at run time). MPSL stays
 * initialised, so the interrupt goes back to it here, as MPSL connects it itself (priority 0,
 * zero latency), and the radio is reset to its power-on state. */
static void radio_to_mpsl(void)
{
	irq_disable(RADIO_IRQn);
	nrf_radio_power_set(NRF_RADIO, false);
	nrf_radio_power_set(NRF_RADIO, true);
	irq_connect_dynamic(RADIO_IRQn, MPSL_HIGH_IRQ_PRIORITY, mpsl_radio_isr, NULL,
			    IS_ENABLED(CONFIG_ZERO_LATENCY_IRQS) ? IRQ_ZERO_LATENCY : 0);
	irq_enable(RADIO_IRQn);
}

bool osc2_ble_owns_radio(void)
{
	return atomic_get(&owns_radio) != 0;
}

static int8_t read_tx_power(void)
{
	struct net_buf *rsp = NULL;
	int8_t dbm = 6;

	if (bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_ADV_CHAN_TX_POWER, NULL, &rsp) == 0 && rsp) {
		const struct bt_hci_rp_le_read_chan_tx_power *rp = (const void *)rsp->data;

		dbm = rp->tx_power_level;
	}
	if (rsp) {
		net_buf_unref(rsp);
	}
	return dbm;
}

/* The stack on: the radio first (ESB off), then the stack, its stored bonds and identities, five
 * identities, the name. */
static int stack_up(void)
{
	struct opensc2_identity id;
	size_t count = 0;
	int err;

	atomic_set(&owns_radio, 1);
	for (int ms = 0; !osc2_radio_idle(); ms += 5) {
		if (ms >= RADIO_WAIT_MS) {
			LOG_ERR("The puck's radio did not stop: Bluetooth stays off");
			atomic_set(&owns_radio, 0);
			return -EBUSY;
		}
		k_msleep(5);
	}
	radio_to_mpsl();
	err = bt_enable(NULL);
	if (err && err != -EALREADY) {
		LOG_ERR("Bluetooth did not start (%d)", err);
		atomic_set(&owns_radio, 0);
		return err;
	}
	ble.enabled = true;
	err = settings_load_subtree("bt");
	if (err) {
		LOG_WRN("Bluetooth bonds not loaded (%d)", err);
	}
	bt_id_get(NULL, &count);
	while (count < 5) {
		err = bt_id_create(NULL, NULL);
		if (err < 0) {
			LOG_WRN("Bluetooth identity %u not created (%d)", (unsigned int)count, err);
			break;
		}
		LOG_INF("Created Bluetooth identity %d", err);
		count++;
	}
	/* The name and the device information from the identity in use, at every start. */
	opensc2_identity_get(&id);
	osc2_ble_name(CONFIG_OPENSC2_BLE_NAME_PREFIX, id.unit_serial, ble.name, sizeof(ble.name));
	err = bt_set_name(ble.name);   /* stored only when it changed */
	if (err) {
		LOG_WRN("Bluetooth name not set (%d)", err);
	}
	memcpy(dis_serial, id.unit_serial, OPENSC2_SERIAL_LEN);
	osc2_ble_hex32(id.build_time, dis_revision);
	ble.tx_power = read_tx_power();
	return 0;
}

/* The stack off, so ESB can have the radio. */
static void stack_down(void)
{
	int err;

	if (!ble.enabled) {
		atomic_set(&owns_radio, 0);
		return;
	}
	err = bt_disable();
	if (err) {
		LOG_ERR("Bluetooth did not stop (%d): the radio stays with it", err);
		return;
	}
	ble.enabled = false;
	atomic_set(&owns_radio, 0);
}

/* ---- advertising -------------------------------------------------------------------------------- */

static void first_bond(const struct bt_bond_info *info, void *user_data)
{
	bt_addr_le_t *peer = user_data;

	if (bt_addr_le_eq(peer, BT_ADDR_LE_ANY)) {
		bt_addr_le_copy(peer, &info->addr);
	}
}

/* Raw AD structures into the host's form (the data stays in the buffers given). */
static size_t to_bt_data(const uint8_t *raw, size_t raw_len, struct bt_data *out, size_t max)
{
	size_t pos = 0, n = 0;
	uint8_t type, len;
	const uint8_t *data;

	while (n < max && osc2_ble_ad_next(raw, raw_len, &pos, &type, &data, &len)) {
		out[n].type = type;
		out[n].data_len = len;
		out[n].data = data;
		n++;
	}
	return n;
}

static void adv_stop(void)
{
	if (ble.advertising) {
		(void)bt_le_adv_stop();
		ble.advertising = false;
	}
}

/* Advertise for the slot: its first identity (identity 1 for slot 1, 3 for slot 2); open while it
 * has no bond or a pairing was asked for, else only for its bonded host. */
static int adv_start(void)
{
	static uint8_t ad_raw[OSC2_BLE_AD_MAX], sr_raw[OSC2_BLE_AD_MAX];
	struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN,
							    BT_GAP_ADV_FAST_INT_MIN_2,
							    BT_GAP_ADV_FAST_INT_MAX_2, NULL);
	struct osc2_ble_adv_in in = {
		.tx_power_dbm = ble.tx_power,
		.appearance = CONFIG_BT_DEVICE_APPEARANCE,
		.company_id = CONFIG_OPENSC2_BLE_COMPANY_ID,
		.name = ble.name,
	};
	bt_addr_le_t peer;
	struct bt_data ad[5], sd[1];
	size_t ad_len, sr_len, n_ad, n_sd;
	int err;

	adv_stop();
	ble.id = (uint8_t)(1 + 2 * ble.slot);
	bt_addr_le_copy(&peer, BT_ADDR_LE_ANY);
	bt_foreach_bond(ble.id, first_bond, &peer);
	ble.form = osc2_ble_adv_form(!bt_addr_le_eq(&peer, BT_ADDR_LE_ANY), ble.pair_request);
	in.form = ble.form;
	ad_len = osc2_ble_adv_data(&in, ad_raw, sr_raw, &sr_len);
	n_ad = to_bt_data(ad_raw, ad_len, ad, ARRAY_SIZE(ad));
	n_sd = to_bt_data(sr_raw, sr_len, sd, ARRAY_SIZE(sd));
	param.id = ble.id;
	if (ble.form == OSC2_BLE_ADV_ALLOW_LISTED) {
		(void)bt_le_filter_accept_list_clear();
		err = bt_le_filter_accept_list_add(&peer);
		if (err) {
			LOG_WRN("Bonded host not on the accept list (%d)", err);
		}
		param.options |= BT_LE_ADV_OPT_FILTER_CONN | BT_LE_ADV_OPT_FILTER_SCAN_REQ;
	}
	err = bt_le_adv_start(&param, ad, n_ad, n_sd ? sd : NULL, n_sd);
	if (err) {
		LOG_ERR("Bluetooth advertising did not start (%d)", err);
		set_state(OPENSC2_LINK_OFF, OPENSC2_REASON_NONE);
		return err;
	}
	ble.advertising = true;
	set_state(ble.form == OSC2_BLE_ADV_PAIRING ? OPENSC2_LINK_PAIRING : OPENSC2_LINK_LISTENING,
		  OPENSC2_REASON_NONE);
	return 0;
}

static void readvertise_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	post(MSG_READVERTISE, NULL, 0, 0, 0);
}
static K_WORK_DELAYABLE_DEFINE(readvertise_work, readvertise_fn);

/* ---- report pacing: the connection-event trigger ------------------------------------------------ */

static K_SEM_DEFINE(rpt_wake, 0, 1);
static atomic_t rpt_event;
static int64_t event_ticks;

static void egu_isr(const void *arg)
{
	ARG_UNUSED(arg);
	nrf_egu_event_clear(EGU, NRF_EGU_EVENT_TRIGGERED0);
	event_ticks = k_uptime_ticks();
	atomic_set(&rpt_event, 1);
	k_sem_give(&rpt_wake);
}

/* Without the trigger (it could not be set), a timer at the connection interval paces the
 * reports instead, not locked to the events. */
static void pace_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	event_ticks = k_uptime_ticks();
	atomic_set(&rpt_event, 1);
	k_sem_give(&rpt_wake);
}
static K_TIMER_DEFINE(pace_timer, pace_timer_fn, NULL);

static void trigger_set(bool on)
{
	sdc_hci_cmd_vs_set_event_start_task_t p = {
		.handle_type = SDC_HCI_VS_SET_EVENT_START_TASK_HANDLE_TYPE_CONN,
	};
	uint16_t handle = 0;
	int err;

	k_timer_stop(&pace_timer);
	if (!on) {
		nrf_egu_int_disable(EGU, NRF_EGU_INT_TRIGGERED0);
		if (ble.trigger && ble.conn && bt_hci_get_conn_handle(ble.conn, &handle) == 0) {
			p.handle = handle;
			p.task_address = 0;
			(void)hci_vs_sdc_set_event_start_task(&p);
		}
		ble.trigger = false;
		return;
	}
	err = bt_hci_get_conn_handle(ble.conn, &handle);
	if (err == 0) {
		p.handle = handle;
		p.task_address = nrf_egu_task_address_get(EGU, NRF_EGU_TASK_TRIGGER0);
		nrf_egu_event_clear(EGU, NRF_EGU_EVENT_TRIGGERED0);
		nrf_egu_int_enable(EGU, NRF_EGU_INT_TRIGGERED0);
		err = hci_vs_sdc_set_event_start_task(&p);
	}
	ble.trigger = err == 0;
	osc2_test_note_ble(OSC2_TEST_BLE_TRIGGER, ble.trigger, (uint16_t)err, 0);
	if (err) {
		LOG_WRN("Connection event trigger failed (%d): a timer paces the reports", err);
		nrf_egu_int_disable(EGU, NRF_EGU_INT_TRIGGERED0);
		k_timer_start(&pace_timer, K_USEC(interval_us), K_USEC(interval_us));
	}
}

/* ---- the gate --------------------------------------------------------------------------------- */

static void gate_check(void)
{
	bool now = ble.conn && osc2_ble_gate(ble.security, ble.mtu);

	if (now == gate) {
		return;
	}
	gate = now;
	osc2_test_note_ble(OSC2_TEST_BLE_GATE, now, ble.mtu, interval_us);
	if (now) {
		LOG_INF("Bluetooth active: encrypted, ATT MTU %u, interval %u us", ble.mtu,
			interval_us);
		trigger_set(true);
		osc2_transport_link_up((uint8_t)(OSC2_TRANSPORT_BLE_1 + ble.slot));
	} else {
		trigger_set(false);
	}
}

bool osc2_ble_active(void)
{
	return gate;
}

uint32_t osc2_ble_interval_us(void)
{
	return gate ? interval_us : 0;
}

/* ---- sending ------------------------------------------------------------------------------------ */

static atomic_t inflight;
static K_SEM_DEFINE(tx_room, 0, 1);

struct tx_item {
	uint8_t len;
	uint8_t droppable;
	uint8_t data[64];
};
K_MSGQ_DEFINE(osc2_ble_tx_q, sizeof(struct tx_item), CONFIG_OPENSC2_BLE_TX_QUEUE_DEPTH, 1);

static K_THREAD_STACK_DEFINE(rpt_stack, CONFIG_OPENSC2_BLE_REPORT_THREAD_STACK_SIZE);
static struct k_thread rpt_thread_data;

static void sent_cb(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(user_data);
	if (atomic_dec(&inflight) <= 0) {
		atomic_set(&inflight, 0);
	}
	k_sem_give(&tx_room);
}

static struct bt_conn *conn_get(void)
{
	struct bt_conn *conn;
	k_spinlock_key_t key = k_spin_lock(&conn_lock);

	conn = ble.conn ? bt_conn_ref(ble.conn) : NULL;
	k_spin_unlock(&conn_lock, key);
	return conn;
}

/* One input report as a notification on its characteristic, where the host enabled them. A
 * droppable one (state, battery) is dropped when too many are in flight; the others wait a
 * little for room. */
static int notify_report(const uint8_t *report, size_t len, bool droppable)
{
	uint8_t pad[64];
	const uint8_t *value;
	uint8_t value_len;
	int index = osc2_ble_input_value(report, len, pad, &value, &value_len);
	struct bt_gatt_notify_params p = { 0 };
	struct bt_conn *conn;
	int err;

	if (index < 0) {
		return -EINVAL;
	}
	if (report[0] == OPENSC2_RPT_BATTERY && len >= 3 && report[2] != bas_level) {
		bas_level = report[2];
		(void)bt_bas_set_battery_level(bas_level);   /* notifies its subscribers */
	}
	conn = conn_get();
	if (!conn || !gate) {
		err = -ENOTCONN;
		goto out;
	}
	if (!bt_gatt_is_subscribed(conn, INPUT_ATTR(index), BT_GATT_CCC_NOTIFY)) {
		err = -ENOENT;   /* the host did not ask for this report: dropped */
		goto out;
	}
	while (atomic_get(&inflight) >= TX_LIMIT) {
		if (droppable || k_sem_take(&tx_room, K_MSEC(50)) != 0) {
			err = -ENOMEM;
			goto out;
		}
	}
	p.attr = INPUT_ATTR(index);
	p.data = value;
	p.len = value_len;
	p.func = sent_cb;
	atomic_inc(&inflight);
	err = bt_gatt_notify_cb(conn, &p);
	if (err) {
		atomic_dec(&inflight);
		LOG_DBG("Unable to send report 0x%02x (%d)", report[0], err);
	} else {
		k_spinlock_key_t key = k_spin_lock(&value_lock);

		memcpy(input_last[index], value, value_len);
		k_spin_unlock(&value_lock, key);
	}
out:
	if (conn) {
		bt_conn_unref(conn);
	}
	return err;
}

int osc2_ble_send(const uint8_t *report, size_t len, bool droppable)
{
	struct tx_item item = { .len = (uint8_t)len, .droppable = droppable };

	if (len < 1 || len > sizeof(item.data)) {
		return -EINVAL;
	}
	if (!gate) {
		return -ENOTCONN;
	}
	if (k_current_get() == &rpt_thread_data) {
		/* The state report's own lizard reports: right after it, in order. */
		int err = notify_report(report, len, droppable);

		return err == -ENOENT ? 0 : err;
	}
	memcpy(item.data, report, len);
	if (k_msgq_put(&osc2_ble_tx_q, &item, K_NO_WAIT) != 0) {
		return -ENOMEM;
	}
	k_sem_give(&rpt_wake);
	return 0;
}

/* The state report: sampled, built (the shared sequence number), cut to 0x45; then the frame's
 * lizard reports. */
static void send_state(void)
{
	uint8_t report[OPENSC2_STATE_REPORT_LEN];
	struct opensc2_state s;
	int err;

	osc2_report_state(report, &s);
	err = notify_report(report, sizeof(report), true);
	if (err == 0) {
		ble.reports++;
	} else if (err != -ENOENT) {
		ble.dropped++;
	}
	osc2_report_lizard(&s);
}

static void rpt_thread_fn(void *p1, void *p2, void *p3)
{
	struct tx_item item;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		(void)k_sem_take(&rpt_wake, K_FOREVER);
		if (atomic_cas(&rpt_event, 1, 0) && gate) {
			unsigned int key = irq_lock();
			int64_t at = osc2_ble_slot_ticks(event_ticks, interval_us,
							 CONFIG_OPENSC2_BLE_REPORT_LEAD_US,
							 CONFIG_SYS_CLOCK_TICKS_PER_SEC);

			irq_unlock(key);
			if (at > k_uptime_ticks()) {
				k_sleep(K_TIMEOUT_ABS_TICKS(at));
			}
			if (gate) {
				send_state();
			}
		}
		while (k_msgq_get(&osc2_ble_tx_q, &item, K_NO_WAIT) == 0) {
			if (gate) {
				(void)notify_report(item.data, item.len, item.droppable);
			}
		}
	}
}

/* ---- the link ------------------------------------------------------------------------------------ */

static K_SEM_DEFINE(disconnected_sem, 0, 1);

/* A new link, or one that has ended: the controller forgets an ended link's trigger itself. */
static void link_reset(void)
{
	gate = false;
	ble.trigger = false;
	trigger_set(false);
	atomic_set(&inflight, 0);
	k_msgq_purge(&osc2_ble_tx_q);
}

/* End the link ourselves: an all-zero 0x45 first when it is past the gate, so the host releases
 * every button, then a disconnect (0x13). Waits for the disconnect. */
static void end_link(void)
{
	static const uint8_t zero[OPENSC2_STATE_REPORT_LEN] = { OPENSC2_RPT_STATE };
	struct bt_conn *conn;
	k_spinlock_key_t key;

	if (!ble.conn) {
		return;
	}
	if (gate) {
		(void)osc2_ble_send(zero, sizeof(zero), false);
		k_msleep(ZERO_REPORT_MS);
	}
	k_sem_reset(&disconnected_sem);
	(void)bt_conn_disconnect(ble.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	(void)k_sem_take(&disconnected_sem, K_MSEC(DISCONNECT_WAIT_MS));
	link_reset();
	key = k_spin_lock(&conn_lock);
	conn = ble.conn;
	ble.conn = NULL;
	k_spin_unlock(&conn_lock, key);
	bt_conn_unref(conn);
	osc2_output_link_down();
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
		return;
	}
	if (!err) {
		/* Here, in the host's receive thread, before this host's first request. */
		values_reset();
	}
	post(MSG_CONNECTED, err ? NULL : bt_conn_ref(conn), err, 0, 0);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_sem_give(&disconnected_sem);
	post(MSG_DISCONNECTED, conn, reason, 0, 0);
}

static bool on_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	ARG_UNUSED(conn);
	LOG_INF("The host asks for %u..%u x 1.25 ms, latency %u, timeout %u x 10 ms",
		param->interval_min, param->interval_max, param->latency, param->timeout);
	return true;   /* every request is accepted, as on the real controller */
}

static void on_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(interval);
	ARG_UNUSED(latency);
	ARG_UNUSED(timeout);
	post(MSG_PARAMS, conn, 0, 0, 0);
}

static void on_security(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	post(MSG_SECURITY, conn, (uint8_t)level, (uint8_t)err, 0);
}

BT_CONN_CB_DEFINE(osc2_ble_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.le_param_req = on_param_req,
	.le_param_updated = on_param_updated,
	.security_changed = on_security,
};

static void on_mtu(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(tx);
	ARG_UNUSED(rx);
	post(MSG_MTU, conn, 0, 0, bt_gatt_get_mtu(conn));
}

static struct bt_gatt_cb gatt_cb = {
	.att_mtu_updated = on_mtu,
};

static void on_pairing_complete(struct bt_conn *conn, bool bonded)
{
	post(MSG_PAIRED, conn, bonded, 0, 0);
}

static void on_pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	LOG_WRN("Bluetooth pairing failed (%d)", reason);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = on_pairing_complete,
	.pairing_failed = on_pairing_failed,
};

static void interval_update(void)
{
	struct bt_conn_info info;

	if (ble.conn && bt_conn_get_info(ble.conn, &info) == 0) {
		interval_us = info.le.interval_us;
		osc2_test_note_ble(OSC2_TEST_BLE_PARAMS, 0, info.le.latency, interval_us);
		if (osc2_ble_interval_suboptimal(interval_us)) {
			LOG_WRN("Suboptimal connection interval: %u us (7500 wanted)", interval_us);
		} else {
			LOG_INF("Connection interval %u us", interval_us);
		}
		if (gate && !ble.trigger) {
			k_timer_start(&pace_timer, K_USEC(interval_us), K_USEC(interval_us));
		}
	}
}

/* ---- the Bluetooth thread ------------------------------------------------------------------------ */

static void do_start(uint8_t transport)
{
	uint8_t slot = (uint8_t)osc2_transport_ble_slot(transport);

	ble.want = true;
	if (!ble.enabled && stack_up() != 0) {
		ble.want = false;
		set_state(OPENSC2_LINK_OFF, OPENSC2_REASON_NONE);
		return;
	}
	if (ble.conn) {
		if (slot != ble.slot) {
			ble.slot = slot;
			end_link();   /* another slot: leave this host, then advertise for that one */
			(void)adv_start();
		}
		return;
	}
	ble.slot = slot;
	(void)adv_start();
}

static void do_stop(bool release, bool then_radio)
{
	ble.want = false;
	ble.pair_request = false;
	(void)k_work_cancel_delayable(&readvertise_work);
	if (ble.enabled) {
		adv_stop();
		end_link();
	}
	set_state(OPENSC2_LINK_OFF, OPENSC2_REASON_DISABLED);
	if (release) {
		stack_down();
		if (then_radio && !osc2_ble_owns_radio()) {
			osc2_radio_post(OSC2_EV_START);
		}
	}
}

static void do_pair(void)
{
	/* Pairing mode for the slot: its identity forgets its host and advertises open. (The real
	 * controller pairs on the slot's other identity and keeps the old bond until a new host
	 * has paired; here the slot has one identity so far.) */
	ble.pair_request = true;
	if (!ble.enabled) {
		return;   /* the next start advertises open */
	}
	(void)bt_unpair(ble.id, NULL);   /* also ends a link to that host */
	if (!ble.conn) {
		(void)adv_start();
	}
}

static void on_msg(const struct ble_msg *m)
{
	switch (m->type) {
	case MSG_START:
		do_start(m->a);
		break;
	case MSG_STOP:
		do_stop(m->a, m->b);
		break;
	case MSG_PAIR:
		do_pair();
		break;
	case MSG_CONNECTED:
		if (m->a) {
			/* A connection that failed while advertising: advertise again. */
			LOG_INF("Bluetooth connection failed (0x%02x)", m->a);
			ble.advertising = false;
			if (ble.want && ble.enabled) {
				(void)adv_start();
			}
			break;
		}
		if (ble.conn || !ble.want) {
			bt_conn_disconnect(m->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			bt_conn_unref(m->conn);
			break;
		}
		{
			k_spinlock_key_t key = k_spin_lock(&conn_lock);

			ble.conn = m->conn;
			k_spin_unlock(&conn_lock, key);
		}
		ble.advertising = false;
		ble.connected_at = k_uptime_get();
		ble.reports = ble.dropped = 0;
		ble.security = (uint8_t)bt_conn_get_security(m->conn);
		ble.mtu = bt_gatt_get_mtu(m->conn);
		link_reset();
		interval_update();
		set_state(OPENSC2_LINK_CONNECTED, OPENSC2_REASON_NONE);
		gate_check();
		break;
	case MSG_DISCONNECTED: {
		struct bt_conn *conn;
		k_spinlock_key_t key;

		if (!ble.conn || m->conn != ble.conn) {
			break;   /* ended by end_link(), or not ours */
		}
		osc2_test_note_ble(OSC2_TEST_BLE_DISCONNECT, m->a, 0, 0);
		LOG_INF("Bluetooth link ended (reason 0x%02x)", m->a);
		link_reset();
		key = k_spin_lock(&conn_lock);
		conn = ble.conn;
		ble.conn = NULL;
		k_spin_unlock(&conn_lock, key);
		bt_conn_unref(conn);
		osc2_output_link_down();
		set_state(ble.want ? OPENSC2_LINK_LISTENING : OPENSC2_LINK_OFF,
			  OPENSC2_REASON_LINK_LOST);
		if (ble.want) {
			/* Whatever the reason, the same slot advertises again after 500 ms. */
			k_work_reschedule(&readvertise_work, K_MSEC(READVERTISE_MS));
		}
		break;
	}
	case MSG_READVERTISE:
		if (ble.want && ble.enabled && !ble.conn) {
			(void)adv_start();
		}
		break;
	case MSG_SECURITY:
		if (m->conn != ble.conn) {
			break;
		}
		osc2_test_note_ble(OSC2_TEST_BLE_SECURITY, m->a, m->b, 0);
		if (m->b) {
			LOG_WRN("Bluetooth security failed (%u)", m->b);
		}
		ble.security = (uint8_t)bt_conn_get_security(ble.conn);
		gate_check();
		break;
	case MSG_MTU:
		if (m->conn != ble.conn) {
			break;
		}
		ble.mtu = (uint16_t)m->d;
		osc2_test_note_ble(OSC2_TEST_BLE_MTU, 0, ble.mtu, 0);
		LOG_INF("ATT MTU %u", ble.mtu);
		gate_check();
		break;
	case MSG_PARAMS:
		if (m->conn == ble.conn) {
			interval_update();
		}
		break;
	case MSG_PAIRED:
		if (m->conn == ble.conn) {
			LOG_INF("Bluetooth host paired%s", m->a ? " and bonded" : "");
			ble.pair_request = false;
			set_state(OPENSC2_LINK_CONNECTED, OPENSC2_REASON_PAIRED);
		}
		break;
	default:
		break;
	}
}

static void ble_thread_fn(void *p1, void *p2, void *p3)
{
	struct ble_msg m;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		if (k_msgq_get(&osc2_ble_q, &m, K_FOREVER) == 0) {
			on_msg(&m);
		}
	}
}

static K_THREAD_STACK_DEFINE(ble_stack, CONFIG_OPENSC2_BLE_THREAD_STACK_SIZE);
static struct k_thread ble_thread_data;

/* ---- the library's side --------------------------------------------------------------------------- */

int osc2_ble_init(void)
{
	ble.state = OPENSC2_LINK_OFF;
	IRQ_CONNECT(EGU_IRQN, EGU_PRIO, egu_isr, NULL, 0);
	irq_enable(EGU_IRQN);
	bt_gatt_cb_register(&gatt_cb);
	(void)bt_conn_auth_info_cb_register(&auth_info_cb);
	k_thread_create(&ble_thread_data, ble_stack, K_THREAD_STACK_SIZEOF(ble_stack), ble_thread_fn,
			NULL, NULL, NULL, CONFIG_OPENSC2_BLE_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&ble_thread_data, "opensc2_ble");
	k_thread_create(&rpt_thread_data, rpt_stack, K_THREAD_STACK_SIZEOF(rpt_stack), rpt_thread_fn,
			NULL, NULL, NULL, CONFIG_OPENSC2_BLE_REPORT_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&rpt_thread_data, "opensc2_blerpt");
	return 0;
}

int osc2_ble_start(uint8_t t)
{
	if (!osc2_transport_is_ble(t)) {
		return -EINVAL;
	}
	atomic_set(&owns_radio, 1);   /* from now on ESB stays off */
	post(MSG_START, NULL, t, 0, 0);
	return 0;
}

int osc2_ble_stop(bool release, bool then_radio)
{
	post(MSG_STOP, NULL, release, then_radio, 0);
	return 0;
}

int osc2_ble_pair(void)
{
	post(MSG_PAIR, NULL, 0, 0, 0);
	return 0;
}
