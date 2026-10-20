# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# fwemu.py: a dry-run backend that runs the firmware's own code (`--dry-run --fw-emu <exe>`). The
# program is tests/host/fwemu.c, built by tests/host/run.ps1 and run.sh from the library's sources
# with the test hooks: the vendor command channel with its USB framing and STALLs, the test channel
# with every op, the lease and the held answers, the console's "opensc2 test" lines, the ring, the
# counters, the settings, input injection and the state report's rules. Here only the PC side is
# Python: the same Rig, transport and groups as on hardware, so a mismatch between what the runner
# sends or expects and what the firmware does shows up as a FAIL or an ERROR.
#
# The emulator has no USB descriptors, chip ID, radio protocol or puck: rows that need them are
# SKIP ("not modelled").
import subprocess

from . import config as C
from . import safety
from .proto import reports as R
from .proto import shellfmt as SF
from .transport import Stall

SIM_UNIT = "OSC0123456789"
SIM_BOARD = "OSB0123456789"
SIM_PUCK = "FAKEPUCK00001"


class EmuError(Exception):
    pass


class EmuClock:
    """The emulator's clock: every sleep moves the firmware's time on by the same amount."""

    virtual = True

    def __init__(self, emu):
        self._emu = emu
        self._t = 1000.0

    def monotonic(self):
        return self._t

    def perf_ns(self):
        return int(self._t * 1e9)

    def sleep(self, seconds):
        if seconds and seconds > 0:
            ms = int(round(seconds * 1000))
            if ms:
                self._emu.advance(ms)
            self._t += seconds


class _Port:
    """The board's console, on the emulator: `opensc2 test ...` runs the firmware's console
    command; the other commands print shell.c's lines from what the emulator knows."""

    def __init__(self, emu):
        self.emu = emu
        self.gone = False
        self._buf = ""
        self._out = []
        self.echo = True

    def write(self, text):
        self._buf += text
        while "\r" in self._buf:
            cmd, self._buf = self._buf.split("\r", 1)
            self._run(cmd.strip())

    def drain_lines(self):
        out, self._out = self._out, []
        return out

    def _run(self, cmd):
        if not cmd:
            return
        if self.echo:
            self._out.append("opensc2:~$ " + cmd)
        emu = self.emu
        if cmd.startswith("opensc2 test "):
            self._out.extend(emu.cmd("CONSOLE " + cmd[len("opensc2 test "):]))
        elif cmd == "shell echo off":
            self.echo = False
        elif cmd == "shell echo on":
            self.echo = True
        elif cmd.startswith("shell "):
            pass
        elif cmd == "opensc2 id":
            self._out.extend(SF.id_lines(emu.identity()))
        elif cmd == "opensc2 status":
            st = emu.state()
            ident = emu.identity()
            if st["mode"] == "W":
                self._out.extend(SF.status_lines(True, 1, "auto", True, 0, ident,
                                                 usb=(3, False, 0, 0, 0, 0), last=emu.last))
            else:
                self._out.extend(SF.status_lines(False, 2, "auto", True, 0, ident,
                                                 link=(st["link"], 2 if st["link"] == 0 else 0, 0,
                                                       27, (47, 63, 11), 2, -40, 100, 100, 0, 10),
                                                 last=emu.last))
        elif cmd == "opensc2 bonds":
            self._out.extend(SF.bond_lines([SIM_PUCK, None], 0))
        elif cmd == "opensc2 mode":
            st = emu.state()
            self._out.append(SF.mode_line(st["mode"] == "W", 1 if st["mode"] == "W" else 2,
                                          "auto"))
        elif cmd.startswith("opensc2 mode "):
            # the emulator's policy is auto; confirming it stores it again, as shell.c does
            arg = cmd.split()[2]
            if arg == "auto":
                self._out.append(SF.line("mode_stored", arg))
            else:
                self._out.append(SF.line("mode_usage") if arg != "wireless" else
                                 "opensc2: the emulator keeps the policy auto")
        elif cmd == "opensc2 slot":
            self._out.append(SF.line("slot", 1))
        elif cmd == "opensc2 stop":
            emu.cmd("RADIO OFF")
        elif cmd == "opensc2 start":
            emu.cmd("RADIO ON")
        elif cmd in ("opensc2 reboot", "opensc2 reboot wireless", "opensc2 reboot wired"):
            self._out.append(SF.line("reboot", {"opensc2 reboot": "",
                                                "opensc2 reboot wireless": " into wireless mode",
                                                "opensc2 reboot wired": " into wired mode"}[cmd]))
            # the emulator decides the mode (a boot-time injection of the system ops can make a
            # plain reboot wireless); the port goes away with the boot
            emu.cmd("REBOOT " + {"opensc2 reboot": "PLAIN", "opensc2 reboot wireless": "WIRELESS",
                                 "opensc2 reboot wired": "WIRED"}[cmd])
        elif cmd in ("opensc2 usb", "opensc2 usb log"):
            self._out.append(SF.line("usb_wireless", "present"))
            self._out.append(SF.last_line(emu.last))
        elif cmd == "opensc2 state":
            self._out.extend([SF.line("state_buttons", 0), SF.line("state_sticks", 0, 0, 0, 0),
                              SF.line("state_triggers", 0, 0),
                              SF.line("state_pad", "L", "-", 0, 0, 0, ""),
                              SF.line("state_pad", "R", "-", 0, 0, 0, ""),
                              SF.line("state_imu", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)])
        else:
            self._out.append("%s: command not found" % cmd.split()[0])


class FwEmuBackend:
    kind = "fwemu"
    # what this backend shows (the groups ask before checking a behaviour)
    models = {"firmware", "test_channel", "usb_framing", "console_test", "stream", "battery",
              "battery_cadence", "app_send", "outputs", "settings", "lease", "reboot", "radio",
              "inputs", "lizard", "watchdog", "rumble_timeout", "detach_report",
              "app_raw_sticks", "system"}

    def __init__(self, exe, start_mode="W"):
        self.safety_ctx = None       # the Rig's live safety.Context (bound by the Rig)
        self.exe = exe
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, bufsize=1)
        self.clock = EmuClock(self)
        self.events = []
        self._events_seen = 0
        self._pending = []           # reports the emulator sent outside FRAMES
        self._boot_gen = 0
        self.pending_boot = None
        self.last = None            # the last wired session, as `opensc2 status` shows it
        self.mode = start_mode
        self._port = None
        self.cmd("BOOT " + start_mode)

    def close(self):
        try:
            self.proc.stdin.write("QUIT\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()

    # ---- the line protocol ----
    def cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        out = []
        while True:
            reply = self.proc.stdout.readline()
            if reply == "":
                raise EmuError("the emulator stopped")
            reply = reply.rstrip("\n")
            if reply == ".":
                return out
            if reply.startswith("EVENT BOOT "):
                self._booted(reply.split()[2])
                out.append("BOOT")           # what follows belongs to the new boot
                continue
            if reply.startswith("RPT ") and not line.startswith("FRAMES"):
                self._pending.append((self.clock.perf_ns(), self._boot_gen,
                                      bytes.fromhex(reply[4:])))
                continue
            out.append(reply)

    def _booted(self, mode):
        self.events.append(("boot", mode))
        self._boot_gen += 1
        if self.mode == "W" and mode == "R":
            self.last = (1, 0, 0, 0, 0, 0, "host: reboot into wireless")
        self.mode = mode
        if self._port is not None:
            self._port.gone = True
            self._port = None

    def advance(self, ms):
        self.cmd("ADVANCE %d" % ms)

    def state(self):
        st = {}
        for ln in self.cmd("STATE"):
            k, v = ln.split()
            st[k.lower()] = v if k == "MODE" else int(v)
        return st

    def identity(self):
        # the host stubs' identity (tests/host/stubs.c), which the emulator's caps describe
        return {"unit": SIM_UNIT, "board": SIM_BOARD, "git": "opensc2-0.2", "pid": 0x1302,
                "rev": 0x48, "build": 0x6A628345, "blbuild": 0x68D2F92E,
                "name": C.DEFAULT_PRODUCT_NAME}

    # ---- devices ----
    def discover(self):
        board = console = None
        if self.mode == "W":
            board = {"kind": "hid", "serial": SIM_UNIT,
                     "instance_id": "USB\\VID_28DE&PID_1302\\" + SIM_UNIT,
                     "interface_id": "\\\\?\\hid#vid_28de&pid_1302&col03#emu",
                     "usb_product": C.DEFAULT_PRODUCT_NAME}
        else:
            console = {"kind": "port", "com": "COM9", "serial": "EMUCONSOLE01",
                       "instance_id": "USB\\VID_2FE3&PID_0004\\EMUCONSOLE01",
                       "interface_id": "\\\\?\\usb#vid_2fe3&pid_0004&mi_00#emu",
                       "usb_product": C.CONSOLE_PRODUCT}
        return {"board": board, "console": console, "puck_slots": [], "mode": self.mode,
                "extra_boards": 0, "uf2": False}

    def steam_running(self):
        return False

    def sole_reader_ok(self):
        return True

    def sole_reader_selfcheck_ok(self):
        return True

    def leak_fired(self):
        return False

    def console_open_ok(self):
        return True

    # ---- W ----
    def feature_set(self, buf):
        if self.mode != "W":
            raise Stall(1167, "not wired")
        # S5/S7 where the bytes go out, with the live Context, as win/hid.py does
        safety.check_feature_buffer(bytes(buf), "usb", self.safety_ctx)
        reply = self.cmd("USBSET " + bytes(buf).hex())
        if not reply or reply[0] != "OK":
            raise Stall(31, reply[0] if reply else "no reply")

    def feature_get(self):
        if self.mode != "W":
            raise Stall(1167, "not wired")
        reply = self.cmd("USBGET 64")
        if not reply or not reply[0].startswith("ANS "):
            raise Stall(31, reply[0] if reply else "no reply")
        data = bytes.fromhex(reply[0][4:])
        return data + bytes(C.FEATURE_LEN - len(data))

    def write_output(self, report):
        """WriteFile on interrupt OUT: Windows hands the report over padded to 64 bytes."""
        safety.check_output_report(report)
        if report[0] not in range(0x80, 0x8A):
            raise OSError(87, "Windows refuses an undeclared output report ID")
        self.cmd("OUT " + (bytes(report) + bytes(64 - len(report))).hex())

    # ---- R ----
    def console_port(self):
        if self.mode != "R":
            raise Stall(1167, "no console while wired")
        self._port = _Port(self)
        return self._port

    def puck_writer(self):
        raise Stall(0, "the emulator has no puck")

    # ---- the stream ----
    def flush(self, path=None):
        self._pending = []
        self.cmd("FRAMES 0")

    def read_stream(self, path, duration_s=None, count=None):
        if count is None:
            count = max(1, int(round((duration_s or 0) * 1000.0 / C.STATE_PERIOD_MS)))
        gen = self._boot_gen
        stale = any(g != gen for _t, g, _r in self._pending)
        out = [(ts, R.pad_input(r)) for ts, g, r in self._pending if g == gen or path == "W"]
        self._pending = []
        if self.mode != path or (path == "W" and stale):
            return out
        t = self.clock.monotonic()
        lines = self.cmd("FRAMES %d" % count)
        n = 0
        for ln in lines:
            if ln == "BOOT":
                break                        # the device read from went away
            if not ln.startswith("RPT "):
                continue
            rep = bytes.fromhex(ln[4:])
            if rep[0] == C.RPT_STATE:
                n += 1
            ts = int((t + n * C.STATE_PERIOD_MS / 1000.0) * 1e9)
            out.append((ts, rep + bytes(max(0, C.PADDED_INPUT_LEN - len(rep)))))
        # the emulator's clock moved by count frames; keep ours with it
        self.clock._t = t + count * 4.028 / 1000.0
        return out

    # ---- reboots ----
    def find_board(self, timeout):
        """The emulator never resets unplanned: it is always there."""
        return self.mode

    def wait_reboot(self, from_mode, to_mode, timeout):
        """After a reboot request: the emulator reboots when the request falls due (a vendor
        command, 20 ms) or at once (a console reboot)."""
        if self.pending_boot:
            mode, self.pending_boot = self.pending_boot, None
            self.cmd("BOOT " + mode)
            self._booted(mode)
            self._events_seen = len(self.events)
            return mode
        waited = 0.0
        while True:
            if len(self.events) > self._events_seen:
                self._events_seen = len(self.events)
                self._pending = []
                return self.events[-1][1]
            if waited >= timeout:
                raise TimeoutError("no reboot within %.1f s" % timeout)
            self.clock.sleep(0.05)
            waited += 0.05
