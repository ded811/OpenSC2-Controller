# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# steamlog.py: Steam's controller log. The tail reader tracks the file's size so each
# step reads only what Steam added (a smaller file means it rotated: read from the start). Only
# lines holding one of the known phrases are kept, and of those only the ones about the board
# (board_lines: a line that names another device's serial is dropped). A "Got bond for slot" line
# never keeps its bond: its IDs are replaced by "<bond>" (or "empty" for Steam's "0 0") before
# the line is kept, whatever base Steam prints them in. The `Controller Info` line is parsed.
# Masking is done by mask.py at the writer. The self-tests use synthetic lines only; the real file
# is read at run time, on the PC the suite runs on.
import re

PHRASES = [
    "Local Device Found", "type: 28de 1302", "Release:", "Interface:",
    "Controller uses V1 HID protocol via USB", "opened for index", "Controller Info: HWID:",
    "Got bond for slot", "Rebooting into wireless mode", "wireless_unknown",
    "wireless_established", "Controller Connected to Dongle", "hid_read failure", "Lizard Mode:",
]
CONTROLLER_INFO = re.compile(r"Controller Info: HWID: (\d+), FWTimestamp: 0x([0-9A-Fa-f]{8})")
# "Got bond for slot <n> (<code>): <puck ID> <controller ID> <puck serial>", or "0 0" when empty
_BOND = re.compile(r"(Got bond for slot \S+ \([^)]*\):)(.*)$")
# a 13-character serial-like word (0-9, A-Z, at least one letter and one digit)
_SERIAL_WORD = re.compile(r"\b(?=[0-9A-Z]{13}\b)(?=[0-9A-Z]*[A-Z])(?=[0-9A-Z]*[0-9])[0-9A-Z]{13}\b")


def keep_line(line):
    """True when a log line carries one of the phrases the suite looks at."""
    return any(p in line for p in PHRASES)


def redact(line):
    """A kept line as it may be stored: a "Got bond for slot" line loses its IDs and serial
    ("<bond>"; "empty" for the "0 0" of an empty bond). Other lines pass unchanged."""
    m = _BOND.search(line)
    if not m:
        return line
    words = m.group(2).split()
    if not words or words in (["empty"], ["<bond>"]):
        return line                   # nothing to hide, or already redacted
    what = "empty" if all(w == "0" for w in words) else "<bond>"
    return line[:m.end(1)] + " " + what


def filter_lines(text):
    """Split `text` into lines and keep only the ones with a known phrase, redacted."""
    return [redact(ln) for ln in text.splitlines() if keep_line(ln)]


def board_lines(lines, board_serials):
    """The board's known serial selects the lines about the board: a kept line that
    names a device serial (a 13-character word of 0-9/A-Z) other than the board's own (its unit
    serial, its puck) is about another device and dropped. Lines that name no serial cannot be
    told apart and stay (masked at the writer)."""
    mine = {s.upper() for s in board_serials if s}
    out = []
    for ln in lines:
        named = {w.upper() for w in _SERIAL_WORD.findall(ln)}
        if named - mine:
            continue
        out.append(ln)
    return out


def parse_controller_info(line):
    """`Controller Info: HWID: <d>, FWTimestamp: 0x<8hex>` -> (hwid int, build_time int) or None."""
    m = CONTROLLER_INFO.search(line)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2), 16)


class Tail:
    """Reads the bytes added to a growing (or rotated) file since the last read. The runner gives
    it the path; it never holds the file open. Decodes UTF-8 with replacement."""

    def __init__(self):
        self._size = 0

    def mark(self, current_size):
        """Remember the file's size at the start of a step."""
        self._size = current_size

    def read_added(self, data_since_mark):
        """Given the bytes the caller read from the mark to now, return the kept, decoded lines.
        (The caller handles the file; this keeps the parsing testable.)"""
        text = bytes(data_since_mark).decode("utf-8", "replace")
        return filter_lines(text)

    @staticmethod
    def rotated(previous_size, current_size):
        return current_size < previous_size
