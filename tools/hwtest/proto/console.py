# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# console.py: the board's Zephyr console. Builds the `opensc2 ...` commands the
# runner is allowed to send, filters a raw byte stream into HWT lines and other text, and parses
# the HWT grammar and the existing commands' output (shell.c formats). Pure and testable with
# synthetic transcripts (no port).
import re

VT100 = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
LOG_LINE = re.compile(r"^\[\d\d:\d\d:\d\d\.\d{3},\d{3}\] <(inf|wrn|err|dbg)> ")
HWT = re.compile(r"HWT\s+(\S+)\s+([A-Z_]+)\s*(.*)$")


def strip_vt100(line):
    return VT100.sub("", line).replace("\r", "")


def is_log_line(line):
    return bool(LOG_LINE.match(line.strip()))


def split_stream(blob):
    """Split a raw console blob into clean lines (LF separated, CR and VT100 removed)."""
    text = VT100.sub("", blob.replace("\r", ""))
    return text.split("\n")


def find_hwt(line):
    """From the first 'HWT ' on a line (anything before is prompt/echo debris): (id, tag, rest) or
    None."""
    at = line.find("HWT ")
    if at < 0:
        return None
    m = HWT.match(line[at:])
    if not m:
        return None
    return m.group(1), m.group(2), m.group(3).strip()


# ---- building commands ----------------------------------------------------------------------

def test_raw(request_hex, req_id):
    return "opensc2 test %d raw %s" % (req_id, request_hex)


def test_sync(req_id):
    return "opensc2 test %d sync" % req_id


def test_status(req_id):
    return "opensc2 test %d status" % req_id


# The existing commands the runner is allowed to send (S6). Nothing else.
EXISTING_COMMANDS = {
    "opensc2 status", "opensc2 id", "opensc2 bonds", "opensc2 slot", "opensc2 mode",
    "opensc2 usb", "opensc2 usb log", "opensc2 state", "opensc2 start", "opensc2 stop",
    "opensc2 reboot", "opensc2 reboot wireless", "opensc2 reboot wired",
    "shell echo on", "shell echo off", "shell colors on", "shell colors off",
    "shell prompt on", "shell prompt off",
}
REBOOT_COMMANDS = {"opensc2 reboot", "opensc2 reboot wireless", "opensc2 reboot wired"}


# ---- parsing the HWT grammar (docs/test-protocol.md, The console) ---------------------------

def parse_end(rest):
    """`END <rc>` -> rc int."""
    return int(rest.split()[0])


def parse_status_kv(rest):
    """`STATUS <key=value ...>` -> dict of strings. Unknown keys are kept (later versions add)."""
    out = {}
    for tok in rest.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def status_bool(kv, key):
    return kv.get(key) == "1"


# ---- parsers of the existing commands' output (shell.c formats) -----------------------------

def parse_status(lines):
    """`opensc2 status`: mode/policy, usb or link, lizard, identity, last session."""
    out = {}
    for ln in lines:
        s = ln.strip()
        m = re.match(r"mode:\s+(\w+)\s+\(([^)]*)\), policy (\w+)", s)
        if m:
            out["mode"] = m.group(1)
            out["mode_reason"] = m.group(2)
            out["policy"] = m.group(3)
            continue
        m = re.match(r"link:\s+([\w ]+?) \(last reason (-?\d+)\), bond slot (\d+)", s)
        if m:
            out["link"] = m.group(1).strip()
            out["link_reason"] = int(m.group(2))
            out["slot"] = int(m.group(3))
            continue
        m = re.match(r"usb:\s+([\w ]+?)(?: \(suspended\))?, (\d+) reports", s)
        if m:
            out["usb"] = m.group(1).strip()
            out["usb_suspended"] = "(suspended)" in s
            continue
        m = re.match(r"channels:\s+(\d+), alternates (\d+) (\d+) (\d+); protocol (\d+); "
                     r"(-?\d+) dBm", s)
        if m:
            out["channel"] = int(m.group(1))
            out["alternates"] = [int(m.group(i)) for i in (2, 3, 4)]
            out["protocol"] = int(m.group(5))
            out["rssi_dbm"] = int(m.group(6))
            continue
        m = re.match(r"traffic:\s+(\d+) packets, (\d+) reports, (\d+) dropped, up (\d+) s", s)
        if m:
            out["traffic"] = [int(m.group(i)) for i in (1, 2, 3, 4)]
            continue
        m = re.match(r"lizard:\s+(on|off), IMU mode 0x([0-9a-fA-F]+)", s)
        if m:
            out["lizard"] = m.group(1) == "on"
            out["imu_mode"] = int(m.group(2), 16)
            continue
        m = re.match(r"last wired: none since", s)
        if m:
            out["last_wired_end"] = None
            continue
        m = re.match(r"last wired: boot (\d+), (\d+) requests, (\d+) stalls, (\d+) outputs, "
                     r"(\d+) reports, (\d+) dropped; ended: (.*)$", s)
        if m:
            out["last_wired"] = [int(m.group(i)) for i in range(1, 7)]
            out["last_wired_end"] = m.group(7)
    return out


def parse_id(lines):
    """`opensc2 id`: unit, board, git, pid, rev, build, blbuild and name (the USB product name,
    the rest of its line: it may hold spaces, never at either end)."""
    out = {}
    for ln in lines:
        s = ln.strip()
        for key, pat in (("unit", r"unit\s+(\S+)"), ("board", r"board\s+(\S+)"),
                         ("git", r"git\s+(\S+)"), ("pid", r"pid\s+0x([0-9a-fA-F]+)"),
                         ("rev", r"rev\s+0x([0-9a-fA-F]+)"), ("build", r"build\s+0x([0-9a-fA-F]+)"),
                         ("blbuild", r"blbuild\s+0x([0-9a-fA-F]+)"), ("name", r"name\s+(.+)$")):
            m = re.match(pat, s)
            if m:
                out[key] = int(m.group(1), 16) if key in ("pid", "rev", "build", "blbuild") \
                    else m.group(1)
    return out


def parse_bonds(lines):
    """`opensc2 bonds`: per slot, the puck serial or empty, and which is selected."""
    out = []
    for ln in lines:
        s = ln.strip()
        m = re.match(r"slot (\d+)( \(selected\))?: puck (\S+)", s)
        if m:
            out.append({"slot": int(m.group(1)), "selected": bool(m.group(2)),
                        "puck_serial": m.group(3)})
            continue
        m = re.match(r"slot (\d+)( \(selected\))?: empty", s)
        if m:
            out.append({"slot": int(m.group(1)), "selected": bool(m.group(2)),
                        "puck_serial": None})
    return out


def parse_mode(lines):
    """`opensc2 mode` (no args): '<mode> now (<reason>); policy <policy>'."""
    for ln in lines:
        m = re.match(r"\s*(\w+) now \(([^)]*)\); policy (\w+)", ln)
        if m:
            return {"mode": m.group(1), "mode_reason": m.group(2), "policy": m.group(3)}
    return {}
