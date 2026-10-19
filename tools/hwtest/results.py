# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# results.py: collect per-test results and write them out (docs/testing.md, "Reading the report").
# The console prints a line per test and the totals; the results folder holds report.json,
# report.md (failures first), console.log (the results and the board's console transcript) and
# ring.jsonl (ring entries of failing tests, decoded). Everything goes through mask.py twice (the
# sources mask, then every writer), raw hex of requests and answers is never written (the console
# transcript shows the op and its status instead, and a custom product name only its length),
# and a 16+ hex-digit backstop in every writer catches a field a decoder missed (a non-zero count
# is a WARN).
import datetime
import json
import os
import re

from . import config as C

PASS, FAIL, WARN, SKIP, ERROR = "PASS", "FAIL", "WARN", "SKIP", "ERROR"
_ORDER = {FAIL: 0, ERROR: 1, WARN: 2, SKIP: 3, PASS: 4}
_OP_NAMES = C.OP_NAMES
_RAW_REQ = re.compile(r"^(.*opensc2 test \d+ raw )([0-9a-fA-F]+)\s*$")
_RAW_ANS = re.compile(r"^(< .*HWT \d+ RAW )([0-9a-fA-F]+)\s*$")
# the `name` line of `opensc2 id` (the USB product name, which may be personal)
_NAME_LINE = re.compile(r"^(< \s*name {2,})(\S.*?)\s*$")


def _report_options(options):
    """The run's options as report.json keeps them. Paths the user chose (--results, --fw-emu) are
    not written as they are: masking catches a user folder, but a name elsewhere in a path the user
    picked would stay. The default results folder is named by its place in the repository, any
    other only as "(--results)"; the firmware emulator by its file name."""
    out = {k: v for k, v in options.items() if k not in ("started", "suite_version")}
    if out.get("results_dir"):
        default = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")
        same = os.path.normcase(os.path.abspath(out["results_dir"])) == os.path.normcase(default)
        out["results_dir"] = "tools/hwtest/results" if same else "(--results)"
    if out.get("fw_emu"):
        out["fw_emu"] = os.path.basename(out["fw_emu"])
    return out


class Result:
    def __init__(self, test_id, group, path):
        self.id = test_id
        self.group = group
        self.path = path
        self.status = SKIP
        self.duration_s = 0.0
        self.expected = None
        self.observed = None
        self.notes = ""
        self.ring_excerpt = []

    def set(self, status, notes="", expected=None, observed=None, ring=None):
        self.status = status
        if notes:
            self.notes = notes
        if expected is not None:
            self.expected = expected
        if observed is not None:
            self.observed = observed
        if ring is not None:
            self.ring_excerpt = ring
        return self

    def label(self):
        return "%s@%s" % (self.id, self.path) if self.path else self.id

    def to_json(self, mask):
        return {
            "id": self.id, "path": self.path, "status": self.status,
            "duration_s": round(self.duration_s, 4),
            "expected": mask.value(self.expected), "observed": mask.value(self.observed),
            "notes": mask.text(self.notes),
            "ring_excerpt": mask.value(self.ring_excerpt),
        }


def decode_transcript_line(line):
    """A console transcript line without raw hex: a `raw` request shows its op, an `RAW` answer
    its op, status and length; a custom product name in `opensc2 id` shows its length only."""
    m = _RAW_REQ.match(line)
    if m:
        try:
            req = bytes.fromhex(m.group(2))
            return m.group(1) + "<%s, %d bytes>" % (_OP_NAMES.get(req[6], "op 0x%02x" % req[6])
                                                    if len(req) > 6 else "?", len(req))
        except ValueError:
            return m.group(1) + "<not hex>"
    m = _RAW_ANS.match(line)
    if m:
        try:
            ans = bytes.fromhex(m.group(2))
            return m.group(1) + "<%s %s, %d result bytes>" % (
                _OP_NAMES.get(ans[2], "op 0x%02x" % ans[2]) if len(ans) > 3 else "?",
                C.STATUS_NAMES.get(ans[3], "?") if len(ans) > 3 else "?", max(0, len(ans) - 4))
        except ValueError:
            return m.group(1) + "<not hex>"
    m = _NAME_LINE.match(line)
    if m and m.group(2) != C.DEFAULT_PRODUCT_NAME:
        return m.group(1) + "<a custom name, %d characters>" % len(m.group(2))
    return line


class Results:
    def __init__(self, mask, quiet=False):
        self.mask = mask
        self.quiet = quiet
        self.groups = []            # list of {name, path, tests: [Result]}
        self._by_group = {}
        self.safety = {"refusals": 0, "puck_writes": 0, "flash_writes": 0, "hex_dropped": 0,
                       "leak_events": 0, "leak_ignored": 0, "canary": None, "sole_reader": None,
                       "lease_lost": 0}
        self.left_as_found = {}
        self.console_lines = []
        self.console_lines_raw = []
        self.ring_lines = []
        self.puck_slot = None
        self.snapshot_notes = []
        self.steam_log = []

    def group(self, name, path=None):
        key = name
        if key not in self._by_group:
            g = {"name": name, "path": path, "tests": []}
            self._by_group[key] = g
            self.groups.append(g)
        return self._by_group[key]

    def add(self, result):
        self.group(result.group, result.path)["tests"].append(result)
        line = "%-5s %-30s %6.2fs  %s" % (result.status, self.mask.text(result.label()),
                                          result.duration_s, self.mask.text(result.notes))
        if not self.quiet:
            print(line)
        self.console_lines.append(line)
        for e in result.ring_excerpt:
            self.ring_lines.append({"test": result.id, "entry": self.mask.value(e)})

    def log(self, line):
        """A line that is not a test result (the through-puck writes, say): printed unless
        quiet, and kept in console.log, masked."""
        line = self.mask.text(line)
        if not self.quiet:
            print(line)
        self.console_lines.append(line)

    def counts(self):
        c = {PASS: 0, FAIL: 0, WARN: 0, SKIP: 0, ERROR: 0}
        for g in self.groups:
            for t in g["tests"]:
                c[t.status] = c.get(t.status, 0) + 1
        return c

    def exit_code(self, base=0):
        c = self.counts()
        code = base
        if c[FAIL] or c[ERROR]:
            code = max(code, 1)
        return code

    def print_summary(self, code):
        c = self.counts()
        print("")
        for g in self.groups:
            gc = {}
            for t in g["tests"]:
                gc[t.status] = gc.get(t.status, 0) + 1
            print("  %-10s %s" % (g["name"], "  ".join("%s %d" % (k, gc[k]) for k in
                                                        (PASS, FAIL, WARN, ERROR, SKIP)
                                                        if gc.get(k))))
        print("")
        print("  ".join("%s %d" % (k, c[k]) for k in (PASS, FAIL, WARN, ERROR, SKIP)))
        if self.mask.hex_dropped:
            print("WARN: %d hex runs masked by the backstop (a decoder missed a field)"
                  % self.mask.hex_dropped)
        print("exit code %d" % code)

    def write(self, out_dir, options, host, board, puck, steam, exit_code):
        self.safety["hex_dropped"] = self.mask.hex_dropped
        os.makedirs(out_dir, exist_ok=True)
        puck = dict(puck or {})
        if self.puck_slot is not None:
            puck["slot"] = self.puck_slot
        report = {
            "suite_version": options.get("suite_version"),
            "started": options.get("started"),
            "finished": datetime.datetime.now().isoformat(timespec="seconds"),
            "options": _report_options(options),
            "host": host,
            "board": self.mask.value(board),
            "puck": self.mask.value(puck),
            "steam": steam,
            "groups": [{"name": g["name"], "path": g["path"],
                        "status": _group_status(g),
                        "tests": [t.to_json(self.mask) for t in g["tests"]]} for g in self.groups],
            "safety": self.mask.value(self.safety),
            "left_as_found": self.mask.value(self.left_as_found),
            "exit_code": exit_code,
        }
        with open(os.path.join(out_dir, "report.json"), "w", encoding="utf-8") as f:
            json.dump(_hex_backstop(self.mask.value(report), self.mask), f, indent=2)
        with open(os.path.join(out_dir, "report.md"), "w", encoding="utf-8") as f:
            f.write(_hex_backstop(self._markdown(exit_code), self.mask))
        with open(os.path.join(out_dir, "console.log"), "w", encoding="utf-8") as f:
            for ln in self.console_lines:
                f.write(_hex_backstop(self.mask.text(ln), self.mask) + "\n")
            if self.console_lines_raw:
                f.write("\n# the board's console, masked\n")
                for ln in self.console_lines_raw:
                    f.write(_hex_backstop(self.mask.text(decode_transcript_line(ln),
                                                         transcript=True), self.mask) + "\n")
            if self.steam_log:
                f.write("\n# Steam's controller log (the lines with known phrases), masked\n")
                for ln in self.steam_log:
                    f.write(_hex_backstop(self.mask.text(ln, transcript=True), self.mask) + "\n")
        with open(os.path.join(out_dir, "ring.jsonl"), "w", encoding="utf-8") as f:
            for row in self.ring_lines:
                f.write(json.dumps(_hex_backstop(row, self.mask)) + "\n")
        return report

    def _markdown(self, code):
        lines = ["# OpenSC2 hardware test report", ""]
        c = self.counts()
        lines.append("`%s`  exit code **%d**" % ("  ".join("%s %d" % (k, c[k]) for k in
                     (PASS, FAIL, WARN, ERROR, SKIP)), code))
        lines.append("")
        failing = [t for g in self.groups for t in g["tests"] if t.status in (FAIL, ERROR)]
        if failing:
            lines += ["## Failures first", "", "| Test | Status | Notes |", "| --- | --- | --- |"]
            for t in failing:
                lines.append("| %s | %s | %s |" % (self.mask.text(t.label()), t.status,
                                                   _cell(self.mask.text(t.notes))))
            lines.append("")
        for g in self.groups:
            lines += ["## %s" % g["name"], "", "| Test | Status | Time | Notes |",
                      "| --- | --- | --- | --- |"]
            for t in sorted(g["tests"], key=lambda r: _ORDER.get(r.status, 9)):
                lines.append("| %s | %s | %.2f s | %s |" % (self.mask.text(t.label()), t.status,
                                                           t.duration_s,
                                                           _cell(self.mask.text(t.notes))))
            lines.append("")
        lines += ["## Safety", "", "| | |", "| --- | --- |"]
        for k, v in sorted(self.safety.items()):
            lines.append("| %s | %s |" % (k, v))
        if self.left_as_found:
            lines += ["", "## Left as found", "", "| | |", "| --- | --- |"]
            for k, v in sorted(self.left_as_found.items()):
                lines.append("| %s | %s |" % (k, v))
        return "\n".join(lines) + "\n"


def _cell(text):
    return (text or "").replace("|", "/").replace("\n", " ")


def _group_status(g):
    statuses = [t.status for t in g["tests"]]
    for s in (FAIL, ERROR, WARN, PASS):
        if s in statuses:
            return s
    return SKIP


_HEXRUN = re.compile(r"[0-9A-Fa-f]{16,}")


def _hex_backstop(obj, mask):
    """A final pass over everything written, which also makes it JSON-safe: a run of 16+ hex digits
    in a string becomes <hex:N> and is counted; bytes become lists of numbers (never hex strings);
    sets become sorted lists; the raw entry/data byte blobs of a ring excerpt are dropped (the
    decoded fields stay). Numbers pass through."""
    if isinstance(obj, str):
        out, n = _HEXRUN.subn(lambda m: "<hex:%d>" % len(m.group(0)), obj)
        mask.hex_dropped += n
        return out
    if isinstance(obj, (bytes, bytearray)):
        return list(obj)
    if isinstance(obj, set):
        return sorted(obj)
    if isinstance(obj, dict):
        return {k: _hex_backstop(v, mask) for k, v in obj.items()
                if k not in ("raw", "data")}
    if isinstance(obj, (list, tuple)):
        return [_hex_backstop(v, mask) for v in obj]
    return obj
