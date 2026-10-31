/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * ble.c: Bluetooth LE (CONFIG_OPENSC2_BLE): the board as a Steam Controller 2 over Bluetooth, a
 * HID-over-GATT peripheral, on Nordic's SoftDevice Controller and Zephyr's host. What a host
 * sees follows the protocol documentation's Bluetooth page (not public yet; docs/protocol.md lists
 * the pages; its gap list says where this differs):
 *
 *   - the name "Steam Ctrl (BT) " and the unit serial; five identities, two per slot (identities
 *     1 and 2 for slot 1, 3 and 4 for slot 2, 0 never advertises), each slot bonded to one host
 *     at a time; a pairing request resets the slot's other identity, so the old host keeps its
 *     bond until a new one has paired;
 *   - connectable legacy advertising in three forms: high duty cycle directed to the bonded host,
 *     then allow-listed for it every 100 to 150 ms, or open to any host (pairing mode) while the
 *     identity has no bond; every start arms a 60 s window, after which wireless goes off;
 *   - after a lost link the same identity advertises again 500 ms later; a host that ends the
 *     link (0x13) or powers off (0x15) turns wireless off;
 *   - Just Works bonding with LE Secure Connections; every HID and vendor value needs an
 *     encrypted link;
 *   - OpenSC2's own HID service with the USB device's 372-byte report map: inputs 0x40 to 0x45,
 *     outputs 0x80 to 0x89, feature report 1 (the vendor commands, refusals as ATT errors);
 *     Valve's vendor service mirroring the same reports for hosts that talk GATT themselves; its
 *     own Device Information Service (PnP ID 28DE:1303) and Zephyr's Battery Service;
 *   - reports only once the link is encrypted and its ATT MTU is 64 or more (October's gate);
 *     then Bluetooth is the active transport, and the choice is stored;
 *   - one state report per connection event: the SoftDevice Controller's event-start trigger
 *     fires an EGU interrupt, and the report thread builds the report one interval later, less
 *     500 µs; 0x42 goes out as 0x45, cut after the angular rate.
 *
 * One thread does all Bluetooth work in order (start, stop, advertising, identities, the link's
 * events): the host's callbacks only post messages to its queue, as on the real controller. A
 * second thread sends the reports. The radio is shared with the puck's ESB, one at a time:
 * Bluetooth starts only once ESB is off, and gives the radio back by disabling its stack. MPSL,
 * Nordic's radio scheduler and (in this build) the clock driver for everything, the USB console's
 * crystal included, stays initialised all the time; ESB takes the radio interrupt while it runs,
 * and this file hands the interrupt back to MPSL before the stack starts again.
 */
#include <errno.h>
#include <stdio.h>
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
#include "system.h"
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
BUILD_ASSERT(CONFIG_BT_ID_MAX >= OSC2_BLE_ID_COUNT,
	     "Set CONFIG_BT_ID_MAX=5: identities 1 and 2 are slot 1, 3 and 4 slot 2");
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_SETTINGS),
	     "Set CONFIG_BT_SETTINGS=y: bonds and identities must survive a reboot");
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_ID_UNPAIR_MATCHING_BONDS),
	     "Set CONFIG_BT_ID_UNPAIR_MATCHING_BONDS=y: a host that bonds on one identity loses its "
	     "bond on another (one host, one bond), and the stack would refuse the pairing without it");
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

#define ZERO_REPORT_MS     100    /* the zero report, then the disconnect */
#define RADIO_WAIT_MS      1000   /* for ESB to let go of the radio */
#define DISCONNECT_WAIT_MS 1000
#define POST_WAIT_MS       200    /* a poster waits this long for room in the queue */
#define TEST_DROP_MS       100    /* the test channel's BLE_DROP: its answer goes out first */
#define TX_LIMIT           MAX(1, CONFIG_BT_ATT_TX_COUNT - 2)   /* notifications in flight */
#define FEATURE_CMD_LEN    63     /* {opcode, length, payload}: the value's first 63 bytes */
#define FORGET_MAX_KEYS    64     /* the factory reset deletes at most this many bt/ keys */

/* How an advertising round starts (ring entries name them). */
enum adv_how {
	ADV_START = 0,      /* the transport starts: the slot's identity is picked */
	ADV_AGAIN = 1,      /* 500 ms after a lost link: the same identity, directed first */
	ADV_FALLBACK = 2,   /* after a failed connection: the same identity, allow-listed */
};

/* ---- state (the Bluetooth thread writes it; others read the flags) ---------------------------- */

static struct {
	bool enabled;              /* the stack is enabled */
	bool want;                 /* advertise whenever no host is connected */
	bool pair_request;         /* the next start opens the slot for a new host */
	bool advertising;
	bool wireless_off;         /* the window or the host turned wireless off */
	bool bonded_at_start;      /* the identity in use had a bond when its advertising started */
	bool new_host_done;        /* the bond rule ran for this connection */
	uint8_t slot;              /* the Bluetooth slot, 0 or 1 */
	uint8_t id;                /* the identity in use */
	uint8_t bonded;            /* one bit per identity with a bond (kept by this thread) */
	uint8_t identities;        /* identities that exist */
	uint8_t last_reason;       /* the HCI reason of the last disconnect */
	uint8_t drop_reason;       /* the test channel's BLE_DROP: the reason to act on */
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
static atomic_t forgetting;            /* a factory reset deleted bt/: nothing more until reboot */
static atomic_t deleted_ids;           /* identities whose bond the stack reported deleted */
static volatile bool gate;              /* past the gate: the active transport */
static volatile bool ending;            /* the board is ending the link: no more state reports */
static volatile uint32_t interval_us;
static struct k_spinlock conn_lock;     /* ble.conn for the other threads */

/* The device information strings set at every start. */
static char dis_serial[OPENSC2_SERIAL_LEN + 1];
static char dis_revision[9];

/* ---- the queue of the Bluetooth thread ------------------------------------------------------- */

enum msg_type {
	MSG_START,          /* a: the transport, b: a pairing request */
	MSG_STOP,           /* a: release the radio, b: start the puck's radio after */
	MSG_PAIR,
	MSG_CONNECTED,      /* conn (a reference), a: the HCI error */
	MSG_DISCONNECTED,   /* conn (no reference), a: the reason */
	MSG_SECURITY,       /* conn, a: the level, b: the error */
	MSG_MTU,            /* conn, d: the MTU */
	MSG_PARAMS,         /* conn */
	MSG_PAIRED,         /* conn, a: bonded */
	MSG_READVERTISE,    /* d: the round it belongs to */
	MSG_WINDOW,         /* d: the round whose window ended */
	MSG_BOND_DELETED,   /* the identities are in deleted_ids */
	MSG_TEST_DROP,      /* a: the reason to act on (the test channel's BLE_DROP) */
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

/* Both services' report characteristics point at their entry of osc2_ble_reports, so the two
 * views of a report share these handlers and their storage. */
static const struct osc2_hid_report *report_of(const struct bt_gatt_attr *attr)
{
	return (const struct osc2_hid_report *)attr->user_data;
}

static int index_of(const struct bt_gatt_attr *attr)
{
	return (int)(report_of(attr) - osc2_ble_reports);
}

/* ---- the reports' handlers, shared by the HID and vendor services ---------------------------- */

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

/* ---- the services ----------------------------------------------------------------------------
 * Static services are laid out in the order of their names: Zephyr's GATT and GAP services first,
 * then the battery service ("bas"), then these three, named for the real controller's order: the
 * vendor service, the device information, then the HID service (which the real controller
 * registers at boot, after its fixed ones). With this build's Zephyr services that should give the
 * real controller's handles too: vendor 21 to 61, device information 62 to 74, HID 75 to 140 (not
 * yet read on a host). */

/* ---- Device Information ---- */

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
BT_GATT_SERVICE_DEFINE(osc2_2_dis_svc,
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

/* ---- Valve's vendor service: the same reports as plain characteristics ---- */

#if defined(CONFIG_OPENSC2_BLE_VENDOR_SERVICE)
/* 100F6Cxx-1735-4313-B402-38567131E5F3 (osc2_ble_vendor_uuid() gives the same bytes; a host test
 * checks the rule, a BUILD_ASSERT below the count). */
#define VENDOR_UUID(xx) \
	BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x100F6C00u | (xx), 0x1735, 0x4313, 0xB402, \
					       0x38567131E5F3ULL))

/* Input i (report 0x40 + i): its value and configuration, no report reference. */
#define VENDOR_INPUT(i)                                                                           \
	BT_GATT_CHARACTERISTIC(VENDOR_UUID(OSC2_BLE_VENDOR_OFFSET + 0x40 + (i)),                    \
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, PERM_R, read_input, NULL,  \
			       (void *)&osc2_ble_reports[i]),                                       \
	BT_GATT_CCC(ccc_changed, PERM_RW)

/* Output j (report 0x80 + j): read back, written with or without response. */
#define VENDOR_OUTPUT(j)                                                                          \
	BT_GATT_CHARACTERISTIC(VENDOR_UUID(OSC2_BLE_VENDOR_OFFSET + 0x80 + (j)),                    \
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |                            \
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,                                    \
			       PERM_RW, read_output, write_output,                                 \
			       (void *)&osc2_ble_reports[OSC2_BLE_INPUTS + (j)])

BT_GATT_SERVICE_DEFINE(osc2_1_vendor_svc,
	BT_GATT_PRIMARY_SERVICE(VENDOR_UUID(OSC2_BLE_VENDOR_SERVICE)),
	BT_GATT_CHARACTERISTIC(VENDOR_UUID(OSC2_BLE_VENDOR_COMMAND),
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE, PERM_RW, read_feature,
			       write_feature, (void *)&osc2_ble_reports[16]),
	VENDOR_INPUT(0), VENDOR_INPUT(1), VENDOR_INPUT(2), VENDOR_INPUT(3), VENDOR_INPUT(4),
	VENDOR_INPUT(5),
	VENDOR_OUTPUT(0), VENDOR_OUTPUT(1), VENDOR_OUTPUT(2), VENDOR_OUTPUT(3), VENDOR_OUTPUT(4),
	VENDOR_OUTPUT(5), VENDOR_OUTPUT(6), VENDOR_OUTPUT(7), VENDOR_OUTPUT(8), VENDOR_OUTPUT(9),
);

BUILD_ASSERT(ARRAY_SIZE(attr_osc2_1_vendor_svc) == OSC2_BLE_VENDOR_ATTRS,
	     "the vendor service has 41 attributes");

/* The value attribute of input i (after the service, the command channel's two, three per
 * input). */
#define VENDOR_INPUT_ATTR(i) (&attr_osc2_1_vendor_svc[4 + 3 * (i)])
#endif

/* ---- the HID service ---- */

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

BT_GATT_SERVICE_DEFINE(osc2_3_hid_svc,
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
BUILD_ASSERT(ARRAY_SIZE(attr_osc2_3_hid_svc) == 66, "the HID service has 66 attributes");
BUILD_ASSERT(OSC2_BLE_INPUTS == 6 && OSC2_BLE_OUTPUTS == 10 && OSC2_BLE_REPORTS == 17,
	     "the services list six inputs, ten outputs and one feature report");

/* The value attribute of input i (after the service, protocol mode's two, four per input). */
#define HID_INPUT_ATTR(i) (&attr_osc2_3_hid_svc[4 + 4 * (i)])

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

static int64_t window_end;   /* the uptime the advertising window ends at, 0 when none runs */

void osc2_ble_status(struct osc2_ble_status *st)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	int64_t left = window_end ? window_end - k_uptime_get() : 0;

	memset(st, 0, sizeof(*st));
	st->enabled = ble.enabled;
	st->advertising = ble.advertising;
	st->open = ble.advertising && ble.form == OSC2_BLE_ADV_PAIRING;
	st->connected = ble.conn != NULL;
	st->active = gate;
	st->trigger = ble.trigger;
	st->running = osc2_transport_is_ble(osc2_transport_running());
	st->pair_request = ble.pair_request;
	st->wireless_off = ble.wireless_off;
	st->form = ble.advertising ? (uint8_t)ble.form : OSC2_BLE_ADV_NONE;
	st->slot = ble.slot;
	st->id = ble.id;
	st->bonded = ble.bonded;
	st->identities = ble.identities;
	st->security = ble.security;
	st->last_reason = ble.last_reason;
	st->state = (uint8_t)ble.state;
	st->reason = (uint8_t)ble.reason;
	st->mtu = ble.mtu;
	st->interval_us = interval_us;
	st->window_left_ms = left > 0 ? (uint32_t)left : 0;
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

	enum opensc2_link_state was = ble.state;

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
	/* The system state machine hears of Bluetooth while it is the transport in use: wireless
	 * on again, pairing mode, a new host paired (wireless off: wireless_off()). */
	if (osc2_transport_is_ble(osc2_transport_running())) {
		if (was == OPENSC2_LINK_OFF && state != OPENSC2_LINK_OFF) {
			osc2_sys_bt_event(OSC2_SYS_BT_STARTED);
		}
		if (state == OPENSC2_LINK_PAIRING && was != OPENSC2_LINK_PAIRING) {
			osc2_sys_bt_event(OSC2_SYS_BT_PAIRING);
		}
		if (state == OPENSC2_LINK_CONNECTED && reason == OPENSC2_REASON_PAIRED) {
			osc2_sys_bt_event(OSC2_SYS_BT_PAIRED);
		}
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

/* ---- identities and bonds ----------------------------------------------------------------------- */

struct bond_find {
	bt_addr_le_t addr;
	bool found;
};

static void bond_found(const struct bt_bond_info *info, void *user_data)
{
	struct bond_find *f = user_data;

	if (!f->found) {
		bt_addr_le_copy(&f->addr, &info->addr);
		f->found = true;
	}
}

/* Does identity `id` hold a bond? Its host's address into `peer` (when not NULL). */
static bool id_bond(uint8_t id, bt_addr_le_t *peer)
{
	struct bond_find f = { .found = false };

	if (!ble.enabled) {
		return false;
	}
	bt_foreach_bond(id, bond_found, &f);
	if (f.found && peer) {
		bt_addr_le_copy(peer, &f.addr);
	}
	return f.found;
}

/* The bonds of identities 1 to 4 as a bit mask, kept for the status. */
static void bonds_refresh(void)
{
	uint8_t mask = 0;

	for (uint8_t id = 1; id < OSC2_BLE_ID_COUNT; id++) {
		if (id_bond(id, NULL)) {
			mask |= BIT(id);
		}
	}
	ble.bonded = mask;
}

/* A fresh random address and no keys for identity `id`. The bonds it held are reported deleted
 * by the stack itself (on_bond_deleted()); this reset already covers them. */
static int id_reset(uint8_t id, const char *why)
{
	int err = bt_id_reset(id, NULL, NULL);

	atomic_clear_bit(&deleted_ids, id);
	osc2_test_note_ble(OSC2_TEST_BLE_IDENTITY, err < 0 ? OSC2_TEST_BLE_ID_RESET_FAILED :
			   OSC2_TEST_BLE_ID_RESET, (uint16_t)(err < 0 ? -err : 0), id);
	if (err < 0) {
		LOG_WRN("Bluetooth identity %u not reset (%d)", id, err);
	} else {
		LOG_INF("Bluetooth identity %u reset (%s): a new address, no keys", id, why);
	}
	bonds_refresh();
	return err < 0 ? err : 0;
}

/* The stack reports a deleted bond, by any route (the bond rule, a host bonding on another
 * identity, a reset): that identity is reset too, from the Bluetooth thread. */
static void on_bond_deleted(uint8_t id, const bt_addr_le_t *peer)
{
	ARG_UNUSED(peer);
	if (id == BT_ID_DEFAULT || id >= OSC2_BLE_ID_COUNT) {
		return;
	}
	atomic_set_bit(&deleted_ids, id);
	post(MSG_BOND_DELETED, NULL, id, 0, 0);
}

static void bonds_deleted(void)
{
	for (uint8_t id = 1; id < OSC2_BLE_ID_COUNT; id++) {
		if (!atomic_test_and_clear_bit(&deleted_ids, id)) {
			continue;
		}
		LOG_INF("Bluetooth: the bond of identity %u was deleted", id);
		if (id == ble.id && (ble.advertising || ble.conn)) {
			/* As on the real controller, a reset that cannot be done is only logged. */
			osc2_test_note_ble(OSC2_TEST_BLE_BOND, OSC2_TEST_BLE_BOND_KEPT, 0, id);
			LOG_WRN("Bluetooth identity %u is in use: not reset", id);
			continue;
		}
		osc2_test_note_ble(OSC2_TEST_BLE_BOND, OSC2_TEST_BLE_BOND_DELETED, 0, id);
		(void)id_reset(id, "its bond was deleted");
	}
	bonds_refresh();
}

/* The bond rule, once the link is encrypted: a host that bonded on an identity that had no bond
 * when its advertising started is the slot's new host. The bond on the slot's other identity goes
 * (the stack then reports it deleted, which resets that identity). */
static void new_host_check(void)
{
	uint8_t other;

	if (!ble.conn || ble.new_host_done ||
	    !osc2_ble_new_host(ble.bonded_at_start, id_bond(ble.id, NULL), ble.security)) {
		return;
	}
	ble.new_host_done = true;
	ble.bonded_at_start = true;
	other = osc2_ble_other_id(ble.id);
	osc2_test_note_ble(OSC2_TEST_BLE_BOND, OSC2_TEST_BLE_BOND_NEW_HOST, 0,
			   ble.id | ((uint32_t)other << 8));
	LOG_INF("Bluetooth: a new host on slot %u (identity %u)", ble.slot + 1, ble.id);
	if (id_bond(other, NULL)) {
		LOG_INF("Bluetooth: the slot's other bond (identity %u) is deleted", other);
		(void)bt_unpair(other, NULL);
	}
	bonds_refresh();
	set_state(OPENSC2_LINK_CONNECTED, OPENSC2_REASON_PAIRED);   /* the "paired" moment */
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
	while (count < OSC2_BLE_ID_COUNT) {
		err = bt_id_create(NULL, NULL);
		if (err < 0) {
			LOG_WRN("Bluetooth identity %u not created (%d)", (unsigned int)count, err);
			break;
		}
		LOG_INF("Created Bluetooth identity %d", err);
		osc2_test_note_ble(OSC2_TEST_BLE_IDENTITY, OSC2_TEST_BLE_ID_CREATED, 0, (uint32_t)err);
		count++;
	}
	ble.identities = (uint8_t)count;
	bonds_refresh();
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

/* ---- the advertising window ------------------------------------------------------------------ */

static atomic_t window_round;          /* changes with every start and stop of a window */
static atomic_t window_once_ms;        /* the test channel's BLE_WINDOW: the next window's length */

static void window_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	post(MSG_WINDOW, NULL, 0, 0, (uint32_t)atomic_get(&window_round));
}
static K_WORK_DELAYABLE_DEFINE(window_work, window_fn);

static void window_cancel(void)
{
	atomic_inc(&window_round);
	window_end = 0;
	(void)k_work_cancel_delayable(&window_work);
}

/* Every start of advertising arms the window, whatever its form, even one that failed to start
 * (which then ends in "wireless off" too, as on the real controller). */
static void window_arm(void)
{
	uint32_t ms = (uint32_t)atomic_set(&window_once_ms, 0);

	if (ms == 0) {
		ms = (uint32_t)CONFIG_OPENSC2_BLE_ADV_WINDOW_S * 1000u;
	}
	window_cancel();
	if (ms == 0) {
		return;   /* no window: advertise until a host connects */
	}
	window_end = k_uptime_get() + ms;
	k_work_reschedule(&window_work, K_MSEC(ms));
}

/* ---- advertising -------------------------------------------------------------------------------- */

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
		osc2_test_note_ble(OSC2_TEST_BLE_ADV_END, OSC2_BLE_ADV_END_STOPPED, 0, ble.id);
	}
}

static uint32_t readvertise_round;     /* MSG_READVERTISE of an older round is ignored */

/* One round of advertising for the slot. ADV_START picks the identity (the slot's bonded one, or
 * with a pairing request the other one, reset first); the others keep it. The form follows from
 * its bond: directed to its host first, allow-listed after a failed connection, pairing mode
 * without a bond. Every round arms the 60 s window. */
static int adv_start(enum adv_how how)
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
	bool reset = false;
	bt_addr_le_t peer;
	struct bt_data ad[5], sd[1];
	size_t ad_len, sr_len, n_ad, n_sd;
	bool bonded;
	int err;

	if (atomic_get(&forgetting)) {
		return -ECANCELED;   /* a factory reset deleted bt/: the reboot follows */
	}
	adv_stop();
	readvertise_round++;
	if (how == ADV_START) {
		uint8_t first = osc2_ble_slot_first_id(ble.slot);

		ble.id = osc2_ble_pick_id(ble.slot, id_bond(first, NULL),
					  id_bond((uint8_t)(first + 1), NULL), ble.pair_request,
					  &reset);
		if (reset) {
			ble.pair_request = false;
			(void)id_reset(ble.id, "a pairing request");
		}
	}
	bt_addr_le_copy(&peer, BT_ADDR_LE_ANY);
	bonded = id_bond(ble.id, &peer);
	ble.bonded_at_start = bonded;
	ble.new_host_done = false;
	ble.form = osc2_ble_adv_form(bonded, how == ADV_FALLBACK);
	in.form = ble.form;
	ad_len = osc2_ble_adv_data(&in, ad_raw, sr_raw, &sr_len);
	n_ad = to_bt_data(ad_raw, ad_len, ad, ARRAY_SIZE(ad));
	n_sd = to_bt_data(sr_raw, sr_len, sd, ARRAY_SIZE(sd));
	param.id = ble.id;
	if (ble.form == OSC2_BLE_ADV_DIRECTED) {
		/* High duty cycle: the controller picks the interval and gives up after 1.28 s, which
		 * comes back as a failed connection (0x3C): then allow-listed. */
		param.interval_min = 0;
		param.interval_max = 0;
		param.peer = &peer;
	} else if (ble.form == OSC2_BLE_ADV_ALLOW_LISTED) {
		(void)bt_le_filter_accept_list_clear();
		err = bt_le_filter_accept_list_add(&peer);
		if (err) {
			LOG_WRN("Bonded host not on the accept list (%d)", err);
		}
		param.options |= BT_LE_ADV_OPT_FILTER_CONN | BT_LE_ADV_OPT_FILTER_SCAN_REQ;
	}
	err = bt_le_adv_start(&param, n_ad ? ad : NULL, n_ad, n_sd ? sd : NULL, n_sd);
	window_arm();
	osc2_test_note_ble(OSC2_TEST_BLE_ADV, (uint8_t)ble.form, (uint16_t)(err < 0 ? -err : 0),
			   ble.slot | ((uint32_t)ble.id << 8) | ((uint32_t)how << 16) |
			   ((uint32_t)reset << 24));
	if (err) {
		LOG_ERR("Bluetooth advertising did not start (%d)", err);
	} else {
		ble.advertising = true;
	}
	set_state(ble.form == OSC2_BLE_ADV_PAIRING ? OPENSC2_LINK_PAIRING : OPENSC2_LINK_LISTENING,
		  OPENSC2_REASON_NONE);
	return err;
}

static void readvertise_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	post(MSG_READVERTISE, NULL, 0, 0, readvertise_round);
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
		LOG_WRN("Bluetooth: no connection-event trigger (%d); a timer paces the reports",
			err);
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

/* Notifications in flight. Only the report thread sends them, and it never waits for room:
 * sent_cb() frees it from the system work queue, which may itself wait for a lock the report
 * thread holds while it sends (lizard mode's, the haptics' flush), or for a flash write. */
static atomic_t inflight;

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
	if (k_msgq_num_used_get(&osc2_ble_tx_q) > 0) {
		k_sem_give(&rpt_wake);   /* a report that waits for room goes now */
	}
}

static struct bt_conn *conn_get(void)
{
	struct bt_conn *conn;
	k_spinlock_key_t key = k_spin_lock(&conn_lock);

	conn = ble.conn ? bt_conn_ref(ble.conn) : NULL;
	k_spin_unlock(&conn_lock, key);
	return conn;
}

/* Where input `index` goes for this host: the vendor service's characteristic if the host enabled
 * notifications there, else the HID service's, else nowhere. */
static const struct bt_gatt_attr *input_target(struct bt_conn *conn, int index)
{
	bool vendor = false;

#if defined(CONFIG_OPENSC2_BLE_VENDOR_SERVICE)
	vendor = bt_gatt_is_subscribed(conn, VENDOR_INPUT_ATTR(index), BT_GATT_CCC_NOTIFY);
#endif
	switch (osc2_ble_input_target(vendor, bt_gatt_is_subscribed(conn, HID_INPUT_ATTR(index),
								   BT_GATT_CCC_NOTIFY))) {
#if defined(CONFIG_OPENSC2_BLE_VENDOR_SERVICE)
	case OSC2_BLE_TARGET_VENDOR:
		return VENDOR_INPUT_ATTR(index);
#endif
	case OSC2_BLE_TARGET_HID:
		return HID_INPUT_ATTR(index);
	default:
		return NULL;
	}
}

/* One input report as a notification on its characteristic, where the host enabled them; -ENOMEM
 * when too many are in flight (a droppable one, state or battery, is then dropped; the others
 * never get here without room: they wait in the queue). Never waits. */
static int notify_report(const uint8_t *report, size_t len, bool droppable)
{
	uint8_t pad[64];
	const uint8_t *value;
	uint8_t value_len;
	int index = osc2_ble_input_value(report, len, pad, &value, &value_len);
	struct bt_gatt_notify_params p = { 0 };
	struct bt_conn *conn;
	int err;

	ARG_UNUSED(droppable);   /* the callers decide what waits */
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
	p.attr = input_target(conn, index);
	if (!p.attr) {
		err = -ENOENT;   /* the host did not ask for this report: dropped */
		goto out;
	}
	if (atomic_get(&inflight) >= TX_LIMIT) {
		err = -ENOMEM;
		goto out;
	}
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
	/* The state reports take every free buffer at each connection event, so a battery report
	 * that may be dropped would often be lost (seen on hardware); it waits for room instead. */
	if (report[0] == OPENSC2_RPT_BATTERY) {
		item.droppable = droppable = false;
	}
	if (k_current_get() == &rpt_thread_data &&
	    (droppable || (atomic_get(&inflight) < TX_LIMIT &&
			   k_msgq_num_used_get(&osc2_ble_tx_q) == 0))) {
		/* The state report's own lizard reports: right after it, in order. Without room, or
		 * behind reports that already wait, one that must not be lost waits in the queue
		 * instead (never here: this thread may hold lizard mode's or the haptics' lock). */
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
			if (gate && !ending) {
				send_state();
			}
		}
		/* The queue, in order: a report that must not be lost waits at its head until a
		 * notification completes (sent_cb() wakes this thread); a droppable one goes or is
		 * dropped. Without the link everything goes. */
		while (k_msgq_peek(&osc2_ble_tx_q, &item) == 0) {
			if (gate && !item.droppable && atomic_get(&inflight) >= TX_LIMIT) {
				break;
			}
			(void)k_msgq_get(&osc2_ble_tx_q, &item, K_NO_WAIT);
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
	ending = false;
	ble.trigger = false;
	trigger_set(false);
	atomic_set(&inflight, 0);
	k_msgq_purge(&osc2_ble_tx_q);
}

/* The link is gone: no host any more. */
static void conn_drop(void)
{
	struct bt_conn *conn;
	k_spinlock_key_t key;

	link_reset();
	ble.drop_reason = 0;   /* a BLE_DROP's reason belongs to this link only */
	key = k_spin_lock(&conn_lock);
	conn = ble.conn;
	ble.conn = NULL;
	k_spin_unlock(&conn_lock, key);
	if (conn) {
		bt_conn_unref(conn);
	}
	osc2_output_link_down();
}

/* End the link ourselves: an all-zero 0x45 first when it is past the gate, so the host releases
 * every button, then a disconnect (0x13). Waits for the disconnect. Nothing follows it (the
 * caller decides). */
static void end_link(void)
{
	static const uint8_t zero[OPENSC2_STATE_REPORT_LEN] = { OPENSC2_RPT_STATE };

	if (!ble.conn) {
		return;
	}
	if (gate) {
		/* The state reports stop first (as on USB before it leaves the bus), so the zero
		 * report is the last state the host sees: a state report built meanwhile goes out
		 * before it, none after. */
		ending = true;
		(void)osc2_ble_send(zero, sizeof(zero), false);
		k_msleep(ZERO_REPORT_MS);
	}
	k_sem_reset(&disconnected_sem);
	(void)bt_conn_disconnect(ble.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	(void)k_sem_take(&disconnected_sem, K_MSEC(DISCONNECT_WAIT_MS));
	osc2_test_note_ble(OSC2_TEST_BLE_DISCONNECT, OSC2_HCI_LOCAL_TERM, OSC2_BLE_AFTER_NOTHING, 0);
	conn_drop();
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (err) {
		/* A connection that failed while advertising: 0x3C when directed advertising went
		 * unanswered. The stack sets a connection's role only once one is established, so a
		 * failed one cannot be told by its role (it reads central). With a central role in the
		 * build too (an application's), only that error is advertising's. */
		if (!IS_ENABLED(CONFIG_BT_CENTRAL) || err == BT_HCI_ERR_ADV_TIMEOUT) {
			post(MSG_CONNECTED, NULL, err, 0, 0);
		}
		return;
	}
	if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
		return;
	}
	/* Here, in the host's receive thread, before this host's first request. */
	values_reset();
	post(MSG_CONNECTED, bt_conn_ref(conn), 0, 0, 0);
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
	.bond_deleted = on_bond_deleted,
};

static void interval_update(void)
{
	struct bt_conn_info info;

	if (ble.conn && bt_conn_get_info(ble.conn, &info) == 0) {
		interval_us = info.le.interval_us;
		osc2_test_note_ble(OSC2_TEST_BLE_PARAMS, 0, info.le.latency, interval_us);
		if (osc2_ble_interval_suboptimal(interval_us)) {
			LOG_WRN("Bluetooth: the host set a %u us interval (7500 asked for)",
				interval_us);
		} else {
			LOG_INF("Connection interval %u us", interval_us);
		}
		if (gate && !ble.trigger) {
			k_timer_start(&pace_timer, K_USEC(interval_us), K_USEC(interval_us));
		}
	}
}

/* ---- the Bluetooth thread ------------------------------------------------------------------------ */

/* The power request of a wireless off, through a work item of its own (as usb.c's unplug): the
 * library's single pending power request may hold a host's reboot, bootloader, turn-off or
 * factory reset, asked for just before the host left, which it must not replace. */
static const char *volatile off_why;

static void off_power_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	osc2_power_now(OPENSC2_POWER_WIRELESS_OFF, off_why);
}
static K_WORK_DEFINE(off_power_work, off_power_fn);

/* "Wireless off": the window ran out, or the host ended the link (0x13) or powered off (0x15). The
 * stack stays enabled and nothing advertises until Bluetooth is started again; the application
 * decides the rest (the real controller turns off on battery). */
static void wireless_off(enum osc2_ble_off_cause cause, uint8_t reason)
{
	ble.want = false;
	ble.pair_request = false;
	ble.wireless_off = true;
	(void)k_work_cancel_delayable(&readvertise_work);
	window_cancel();
	adv_stop();
	osc2_test_note_ble(OSC2_TEST_BLE_OFF, (uint8_t)cause, reason, 0);
	LOG_INF("Bluetooth: wireless off (%s)", cause == OSC2_BLE_OFF_WINDOW ?
		"no host within the advertising window" : "the host ended the link");
	set_state(OPENSC2_LINK_OFF, cause == OSC2_BLE_OFF_WINDOW ? OPENSC2_REASON_LISTEN_TIMEOUT :
		  OPENSC2_REASON_HOST_DISCONNECTED);
	/* With the system state machine it decides (on USB power wireless off, on a battery a
	 * shutdown) and takes the power request; without it the application gets it. */
	if (osc2_transport_is_ble(osc2_transport_running())) {
		osc2_sys_bt_event(OSC2_SYS_BT_GAVE_UP);
	}
	off_why = cause == OSC2_BLE_OFF_WINDOW ? "bluetooth: no host within the window" :
						 "bluetooth: the host left";
	(void)k_work_submit(&off_power_work);
}

/* Bluetooth on for `transport` (with a pairing request: the slot opens for a new host). A host
 * connected on the same slot stays, unless pairing was asked for. */
static void do_start(uint8_t transport, bool pair)
{
	uint8_t slot = (uint8_t)osc2_transport_ble_slot(transport);

	ble.want = true;
	ble.wireless_off = false;
	if (pair) {
		ble.pair_request = true;
	}
	if (!ble.enabled && stack_up() != 0) {
		ble.want = false;
		set_state(OPENSC2_LINK_OFF, OPENSC2_REASON_NONE);
		return;
	}
	(void)k_work_cancel_delayable(&readvertise_work);
	if (ble.conn) {
		if (slot == ble.slot && !pair) {
			return;   /* already connected for this slot */
		}
		end_link();   /* another slot, or a new host: leave this one first */
	}
	ble.slot = slot;
	(void)adv_start(ADV_START);
}

static void do_stop(bool release, bool then_radio)
{
	/* Bluetooth already went off by itself ("wireless off": the window, or the host ended the
	 * link). A stop that keeps the stack (the system state machine's drop, which follows that
	 * wireless off at once) changes nothing: the flag and its reason stay for BLE_STATUS, `opensc2
	 * ble status` and the application, and no second OFF is reported. */
	bool already_off = ble.wireless_off && !ble.conn && !ble.advertising &&
			   ble.state == OPENSC2_LINK_OFF;

	(void)k_work_cancel_delayable(&readvertise_work);
	window_cancel();
	if (already_off && !release) {
		ble.want = false;
		ble.pair_request = false;
		return;
	}
	ble.want = false;
	ble.pair_request = false;
	ble.wireless_off = false;
	if (ble.enabled) {
		adv_stop();
		end_link();
	}
	set_state(OPENSC2_LINK_OFF, OPENSC2_REASON_DISABLED);
	if (release) {
		stack_down();
		/* The puck's radio only while it is still the chosen transport: after a switch back to
		 * Bluetooth, queued behind this stop, ESB must stay off (that start takes the radio
		 * again, and would wait for ESB in vain). */
		if (then_radio && !osc2_ble_owns_radio() &&
		    osc2_transport_is_puck(osc2_transport_running())) {
			osc2_radio_post(OSC2_EV_START);
		}
	}
}

static void on_disconnected_msg(const struct ble_msg *m)
{
	uint8_t reason;
	enum osc2_ble_after after;

	if (!ble.conn || m->conn != ble.conn) {
		return;   /* ended by end_link(), or not ours */
	}
	/* The test channel's BLE_DROP ended the link here and names the reason to act on. */
	reason = ble.drop_reason ? ble.drop_reason : m->a;
	ble.drop_reason = 0;
	ble.last_reason = reason;
	after = osc2_ble_after_disconnect(reason, false);
	osc2_test_note_ble(OSC2_TEST_BLE_DISCONNECT, reason, (uint16_t)after, m->a);
	LOG_INF("Bluetooth link ended (reason 0x%02x)", reason);
	conn_drop();
	if (after == OSC2_BLE_AFTER_WIRELESS_OFF) {
		wireless_off(OSC2_BLE_OFF_HOST, reason);
		return;
	}
	set_state(ble.want ? OPENSC2_LINK_LISTENING : OPENSC2_LINK_OFF, OPENSC2_REASON_LINK_LOST);
	if (ble.want) {
		/* The same identity advertises again 500 ms later: directed to its host first. */
		readvertise_round++;
		k_work_reschedule(&readvertise_work, K_MSEC(OSC2_BLE_READVERTISE_MS));
	}
}

static void on_connected_msg(const struct ble_msg *m)
{
	if (m->a) {
		/* A connection that failed while advertising (0x3C: directed advertising unanswered):
		 * the same identity again, allow-listed, or in pairing mode without a bond. */
		LOG_INF("Bluetooth connection failed (0x%02x)", m->a);
		ble.advertising = false;
		window_cancel();
		osc2_test_note_ble(OSC2_TEST_BLE_ADV_END, OSC2_BLE_ADV_END_FAILED, m->a, ble.id);
		if (ble.want && ble.enabled && !ble.conn) {
			(void)adv_start(ADV_FALLBACK);
		}
		return;
	}
	if (ble.conn || !ble.want) {
		bt_conn_disconnect(m->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(m->conn);
		return;
	}
	{
		k_spinlock_key_t key = k_spin_lock(&conn_lock);

		ble.conn = m->conn;
		k_spin_unlock(&conn_lock, key);
	}
	ble.advertising = false;
	window_cancel();
	osc2_test_note_ble(OSC2_TEST_BLE_ADV_END, OSC2_BLE_ADV_END_CONNECTED, 0, ble.id);
	ble.connected_at = k_uptime_get();
	ble.reports = ble.dropped = 0;
	ble.security = (uint8_t)bt_conn_get_security(m->conn);
	ble.mtu = bt_gatt_get_mtu(m->conn);
	link_reset();
	interval_update();
	set_state(OPENSC2_LINK_CONNECTED, OPENSC2_REASON_NONE);
	gate_check();
	new_host_check();
}

static void on_msg(const struct ble_msg *m)
{
	switch (m->type) {
	case MSG_START:
		do_start(m->a, m->b);
		break;
	case MSG_STOP:
		do_stop(m->a, m->b);
		break;
	case MSG_PAIR: {
		/* A pairing request on the running slot: a new start with the flag. The running
		 * transport names the slot, also when Bluetooth has not started yet this boot (an
		 * application that starts it later), when ble.slot is still slot 1's. */
		uint8_t t = osc2_transport_running();

		do_start(osc2_transport_is_ble(t) ? t : (uint8_t)(OSC2_TRANSPORT_BLE_1 + ble.slot),
			 true);
		break;
	}
	case MSG_CONNECTED:
		on_connected_msg(m);
		break;
	case MSG_DISCONNECTED:
		on_disconnected_msg(m);
		break;
	case MSG_READVERTISE:
		if (m->d == readvertise_round && ble.want && ble.enabled && !ble.conn) {
			(void)adv_start(ADV_AGAIN);
		}
		break;
	case MSG_WINDOW:
		/* The round's window, and it has really ended (not one armed again meanwhile). */
		if (m->d == (uint32_t)atomic_get(&window_round) && window_end != 0 &&
		    k_uptime_get() + 5 >= window_end && !ble.conn && ble.want) {
			ble.advertising = false;
			(void)bt_le_adv_stop();
			osc2_test_note_ble(OSC2_TEST_BLE_ADV_END, OSC2_BLE_ADV_END_WINDOW, 0, ble.id);
			wireless_off(OSC2_BLE_OFF_WINDOW, 0);
		}
		break;
	case MSG_BOND_DELETED:
		bonds_deleted();
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
		new_host_check();
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
			LOG_INF("Bluetooth host paired%s", m->a ? " and bonded" : " (no bond)");
			bonds_refresh();
			new_host_check();
		}
		break;
	case MSG_TEST_DROP:
		if (ble.conn) {
			ble.drop_reason = m->a;
			(void)bt_conn_disconnect(ble.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
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

/* ---- the factory reset ---------------------------------------------------------------------- */

struct first_key {
	bool found;
	char name[SETTINGS_MAX_NAME_LEN + 1];
};

static int first_key_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
			void *param)
{
	struct first_key *f = param;

	ARG_UNUSED(len);
	ARG_UNUSED(read_cb);
	ARG_UNUSED(cb_arg);
	f->found = true;
	if (key) {
		snprintf(f->name, sizeof(f->name), "bt/%s", key);
	} else {
		snprintf(f->name, sizeof(f->name), "bt");
	}
	return 1;   /* one at a time: deleted outside the load */
}

int osc2_ble_factory_reset(void)
{
	int err = 0;
	int n = 0;

	/* No more advertising, identity resets or bond deletions: the reboot follows. */
	atomic_set(&forgetting, 1);
	for (; n < FORGET_MAX_KEYS; n++) {
		struct first_key f = { .found = false };

		(void)settings_load_subtree_direct("bt", first_key_cb, &f);
		if (!f.found) {
			break;
		}
		err = settings_delete(f.name);
		if (err) {
			break;
		}
	}
	LOG_INF("Factory reset: %d Bluetooth keys deleted%s", n, err ? " (an error)" : "");
	return err;
}

/* ---- the test channel's Bluetooth ops ---------------------------------------------------------- */

#if defined(CONFIG_OPENSC2_TEST_HOOKS)
static uint8_t drop_reason_next;

static void test_drop_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	post(MSG_TEST_DROP, NULL, drop_reason_next, 0, 0);
}
static K_WORK_DELAYABLE_DEFINE(test_drop_work, test_drop_fn);

uint8_t osc2_ble_test_op(uint8_t op, const uint8_t *args, uint8_t alen, uint8_t *result,
			 uint8_t *rlen)
{
	struct osc2_ble_status st;
	uint32_t ms;

	ARG_UNUSED(alen);
	*rlen = 0;
	switch (op) {
	case OSC2_TEST_OP_BLE_STATUS:
		osc2_ble_status(&st);
		osc2_ble_pack_status(&st, result);
		*rlen = OSC2_BLE_STATUS_LEN;
		return OSC2_TEST_OK;
	case OSC2_TEST_OP_BLE_DROP:
		/* Ends the link from here as if the host had ended it with this reason: 0x13 or 0x15
		 * turn wireless off, any other advertises again 500 ms later. The answer goes out
		 * first (100 ms). */
		if (args[0] == 0) {
			return OSC2_TEST_BAD_ARG;
		}
		if (!ble.conn) {
			return OSC2_TEST_NO_LINK;
		}
		drop_reason_next = args[0];
		k_work_reschedule(&test_drop_work, K_MSEC(TEST_DROP_MS));
		return OSC2_TEST_OK;
	case OSC2_TEST_OP_BLE_WINDOW:
		/* The next advertising window lasts this long (0: back to the configured one). */
		ms = (uint32_t)args[0] | ((uint32_t)args[1] << 8) | ((uint32_t)args[2] << 16) |
		     ((uint32_t)args[3] << 24);
		if (ms != 0 && (ms < OSC2_TEST_BLE_WINDOW_MIN_MS ||
				ms > OSC2_TEST_BLE_WINDOW_MAX_MS)) {
			return OSC2_TEST_BAD_ARG;
		}
		atomic_set(&window_once_ms, (atomic_val_t)ms);
		return OSC2_TEST_OK;
	default:
		return OSC2_TEST_BAD_OP;
	}
}
#endif

/* ---- the library's side --------------------------------------------------------------------------- */

#if defined(CONFIG_OPENSC2_SYSTEM)
/* What the system state machine starts and looks at (system.h). Its start selects the transport
 * (a switch from the puck, another slot, the pairing hold of a transport chord) and runs it, as
 * opensc2_select_transport() does. */
static int sys_bt_start(uint8_t transport, bool pairing)
{
	return pairing ? opensc2_select_transport_pairing((enum opensc2_transport)transport) :
			 opensc2_select_transport((enum opensc2_transport)transport);
}

static void sys_bt_stop(void)
{
	if (osc2_transport_is_ble(osc2_transport_running())) {
		(void)osc2_ble_stop(false, false);   /* the zero report, 100 ms, off */
	}
}

static bool sys_bt_connected(void)
{
	return osc2_ble_active();
}

static bool sys_bt_pairing(void)
{
	return osc2_transport_is_ble(osc2_transport_running()) && ble.state == OPENSC2_LINK_PAIRING;
}

static const struct osc2_bt_transport sys_bt = {
	.start = sys_bt_start,
	.stop = sys_bt_stop,
	.connected = sys_bt_connected,
	.pairing = sys_bt_pairing,
};
#endif

int osc2_ble_init(void)
{
#if defined(CONFIG_OPENSC2_SYSTEM)
	osc2_sys_bluetooth(&sys_bt);   /* the Bluetooth chords, and the machine starts Bluetooth */
#endif
	ble.state = OPENSC2_LINK_OFF;
	ble.form = OSC2_BLE_ADV_NONE;
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

int osc2_ble_start(uint8_t t, bool pair)
{
	if (!osc2_transport_is_ble(t)) {
		return -EINVAL;
	}
	atomic_set(&owns_radio, 1);   /* from now on ESB stays off */
	post(MSG_START, NULL, t, pair, 0);
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
