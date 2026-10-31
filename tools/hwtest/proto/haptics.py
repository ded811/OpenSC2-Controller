# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# haptics.py: the haptics synthesiser's part of the test protocol (CONFIG_OPENSC2_HAPTICS_SYNTH;
# docs/test-protocol.md): its ops 0x80..0x83, its ring entries 0x60..0x63, its status, the 0x44
# flags, and the shared vectors of rendered blocks (tests/vectors/hwtest/haptics.txt), which the
# host tests render with the library's own code and the "haptics" group compares with the board's.
import os
import re
import struct

# caps build flag: the board has the synthesiser
CAP_HAPTICS_SYNTH = 19

# ops
OP_HAP_RESET = 0x80      # seed u32
OP_HAP_CAPTURE = 0x81    # path u8 (0 pads, 1 grips, 2 both), blocks u8
OP_HAP_STATUS = 0x82     # -> 38 bytes
OP_HAP_PLAY = 0x83       # what u8, arguments -> rc s32
OP_NAMES = {OP_HAP_RESET: "HAP_RESET", OP_HAP_CAPTURE: "HAP_CAPTURE", OP_HAP_STATUS: "HAP_STATUS",
            OP_HAP_PLAY: "HAP_PLAY"}

PLAY_CUE = 0             # cue, level
PLAY_CLICK = 1           # channels, click (1 A, 2 B), gain
PLAY_SCRIPT = 2          # channels, id, gain
PLAY_FALL = 3            # the random byte the drop sound will use
PLAY_STOP = 4            # channels

# ring entry kinds
RING_HAP_GEN = 0x60
RING_HAP_BLOCK = 0x61
RING_HAP_STREAM = 0x62
RING_HAP_SCRIPT = 0x63
RING_KINDS = (RING_HAP_GEN, RING_HAP_BLOCK, RING_HAP_STREAM, RING_HAP_SCRIPT)

# channels (the bits of OPENSC2_HAPTIC_*) and paths
CH_LEFT_PAD, CH_RIGHT_PAD, CH_LEFT_GRIP, CH_RIGHT_GRIP = 0, 1, 2, 3
PADS, GRIPS = 0, 1
STATUS_ID = {CH_LEFT_PAD: 3, CH_RIGHT_PAD: 4, CH_LEFT_GRIP: 0, CH_RIGHT_GRIP: 1}   # 0x44's ids

# generator types
GEN_CLICK_A, GEN_CLICK_B, GEN_PULSE, GEN_TONE, GEN_RUMBLE, GEN_SWEEP, GEN_STREAM, GEN_CLIP = range(8)
# HAP_GEN what
GEN_START, GEN_STOP, GEN_END, GEN_REFUSED, GEN_LOST, GEN_IGNORED = 1, 2, 3, 4, 5, 6
# HAP_STREAM what
STREAM_QUEUED, STREAM_START, STREAM_DRY, STREAM_EMPTIED, STREAM_FORMAT, STREAM_WRITE, \
    STREAM_STATUS = 1, 2, 3, 4, 5, 6, 7
# HAP_SCRIPT what
SCRIPT_START, SCRIPT_REFUSED, SCRIPT_STEP, SCRIPT_DONE, SCRIPT_CUE, SCRIPT_FALL, SCRIPT_DROP, \
    SCRIPT_REARM = 1, 2, 3, 4, 5, 6, 7, 8
# HAP_BLOCK flags
BLOCK_FIRST, BLOCK_LAST = 0x01, 0x02

# input report 0x44's flags
FLAG_DROPPED, FLAG_STOPPED, FLAG_LOW, FLAG_HIGH, FLAG_NEED_8K, FLAG_FORMAT, FLAG_BUSY = \
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40

STATUS_LEN = 38


def _s8(v):
    return v - 256 if v & 0x80 else v


def decode(entry):
    """The typed fields of one of the synthesiser's ring entries (a dict from
    testproto.decode_entry(), whose `data` they come from); {} for other kinds."""
    d = entry.get("data", b"")
    k = entry.get("kind")
    if k == RING_HAP_GEN and len(d) >= 12:
        a, b, c, e = struct.unpack_from("<HHHH", d, 4)
        return {"channel": d[0], "type": d[1], "what": d[2], "gain": _s8(d[3]), "p": (a, b, c, e)}
    if k == RING_HAP_BLOCK and len(d) >= 12:
        index, crc = struct.unpack_from("<II", d, 1)
        return {"path": d[0], "index": index, "crc": crc, "peak": struct.unpack_from("<H", d, 9)[0],
                "flags": d[11]}
    if k == RING_HAP_STREAM and len(d) >= 8:
        return {"channel": d[0], "what": d[1], "fill": struct.unpack_from("<H", d, 2)[0],
                "flags": struct.unpack_from("<I", d, 4)[0]}
    if k == RING_HAP_SCRIPT and len(d) >= 6:
        return {"what": d[0], "channel": d[1], "id": d[2], "gain": _s8(d[3]),
                "extra": struct.unpack_from("<H", d, 4)[0]}
    return {}


def entries(ring_entries, kind, **match):
    """The decoded entries of `kind` whose fields equal `match`, each with its ring time."""
    out = []
    for e in ring_entries:
        if e.get("kind") != kind:
            continue
        f = decode(e)
        if all(f.get(k) == v for k, v in match.items()):
            f["time_us"] = e["time_us"]
            out.append(f)
    return out


def decode_status(result):
    """HAP_STATUS's 38 bytes."""
    if len(result) < STATUS_LEN:
        raise ValueError("short haptics status")
    r = bytes(result)
    return {
        "features": r[0], "drop_sounds": bool(r[0] & 1), "busy": r[1], "outputs": r[2],
        "amp_mode": r[3], "master_db": _s8(r[4]), "players": r[5], "drop_state": r[6],
        "running": r[7],
        "blocks": struct.unpack_from("<II", r, 8),
        "underruns": struct.unpack_from("<HH", r, 16),
        "lost": struct.unpack_from("<H", r, 20)[0],
        "gens": tuple(r[22:26]),
        "fill": struct.unpack_from("<HHHH", r, 26),
        "format": tuple(r[34:38]),
    }


def wait_quiet(rig, timeout=2.0):
    """Until the synthesiser plays nothing, runs no script and both paths' threads are idle (a
    command sent then is applied at once, as on the real controller): the status, or None when
    the board does not answer HAP_STATUS."""
    deadline = rig.clock.monotonic() + rig.t(timeout)
    while True:
        a = rig.test(OP_HAP_STATUS)
        if a is None or a["status"] != 0:
            return None
        st = decode_status(a["result"])
        quiet = not st["busy"] and not st["running"] and not st["players"]
        if quiet or rig.clock.monotonic() >= deadline:
            return st
        rig.sleep(0.02)


def status_report(channel, flags):
    """Input report 0x44 as the board sends it for `channel` (0..3)."""
    return bytes([0x44, STATUS_ID[channel]]) + struct.pack("<I", flags)


# ---- the shared vectors ------------------------------------------------------------------------

VECTORS = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "tests",
                                        "vectors", "hwtest", "haptics.txt"))
_HEX = re.compile(r"^[0-9A-Fa-f]+$")


def read_vectors(path=VECTORS):
    """The RENDER records: [{name, path, seed, master, s27, blocks, report, crcs}]."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            tok = line.split("#", 1)[0].split()
            if not tok:
                continue
            if tok[0] != "RENDER":
                raise ValueError("unknown haptics vector record: %s" % line.strip())
            rec = {"name": tok[1]}
            i = 2
            while i < len(tok):
                word = tok[i]
                if word == "OUT":
                    parts = []
                    i += 1
                    while i < len(tok) and _HEX.match(tok[i]) and tok[i] != "CRC":
                        parts.append(tok[i])
                        i += 1
                    rec["report"] = bytes.fromhex("".join(parts))
                    continue
                if word == "CRC":
                    rec["crcs"] = [int(t, 16) for t in tok[i + 1:]]
                    break
                key = {"PATH": "path", "SEED": "seed", "MASTER": "master", "S27": "s27",
                       "BLOCKS": "blocks"}.get(word)
                if key is None or i + 1 >= len(tok):
                    raise ValueError("bad haptics vector: %s" % line.strip())
                rec[key] = int(tok[i + 1], 16) if key == "seed" else int(tok[i + 1])
                i += 2
            if len(rec.get("crcs", [])) != rec.get("blocks"):
                raise ValueError("haptics vector %s: %d CRCs for %s blocks"
                                 % (rec["name"], len(rec.get("crcs", [])), rec.get("blocks")))
            out.append(rec)
    return out


# ---- the October rumble emulation's numbers (haptics_rules.c), for expected ring entries -------

_LEVEL_DB = {16: 16, 60: 12, 100: 8, 200: 4, 1000: -4, 8000: -8, 16000: -12}


def rumble_right_db(speed):
    return -20 if speed < 1000 else speed // 10000 - 14


def rumble_left_db(speed, setting27=-10, intensity=0):
    db = speed * 3 // 12500 + 2 + setting27 + _LEVEL_DB.get(intensity, 0) - \
        (18 if speed < 1000 else 12)
    return max(-128, min(127, db))


def rumble_lfo_mhz(speed):
    return 10000 if speed <= 13107 else 7600 + (6000 * speed + 32767) // 65535
