# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# vendor.py: feature report 1, the vendor command channel. Build the 64-byte SET buffer
# `01 <op> <n> <payload>` and parse a GET answer `01 <op> <n> <data>` (commands.c / usb_desc.c).
# Also the decoders of the answers the matrix checks (0x83 attributes, 0xAE strings, 0xF2/0xA1
# device info). Serials in those answers are never returned as text to callers that print; the
# group compares them in memory and masks.
import struct

from .. import config as C


def build_set(opcode, payload=b""):
    """A 64-byte SET_REPORT(Feature 1) buffer: 01 <op> <n> <payload> zero padded."""
    payload = bytes(payload)
    if len(payload) > C.ANSWER_MAX:
        raise ValueError("payload too long for feature report 1")
    buf = bytearray(C.FEATURE_LEN)
    buf[0] = C.FEATURE_ID
    buf[1] = opcode & 0xFF
    buf[2] = len(payload)
    buf[3:3 + len(payload)] = payload
    return bytes(buf)


def build_get_request():
    """A 64-byte GET buffer with byte 0 = 1 (the feature report ID), as both input and output."""
    buf = bytearray(C.FEATURE_LEN)
    buf[0] = C.FEATURE_ID
    return bytes(buf)


def parse_get(buf):
    """Parse a GET answer buffer. Returns (opcode, n, data[:n]). The answer is `01 <op> <n> ...`."""
    if len(buf) < 3:
        raise ValueError("short feature answer")
    opcode = buf[1]
    n = buf[2]
    data = bytes(buf[3:3 + n])
    return opcode, n, data


def settings_triplets(pairs):
    """0x87 payload: {number, value s16} triplets."""
    out = bytearray()
    for number, value in pairs:
        out += struct.pack("<Bh", number & 0xFF, value)
    return bytes(out)


def decode_attributes(data):
    """0x83 answer: six {tag u8, value u32} pairs (30 bytes). Returns a list of (tag, value)."""
    out = []
    for i in range(0, min(len(data), 30), 5):
        if i + 5 > len(data):
            break
        tag = data[i]
        value = struct.unpack_from("<I", data, i + 1)[0]
        out.append((tag, value))
    return out


def decode_string_attribute(data):
    """0xAE answer (20 bytes): tag, then the string zero padded. Returns (tag, text)."""
    if not data:
        return None, ""
    tag = data[0]
    text = bytes(data[1:]).split(b"\0", 1)[0].decode("ascii", "replace")
    return tag, text


def decode_device_info0(data):
    """0xF2 selector 0 (41 bytes): selector, build time u32, revision u32, git (16), serial (16)."""
    sel = data[0]
    build_time = struct.unpack_from("<I", data, 1)[0]
    revision = struct.unpack_from("<I", data, 5)[0]
    git = bytes(data[9:25]).split(b"\0", 1)[0].decode("ascii", "replace")
    serial = bytes(data[25:41]).split(b"\0", 1)[0].decode("ascii", "replace")
    return {"selector": sel, "build_time": build_time, "revision": revision,
            "git_revision": git, "serial": serial}


def decode_device_info2(data):
    """0xF2 selector 2 (9 bytes): selector, uptime ms u32, last disconnect reason u32."""
    return {"selector": data[0], "uptime_ms": struct.unpack_from("<I", data, 1)[0],
            "reason": struct.unpack_from("<I", data, 5)[0]}


RESET_INFO_LEN = 18


def decode_reset_info(data):
    """0xA1 selector 1 (18 bytes): selector, the reset-cause flags (RESET_* below), the chip ID
    (8), 8 zero bytes."""
    return {"selector": data[0], "reset_cause": data[1], "chip_id": bytes(data[2:10])}


def decode_led(data):
    """0xE9 answer (4 bytes): red, green, blue, white."""
    return {"red": data[0], "green": data[1], "blue": data[2], "white": data[3]}


def decode_user_store(data):
    """0xDB answer (2 bytes): type (1), chime level."""
    return {"type": data[0], "chime": data[1]}


def decode_setting(data):
    """0x89 answer (3 bytes): number, value s16."""
    return {"number": data[0], "value": struct.unpack_from("<h", data, 1)[0]}


# Reset-cause flags of 0xA1, the real controller's encoding (proto_rules.c), for cmd.reset_info
# and the note of an unplanned reset. The chip's register is never cleared, so the flags of
# several resets add up until a power-on.
RESET_POWER_ON = 0x01
RESET_BROWNOUT = 0x02
RESET_OTHER = 0x08
RESET_PIN = 0x10
RESET_WATCHDOG = 0x20
RESET_SOFTWARE = 0x40
RESET_NAMES = {RESET_POWER_ON: "power-on", RESET_BROWNOUT: "brown-out",
               RESET_OTHER: "debug, low-power wake, lock-up or other", RESET_PIN: "pin",
               RESET_WATCHDOG: "watchdog", RESET_SOFTWARE: "software"}


def reset_cause_text(flags):
    """The reset cause bits by name ("pin, software"), or "none" for a fresh power-on."""
    names = [n for bit, n in sorted(RESET_NAMES.items(), reverse=True) if flags & bit]
    return ", ".join(names) if names else "none (a power-on)"
