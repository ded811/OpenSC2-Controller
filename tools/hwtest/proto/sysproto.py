# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# sysproto.py: the test channel's system ops (0x70..0x74) and ring entries (0x50..0x56) for the
# firmware's system state machine (lib/opensc2/test_sys.h, docs/test-protocol.md): the names of
# the machine's states and events, SYS_INJECT's and SYS_BOOT's arguments, the 30-byte SYS_STATUS,
# and the typed fields of each ring entry. Pure.
import struct

# Sources (enum osc2_sys_source).
SRC_BATTERY = 0
SRC_USB = 1
SRC_DOCK = 2

# SYS_INJECT / SYS_BOOT flags.
INJECT_BUTTONS = 0x01
INJECT_LEVEL = 0x02
INJECT_SOURCE = 0x04

# SYS_EVENT kinds.
EV_WIRELESS_OFF = 1
EV_HOST_ASLEEP = 2

# The machine's states and events, in the firmware's numbering (lib/opensc2/sys_rules.h).
STATES = ["start", "battery-wait", "wired-wait", "wired", "usb-link-on", "usb-link-off",
          "dock-off", "lifted", "dock-chord", "dock-on", "chord", "battery", "shutdown",
          "shutdown-silent", "shutdown-low", "shutdown-locked", "reboot"]
EVENTS = ["power-none", "power-usb", "power-dock", "configured", "steam-held", "steam-pressed",
          "idle", "wireless-off", "level", "keylock-held", "chord-down", "chord-up", "timer",
          "bootloader", "reboot", "warm-reboot", "turn-off", "steam-up", "link-started",
          "unplugged"]
STATE = {name: i for i, name in enumerate(STATES)}
EVENT = {name: i for i, name in enumerate(EVENTS)}

# Chords (enum osc2_chord) and the CHORD entries' "what".
CHORD_NONE, CHORD_STEAM, CHORD_BT1, CHORD_BT2, CHORD_PUCK1, CHORD_PUCK2, CHORD_KEYLOCK = range(7)
CHORD_WHAT = {1: "down", 2: "up", 3: "steam-press", 4: "steam-hold", 5: "keylock-tap",
              6: "keylock-hold"}

# LED modes (enum opensc2_led_mode) and the LED_MODE entries' flag bits.
LED_BLUETOOTH, LED_PUCK, LED_WIRED, LED_AMBER, LED_OFF, LED_LOW_BATTERY, LED_KEYLOCK = range(1, 8)
LED_EXTERNAL = 0x01
LED_STEADY = 0x02
LED_SHUTTING_DOWN = 0x04

# Cues (enum opensc2_cue).
CUE_POWER_ON = 0
CUE_POWER_OFF = 5
CUE_PAIRING = 6
CUE_PAIRED = 7
CUE_HOST_ASLEEP = 10

# How an end came out (SYS_END entries, SYS_STATUS's last end).
END_POWER_OFF = 1
END_REBOOT = 2
HOW_POWER_OFF = 1
HOW_REBOOT = 2
HOW_NO_BATTERY = 3
HOW_TEST = 4

# Buttons (the button word of report 0x42).
BTN_A = 1 << 0
BTN_B = 1 << 1
BTN_X = 1 << 2
BTN_R4 = 1 << 7
BTN_R5 = 1 << 8
BTN_RB = 1 << 9
BTN_STEAM = 1 << 16
BTN_L4 = 1 << 17
BTN_L5 = 1 << 18
BTN_LB = 1 << 19
KEYLOCK_CHORD = BTN_STEAM | BTN_L4 | BTN_L5 | BTN_R4 | BTN_R5

SYS_STATUS_LEN = 30


def sys_args(flags, buttons=0, level=0, source=0, last=0):
    """SYS_INJECT's or SYS_BOOT's 8 argument bytes (`last`: SYS_BOOT's hold in 100 ms, 0 for
    SYS_INJECT)."""
    return struct.pack("<BIBBB", flags & 0xFF, buttons & 0xFFFFFFFF, level & 0xFF, source & 0xFF,
                       last & 0xFF)


def decode_sys_status(d):
    """The 30-byte SYS_STATUS result."""
    if len(d) < SYS_STATUS_LEN:
        raise ValueError("short system status")
    flags = d[7]
    return {
        "state": d[0], "state_name": STATES[d[0]] if d[0] < len(STATES) else "?",
        "transport": d[1], "source": d[2], "led_mode": d[3], "led_flags": d[4], "chord": d[5],
        "level": d[6], "flags": flags,
        "keylock": bool(flags & 0x01), "buttons_injected": bool(flags & 0x02),
        "level_injected": bool(flags & 0x04), "source_injected": bool(flags & 0x08),
        "boot_injected": bool(flags & 0x10), "bluetooth": bool(flags & 0x20),
        "battery_build": bool(flags & 0x40), "ended": bool(flags & 0x80),
        "idle_history": struct.unpack_from("<H", d, 8)[0],
        "idle_period_ms": struct.unpack_from("<I", d, 10)[0],
        "last_cue": d[14], "boot_state": d[15], "boot_cue": d[16], "boot_chord": d[17],
        "boot_pairing": d[18],
        "end_valid": bool(d[19]), "end_state": d[20], "end_cause": d[21], "end_how": d[22],
        "end_uptime_ms": struct.unpack_from("<I", d, 23)[0],
        "full_level": d[27], "idle_timeout_s": struct.unpack_from("<h", d, 28)[0],
    }


def encode_sys_status(s):
    out = bytearray(SYS_STATUS_LEN)
    out[0] = s.get("state", 0)
    out[1] = s.get("transport", 0xFF)
    out[2] = s.get("source", SRC_USB)
    out[3] = s.get("led_mode", 0)
    out[4] = s.get("led_flags", 0)
    out[5] = s.get("chord", 0)
    out[6] = s.get("level", 255)
    out[7] = s.get("flags", 0)
    struct.pack_into("<H", out, 8, s.get("idle_history", 0xFFFF))
    struct.pack_into("<I", out, 10, s.get("idle_period_ms", 56250))
    out[14] = s.get("last_cue", 0xFF)
    out[15] = s.get("boot_state", 0)
    out[16] = s.get("boot_cue", 0xFF)
    out[17] = s.get("boot_chord", 0)
    out[18] = s.get("boot_pairing", 0)
    out[19] = 1 if s.get("end_valid") else 0
    out[20] = s.get("end_state", 0)
    out[21] = s.get("end_cause", 0)
    out[22] = s.get("end_how", 0)
    struct.pack_into("<I", out, 23, s.get("end_uptime_ms", 0))
    out[27] = s.get("full_level", 3)
    struct.pack_into("<h", out, 28, s.get("idle_timeout_s", 900))
    return bytes(out)


# ---- ring entries -------------------------------------------------------------------------------

KIND_SYS_STATE = 0x50
KIND_LED_MODE = 0x51
KIND_CUE = 0x52
KIND_SYS_END = 0x53
KIND_CHORD = 0x54
KIND_SOURCE = 0x55
KIND_IDLE = 0x56
KINDS = (KIND_SYS_STATE, KIND_LED_MODE, KIND_CUE, KIND_SYS_END, KIND_CHORD, KIND_SOURCE,
         KIND_IDLE)


def _name(table, i):
    return table[i] if i < len(table) else "?"


def decode_entry_data(kind, d):
    """The typed fields of one of these entries (a dict), or None for another kind."""
    if kind == KIND_SYS_STATE and len(d) >= 5:
        return {"from": d[0], "to": d[1], "cause": d[2], "transport": d[3], "source": d[4],
                "from_name": _name(STATES, d[0]), "to_name": _name(STATES, d[1]),
                "cause_name": _name(EVENTS, d[2])}
    if kind == KIND_LED_MODE and len(d) >= 4:
        return {"mode": d[0], "flags": d[1], "level": d[2], "full": d[3]}
    if kind == KIND_CUE and len(d) >= 4:
        return {"cue": d[0], "script": d[1], "gain": d[2] - 256 if d[2] & 0x80 else d[2],
                "played": d[3]}
    if kind == KIND_SYS_END and len(d) >= 4:
        return {"state": d[0], "cause": d[1], "end": d[2], "how": d[3],
                "state_name": _name(STATES, d[0]), "cause_name": _name(EVENTS, d[1])}
    if kind == KIND_CHORD and len(d) >= 2:
        return {"what": d[0], "what_name": CHORD_WHAT.get(d[0], "?"), "chord": d[1]}
    if kind == KIND_SOURCE and len(d) >= 3:
        return {"from": d[0], "to": d[1], "event": d[2]}
    if kind == KIND_IDLE and len(d) >= 2:
        return {"history": struct.unpack_from("<H", d, 0)[0]}
    return None


def encode_entry_data(e):
    """The data bytes from the typed fields (the inverse of decode_entry_data), or None."""
    k = e["kind"]
    if k == KIND_SYS_STATE:
        return bytes([e["from"], e["to"], e["cause"], e["transport"], e["source"]])
    if k == KIND_LED_MODE:
        return bytes([e["mode"], e["flags"], e["level"], e["full"]])
    if k == KIND_CUE:
        return bytes([e["cue"], e["script"], e["gain"] & 0xFF, e["played"]])
    if k == KIND_SYS_END:
        return bytes([e["state"], e["cause"], e["end"], e["how"]])
    if k == KIND_CHORD:
        return bytes([e["what"], e["chord"]])
    if k == KIND_SOURCE:
        return bytes([e["from"], e["to"], e["event"]])
    if k == KIND_IDLE:
        return struct.pack("<H", e["history"])
    return None
