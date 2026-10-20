# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# testproto.py: the test channel, vendor command 0x7E (docs/test-protocol.md). Builds requests,
# parses answers, the 36-byte caps and the 32-byte status, and decodes and re-encodes every ring
# entry kind and the counter and settings pages. The ring round-trip (decode then encode -> the
# same bytes) is how the shared vectors (ring.txt) tie the two sides together.
import binascii
import struct

from .. import config as C


def crc32(data):
    return binascii.crc32(bytes(data)) & 0xFFFFFFFF


# ---- requests and answers -------------------------------------------------------------------

HEADER_LEN = 7          # key u32, version, request ID, op
REQUEST_MAX = 61        # a vendor payload
ARGS_MAX = REQUEST_MAX - HEADER_LEN


class ProtocolError(Exception):
    """An answer that does not belong to the request (another op or request ID, a short answer),
    or a console reply that is not the test channel's: a bug or a desynchronised link, never a
    test result."""


def build_request(op, args=b"", request_id=0):
    """The vendor payload of a 0x7E request: key(4) version(1) request_id(1) op(1) args."""
    req = C.TEST_KEY_BYTES + bytes([C.TEST_VERSION, request_id & 0xFF, op & 0xFF]) + bytes(args)
    if len(req) > REQUEST_MAX:
        raise ValueError("test request too long (%d bytes, at most %d)" % (len(req), REQUEST_MAX))
    return req


def is_request(payload):
    """Key, version and length: a request of the test channel at all (test_core.c)."""
    payload = bytes(payload)
    return (HEADER_LEN <= len(payload) <= REQUEST_MAX and payload[:4] == C.TEST_KEY_BYTES
            and payload[4] == C.TEST_VERSION)


def claims(payload, armed):
    """The claiming rule of USB and the radio: a test request, and armed or the op is ARM."""
    return is_request(payload) and (armed or bytes(payload)[6] == C.OP_ARM)


def parse_answer(payload):
    """Parse a 0x7E answer: version(1) request_id(1) op(1) status(1) result. Returns a dict."""
    if len(payload) < 4:
        raise ProtocolError("short test-channel answer (%d bytes)" % len(payload))
    return {"version": payload[0], "request_id": payload[1], "op": payload[2],
            "status": payload[3], "result": bytes(payload[4:])}


def check_answer(answer, op, request_id):
    """The answer must echo the request: version 1, its request ID (low byte) and op."""
    if answer["version"] != C.TEST_VERSION or answer["op"] != (op & 0xFF) or \
            answer["request_id"] != (request_id & 0xFF):
        raise ProtocolError("answer of another request (version %d, id %d, op 0x%02x; expected "
                            "id %d, op 0x%02x)" % (answer["version"], answer["request_id"],
                                                   answer["op"], request_id & 0xFF, op & 0xFF))
    return answer


# ---- caps (the ARM result, 36 bytes; firmware before the product name sent 32) ---------------

def decode_caps(data):
    if len(data) < 32:
        raise ValueError("short caps")
    flags = struct.unpack_from("<I", data, 2)[0]
    git = bytes(data[20:32]).split(b"\0", 1)[0].decode("ascii", "replace")
    product_crc = struct.unpack_from("<I", data, 32)[0] if len(data) >= 36 else None
    return {
        "version": data[0], "mode": data[1], "build_flags": flags,
        "battery_interval_ms": struct.unpack_from("<H", data, 6)[0],
        "rumble_timeout_ms": struct.unpack_from("<H", data, 8)[0],
        "ring_size": struct.unpack_from("<H", data, 10)[0],
        "sequence_depth": data[12], "counter_count": data[13],
        "radio_report_delay_us": struct.unpack_from("<H", data, 14)[0],
        "lease_s": struct.unpack_from("<H", data, 16)[0],
        "policy": data[18], "git_revision": git,
        # the CRC-32 of the USB product name in use (the name itself does not fit), or None
        "product_crc": product_crc,
    }


def encode_caps(c):
    out = bytearray(36)
    out[0] = c.get("version", 1)
    out[1] = c.get("mode", 1)
    struct.pack_into("<I", out, 2, c.get("build_flags", 0))
    struct.pack_into("<H", out, 6, c.get("battery_interval_ms", C.BATTERY_INTERVAL_MS))
    struct.pack_into("<H", out, 8, c.get("rumble_timeout_ms", C.RUMBLE_TIMEOUT_MS))
    struct.pack_into("<H", out, 10, c.get("ring_size", 2048))
    out[12] = c.get("sequence_depth", 8)
    out[13] = c.get("counter_count", C.COUNTER_COUNT)
    struct.pack_into("<H", out, 14, c.get("radio_report_delay_us", 2000))
    struct.pack_into("<H", out, 16, c.get("lease_s", C.LEASE_DEFAULT_S))
    out[18] = c.get("policy", 0)
    git = c.get("git_revision", "").encode("ascii", "replace")[:12]
    out[20:20 + len(git)] = git
    crc = c.get("product_crc")
    if crc is None:
        crc = crc32(c.get("product_name", C.DEFAULT_PRODUCT_NAME).encode("ascii", "replace"))
    struct.pack_into("<I", out, 32, crc)
    return bytes(out)


def cap_flag(caps, bit):
    return bool(caps["build_flags"] & (1 << bit))


# ---- status (the STATUS result, 32 bytes) ---------------------------------------------------

STATUS_FLAG_ARMED = 1 << 0
STATUS_FLAG_GUARD = 1 << 1
STATUS_FLAG_INJECT = 1 << 2
STATUS_FLAG_BATTERY = 1 << 3
STATUS_FLAG_SEQUENCE = 1 << 4
STATUS_FLAG_LIZARD = 1 << 5
STATUS_FLAG_DISARM = 1 << 6
STATUS_FLAG_SNAPSHOT = 1 << 7


def decode_status(data):
    if len(data) < 32:
        raise ValueError("short status")
    flags = data[0]
    return {
        "flags": flags,
        "armed": bool(flags & STATUS_FLAG_ARMED), "guard": bool(flags & STATUS_FLAG_GUARD),
        "inject": bool(flags & STATUS_FLAG_INJECT), "battery": bool(flags & STATUS_FLAG_BATTERY),
        "sequence": bool(flags & STATUS_FLAG_SEQUENCE), "lizard": bool(flags & STATUS_FLAG_LIZARD),
        "disarm": bool(flags & STATUS_FLAG_DISARM), "snapshot": bool(flags & STATUS_FLAG_SNAPSHOT),
        "mode": data[1], "mode_reason": data[2], "link_state": data[3], "usb_state": data[4],
        "slot": data[5], "last_disconnect_reason": data[6], "last_seq": data[7],
        "nonce": struct.unpack_from("<I", data, 8)[0],
        "uptime_ms": struct.unpack_from("<I", data, 12)[0],
        "ring_first": struct.unpack_from("<I", data, 16)[0],
        "ring_next": struct.unpack_from("<I", data, 20)[0],
        "frames": struct.unpack_from("<I", data, 24)[0],
        "seq_step": data[28], "seq_loaded": data[29],
        "seq_frames_left": struct.unpack_from("<H", data, 30)[0],
    }


def encode_status(s):
    out = bytearray(32)
    out[0] = s.get("flags", 0)
    out[1] = s.get("mode", 1)
    out[2] = s.get("mode_reason", 0)
    out[3] = s.get("link_state", 0)
    out[4] = s.get("usb_state", 0)
    out[5] = s.get("slot", 0)
    out[6] = s.get("last_disconnect_reason", 0)
    out[7] = s.get("last_seq", 0)
    struct.pack_into("<I", out, 8, s.get("nonce", 0))
    struct.pack_into("<I", out, 12, s.get("uptime_ms", 0))
    struct.pack_into("<I", out, 16, s.get("ring_first", 0))
    struct.pack_into("<I", out, 20, s.get("ring_next", 0))
    struct.pack_into("<I", out, 24, s.get("frames", 0))
    out[28] = s.get("seq_step", 0xFF)
    out[29] = s.get("seq_loaded", 0)
    struct.pack_into("<H", out, 30, s.get("seq_frames_left", 0))
    return bytes(out)


# ---- BLE_STATUS (0x60, a Bluetooth build: 28 bytes) ------------------------------------------

_BLE_FLAGS = ("enabled", "advertising", "connected", "active", "trigger", "running",
              "pair_request", "wireless_off")


def decode_ble_status(data):
    """The Bluetooth link's state: no name and no address (those only on the console)."""
    if len(data) < C.BLE_STATUS_LEN:
        raise ValueError("short BLE_STATUS")
    out = {name: bool(data[0] & (1 << i)) for i, name in enumerate(_BLE_FLAGS)}
    out.update(form=data[1], form_name=C.BLE_FORM_NAMES.get(data[1], "?"), slot=data[2],
               identity=data[3], bonded=data[4], identities=data[5], security=data[6],
               last_reason=data[7], link_state=data[8], reason=data[9])
    out["mtu"], out["interval_us"], out["window_left_ms"], out["reports"], out["dropped"] = \
        struct.unpack_from("<HIIII", data, 10)
    return out


def encode_ble_status(s):
    out = bytearray(C.BLE_STATUS_LEN)
    for i, name in enumerate(_BLE_FLAGS):
        if s.get(name):
            out[0] |= 1 << i
    for at, name, default in ((1, "form", 0xFF), (2, "slot", 0), (3, "identity", 1),
                              (4, "bonded", 0), (5, "identities", 5), (6, "security", 0),
                              (7, "last_reason", 0), (8, "link_state", 0), (9, "reason", 0)):
        out[at] = s.get(name, default) & 0xFF
    struct.pack_into("<HIIII", out, 10, s.get("mtu", 23), s.get("interval_us", 0),
                     s.get("window_left_ms", 0), s.get("reports", 0), s.get("dropped", 0))
    return bytes(out)


# ---- counters, settings ---------------------------------------------------------------------

def decode_counters(data):
    """CNT_READ result: first u8, count u8, u32 * count. Returns {name: value}."""
    first = data[0]
    count = data[1]
    out = {}
    for i in range(count):
        value = struct.unpack_from("<I", data, 2 + 4 * i)[0]
        idx = first + i
        name = C.COUNTER_NAMES[idx] if idx < len(C.COUNTER_NAMES) else "counter_%d" % idx
        out[name] = value
    return out


def decode_settings(data):
    """SETTINGS_READ result: first u8, count u8, s16 * count. Returns {number: value}."""
    first = data[0]
    count = data[1]
    out = {}
    for i in range(count):
        out[first + i] = struct.unpack_from("<h", data, 2 + 2 * i)[0]
    return out


# ---- ring entries (docs/test-protocol.md, The ring) -----------------------------------------

_PCM_KINDS = {0: "pulse", 1: "command", 2: "tone", 3: "sweep", 4: "script", 5: "pcm_control",
              6: "pcm_data"}


def _decode_entry_data(kind, d):
    """Decode the `data` field of a ring entry by kind, into typed fields."""
    e = {}
    if kind == C.RING_ARMED:
        e["nonce"] = struct.unpack_from("<I", d, 0)[0]
    elif kind == C.RING_MARK:
        e["marker"] = struct.unpack_from("<I", d, 0)[0]
    elif kind == C.RING_SEQ_STEP:
        e["step"] = d[0]
        e["frame"] = struct.unpack_from("<I", d, 1)[0]
        e["seq"] = d[5]
    elif kind == C.RING_SEQ_END:
        e["frame"] = struct.unpack_from("<I", d, 0)[0]
        e["how"] = d[4]
    elif kind == C.RING_INJECT_STOP:
        e["reason"] = d[0]
        e["frame"] = struct.unpack_from("<I", d, 1)[0]
        e["what"] = d[5]
    elif kind == C.RING_OUTPUT:
        e["report_id"] = d[0]
        e["total_len"] = d[1]
        e["decode_result"] = d[2] - 256 if d[2] & 0x80 else d[2]
        e["tail"] = bytes(d[3:])
    elif kind == C.RING_RUMBLE:
        (e["left"], e["right"], e["intensity"]) = struct.unpack_from("<HHH", d, 0)
        e["left_gain"] = d[6] - 256 if d[6] & 0x80 else d[6]
        e["right_gain"] = d[7] - 256 if d[7] & 0x80 else d[7]
        e["type"] = d[8]
        e["timeout"] = d[9]
    elif kind == C.RING_HAPTIC:
        e["haptic_kind"] = d[0]
        e["kind_name"] = _PCM_KINDS.get(d[0], "?")
        e["report_id"] = d[1]
        e["target"] = d[2]
        e["channels"] = d[3]
        body = d[4:]
        k = d[0]
        if k == 0:       # pulse
            e["on_us"], e["off_us"], e["count"] = struct.unpack_from("<HHH", body, 0)
        elif k == 1:     # command
            e["command"] = body[0]
            e["gain"] = body[1] - 256 if body[1] & 0x80 else body[1]
        elif k == 2:     # tone
            e["gain"] = body[0] - 256 if body[0] & 0x80 else body[0]
            e["freq"], e["duration"], e["lfo"] = struct.unpack_from("<HHH", body, 1)
            e["depth"] = body[7]
        elif k == 3:     # sweep
            e["gain"] = body[0] - 256 if body[0] & 0x80 else body[0]
            e["duration"], e["start"], e["end"] = struct.unpack_from("<HHH", body, 1)
            e["antiphase"] = body[7]
        elif k == 4:     # script
            e["id"] = body[0]
            e["gain"] = body[1] - 256 if body[1] & 0x80 else body[1]
        elif k == 5:     # pcm control
            e["operation"] = body[0]
            e["format"] = body[1]
            e["encoding"] = body[2]
            e["rate"] = struct.unpack_from("<H", body, 3)[0]
        elif k == 6:     # pcm data
            e["len"] = body[0]
            e["crc32"] = struct.unpack_from("<I", body, 1)[0]
    elif kind == C.RING_LED:
        e.update(red=d[0], green=d[1], blue=d[2], white=d[3], override=d[4], brightness=d[5])
    elif kind == C.RING_SETTING:
        e["number"] = d[0]
        e["value"] = struct.unpack_from("<h", d, 1)[0]
    elif kind == C.RING_LIZARD:
        e["what"] = d[0]
    elif kind == C.RING_LIZARD_RPT:
        e["flags"] = d[0]
        e["diverted"] = bool(d[0] & 1)
        e["frame"] = struct.unpack_from("<I", d, 1)[0]
        e["report"] = bytes(d[5:])
    elif kind == C.RING_POWER:
        e["request"] = d[0]
        e["reason"] = bytes(d[1:]).decode("ascii", "replace")
    elif kind == C.RING_VENDOR:
        e["direction"] = d[0]
        e["opcode"] = d[1]
        e["n"] = d[2]
        e["result"] = struct.unpack_from("<i", d, 3)[0]
        e["payload"] = bytes(d[7:])
    elif kind == C.RING_HOST_ASLEEP:
        pass
    elif kind == C.RING_LINK:
        e.update(state=d[0], reason=d[1], slot=d[2], channel=d[3], protocol=d[4])
    elif kind == C.RING_USB:
        e.update(state=d[0], active=d[1], suspended=d[2])
    elif kind == C.RING_VERSION:
        e["version"] = d[0]
    elif kind == C.RING_IDENTITY:
        e["product_id"] = struct.unpack_from("<H", d, 0)[0]
        e["build_time"] = struct.unpack_from("<I", d, 2)[0]
        e["bootloader_build_time"] = struct.unpack_from("<I", d, 6)[0]
        e["board_revision"] = struct.unpack_from("<I", d, 10)[0]
        e["git_revision"] = bytes(d[14:26]).split(b"\0", 1)[0].decode("ascii", "replace")
    elif kind == C.RING_CHANNELS:
        e["channels"] = list(d[0:4])
    elif kind == C.RING_STOP_STATUS:
        e["channel"] = d[0]
        e["flags"] = struct.unpack_from("<I", d, 1)[0]
    elif kind == C.RING_BLE and len(d) >= 8:
        e["event"] = d[0]
        e["event_name"] = C.BLE_EVENT_NAMES.get(d[0], "?")
        e["a"] = d[1]
        e["b"], e["c"] = struct.unpack_from("<HI", d, 2)
    return e


def decode_entry(entry):
    """Decode one ring entry (7-byte header + data). Returns a dict with time_us, kind, link, and
    the typed fields; `raw` keeps the whole entry bytes."""
    if len(entry) < 7:
        raise ValueError("short ring entry")
    time_us = struct.unpack_from("<I", entry, 0)[0]
    kind = entry[4]
    link = entry[5]
    length = entry[6]
    data = bytes(entry[7:7 + length])
    e = {"time_us": time_us, "kind": kind, "link": link,
         "link_name": C.RING_LINK_NAMES.get(link, "?"), "len": length, "data": data,
         "raw": bytes(entry[:7 + length])}
    e.update(_decode_entry_data(kind, data))
    return e


def _encode_entry_data(e):
    """Rebuild the data field from typed fields (the inverse of _decode_entry_data)."""
    kind = e["kind"]
    if kind == C.RING_ARMED:
        return struct.pack("<I", e["nonce"])
    if kind == C.RING_MARK:
        return struct.pack("<I", e["marker"])
    if kind == C.RING_SEQ_STEP:
        return bytes([e["step"]]) + struct.pack("<I", e["frame"]) + bytes([e["seq"]])
    if kind == C.RING_SEQ_END:
        return struct.pack("<I", e["frame"]) + bytes([e["how"]])
    if kind == C.RING_INJECT_STOP:
        return bytes([e["reason"]]) + struct.pack("<I", e["frame"]) + bytes([e["what"]])
    if kind == C.RING_OUTPUT:
        r = e["decode_result"] & 0xFF
        return bytes([e["report_id"], e["total_len"], r]) + e["tail"]
    if kind == C.RING_RUMBLE:
        return (struct.pack("<HHH", e["left"], e["right"], e["intensity"]) +
                bytes([e["left_gain"] & 0xFF, e["right_gain"] & 0xFF, e["type"], e["timeout"]]))
    if kind == C.RING_HAPTIC:
        head = bytes([e["haptic_kind"], e["report_id"], e["target"], e["channels"]])
        k = e["haptic_kind"]
        if k == 0:
            return head + struct.pack("<HHH", e["on_us"], e["off_us"], e["count"])
        if k == 1:
            return head + bytes([e["command"], e["gain"] & 0xFF])
        if k == 2:
            return head + bytes([e["gain"] & 0xFF]) + struct.pack("<HHH", e["freq"], e["duration"],
                                                                   e["lfo"]) + bytes([e["depth"]])
        if k == 3:
            return head + bytes([e["gain"] & 0xFF]) + struct.pack("<HHH", e["duration"], e["start"],
                                                                   e["end"]) + bytes([e["antiphase"]])
        if k == 4:
            return head + bytes([e["id"], e["gain"] & 0xFF])
        if k == 5:
            return head + bytes([e["operation"], e["format"], e["encoding"]]) + \
                struct.pack("<H", e["rate"])
        if k == 6:
            return head + bytes([e["len"]]) + struct.pack("<I", e["crc32"])
        return head
    if kind == C.RING_LED:
        return bytes([e["red"], e["green"], e["blue"], e["white"], e["override"], e["brightness"]])
    if kind == C.RING_SETTING:
        return bytes([e["number"]]) + struct.pack("<h", e["value"])
    if kind == C.RING_LIZARD:
        return bytes([e["what"]])
    if kind == C.RING_LIZARD_RPT:
        return bytes([e["flags"]]) + struct.pack("<I", e["frame"]) + e["report"]
    if kind == C.RING_POWER:
        return bytes([e["request"]]) + e["reason"].encode("ascii", "replace")
    if kind == C.RING_VENDOR:
        return bytes([e["direction"], e["opcode"], e["n"]]) + struct.pack("<i", e["result"]) + \
            e["payload"]
    if kind == C.RING_HOST_ASLEEP:
        return b""
    if kind == C.RING_LINK:
        return bytes([e["state"], e["reason"], e["slot"], e["channel"], e["protocol"]])
    if kind == C.RING_USB:
        return bytes([e["state"], e["active"], e["suspended"]])
    if kind == C.RING_VERSION:
        return bytes([e["version"]])
    if kind == C.RING_IDENTITY:
        git = e["git_revision"].encode("ascii", "replace")[:12]
        return (struct.pack("<H", e["product_id"]) + struct.pack("<I", e["build_time"]) +
                struct.pack("<I", e["bootloader_build_time"]) +
                struct.pack("<I", e["board_revision"]) + git + bytes(12 - len(git)))
    if kind == C.RING_CHANNELS:
        return bytes(e["channels"][:4])
    if kind == C.RING_STOP_STATUS:
        return bytes([e["channel"]]) + struct.pack("<I", e["flags"])
    if kind == C.RING_BLE and "event" in e:
        return bytes([e["event"], e["a"]]) + struct.pack("<HI", e["b"], e["c"])
    return e.get("data", b"")


def encode_entry(e):
    """Rebuild a whole ring entry from a decoded dict (header + data). Inverse of decode_entry."""
    data = _encode_entry_data(e)
    return struct.pack("<IBBB", e["time_us"], e["kind"], e["link"], len(data)) + data


def parse_ring_read(result):
    """A RING_READ answer: first u32, next u32, count u8, then `count` whole entries. Returns
    (first, next, [entry dicts])."""
    first = struct.unpack_from("<I", result, 0)[0]
    nxt = struct.unpack_from("<I", result, 4)[0]
    count = result[8]
    entries = []
    off = 9
    for _ in range(count):
        if off + 7 > len(result):
            break
        length = result[off + 6]
        entry = result[off:off + 7 + length]
        entries.append(decode_entry(entry))
        off += 7 + length
    return first, nxt, entries
