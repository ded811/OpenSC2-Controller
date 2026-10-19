#!/usr/bin/env python3
# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# match_steam_version.py: make an OpenSC2 board report exactly the build time of the controller
# firmware Steam ships. Steam offers its controller firmware update to a controller that reports an
# older build time (attribute 4 of vendor command 0x83) than the controller firmware Steam ships,
# TRITON_FW_TS in its updater's bin\hardwareupdater\hardwareupdater.cfg (four KEY:HEX lines). The
# prompt goes away when the board reports a build time at least as new as Steam's current controller
# firmware (tested). A fresh build reports the time it was built, so it is newer than any controller
# firmware Steam had shipped by then; a prebuilt release reports its own build time. Run this once
# Steam ships a newer controller firmware, or for a board that reports an older value, then reboot
# the board. Declining Steam's prompt is always safe.
#
#   python -I tools\match_steam_version.py --dry-run    # what it would do; no device is opened
#   python -I tools\match_steam_version.py              # do it (the board wireless, console free)
#   python -I tools\match_steam_version.py --selftest   # its own tests: no device, no Steam
#
# Windows, Python 3.11, standard library and ctypes only. It reads Steam's file and never writes
# it. It opens exactly one device, the way tools/hwtest does: a serial port whose USB ID is
# VID_2FE3&PID_0004 with the product string "OpenSC2 console" (listed without opening anything),
# and only if it answers OpenSC2's handshake; never the puck, never a controller, never the board's
# wired HID device. To it, besides the handshake and `opensc2 id`, it sends one line:
# `opensc2 id set build 0x<TRITON_FW_TS>`, and only for a value above Steam's forced-update value
# (MUST_UPDATE_TRITON_FW_TS; nothing at all when that line is there but cannot be read) and the
# library's (0x6A18D057). What it prints is masked as hwtest's output is (no serials, no user
# folders).
import argparse
import datetime
import os
import re
import sys

LIB_FLOOR = 0x6A18D057            # the library's forced-update value (lib/opensc2/id_rules.h)
KEY_TS = "TRITON_FW_TS"           # the controller firmware Steam ships
KEY_MUST = "MUST_UPDATE_TRITON_FW_TS"   # Steam forces an update at or below this
CFG_PARTS = ("bin", "hardwareupdater", "hardwareupdater.cfg")
CFG_MAX_BYTES = 65536
DEFAULT_STEAM = r"C:\Program Files (x86)\Steam"
# where Steam records its folder: (root, key, value)
REGISTRY_PLACES = (("HKCU", r"Software\Valve\Steam", "SteamPath"),
                   ("HKLM", r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                   ("HKLM", r"SOFTWARE\Valve\Steam", "InstallPath"))

EXIT_OK, EXIT_FAILED, EXIT_REFUSED, EXIT_ENVIRONMENT, EXIT_USAGE = 0, 1, 2, 3, 4


class Refused(Exception):
    """A rule or a precondition says no (exit 2): nothing was sent."""


class Environment(Exception):
    """Not Windows, Steam's file not found or unreadable (exit 3)."""


class Failed(Exception):
    """The board did not take the value (exit 1)."""


def _hwtest():
    """tools/ on sys.path (with -I the script's own folder is not), then the hwtest modules."""
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    from hwtest import mask, safety
    from hwtest.proto import console as cproto
    return mask, safety, cproto


# ---- Steam's configuration (pure) ------------------------------------------------------------

def parse_cfg(text):
    """KEY:HEX lines -> {key: value}. Blank lines and lines without a colon are skipped; a value
    that is not 1 to 8 hex digits (an optional 0x) is kept as None, so a rule can name it."""
    out = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or ":" not in line:
            continue
        key, _sep, value = line.partition(":")
        key, value = key.strip(), value.strip()
        if value[:2].lower() == "0x":
            value = value[2:]
        out[key] = int(value, 16) if re.fullmatch(r"[0-9A-Fa-f]{1,8}", value) else None
    return out


def plan(cfg):
    """(target, floor): the build time to store, and the value it must stay above. Refused when
    Steam's file has no usable TRITON_FW_TS, when it has a MUST_UPDATE_TRITON_FW_TS that is not
    hex (Steam's floor is unknown then, and might be above the target), or when the target is at
    or below the forced-update value (Steam's MUST_UPDATE_TRITON_FW_TS, never below the library's
    own; the library's alone only when Steam's file has no such line): the board would then get a
    forced update it cannot take."""
    target = cfg.get(KEY_TS)
    if target is None:
        raise Refused("Steam's hardwareupdater.cfg has no %s with a hex value" % KEY_TS)
    if KEY_MUST in cfg and cfg[KEY_MUST] is None:
        raise Refused("Steam's hardwareupdater.cfg has a %s that is not hex, so Steam's "
                      "forced-update value is unknown: nothing sent" % KEY_MUST)
    must = cfg.get(KEY_MUST)
    floor = max(must, LIB_FLOOR) if must is not None else LIB_FLOOR
    if target <= floor:
        raise Refused("%s 0x%08X is at or below the forced-update value 0x%08X: a board "
                      "reporting it would be forced into an update it cannot take; nothing "
                      "sent" % (KEY_TS, target, floor))
    return target, floor


def command(target):
    """The one line this tool sends besides the handshake and `opensc2 id`."""
    return "opensc2 id set build 0x%08X" % target


def check_line(text, target, floor):
    """The check at the send, on the exact line: only command(target), only above the floor."""
    if target <= floor or target > 0xFFFFFFFF or text != command(target):
        raise Refused("refused to send %r: only %r may go out" % (text, command(target)))


def stored(lines):
    """The shell's answer to `opensc2 id set`: "stored; ..." (shell.c), and no error."""
    return any(ln.strip().startswith("stored;") for ln in lines)


def when(ts):
    return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


# ---- finding Steam's file (the registry and the file system are passed in) ---------------------

def steam_dirs(reg_read, environ):
    """Steam's folder as the registry records it (HKCU SteamPath, then HKLM InstallPath), then
    the default under Program Files (x86); each once."""
    out = []
    for root, key, value in REGISTRY_PLACES:
        d = reg_read(root, key, value)
        if d:
            out.append(os.path.normpath(d.replace("/", "\\")))
    pf = environ.get("ProgramFiles(x86)")
    out.append(os.path.join(pf, "Steam") if pf else DEFAULT_STEAM)
    seen, unique = set(), []
    for d in out:
        if d.lower() not in seen:
            seen.add(d.lower())
            unique.append(d)
    return unique


def find_cfg(cfg_arg, steam_dir_arg, reg_read, environ, isfile):
    """The configuration file to read: --cfg, else <--steam-dir or Steam's folder>\\bin\\
    hardwareupdater\\hardwareupdater.cfg."""
    if cfg_arg:
        if not isfile(cfg_arg):
            raise Environment("no file %s" % cfg_arg)
        return cfg_arg
    dirs = [steam_dir_arg] if steam_dir_arg else steam_dirs(reg_read, environ)
    for d in dirs:
        p = os.path.join(d, *CFG_PARTS)
        if isfile(p):
            return p
    raise Environment("Steam's hardwareupdater.cfg was not found under %s; give --steam-dir or "
                      "--cfg" % ", ".join(dirs))


def decode_cfg(data):
    """The file's bytes as text. A UTF-8 byte-order mark (an editor's, or a file written with
    Windows PowerShell's -Encoding utf8) is dropped, so that the first line's key (in Steam's file
    MUST_UPDATE_TRITON_FW_TS) still matches. Anything else that is not ASCII matches no key and no
    hex value."""
    return data.decode("utf-8-sig", "replace")


def read_cfg(path):
    """Read Steam's file, read-only, as text."""
    try:
        with open(path, "rb") as f:
            data = f.read(CFG_MAX_BYTES + 1)
    except OSError as e:
        raise Environment("could not read %s (%s)" % (path, e.strerror or e))
    if len(data) > CFG_MAX_BYTES:
        raise Environment("%s is larger than a hardwareupdater.cfg can be" % path)
    return decode_cfg(data)


def _reg_read(root, key, value):
    import winreg
    hive = {"HKCU": winreg.HKEY_CURRENT_USER, "HKLM": winreg.HKEY_LOCAL_MACHINE}[root]
    try:
        with winreg.OpenKey(hive, key, 0, winreg.KEY_READ) as k:
            v, _kind = winreg.QueryValueEx(k, value)
    except OSError:
        return None
    return v if isinstance(v, str) and v else None


# ---- the board ---------------------------------------------------------------------------------

def choose_console(nodes, safety):
    """The one node to open: exactly one OpenSC2 console as hwtest classifies it (VID_2FE3,
    PID_0004, "OpenSC2 console"). Everything else is refused, with what to do."""
    consoles, wired, uf2 = [], [], False
    for n in nodes:
        cls, _why = safety.classify(n)
        if cls == safety.BOARD_CONSOLE:
            consoles.append(n)
        elif cls == safety.BOARD_HID:
            wired.append(n)
        elif "VID_239A" in ((n.get("instance_id") or "") + (n.get("interface_id") or "")).upper():
            uf2 = True
    if len(consoles) == 1:
        return consoles[0]
    if len(consoles) > 1:
        raise Refused("%d OpenSC2 consoles are connected: leave only the board to change"
                      % len(consoles))
    if uf2:
        raise Refused("a board is in its UF2 bootloader: double-tap reset or replug it first")
    if wired:
        raise Refused("the board is wired, and a wired board has no console: let it come up "
                      "wireless (Steam's pairing reboots it so; or hold the pairing button while "
                      "plugging it in), then run this again")
    raise Refused("no OpenSC2 board found: its console is the USB serial port VID 2FE3, PID 0004, "
                  "\"OpenSC2 console\" (the board in wireless mode, CONFIG_OPENSC2_USB_CONSOLE)")


def set_build(session, target, floor, say, cproto):
    """On an open, handshaken console: read the identity, store the build time unless it is
    already there, read it back. Returns True when it changed."""
    before = cproto.parse_id(session.run("opensc2 id"))
    if "build" not in before:
        raise Failed("the console did not show the board's identity (opensc2 id)")
    say("The board reports build time 0x%08X (%s)." % (before["build"], when(before["build"])))
    if before["build"] == target:
        say("It already matches Steam's controller firmware: nothing to do.")
        return False
    if before["build"] > target:
        say("That is later than Steam's (its own build date): it now reports Steam's instead.")
    line = command(target)
    answer = session.run(line, check=lambda text: check_line(text, target, floor))
    if not stored(answer):
        raise Failed("the board did not store it; it answered: %s"
                     % (" / ".join(a.strip() for a in answer if a.strip()) or "nothing"))
    after = cproto.parse_id(session.run("opensc2 id"))
    if after.get("build") != target:
        raise Failed("read back 0x%08X, not 0x%08X" % (after.get("build") or 0, target))
    say("Stored: the board now reports 0x%08X. Steam reads it at the next connection (reboot the "
        "board, or `opensc2 reboot wireless`); on USB from the next wired boot." % target)
    return True


def run_on_board(node, target, floor, say):
    """Open the console (hwtest's port, which classifies the node again), the handshake, then
    set_build; the shell's echo, colours and prompt back on at the end. A port that does not
    answer the handshake gets nothing more."""
    _mask, _safety, cproto = _hwtest()
    from hwtest import transport
    from hwtest.board import RealClock
    from hwtest.win.serial import ConsolePort
    try:
        port = ConsolePort(node)
    except OSError as e:
        raise Refused(str(e))
    session = None
    try:
        session = transport.ConsoleSession(port, RealClock())
        try:
            session.handshake()
        except transport.ConsoleRefused as e:
            session = None
            raise Refused("%s: nothing more was sent" % e)
        return set_build(session, target, floor, say, cproto)
    except transport.ConsoleError as e:
        raise Failed("the console stopped answering (%s)" % e)
    finally:
        if session is not None and not getattr(port, "gone", False):
            session.restore_shell()
        port.close()


# ---- the command line --------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(
        prog="match_steam_version.py",
        description="Store exactly the build time of the controller firmware Steam ships on an "
                    "OpenSC2 board (over its console, wireless mode), for a board that Steam "
                    "offers an update to.")
    ap.add_argument("--dry-run", action="store_true",
                    help="only print what it would do: Steam's values, the console it would "
                         "open, the line it would send; no device is opened")
    ap.add_argument("--steam-dir", help="Steam's folder (default: from the registry)")
    ap.add_argument("--cfg", help="a hardwareupdater.cfg to read instead of Steam's")
    ap.add_argument("--selftest", action="store_true", help="run the tool's own tests and exit")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return EXIT_USAGE if e.code else EXIT_OK
    if args.selftest:
        return selftest()
    mask_mod, safety, _cproto = _hwtest()
    masker = mask_mod.Masker()

    def say(text):
        print(masker.text(text))

    windows = sys.platform == "win32"
    try:
        if not windows and not args.cfg:
            raise Environment("Steam's folder is looked up in the Windows registry: give --cfg")
        path = find_cfg(args.cfg, args.steam_dir, _reg_read if windows else lambda *a: None,
                        os.environ, os.path.isfile)
        cfg = parse_cfg(read_cfg(path))
        target, floor = plan(cfg)
        say("Steam's controller firmware: %s 0x%08X (%s), in %s"
            % (KEY_TS, target, when(target), path))
        must = cfg.get(KEY_MUST)
        say("Steam forces an update at or below 0x%08X%s." % (
            floor, "" if must == floor else
            " (the library's value; %s is %s)" % (
                KEY_MUST, "missing" if KEY_MUST not in cfg else
                "unreadable" if must is None else "0x%08X" % must)))
        if args.dry_run:
            say("Dry run: on the board's console, once it answers as OpenSC2's, it would read "
                "`opensc2 id` and, unless the board already reports 0x%08X, send:" % target)
            say("  " + command(target))
        if not windows:
            raise Environment("finding and opening the board needs Windows")
        from hwtest.win import devices
        node = choose_console(devices.enumerate_nodes(), safety)
        say("The board's console: %s (\"OpenSC2 console\", VID 2FE3)" % (node.get("com") or "?"))
        if args.dry_run:
            say("No device was opened and nothing was sent.")
            return EXIT_OK
        run_on_board(node, target, floor, say)
        return EXIT_OK
    except Refused as e:
        say("Refused: %s" % e)
        return EXIT_REFUSED
    except Environment as e:
        say("Error: %s" % e)
        return EXIT_ENVIRONMENT
    except Failed as e:
        say("Failed: %s" % e)
        return EXIT_FAILED


# ---- self-tests (no device, no Steam) ----------------------------------------------------------

def selftest():
    import unittest

    _mask, safety, cproto = _hwtest()

    def node(kind, iid, iface, serial=None, product=None, com=None):
        return {"kind": kind, "instance_id": iid, "interface_id": iface, "serial": serial,
                "usb_product": product, "com": com}

    console = node("port", "USB\\VID_2FE3&PID_0004\\0123456789ABCDEF",
                   "\\\\?\\usb#vid_2fe3&pid_0004&mi_00#x", "0123456789ABCDEF", "OpenSC2 console",
                   "COM9")
    puck_port = node("port", "USB\\VID_28DE&PID_1304&MI_00\\x", "\\\\?\\usb#vid_28de&pid_1304#x",
                     None, "Steam Controller Puck", "COM7")
    other_zephyr = node("port", "USB\\VID_2FE3&PID_0004\\X", "\\\\?\\usb#vid_2fe3&pid_0004#x",
                        "X", "Zephyr CDC ACM sample", "COM5")
    real_controller = node("hid", "USB\\VID_28DE&PID_1302\\NOTOSC000SIM1",
                           "\\\\?\\hid#vid_28de&pid_1302&col03#x", "NOTOSC000SIM1")
    wired_board = node("hid", "USB\\VID_28DE&PID_1302\\OSC0123456789",
                       "\\\\?\\hid#vid_28de&pid_1302&col03#x", "OSC0123456789")
    uf2 = node("port", "USB\\VID_239A&PID_0029\\x", "\\\\?\\usb#vid_239a#x")

    class FakeClock:
        def __init__(self):
            self.t = 0.0

        def monotonic(self):
            return self.t

        def sleep(self, s):
            self.t += max(s, 0.001)

    class FakeBoard:
        """The board's console as shell.c and test_core.c answer, for the lines this tool sends."""

        def __init__(self, build, hooks=True, refuse=False):
            self.build, self.hooks, self.refuse = build, hooks, refuse
            self.gone = False
            self.sent = []
            self._buf = ""
            self._out = []

        def write(self, text):
            self._buf += text
            while "\r" in self._buf:
                line, self._buf = self._buf.split("\r", 1)
                if line:
                    self.sent.append(line)
                    self._answer(line)

        def drain_lines(self):
            out, self._out = self._out, []
            return out

        def _answer(self, line):
            m = re.fullmatch(r"opensc2 test (\d+) sync", line)
            if m:
                self._out.append("HWT %s END 0" % m.group(1) if self.hooks else
                                 "opensc2: test: command not found")
            elif line == "opensc2 status":
                self._out.append("mode:      wireless (warm restart), policy auto")
            elif line == "opensc2 id":
                self._out += ["unit      OSC0123456789", "board     OSB0123456789",
                              "git       osc2-app-0.2", "pid       0x1302", "rev       0x48",
                              "build     0x%08x" % self.build, "blbuild   0x00000000",
                              "name      Steam Controller"]
            elif line.startswith("opensc2 id set build "):
                if self.refuse:
                    self._out.append("could not store it (-5)")
                else:
                    self.build = int(line.split()[-1], 16)
                    self._out.append("stored; new connections use it (USB: from the next boot)")

    def session_on(board):
        from hwtest import transport
        s = transport.ConsoleSession(board, FakeClock())
        s.handshake()
        return s

    class Tests(unittest.TestCase):
        CFG = "OTHER_FW_TS:6AC6869B\nTRITON_FW_TS:6AC686B3\nMUST_UPDATE_TRITON_FW_TS:6A18D057\n" \
              "MUST_UPDATE_OTHER_FW_TS:6A18D056\n"

        def test_parse(self):
            c = parse_cfg(self.CFG)
            self.assertEqual(c[KEY_TS], 0x6AC686B3)
            self.assertEqual(c[KEY_MUST], 0x6A18D057)
            self.assertEqual(parse_cfg(" TRITON_FW_TS : 0x6ac686b3 \r\n\r\nnoise\n")[KEY_TS],
                             0x6AC686B3)
            self.assertIsNone(parse_cfg("TRITON_FW_TS:6AC686G3\n")[KEY_TS])
            self.assertIsNone(parse_cfg("TRITON_FW_TS:16AC686B3\n")[KEY_TS])   # nine digits

        def test_plan(self):
            self.assertEqual(plan(parse_cfg(self.CFG)), (0x6AC686B3, 0x6A18D057))
            # at or below Steam's forced-update value, or the library's: refused
            for text in ("TRITON_FW_TS:6A18D057\nMUST_UPDATE_TRITON_FW_TS:6A18D057\n",
                         "TRITON_FW_TS:6A628345\nMUST_UPDATE_TRITON_FW_TS:6A628345\n",
                         "TRITON_FW_TS:6A628345\nMUST_UPDATE_TRITON_FW_TS:6AC686B3\n",
                         "TRITON_FW_TS:6A18D057\n", "TRITON_FW_TS:0\n"):
                with self.assertRaises(Refused):
                    plan(parse_cfg(text))
            # Steam's value raised above the library's: it counts
            self.assertEqual(plan(parse_cfg("TRITON_FW_TS:6AC686B3\n"
                                            "MUST_UPDATE_TRITON_FW_TS:6A628345\n")),
                             (0x6AC686B3, 0x6A628345))
            # without Steam's value the library's counts
            self.assertEqual(plan(parse_cfg("TRITON_FW_TS:6AC686B3\n")), (0x6AC686B3, LIB_FLOOR))
            for text in ("", "MUST_UPDATE_TRITON_FW_TS:6A18D057\n", "TRITON_FW_TS:x\n"):
                with self.assertRaises(Refused):
                    plan(parse_cfg(text))

        def test_unreadable_must(self):
            # a MUST_UPDATE_TRITON_FW_TS line that is there but not hex: Steam's floor is unknown
            # (it might be above the target), so nothing is sent; the library's floor stands in
            # only for a missing line
            for text in ("TRITON_FW_TS:6AC686B3\nMUST_UPDATE_TRITON_FW_TS:zz\n",
                         "MUST_UPDATE_TRITON_FW_TS:\nTRITON_FW_TS:6AC686B3\n",
                         "MUST_UPDATE_TRITON_FW_TS:16AC686B3\nTRITON_FW_TS:6AC686B3\n"):
                cfg = parse_cfg(text)
                self.assertIn(KEY_MUST, cfg)
                self.assertIsNone(cfg[KEY_MUST])
                with self.assertRaises(Refused):
                    plan(cfg)

        def test_byte_order_mark(self):
            # Steam's layout, MUST_UPDATE_TRITON_FW_TS first, behind a UTF-8 byte-order mark: the
            # first key still counts
            import tempfile
            bom = b"\xef\xbb\xbf"
            body = (b"MUST_UPDATE_TRITON_FW_TS:6A18D057\r\nMUST_UPDATE_OTHER_FW_TS:6A18D053\r\n"
                    b"TRITON_FW_TS:6AC686B3\r\nOTHER_FW_TS:6AC6869B\r\n")
            cfg = parse_cfg(decode_cfg(bom + body))
            self.assertEqual(cfg[KEY_MUST], 0x6A18D057)
            self.assertEqual(plan(cfg), (0x6AC686B3, 0x6A18D057))
            self.assertEqual(parse_cfg(decode_cfg(body)), cfg)      # the same without the mark
            # Steam's forced-update value raised to its current one, behind the mark: refused
            raised = bom + b"MUST_UPDATE_TRITON_FW_TS:6AC686B3\r\nTRITON_FW_TS:6AC686B3\r\n"
            with self.assertRaises(Refused):
                plan(parse_cfg(decode_cfg(raised)))
            # and the same through read_cfg, from a file
            with tempfile.TemporaryDirectory() as d:
                p = os.path.join(d, "hardwareupdater.cfg")
                with open(p, "wb") as f:
                    f.write(raised)
                with self.assertRaises(Refused):
                    plan(parse_cfg(read_cfg(p)))

        def test_the_one_line(self):
            self.assertEqual(command(0x6AC686B3), "opensc2 id set build 0x6AC686B3")
            check_line("opensc2 id set build 0x6AC686B3", 0x6AC686B3, LIB_FLOOR)
            for text in ("opensc2 id set build 0x6ac686b3", "opensc2 id set build 0x6AC686B4",
                         "opensc2 id set build 0x6AC686B3 force", "opensc2 id clear",
                         "opensc2 id set rev 0x48", "opensc2 id set build 0x6AC686B3\r",
                         "uf2", "opensc2 reboot"):
                with self.assertRaises(Refused):
                    check_line(text, 0x6AC686B3, LIB_FLOOR)
            with self.assertRaises(Refused):
                check_line(command(LIB_FLOOR), LIB_FLOOR, LIB_FLOOR)   # never at the floor

        def test_find_cfg(self):
            reg = {("HKCU", r"Software\Valve\Steam", "SteamPath"): "d:/games/steam"}
            files = {os.path.join(os.path.normpath("d:\\games\\steam"), *CFG_PARTS)}

            def reg_read(root, key, value):
                return reg.get((root, key, value))
            got = find_cfg(None, None, reg_read, {}, lambda p: p in files)
            self.assertEqual(got, os.path.join(os.path.normpath("d:\\games\\steam"), *CFG_PARTS))
            # no registry entry: the default folder
            default = os.path.join("E:\\PF86", "Steam", *CFG_PARTS)
            env = {"ProgramFiles(x86)": "E:\\PF86"}
            self.assertEqual(find_cfg(None, None, lambda *a: None, env, lambda p: p == default),
                             default)
            with self.assertRaises(Environment):
                find_cfg(None, None, lambda *a: None, {}, lambda p: False)
            with self.assertRaises(Environment):
                find_cfg("missing.cfg", None, lambda *a: None, {}, lambda p: False)
            self.assertEqual(find_cfg(None, "X:\\S", lambda *a: None, {},
                                      lambda p: p == os.path.join("X:\\S", *CFG_PARTS)),
                             os.path.join("X:\\S", *CFG_PARTS))

        def test_only_the_board_console(self):
            self.assertIs(choose_console([puck_port, real_controller, console, other_zephyr],
                                         safety), console)
            for nodes in ([], [puck_port, real_controller, other_zephyr], [wired_board],
                          [uf2], [console, dict(console, com="COM10")]):
                with self.assertRaises(Refused):
                    choose_console(nodes, safety)

        def test_set_build(self):
            said = []
            b = FakeBoard(0x6A628345)       # the July value: Steam offers an update
            self.assertTrue(set_build(session_on(b), 0x6AC686B3, LIB_FLOOR, said.append, cproto))
            self.assertEqual(b.build, 0x6AC686B3)
            self.assertEqual([ln for ln in b.sent if ln.startswith("opensc2 id set")],
                             ["opensc2 id set build 0x6AC686B3"])
            # already there: nothing sent
            b = FakeBoard(0x6AC686B3)
            self.assertFalse(set_build(session_on(b), 0x6AC686B3, LIB_FLOOR, said.append, cproto))
            self.assertFalse([ln for ln in b.sent if ln.startswith("opensc2 id set")])
            # a board without the test hooks (no sync barrier) works the same
            b = FakeBoard(0x6B000000, hooks=False)
            self.assertTrue(set_build(session_on(b), 0x6AC686B3, LIB_FLOOR, said.append, cproto))
            self.assertEqual(b.build, 0x6AC686B3)
            # the board refuses: Failed, and the value is unchanged
            b = FakeBoard(0x6A628345, refuse=True)
            with self.assertRaises(Failed):
                set_build(session_on(b), 0x6AC686B3, LIB_FLOOR, said.append, cproto)
            self.assertEqual(b.build, 0x6A628345)
            # every line that went out was the handshake's, `opensc2 id` or the one command
            for ln in b.sent:
                self.assertTrue(re.fullmatch(r"opensc2 test \d+ sync|opensc2 id|opensc2 status|"
                                             r"shell (echo|colors|prompt) (on|off)|"
                                             r"opensc2 id set build 0x6AC686B3", ln), ln)

        def test_stored(self):
            self.assertTrue(stored(["stored; new connections use it (USB: from the next boot)"]))
            self.assertFalse(stored(["could not store it (-5)"]))
            self.assertFalse(stored([]))

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Tests)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return EXIT_OK if result.wasSuccessful() else EXIT_FAILED


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
