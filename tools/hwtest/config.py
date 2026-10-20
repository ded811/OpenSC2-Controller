# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# config.py: every constant the suite shares in one place: USB IDs, the test-channel key and
# opcode, report layouts, timeouts and thresholds.
# Changing the opcode or key here must match the library (one place each).

# ---- USB identities -------------------------------------------------------------------------
BOARD_VID = 0x28DE
BOARD_PID = 0x1302          # the wired board (and the real controller: told apart by the serial)
PUCK_VID = 0x28DE
PUCK_PID = 0x1304
CONSOLE_VID = 0x2FE3
CONSOLE_PID = 0x0004
CONSOLE_PRODUCT = "OpenSC2 console"
# The wired board's USB product string by default (the real controller's); the board may store
# another one (`opensc2 id set name`), which desc.strings then expects instead.
DEFAULT_PRODUCT_NAME = "Steam Controller"
PRODUCT_NAME_MAX = 31
UF2_VID = 0x239A            # the Adafruit UF2 bootloader (S11): must never appear during a run
REAL_CONTROLLER_PID = 0x1303  # another Valve controller form, always FORBIDDEN

# ---- Bluetooth LE (the opt-in Bluetooth build, --bluetooth) ---------------------------------
# Windows builds a Bluetooth controller's HID device from its Device Information Service's PnP ID
# (USB-IF vendor source 02, 28DE:1303), and names its nodes by its address, e.g.
#   HID\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&0228DE_PID&1303_REV&0100_<12 hex>&COL03\...
#   BTHLEDEVICE\{00001812-...}_DEV_VID&0228DE_PID&1303_REV&0100_<12 hex>\...
#   BTHLE\DEV_<12 hex>\...   (the device, whose name is the advertised one)
# The real controller looks the same: only the board's own address (read from its console) and
# its name with an OSC serial make a node the board's (safety.classify_ble, S1).
BLE_PID = 0x1303
BLE_NAME_PREFIX = "Steam Ctrl (BT) "
# the state report over Bluetooth: 0x45, the first 45 bytes of 0x42 (the quaternion is cut)
BLE_STATE_LEN = 45
BLE_STATE52_KEPT = 44       # state52 bytes a 0x45 carries (after the sequence byte)

# The board's serial: 13 characters of 0-9A-Z beginning with OSC (R1). Hex-only was wrong: it
# refused the Kconfig default and any serial stored with `opensc2 id set unit`.
SERIAL_RE = r"^OSC[0-9A-Z]{10}$"
# The board serial (PCBA) answered by 0xAE tags 0/4 begins with OSB.
BOARD_SERIAL_RE = r"^OSB[0-9A-Z]{10}$"

# ---- the test channel: vendor command 0x7E (docs/test-protocol.md) --------------------------
TEST_OPCODE = 0x7E
TEST_KEY = 0x7E57AB1E           # bytes 1E AB 57 7E on the wire
TEST_KEY_BYTES = bytes([0x1E, 0xAB, 0x57, 0x7E])
TEST_VERSION = 1

# Ops.
OP_ARM = 0x01
OP_DISARM = 0x02
OP_STATUS = 0x03
OP_INJECT = 0x10
OP_INJECT_OFF = 0x11
OP_BATTERY = 0x12
OP_BATTERY_OFF = 0x13
OP_SEQ_LOAD = 0x14
OP_SEQ_RUN = 0x15
OP_SEQ_STOP = 0x16
OP_RULES = 0x17          # the rule word: in effect, the build's, the known bits
OP_RULES_SET = 0x18      # another rule word until the end of arming
OP_RING_READ = 0x20
OP_RING_CLEAR = 0x21
OP_CNT_READ = 0x22
OP_CNT_RESET = 0x23
OP_MARK = 0x24
OP_SETTINGS_READ = 0x30
OP_HOST_OUT = 0x40
OP_HOST_SET = 0x41
OP_HOST_GET = 0x42
OP_APP_SEND = 0x43
OP_RADIO_DEAF = 0x44
OP_BLE_STATUS = 0x60        # Bluetooth (a build with CONFIG_OPENSC2_BLE): the link's state
OP_BLE_DROP = 0x61          # end the link as if the host had, with this HCI reason
OP_BLE_WINDOW = 0x62        # the next advertising window's length, ms
# the calibration's raw inputs (CONFIG_OPENSC2_CAL, caps bit 21)
OP_RAW_INJECT = 0xA0
OP_RAW_OFF = 0xA1
OP_CAL_STATUS = 0xA2
# the system state machine's ops (0x70..0x77; proto/sysproto.py)
OP_SYS_INJECT = 0x70
OP_SYS_INJECT_OFF = 0x71
OP_SYS_STATUS = 0x72
OP_SYS_BOOT = 0x73
OP_SYS_EVENT = 0x74
OP_NAMES = {OP_ARM: "ARM", OP_DISARM: "DISARM", OP_STATUS: "STATUS", OP_INJECT: "INJECT",
            OP_INJECT_OFF: "INJECT_OFF", OP_BATTERY: "BATTERY", OP_BATTERY_OFF: "BATTERY_OFF",
            OP_SEQ_LOAD: "SEQ_LOAD", OP_SEQ_RUN: "SEQ_RUN", OP_SEQ_STOP: "SEQ_STOP",
            OP_RULES: "RULES", OP_RULES_SET: "RULES_SET",
            OP_RING_READ: "RING_READ", OP_RING_CLEAR: "RING_CLEAR", OP_CNT_READ: "CNT_READ",
            OP_CNT_RESET: "CNT_RESET", OP_MARK: "MARK", OP_SETTINGS_READ: "SETTINGS_READ",
            OP_HOST_OUT: "HOST_OUT", OP_HOST_SET: "HOST_SET", OP_HOST_GET: "HOST_GET",
            OP_APP_SEND: "APP_SEND", OP_RADIO_DEAF: "RADIO_DEAF", OP_BLE_STATUS: "BLE_STATUS",
            OP_BLE_DROP: "BLE_DROP", OP_BLE_WINDOW: "BLE_WINDOW", OP_RAW_INJECT: "RAW_INJECT",
            OP_RAW_OFF: "RAW_OFF", OP_CAL_STATUS: "CAL_STATUS",
            OP_SYS_INJECT: "SYS_INJECT", OP_SYS_INJECT_OFF: "SYS_INJECT_OFF",
            OP_SYS_STATUS: "SYS_STATUS", OP_SYS_BOOT: "SYS_BOOT", OP_SYS_EVENT: "SYS_EVENT"}
BLE_STATUS_LEN = 28
BLE_WINDOW_MIN_MS = 100
BLE_WINDOW_MAX_MS = 600000
# RAW_INJECT's inputs (OPENSC2_RAW_*): bit 0 left stick, 1 right stick, 2 left trigger, 3 right
# trigger, 4 left pressure, 5 right pressure, 6 left touch words, 7 right touch words
RAW_STICK_L, RAW_STICK_R, RAW_TRIGGER_L, RAW_TRIGGER_R = 0x01, 0x02, 0x04, 0x08
RAW_PRESSURE_L, RAW_PRESSURE_R, RAW_TOUCH_L, RAW_TOUCH_R = 0x10, 0x20, 0x40, 0x80
RAW_ARGS_LEN = 30
CAL_STATUS_LEN = 16

# Answer status codes (docs/test-protocol.md).
ST_OK = 0
ST_BAD_OP = 1
ST_BAD_LEN = 2
ST_NOT_ARMED = 3
ST_BAD_ARG = 4
ST_BUSY = 5
ST_DENIED = 6
ST_NO_LINK = 7
STATUS_NAMES = {
    ST_OK: "OK", ST_BAD_OP: "BAD_OP", ST_BAD_LEN: "BAD_LEN", ST_NOT_ARMED: "NOT_ARMED",
    ST_BAD_ARG: "BAD_ARG", ST_BUSY: "BUSY", ST_DENIED: "DENIED", ST_NO_LINK: "NO_LINK",
}

# ARM flags.
ARM_FLAG_RESTORE = 0x01     # restore settings and the LED colour at the end of arming (R7)
# INJECT / SEQ_RUN flags.
INJECT_FLAG_STOP_SEQ = 0x01
INJECT_FLAG_LIZARD = 0x02   # the lizard-test flag (R3): only the `lizard` group sets it
SEQ_FLAG_LOOP = 0x01
SEQ_FLAG_KEEP_LAST = 0x02
SEQ_FLAG_LIZARD = 0x04

# ---- ring entry kinds (docs/test-protocol.md, The ring) -------------------------------------
RING_ARMED = 0x01
RING_MARK = 0x02
RING_SEQ_STEP = 0x03
RING_SEQ_END = 0x04
RING_INJECT_STOP = 0x05
RING_OUTPUT = 0x10
RING_RUMBLE = 0x11
RING_HAPTIC = 0x12
RING_LED = 0x13
RING_SETTING = 0x14
RING_LIZARD = 0x15
RING_LIZARD_RPT = 0x16
RING_POWER = 0x17
RING_VENDOR = 0x18
RING_HOST_ASLEEP = 0x19
RING_LINK = 0x20
RING_USB = 0x21
RING_VERSION = 0x22
RING_IDENTITY = 0x23
RING_CHANNELS = 0x24
# 0x30..0x33: protocol completeness
RING_STOP_STATUS = 0x30      # a 0x44 the library sent as the reply to a stop
RING_BLE = 0x40           # a Bluetooth LE link event (BLE_EVENT_*)

# the system state machine's entries (0x50..0x57; proto/sysproto.py)
RING_SYS_STATE = 0x50
RING_LED_MODE = 0x51
RING_CUE = 0x52
RING_SYS_END = 0x53
RING_CHORD = 0x54
RING_SOURCE = 0x55
RING_IDLE = 0x56
# BLE events (docs/test-protocol.md, The ring): a, b, c as each one says.
BLE_EVENT_NAMES = {1: "state", 2: "security", 3: "mtu", 4: "params", 5: "gate", 6: "trigger",
                   7: "disconnect", 8: "adv", 9: "adv_end", 10: "identity", 11: "bond",
                   12: "off"}
BLE_EV_STATE = 1
BLE_EV_SECURITY = 2
BLE_EV_MTU = 3
BLE_EV_PARAMS = 4
BLE_EV_GATE = 5
BLE_EV_TRIGGER = 6
BLE_EV_DISCONNECT = 7
BLE_EV_ADV = 8
BLE_EV_ADV_END = 9
BLE_EV_IDENTITY = 10
BLE_EV_BOND = 11
BLE_EV_OFF = 12
# advertising forms (BLE adv entries, BLE_STATUS)
BLE_FORM_NAMES = {0: "pairing", 1: "allow-listed", 2: "directed", 0xFF: "none"}
# what follows a disconnect (BLE disconnect entries)
BLE_AFTER_NAMES = {0: "advertise again", 1: "wireless off", 2: "nothing"}
RING_LINK_NAMES = {0: "radio", 1: "usb", 2: "test", 3: "none", 4: "ble", 5: "dock"}

# 0x44 channel IDs and the "stopped" flag (docs/api.md, Other input reports)
STATUS_LEFT_GRIP = 0
STATUS_RIGHT_GRIP = 1
STATUS_LEFT_PAD = 3
STATUS_RIGHT_PAD = 4
STATUS_STOPPED = 0x02

# LIZARD "what" values (docs/test-protocol.md, The ring).
LIZARD_LEAVE = 0
LIZARD_ENTER = 1
LIZARD_SWITCHES_DOWN = 2
LIZARD_SWITCHES_UP = 3
LIZARD_RESYNC = 4

# ---- counters, in index order (docs/test-protocol.md, The ring) -----------------------------
COUNTER_NAMES = [
    "frames", "usb_sent", "usb_dropped", "radio_sent", "radio_dropped", "radio_packets",
    "radio_keepalives", "battery_sent", "battery_dropped", "lizard_made", "lizard_diverted",
    "app_reports_sent", "out_reports", "out_short", "vendor_set_ok", "vendor_set_err",
    "vendor_get_ok", "vendor_get_err", "usb_frame_err", "ring_entries", "ring_evicted",
    "radio_rx_overflow", "radio_answers_dropped", "radio_extra_dropped", "test_requests",
    "seq_steps", "usb_skipped",
]
COUNTER_COUNT = len(COUNTER_NAMES)   # 27

# ---- report layouts (docs/api.md, Inputs) ---------------------------------------------------
STATE_REPORT_LEN = 54        # with ID and sequence; state52 is bytes 2..53
STATE52_LEN = 52
BATTERY_REPORT_LEN = 15
HAPTIC_STATUS_LEN = 6        # 0x44
LINK_STATUS_LEN = 2          # 0x79
LINK_QUALITY_LEN = 13        # 0x7B
MOUSE_REPORT_LEN = 6         # 0x40 as the ring records it
KEYBOARD_REPORT_LEN = 9      # 0x41 as the ring records it
PADDED_INPUT_LEN = 54        # Windows pads every vendor input report to this

RPT_MOUSE = 0x40
RPT_KEYBOARD = 0x41
RPT_STATE = 0x42
RPT_BATTERY = 0x43
RPT_HAPTIC_STATUS = 0x44
RPT_BT_STATE = 0x45
RPT_LINK_STATUS = 0x79
RPT_LINK_QUALITY = 0x7B

# ---- feature report 1 (vendor commands) -----------------------------------------------------
FEATURE_ID = 1
FEATURE_PUCK_CMD_ID = 2      # the puck's own commands: NEVER sent to the puck (S4)
FEATURE_LEN = 64
ANSWER_MAX = 61

# vendor opcodes the runner uses or names (docs/api.md, docs/test-protocol.md)
VC_LIZARD_OFF = 0x81
VC_LIZARD_DEFAULTS_ON = 0x85
VC_ATTRIBUTES = 0x83
VC_SETTINGS_SET = 0x87
VC_SETTINGS_LOAD_DEFAULTS = 0x8E
VC_SETTINGS_GET = 0x89
VC_TURN_OFF = 0x9F
VC_RESET_INFO = 0xA1
VC_BOND_WRITE = 0xA2
VC_STRING_ATTR = 0xAE
VC_BATTERY_DATA = 0xBE
VC_LED_SET = 0xC5
VC_USER_STORE_GET = 0xDB
VC_USER_STORE_SET = 0xDC
VC_CALIBRATION = 0xE2
VC_LED_GET = 0xE9
VC_BY_NAME_READ = 0xED
VC_BY_NAME_SET = 0xEE
VC_BY_NAME_SAVE = 0xEF
VC_BY_NAME_DELETE = 0xF0
VC_DEVICE_INFO = 0xF2
VC_FACTORY_RESET = 0x86
VC_BOOTLOADER = 0x90
VC_POWER_REBOOT = 0x95

# The 0x95 keys (never built by the runner except the two safe reboots).
KEY_BOOTLOADER = 0xECAABAC0   # forbidden (the vendor-command denylist, S5)
KEY_FACTORY_RESET = 0xEFC1A917
KEY_REBOOT_WIRELESS = 0xA427AF52   # safe: wired -> wireless (modes.to_wireless)
KEY_TURN_OFF = 0xABDF9710          # allowed on HOST_SET (turn off, wireless only)

# key names (0xED/0xEE/0xEF/0xF0), and the ring's key-ID encoding (docs/test-protocol.md); the
# library's key_rules.c numbers them the same way.
KEY_NAMES = {
    1: "esb/bond", 2: "esb/bond_2", 3: "user/wireless_transport", 4: "user/haptic_boot_level",
    5: "opensc2/unit_serial", 6: "opensc2/board_serial", 7: "opensc2/git_revision",
    8: "opensc2/board_revision",
    9: "user/keylock", 10: "user/fully_charged_led_intensity", 11: "mte/charge_level",
    12: "debug/resetreason", 13: "settings/haptics/haptic_master_gain_db",
    14: "settings/haptics/amplifier_mode", 15: "settings/haptics/enabled",
    16: "cal/joy_l", 17: "cal/joy_r", 18: "cal/trg_l", 19: "cal/trg_r", 20: "cal/prs_l",
    21: "cal/prs_r", 22: "cal/touch_l", 23: "cal/touch_r", 24: "cal/rgbw_r", 25: "cal/rgbw_g",
    26: "cal/rgbw_b", 27: "cal/rgbw_w", 28: "cal/volt_offset", 29: "cal/sensors/gyroscope/bias",
}
# The keys the library keeps as plain values ("blobs", keys.c): name -> (size, 0xED reads it).
# Until set, and after 0xF0, zeros (user/fully_charged_led_intensity: 3). The system state machine
# acts on user/keylock and user/fully_charged_led_intensity.
KEPT_KEYS = {
    "user/keylock": (4, True), "user/fully_charged_led_intensity": (1, True),
    "mte/charge_level": (8, True), "debug/resetreason": (33, True),
    "settings/haptics/haptic_master_gain_db": (2, False),
    "settings/haptics/amplifier_mode": (2, False), "settings/haptics/enabled": (2, False),
}
# The calibration keys and their sizes: S5 never lets the suite write, save or delete one; only
# the forms the firmware refuses before touching anything go out. 0xED reads them: without the
# library's calibration (caps bit 21) empty; with it what CAL_SHOWN says.
CAL_KEYS = {
    "cal/joy_l": 18, "cal/joy_r": 18, "cal/trg_l": 7, "cal/trg_r": 7, "cal/prs_l": 8,
    "cal/prs_r": 8, "cal/touch_l": 10, "cal/touch_r": 10, "cal/rgbw_r": 1, "cal/rgbw_g": 1,
    "cal/rgbw_b": 1, "cal/rgbw_w": 1, "cal/volt_offset": 2, "cal/sensors/gyroscope/bias": 12,
}
# How 0xED shows a calibration key with the library's calibration (cal_store.c): "always", "set"
# (once a calibration is stored: a non-zero first word), "trim" (while it lies within 1..199;
# the defaults do) or "never".
CAL_SHOWN = {
    "cal/joy_l": "set", "cal/joy_r": "set", "cal/trg_l": "set", "cal/trg_r": "set",
    "cal/prs_l": "set", "cal/prs_r": "set", "cal/touch_l": "always", "cal/touch_r": "always",
    "cal/rgbw_r": "trim", "cal/rgbw_g": "trim", "cal/rgbw_b": "trim", "cal/rgbw_w": "trim",
    "cal/volt_offset": "always", "cal/sensors/gyroscope/bias": "never",
}
# A name that is no key at all (0xF0 of it is refused before any flash access).
NO_KEY = "test/no_such_key"

# ---- caps build flags (docs/test-protocol.md) -----------------------------------------------
CAP_TRIGGER_CLICK_AUTO = 0
CAP_LIZARD_MODE = 1
CAP_STEAM_WATCHDOG = 2
CAP_USB = 3
CAP_USB_PROFILE_OCTOBER = 4
CAP_USB_CONSOLE = 5
CAP_USB_WIRED_CONSOLE = 6
CAP_IDENTITY_VENDOR_WRITE = 7
CAP_STORAGE = 8
CAP_USB_LOG = 9
CAP_USB_REMOTE_WAKEUP = 10
CAP_USB_PLUG_IN_REBOOT = 11
CAP_SHELL = 12
CAP_FUSION = 13
CAP_NO_HOST_WIRELESS = 14      # the charger test build (a testing aid, not the real controller)
CAP_RULES = 15                 # the rule word (ops RULES, RULES_SET): the input rules and lizard
                               # mode's processing; without it, OpenSC2 0.2.0's rules
CAP_HAPTIC_STOP_STATUS = 16    # a stop's 0x44 replies (CONFIG_OPENSC2_HAPTIC_STOP_STATUS)
CAP_BLE = 17                   # Bluetooth LE (CONFIG_OPENSC2_BLE)
CAP_SYSTEM = 18                # CONFIG_OPENSC2_SYSTEM: the system state machine and its ops
CAP_CAL = 21                   # CONFIG_OPENSC2_CAL: the cal/ keys, raw inputs, RAW_INJECT

# ---- timings and thresholds -----------------------------------------------------------------
STATE_PERIOD_MS = 4.0283            # USB state report period (about 248/s)
STATE_PERIOD_TOL = 0.005            # +/- 0.5 %
BATTERY_INTERVAL_MS = 3500
WATCHDOG_MS = 10000
RUMBLE_TIMEOUT_MS = 500

# per-path first-report deadline after a new nonce (B: Bluetooth, a Windows host's connection
# interval of up to 30 ms or so, and the HID-over-GATT driver in between)
NONCE_DEADLINE_MS = {"W": 200.0, "R": 500.0, "B": 500.0}
# per-path state cadence gap thresholds
CADENCE_GAP_WARN_MS = {"W": 15.0, "R": 50.0, "B": 100.0}
CADENCE_GAP_FAIL_MS = {"W": None, "R": 200.0, "B": 500.0}
# B: the state report rate must be within this share of 1 s / the connection interval
BLE_RATE_TOL = 0.05
# B: Windows reconnects a paired controller when it advertises again (a reboot, a drop)
BLE_RECONNECT_TIMEOUT_S = 20.0

LEASE_DEFAULT_S = 15
LEASE_KEEPALIVE_S = 3

# feature IOCTL timeout and re-enumeration
FEATURE_IOCTL_TIMEOUT_S = 6.0
PUCK_GET_TIMEOUT_S = 1.5
PUCK_GET_POLL_S = 0.010
CONSOLE_CMD_TIMEOUT_S = 2.0
CONSOLE_SLOW_TIMEOUT_S = 5.0        # raw RING_READ / settings
REENUM_TIMEOUT_S = 15.0
REENUM_FIRST_INSTALL_S = 30.0
GONE_TIMEOUT_S = 2.0
# After a console reboot command: read until the port goes away, at most this long, before
# closing it (a closed port can hold the board's shell in its "rebooting" message)
REBOOT_LEAVE_S = 3.0
RECONNECT_TIMEOUT_S = 5.0
POLL_INTERVAL_S = 0.200
LEAK_WINDOW_READY_S = 2.0

# Windows IOCTLs, checked against hidclass.h: HID_IN_CTL_CODE(100)/HID_OUT_CTL_CODE(100).
IOCTL_HID_GET_FEATURE = 0xB0192
IOCTL_HID_SET_FEATURE = 0xB0191
# Win32 errors the runner knows by name.
ERROR_FILE_NOT_FOUND = 2
ERROR_ACCESS_DENIED = 5
ERROR_SHARING_VIOLATION = 32
ERROR_OPERATION_ABORTED = 995
ERROR_DEVICE_NOT_CONNECTED = 1167
# A feature request that fails with one of these found no device: the board is gone, not a
# STALL (a STALL is ERROR_GEN_FAILURE, 31).
DEVICE_GONE_ERRORS = (ERROR_FILE_NOT_FOUND, 6, ERROR_OPERATION_ABORTED, ERROR_DEVICE_NOT_CONNECTED)
RESET_LOOK_S = 30.0                 # look for the board after an unplanned reset

# Device interface class GUIDs.
GUID_DEVINTERFACE_HID = "{4D1E55B2-F16F-11CF-88CB-001111000030}"
GUID_DEVINTERFACE_COMPORT = "{86E0D1E0-8089-11D0-9CE4-08003E301F73}"

# ---- exit codes (docs/testing.md, How to read the report) -----------------------------------
EXIT_OK = 0
EXIT_TEST_FAILED = 1
EXIT_PRECONDITION = 2
EXIT_ENVIRONMENT = 3
EXIT_USAGE = 4
EXIT_INTERRUPTED = 5
EXIT_NOT_AS_FOUND = 6

SUITE_VERSION = "0.1.0"
