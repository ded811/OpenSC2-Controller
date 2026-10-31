# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# reports.py: the input report layouts (docs/api.md, Inputs), as pure encoders and decoders. All
# little endian. Offsets count from the report ID byte (offset 0). The library's own codec.c is the
# reference; these mirror it on the PC so the runner can decode what it reads and build state52 to
# inject.
import struct

from .. import config as C


def _u16(b, o):
    return b[o] | (b[o + 1] << 8)


def _s16(b, o):
    v = _u16(b, o)
    return v - 0x10000 if v & 0x8000 else v


def _u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


# ---- state52 and the 0x42 state report ------------------------------------------------------

# field -> (state52 offset, kind): kinds u16, s16, u32
STATE_FIELDS = [
    ("buttons", 0, "u32"),
    ("left_trigger", 4, "u16"), ("right_trigger", 6, "u16"),
    ("lstick_x", 8, "s16"), ("lstick_y", 10, "s16"),
    ("rstick_x", 12, "s16"), ("rstick_y", 14, "s16"),
    ("lpad_x", 16, "s16"), ("lpad_y", 18, "s16"), ("lpad_pressure", 20, "u16"),
    ("rpad_x", 22, "s16"), ("rpad_y", 24, "s16"), ("rpad_pressure", 26, "u16"),
    ("imu_timestamp", 28, "u32"),
    ("accel_x", 32, "s16"), ("accel_y", 34, "s16"), ("accel_z", 36, "s16"),
    ("gyro_x", 38, "s16"), ("gyro_y", 40, "s16"), ("gyro_z", 42, "s16"),
    ("quat_w", 44, "s16"), ("quat_x", 46, "s16"), ("quat_y", 48, "s16"), ("quat_z", 50, "s16"),
]

# button bit names (docs/api.md, The button word), SDL names
BUTTON_BITS = {
    0: "A", 1: "B", 2: "X", 3: "Y", 4: "QAM", 5: "R3", 6: "VIEW", 7: "R4", 8: "R5", 9: "R",
    10: "DPAD_DOWN", 11: "DPAD_RIGHT", 12: "DPAD_LEFT", 13: "DPAD_UP", 14: "MENU", 15: "L3",
    16: "STEAM", 17: "L4", 18: "L5", 19: "L", 20: "RSTICK_TOUCH", 21: "RPAD_TOUCH",
    22: "RPAD_CLICK", 23: "RTRIGGER_DIGITAL", 24: "LSTICK_TOUCH", 25: "LPAD_TOUCH",
    26: "LPAD_CLICK", 27: "LTRIGGER_DIGITAL", 28: "RGRIP", 29: "LGRIP",
}
BIT = {name: bit for bit, name in BUTTON_BITS.items()}
RPAD_TOUCH_BIT = 21
LPAD_TOUCH_BIT = 25


QUAT_ONE = 32767


def make_state52(**fields):
    """Build a 52-byte state52 from named fields (unset = 0). Values are taken as given (no
    clamping): the firmware/model applies the rules. The IMU timestamp is the nonce field."""
    b = bytearray(C.STATE52_LEN)
    off = {n: (o, k) for n, o, k in STATE_FIELDS}
    for name, value in fields.items():
        if name not in off:
            raise KeyError("unknown state52 field %r" % name)
        o, kind = off[name]
        if kind == "u32":
            struct.pack_into("<I", b, o, value & 0xFFFFFFFF)
        else:
            struct.pack_into("<H", b, o, value & 0xFFFF)
    return bytes(b)


# The application's state on a board with nothing wired, as its reports carry it: all zero but the
# quaternion's W, since the library sends the application's "no rotation" as the identity.
APP_IDLE52 = make_state52(quat_w=QUAT_ONE)


def set_field(state52, name, value):
    b = bytearray(state52)
    off = {n: (o, k) for n, o, k in STATE_FIELDS}
    o, kind = off[name]
    if kind == "u32":
        struct.pack_into("<I", b, o, value & 0xFFFFFFFF)
    else:
        struct.pack_into("<H", b, o, value & 0xFFFF)
    return bytes(b)


def decode_state52(state52):
    """Decode a 52-byte state52 into a dict of fields (buttons as int, others signed/unsigned)."""
    out = {}
    for name, o, kind in STATE_FIELDS:
        if kind == "u32":
            out[name] = _u32(state52, o)
        elif kind == "s16":
            out[name] = _s16(state52, o)
        else:
            out[name] = _u16(state52, o)
    return out


def decode_state_report(report):
    """Decode a 0x42 report (>= 54 bytes, possibly padded). Returns (seq, fields dict). The
    nonce is fields['imu_timestamp']."""
    if len(report) < C.STATE_REPORT_LEN or report[0] != C.RPT_STATE:
        raise ValueError("not a 0x42 state report")
    seq = report[1]
    return seq, decode_state52(report[2:2 + C.STATE52_LEN])


def state52_of_report(report):
    return bytes(report[2:2 + C.STATE52_LEN])


def nonce_of_report(report):
    return _u32(report, 30)   # the IMU timestamp field at report offset 30


def ble_cut(state52):
    """A state52 as Bluetooth carries it: 0x42 goes out as 0x45, the first 44 bytes of the state
    after the sequence byte; the quaternion (offsets 44..51) is cut (reads as zero)."""
    return bytes(state52[:C.BLE_STATE52_KEPT]) + bytes(C.STATE52_LEN - C.BLE_STATE52_KEPT)


RAW_FIELDS = ("lstick_x", "lstick_y", "rstick_x", "rstick_y", "ltrigger", "rtrigger",
              "lpressure", "rpressure", "lstick_touch", "rstick_touch", "lgrip", "rgrip")


def encode_raw_args(nonce, present=0, flags=0, **values):
    """RAW_INJECT's 30 argument bytes: flags u8, nonce u32, present u8 (the RAW_* bits), sticks
    LX, LY, RX, RY u16, triggers L, R u16, pressure L, R s16, stick touch L, R s16, grip L, R
    s16 (docs/test-protocol.md). Unnamed values are 0."""
    for name in values:
        if name not in RAW_FIELDS:
            raise KeyError("unknown raw input %r" % name)
    v = [values.get(n, 0) for n in RAW_FIELDS]
    return struct.pack("<BIB6H6h", flags, nonce, present, *v)


def decode_cal_status(data):
    """CAL_STATUS's 16 bytes."""
    d = bytes(data)
    gen, usable, raw, injected, flags, s0, s1, s2, p0, p1, clicks = \
        struct.unpack_from("<IBBBBBBBhhB", d, 0)
    return {"generation": gen, "usable": usable, "raw": raw, "injected": injected,
            "pad_rules": bool(flags & 1), "pressure_click": bool(flags & 2),
            "steps": (s0, s1, s2), "filters": (p0, p1), "clicks": clicks}


def buttons_set(buttons):
    return {name for bit, name in BUTTON_BITS.items() if buttons & (1 << bit)}


# ---- battery 0x43 / 0xBE --------------------------------------------------------------------

BATTERY_DEFAULT = bytes.fromhex("43 04 64 04 10 E4 0C 88 13 00 00 00 00 00 00".replace(" ", ""))


def decode_battery(report):
    if len(report) < C.BATTERY_REPORT_LEN or report[0] != C.RPT_BATTERY:
        raise ValueError("not a 0x43 battery report")
    return {
        "charge_state": report[1], "level": report[2],
        "battery_mv": _u16(report, 3), "system_mv": _u16(report, 5), "input_mv": _u16(report, 7),
        "current_ma": _s16(report, 9), "input_current_ma": _u16(report, 11),
        "temperature": _u16(report, 13),
    }


def decode_be(answer):
    """The 15-byte 0xBE answer: temperature, battery/system/input mV, current, input current,
    level, 1 while charging, the charge state without the 0x80 flag."""
    if len(answer) < 15:
        raise ValueError("short 0xBE answer")
    return {
        "temperature": _u16(answer, 0), "battery_mv": _u16(answer, 2),
        "system_mv": _u16(answer, 4), "input_mv": _u16(answer, 6),
        "current_ma": _s16(answer, 8), "input_current_ma": _u16(answer, 10),
        "level": answer[12], "charging": answer[13], "charge_state": answer[14],
    }


def encode_battery_args(charge_state, level, battery_mv, system_mv, input_mv, current_ma,
                        input_current_ma, temperature, gauge_flags, send_now):
    """The 16-byte BATTERY op argument (docs/test-protocol.md)."""
    return struct.pack("<BBHHHhHHBB", charge_state & 0xFF, level & 0xFF, battery_mv & 0xFFFF,
                       system_mv & 0xFFFF, input_mv & 0xFFFF, current_ma, input_current_ma & 0xFFFF,
                       temperature & 0xFFFF, gauge_flags & 0xFF, 0x01 if send_now else 0x00)


BATTERY_FIELDS = ("charge_state", "level", "battery_mv", "system_mv", "input_mv", "current_ma",
                  "input_current_ma", "temperature", "gauge_flags")
BATTERY_DEFAULT_FIELDS = {"charge_state": 4, "level": 100, "battery_mv": 4100, "system_mv": 3300,
                          "input_mv": 5000, "current_ma": 0, "input_current_ma": 0,
                          "temperature": 0, "gauge_flags": 0}


def decode_battery_args(args):
    """The nine battery fields of a BATTERY op argument (its flags byte aside)."""
    v = struct.unpack_from("<BBHHHhHHB", bytes(args), 0)
    return dict(zip(BATTERY_FIELDS, v))


def report_level(level):
    """The level as the library sends it (codec.c): 0..100 and 255 (no reading yet, as the real
    controller's first report after a cold start) as they are, anything else as 100."""
    level &= 0xFF
    return level if level <= 100 or level == 255 else 100


def encode_battery(b):
    """The 15-byte 0x43 the library builds from a battery (codec.c): the level as report_level()
    sends it, the charge state as given, no gauge flags."""
    return struct.pack("<BBBHHHhHH", C.RPT_BATTERY, b["charge_state"] & 0xFF,
                       report_level(b["level"]), b["battery_mv"] & 0xFFFF,
                       b["system_mv"] & 0xFFFF, b["input_mv"] & 0xFFFF, b["current_ma"],
                       b["input_current_ma"] & 0xFFFF, b["temperature"] & 0xFFFF)


CHARGE_FLAG_GAVE_UP = 0x80     # 0x43's flag while the real controller's supply check gave up


def encode_be(b):
    """The 15-byte answer of vendor command 0xBE for a battery (codec.c): the level as
    report_level() sends it, at offset 13 1 while the charge state is 2 (charging), at 14 the charge state without the
    0x80 flag. The battery's gauge-flags field is not sent."""
    state = b["charge_state"] & 0x7F
    return struct.pack("<HHHHhHBBB", b["temperature"] & 0xFFFF, b["battery_mv"] & 0xFFFF,
                       b["system_mv"] & 0xFFFF, b["input_mv"] & 0xFFFF, b["current_ma"],
                       b["input_current_ma"] & 0xFFFF, report_level(b["level"]),
                       1 if state == 2 else 0, state)


# ---- 0x44 haptic stream status --------------------------------------------------------------

def decode_haptic_status(report):
    if len(report) < C.HAPTIC_STATUS_LEN or report[0] != C.RPT_HAPTIC_STATUS:
        raise ValueError("not a 0x44 report")
    return {"channel": report[1], "flags": _u32(report, 2)}


# ---- the puck's own reports -----------------------------------------------------------------

def decode_link_status(report):
    """0x79, 2 bytes: ID, then 1 no link, 2 connected, 3 just paired."""
    if len(report) < C.LINK_STATUS_LEN or report[0] != C.RPT_LINK_STATUS:
        raise ValueError("not a 0x79 report")
    return {"status": report[1]}


def decode_link_quality(report):
    """0x7B, 13 bytes (the puck's link-quality report)."""
    if len(report) < C.LINK_QUALITY_LEN or report[0] != C.RPT_LINK_QUALITY:
        raise ValueError("not a 0x7B report")
    rssi = report[9] - 256 if report[9] & 0x80 else report[9]
    return {
        "window_ms": _u16(report, 1), "exchanges": _u16(report, 3), "retries": _u16(report, 5),
        "longest_gap_ms": _u16(report, 7), "rssi_dbm": rssi, "reason": report[10],
        "channel": report[11], "first_alternate": report[12],
    }


# ---- lizard reports as the ring records them ------------------------------------------------

def decode_mouse(report):
    """0x40, 6 bytes: ID, buttons, X, Y, wheel, pan (low 8 bits of signed values)."""
    if len(report) < C.MOUSE_REPORT_LEN or report[0] != C.RPT_MOUSE:
        raise ValueError("not a 0x40 report")

    def s8(v):
        return v - 256 if v & 0x80 else v
    return {"buttons": report[1], "x": s8(report[2]), "y": s8(report[3]),
            "wheel": s8(report[4]), "pan": s8(report[5])}


def decode_keyboard(report):
    """0x41, 9 bytes: ID, modifiers, reserved, six key codes."""
    if len(report) < C.KEYBOARD_REPORT_LEN or report[0] != C.RPT_KEYBOARD:
        raise ValueError("not a 0x41 report")
    return {"modifiers": report[1], "reserved": report[2], "keys": list(report[3:9])}


def pad_input(report, length=C.PADDED_INPUT_LEN):
    """Windows hands every vendor input report over padded to 54 bytes."""
    if len(report) >= length:
        return bytes(report[:length])
    return bytes(report) + bytes(length - len(report))
