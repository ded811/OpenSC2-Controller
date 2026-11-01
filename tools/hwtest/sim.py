# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# sim.py: the offline simulator, the backend of `--dry-run`. A virtual clock (every
# wait goes through it, so a full run takes seconds), the board in both modes (simfw.SimBoard,
# which follows the library's rules), what survives its reboots (bonds, identity, chime, policy,
# the USB session log), the devices Windows would list (the board, its console, the puck's four
# slots with a second controller streaming on one of them, and a real controller of yours, which
# raises if anything opens it), the puck's own reports, Steam, and the console with shell.c's
# lines. `--sim-fault NAME` makes exactly one thing misbehave, to show the suite can fail. A
# --bluetooth dry run (start_mode "B") boots the board on its Bluetooth transport, with a Windows
# host bonded to it and a real controller's Bluetooth device next to it (simble.py).
import random
import struct
import threading

from . import config as C
from . import safety
from . import simble
from . import simfw
from .proto import reports as R
from .proto import shellfmt as SF
from .proto import steamlog
from .proto import testproto as T
from .transport import Stall

SIM_UNIT_SERIAL = "OSC0123456789"
SIM_BOARD_SERIAL = "OSB0123456789"
SIM_PUCK_SERIAL = "FAKEPUCK00001"
SIM_REAL_CONTROLLER = "NOTOSC000SIM1"
SIM_BOND_A = struct.pack("<II", 0x11111111, 0x22222222) + SIM_PUCK_SERIAL.encode().ljust(16, b"\0")
FAULTS = ("guard_leak", "seq_gap", "trigger_bit", "stall_missing", "no_reconnect", "stale_battery",
          "lease_ignored", "not_as_found", "steam_appears", "lizard_ignored", "second_reader",
          "share_not_enforced", "leak_event", "board_reset", "wrong_mode", "puck_reset",
          "com_renumbered", "console_busy", "keepalive_race", "answer_dropped",
          "pad_blank_ignored", "glide_ignored", "ticks_ignored")


class VirtualClock:
    """A clock that advances only when something waits."""

    virtual = True

    def __init__(self, world=None):
        self._t = 1000.0
        self.world = world

    def monotonic(self):
        return self._t

    def perf_ns(self):
        return int(round(self._t * 1e9))

    def sleep(self, seconds):
        if seconds and seconds > 0:
            self._t += seconds
            if self.world is not None:
                self.world.advance_to(self._t)


class _Port:
    """The board's console as the Rig sees a port: write() characters, drain_lines()."""

    def __init__(self, world):
        self.world = world
        self.gone = False
        self.echo = True
        self._buf = ""
        self._out = []

    def write(self, text):
        if self.gone:
            raise OSError(1167, "the console went away")
        self._buf += text
        while "\r" in self._buf:
            line, self._buf = self._buf.split("\r", 1)
            self._run(line.strip())

    def drain_lines(self):
        out, self._out = self._out, []
        return out

    def _say(self, lines):
        self._out.extend(lines)

    def _run(self, cmd):
        w = self.world
        b = w.board
        if not cmd:
            return
        if self.echo:
            self._out.append("\x1b[1;32mopensc2:~$ \x1b[m" + cmd)
        if w.rng.random() < 0.05:
            self._out.append("[00:01:02.345,678] <inf> opensc2: log noise between the lines")
        if cmd.startswith("opensc2 test ") and not w.hooks_built:
            self._say(["test: command not found"])
        elif cmd.startswith("opensc2 test "):
            self._say(self._test(cmd.split()[2:]))
        elif cmd == "shell echo off":
            self.echo = False
        elif cmd == "shell echo on":
            self.echo = True
        elif cmd.startswith("shell "):
            pass
        elif cmd == "opensc2 id":
            self._say(SF.id_lines(w.identity.as_dict()))
        elif cmd == "opensc2 status":
            ble = (b.ble.link_state(), b.reason_link, b.ble.slot) if b.ble else None
            self._say(SF.status_lines(False, b.reason, "wireless" if w.policy else "auto",
                                      b.lizard_on(), b.settings[48] & 0xFF,
                                      w.identity.as_dict(),
                                      link=(b.link_state, b.last_reason, b.slot, w.channel,
                                            (47, 63, 11), 2, -40, 1000, 900, 0, 30),
                                      last=w.last_session, transport=w.transport_names(),
                                      ble_link=ble))
        elif cmd == "opensc2 transport":
            word = "ble" if w.transport == "ble" else "puck"
            self._say([SF.line("transport", word, w.transport_names()[0], word, word)])
        elif cmd == "opensc2 ble status":
            if not w.ble_build:
                self._say(["ble: command not found"])
            elif b.ble is None:
                self._say(SF.ble_status_lines({"enabled": False, "running": False,
                                               "name": None, "address": None}))
            else:
                self._say(SF.ble_status_lines(b.ble.shell_status()))
        elif cmd == "opensc2 ble pair" and b.ble is not None:
            b.ble.start(pair=True)
        elif cmd == "opensc2 mode":
            self._say([SF.mode_line(False, b.reason, "wireless" if w.policy else "auto")])
        elif cmd.startswith("opensc2 mode "):
            arg = cmd.split()[2]
            if arg not in ("auto", "wireless"):
                self._say([SF.line("mode_usage")])
                return
            policy = 1 if arg == "wireless" else 0
            changed = policy != w.policy
            w.policy = policy
            next_wired = policy == 0          # USB power: the board is on the PC's USB
            if not changed or next_wired == (b.mode == "W"):
                self._say([SF.line("mode_stored", arg)])
            else:
                self._say([SF.line("mode_stored_reboot", arg, "into wired mode")])
                b.leave(None, "plain")
                w.reboot_at = w.clock.monotonic() + 0.3
        elif cmd == "opensc2 bonds":
            self._say(SF.bond_lines([SIM_PUCK_SERIAL if w.bonds[0] else None,
                                     SIM_PUCK_SERIAL if w.bonds[1] else None], b.slot))
        elif cmd == "opensc2 slot":
            self._say([SF.line("slot", b.slot + 1)])
        elif cmd == "opensc2 usb":
            self._say([SF.line("usb_wireless", "present"), SF.last_line(w.last_session)])
        elif cmd == "opensc2 usb log":
            used = sum(len(x) + 9 for x in w.session_log)
            self._say([SF.line("usb_log_head", used, 16384, w.boots, "was kept")] +
                      ["%7d.%d %s" % (i, 0, x) for i, x in enumerate(w.session_log)] +
                      [SF.last_line(w.last_session)])
        elif cmd == "opensc2 state":
            self._say([SF.line("state_buttons", 0), SF.line("state_sticks", 0, 0, 0, 0),
                       SF.line("state_triggers", 0, 0),
                       SF.line("state_pad", "L", "-", 0, 0, 0, ""),
                       SF.line("state_pad", "R", "-", 0, 0, 0, ""),
                       SF.line("state_imu", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)])
        elif cmd == "opensc2 stop":
            w.radio_stop(b)
        elif cmd == "opensc2 start":
            w.radio_start(b)
        elif cmd in ("opensc2 reboot", "opensc2 reboot wireless", "opensc2 reboot wired"):
            how = {"opensc2 reboot": "plain", "opensc2 reboot wireless": "wireless",
                   "opensc2 reboot wired": "wired"}[cmd]
            self._say([SF.line("reboot", {"plain": "", "wireless": " into wireless mode",
                                          "wired": " into wired mode"}[how])])
            b.leave(None, how)
            w.reboot_at = w.clock.monotonic() + 0.1
        else:
            self._say(["%s: command not found" % cmd.split()[0]])

    def _test(self, words):
        """`opensc2 test <id> <verb> ...` (test_core.c's lines, for the verbs the runner uses)."""
        b = self.world.board
        rid = words[0] if words and words[0].isdigit() else "-"
        rest = words[1:] if rid != "-" else words
        verb = rest[0] if rest else ""
        if verb == "sync":
            b.hooks._refresh()
            return ["HWT %s END 0" % rid]
        if verb == "raw":
            try:
                req = bytes.fromhex(rest[1])
            except (IndexError, ValueError):
                return ["HWT %s ERR bad hex" % rid, "HWT %s END -22" % rid]
            ans = b.hooks.raw(req)
            if ans is None:
                return ["HWT %s ERR not a test request" % rid, "HWT %s END -22" % rid]
            self.world.after_request()
            return ["HWT %s RAW %s" % (rid, ans.hex()), "HWT %s END 0" % rid]
        if verb == "status":
            st = T.decode_status(b.hooks._op_03(2, b"")[1])
            kv = ("armed=%d guard=%d inject=%d battery=%d seq=%s mode=%s reason=%d link=%s "
                  "usb=%s slot=%d lizard=%d frames=%d ring=%d..%d up=%d nonce=%08x policy=%s "
                  "snapshot=%d" % (st["armed"], st["guard"], st["inject"], st["battery"],
                                   "run" if st["sequence"] else "idle",
                                   "wired" if st["mode"] else "wireless", st["mode_reason"],
                                   SF.LINK_STATE_KEY[st["link_state"]],
                                   SF.USB_STATE_KEY[st["usb_state"]], st["slot"] + 1,
                                   st["lizard"], st["frames"], st["ring_first"], st["ring_next"],
                                   st["uptime_ms"], st["nonce"],
                                   "wireless" if self.world.policy else "auto", st["snapshot"]))
            return ["HWT %s STATUS %s" % (rid, kv), "HWT %s END 0" % rid]
        return ["HWT %s ERR unknown verb" % rid, "HWT %s END -2" % rid]


class _PuckWriter:
    """Through-puck writes in the simulator (made only through safety.puck_writer)."""

    def __init__(self, world):
        self.world = world
        self._pending = None

    def set_feature1(self, buf):
        assert buf[0] == C.FEATURE_ID
        # S5 where it is written, as win/puck_write.py does (through the puck: reading test ops)
        safety.check_feature_buffer(bytes(buf), "puck", self.world.safety_ctx)
        b = self.world.board
        n = buf[2]
        if n > 61:
            raise Stall(31, "the puck refuses a length over its report")
        rec = bytes(buf[1:3 + n])
        self._pending = rec[0]
        rc = b.command_set("radio", rec)
        self.world.after_request()
        self._rc = rc

    def get_feature1_answer(self):
        """The puck's two-step GET: retried until the board's answer is there (at most 1.5 s)."""
        w = self.world
        b = w.board
        opcode, _p = b.stored["radio"]
        if opcode in (0x83, 0xAE):
            rc, op, data = b.command_get("radio")   # the puck answers from the identity
        else:
            w.clock.sleep(0.03)
            rc, op, data = b.command_get("radio")
        if self._pending is not None and op != self._pending:
            raise Stall(31, "the puck dropped an answer for another opcode")
        buf = bytearray(64)
        buf[0], buf[1], buf[2] = 1, op, min(len(data), 61)
        buf[3:3 + len(data[:61])] = data[:61]
        return bytes(buf)

    def write_output(self, report):
        assert 0x80 <= report[0] <= 0x89
        self.world.board.output("radio", bytes(report))
        self.world.after_request()


class SimBackend:
    kind = "sim"
    models = {"firmware", "test_channel", "usb_framing", "usb_report_ids", "console_test", "stream",
              "battery", "battery_cadence", "app_send", "outputs", "rumble_timeout", "settings",
              "watchdog", "lease", "reboot", "radio", "puck", "lizard", "inputs", "desc",
              "session_log", "steam", "detach_report", "deaf", "hwinfo", "app_raw_sticks"}

    def __init__(self, start_mode="W", fault=None, clock=None, seed=1, hooks=True):
        self.hooks_built = hooks      # False: a firmware built without CONFIG_OPENSC2_TEST_HOOKS
        # --bluetooth (start_mode "B"): a Bluetooth build on its Bluetooth transport, bonded to
        # the PC (identity 1 of slot 1), which connects it whenever it advertises for it
        self.ble_build = start_mode == "B"
        self.transport = "ble" if start_mode == "B" else "puck"
        self.ble_slot = 0
        self.ble_bonded = 1 << 1
        self.ble_host_on = True
        self.ble_seen_connected = False
        self._path = start_mode
        self.safety_ctx = None        # the Rig's live safety.Context (bound by the Rig)
        if fault is not None and fault not in FAULTS:
            raise ValueError("unknown --sim-fault %r (one of: %s)" % (fault, ", ".join(FAULTS)))
        self.fault = fault
        self.rng = random.Random(seed)
        self.clock = clock or VirtualClock(self)
        self.clock.world = self
        self.t_ms = self.clock.monotonic() * 1000.0
        self.unit_serial = SIM_UNIT_SERIAL
        self.board_serial = SIM_BOARD_SERIAL
        self.git = "osc2-app-0.2"
        self.identity = simfw.Identity(self)
        self.hid_product = None       # a product string other than the stored name (a test)
        self.extra_build_flags = 0    # caps build flags of another build (a test)
        self.cleared_build_flags = 0  # ... and those another build lacks (a test)
        self.bonds = [SIM_BOND_A, b""]
        self.slot = 0
        self.chime = 2
        self.policy = 0
        self.reset_cause = 0x01
        self.chip_id = bytes([0x5A, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77])
        self.boots = 0
        self.last_session = None
        self.session_log = []
        self.channel = 27
        self.steam = False
        self.puck_present = True
        self.board_slot = 2          # MI_02 is the board's slot
        self.reboot_at = None
        self.down_until = None
        self._com = 7
        self._reset_done = False
        self._reset_armed = False
        self._no_reconnect = False
        self._steam_after = None
        self._puck_reset_done = False
        self.injects = 0
        self._test = (None, None)
        self._race_tick = None
        self._races = 0
        self.proven_slot = None       # the slot the runner kept open (set_board_slot), as on Windows
        self._steam_next = None       # Steam's next 0x81 through the puck (--steam-control)
        self._steam_pulses = False    # Steam's pair of zero pulses sent (once per start)
        self.steam_log_lines = []     # Steam's controller log, synthetic lines only
        self.race_slipped = 0         # keep-alive SETs that landed between a SET and its GET
        self._prove_calls = 0
        self._puck_gone_until = None
        self._puck_reports = []
        self._last_7b = 0.0
        self.writes = 0
        self.reboots_requested = 0
        self._reboots_seen = 0
        if self.ble_build:
            self.extra_build_flags |= 1 << C.CAP_BLE
        self.board = simfw.SimBoard(self, "W" if start_mode == "W" else "R",
                                    1 if start_mode == "W" else 2)

    def transport_names(self):
        """`opensc2 status`'s transport line: (the running one, the stored one)."""
        name = "Bluetooth 1" if self.transport == "ble" else "puck %d" % (self.slot + 1)
        return name, name

    def board_ble_connected(self):
        b = self.board
        return not b.dead and b.mode == "R" and b.ble is not None and b.ble.connected

    def use_path(self, path):
        self._path = path

    def ble_nodes(self):
        """As WinBackend.ble_nodes(): the Bluetooth 28DE:1303 vendor collections listed, as
        (the board's, why)."""
        expect = getattr(self.safety_ctx, "ble_expect", None)
        out = []
        for n in simble.hid_nodes(self):
            f = safety.ble_node_fields(n)
            if f and f[3] == "COL03":
                cls, why = safety.classify_ble(n, expect)
                out.append((cls == safety.BOARD_BLE, why))
        return out

    # ---- time ----
    def now_ms(self):
        return int(self.t_ms)

    def advance_to(self, t):
        self.t_ms = t * 1000.0
        self.board.run_timers()
        self._check_reboot()
        self._steam_tick()

    def _check_reboot(self):
        b = self.board
        if not b.dead:
            return
        if self.reboot_at is None:
            self.reboot_at = self.clock.monotonic() + 0.1
        if self.clock.monotonic() < self.reboot_at:
            return
        how = b.reboot_to
        self.reboot_at = None
        self.boots += 1
        self.reset_cause = getattr(b, "reset_cause_next", 0x02)   # RESET_SOFTWARE, or the fault's
        if how == "wireless":
            mode, reason = "R", 2
        elif how == "wired":
            mode, reason = "W", 3
        else:
            mode, reason = ("R", 4) if self.policy else ("W", 1)
        if self.fault == "wrong_mode" and how == "wireless":
            mode, reason = "W", 1
        if self.fault == "com_renumbered":
            self._com += 1
        self.board = simfw.SimBoard(self, mode, reason)

    def after_request(self):
        self.board.run_timers()

    # ---- devices ----
    def discover(self):
        b = self.board
        nodes_board = console = None
        up = not b.dead and (self.down_until is None or self.clock.monotonic() >= self.down_until)
        if up and b.mode == "W":
            nodes_board = {"kind": "hid", "serial": self.unit_serial,
                           "instance_id": "USB\\VID_28DE&PID_1302\\" + self.unit_serial,
                           "interface_id": "\\\\?\\hid#vid_28de&pid_1302&col03#sim#{guid}",
                           "usb_product": self.usb_product(), "location": "Port_#0003.Hub_#0002"}
        if up and b.mode == "R":
            console = {"kind": "port", "com": "COM%d" % self._com, "serial": "SIMCONSOLE001",
                       "instance_id": "USB\\VID_2FE3&PID_0004\\SIMCONSOLE001",
                       "interface_id": "\\\\?\\usb#vid_2fe3&pid_0004&mi_00#sim#{guid}",
                       "usb_product": C.CONSOLE_PRODUCT}
        ble_nodes = simble.hid_nodes(self) if up else []
        ble_board = None
        expect = getattr(self.safety_ctx, "ble_expect", None)
        if self.ble_build and console and expect is not None:
            mine = [n for n in ble_nodes if safety.classify_ble(n, expect)[0] == safety.BOARD_BLE]
            ble_board = mine[0] if len(mine) == 1 else None
        slots = []
        if self.puck_present and not self._puck_gone():
            for mi in range(2, 6):
                slots.append({"kind": "hid", "serial": SIM_PUCK_SERIAL, "slot": mi,
                              "instance_id": "USB\\VID_28DE&PID_1304\\" + SIM_PUCK_SERIAL,
                              "interface_id": "\\\\?\\hid#vid_28de&pid_1304&mi_0%d&col03#sim" % mi})
        mode = ("B" if ble_board else b.mode) if up else None
        return {"board": nodes_board, "console": console, "ble_board": ble_board,
                "puck_slots": slots, "mode": mode, "extra_boards": 0, "uf2": False,
                "others": [{"kind": "hid", "serial": SIM_REAL_CONTROLLER,
                            "instance_id": "USB\\VID_28DE&PID_1302\\" + SIM_REAL_CONTROLLER,
                            "interface_id": "\\\\?\\hid#vid_28de&pid_1302&col03#real"}]}

    def _puck_gone(self):
        return self._puck_gone_until is not None and self.clock.monotonic() < self._puck_gone_until

    def steam_running(self):
        if self._steam_after is not None and self.injects >= self._steam_after:
            self._steam_after = None
            self.steam = True             # the steam_appears fault: Steam starts mid-group
        return self.steam

    # ---- Steam (--with-steam --steam-control) ----
    def steam_start(self):
        """Steam starts. While the board is connected through the puck it keeps lizard mode off
        with a 0x81 every 2.5 s on the radio link's vendor channel, sends the pads its pair of
        zero pulses once, and its controller log gets the lines of each connection."""
        self.steam = True
        self._steam_next = self.clock.monotonic() + 0.5
        self._steam_pulses = False
        self.steam_log_lines.append("[2026-10-18 12:00:00] Local Device Found")
        if self.board.mode == "R" and self.board.link_state == 4:
            self._steam_log_connect()

    def steam_stop(self):
        self.steam = False
        self._steam_next = None

    def steam_log_mark(self):
        return len(self.steam_log_lines)

    def steam_log_since(self, mark):
        return steamlog.filter_lines("\n".join(self.steam_log_lines[mark:]))

    def _steam_log_connect(self):
        ident = self.identity
        puck_id, controller_id = struct.unpack_from("<II", SIM_BOND_A, 0)
        self.steam_log_lines += [
            "[2026-10-18 12:00:01] Controller Connected to Dongle (wireless_established)",
            "[2026-10-18 12:00:01] Controller Info: HWID: %d, FWTimestamp: 0x%08X"
            % (ident.rev, ident.build),
            # synthetic bond lines: the IDs in decimal (the base Steam uses is not known), the
            # empty slot as Steam logs it; neither may reach the results
            "[2026-10-18 12:00:01] Got bond for slot 0 (1): %d %d %s"
            % (puck_id, controller_id, SIM_PUCK_SERIAL),
            "[2026-10-18 12:00:01] Got bond for slot 1 (1): 0 0",
            # a line about another controller (its serial): not about the board, dropped
            "[2026-10-18 12:00:02] Controller Info: HWID: 72, FWTimestamp: 0x6A628345 %s"
            % SIM_REAL_CONTROLLER]

    def _steam_tick(self):
        if not self.steam or self._steam_next is None:
            return
        while self._steam_next is not None and self.clock.monotonic() >= self._steam_next:
            self._steam_next += 2.5
            b = self.board
            if b.mode == "R" and b.link_state == 4 and not b.dead:
                b.command_set("radio", bytes([C.VC_LIZARD_OFF, 0]))
                if not self._steam_pulses:
                    # once: the pair of all-zero pulses Steam sends the pads (seen on the cable),
                    # target 0 (the right pad) first; the board answers each with a 0x44
                    self._steam_pulses = True
                    for target in (0, 1):
                        b.output("radio", bytes([0x81, target]) + bytes(6))

    def test_started(self, test_id, path):
        """The runner names each test as it starts: the faults that hit one test hook in here."""
        self._test = (test_id, path)
        if self.fault == "steam_appears" and (test_id, path) == ("inputs.triggers", "W"):
            self._steam_after = self.injects + 3
        if self.fault == "board_reset" and (test_id, path) == ("inputs.full", "W"):
            self._reset_armed = True
        if self.fault == "no_reconnect" and (test_id, path) == ("wireless.reconnect", "R"):
            self._no_reconnect = True     # from here on the link does not come back
        if self.fault == "puck_reset" and (test_id, path) == ("wireless.quality", "R") and \
                not self._puck_reset_done:
            self._puck_reset_done = True
            self._puck_gone_until = self.clock.monotonic() + 3.0

    def sole_reader_ok(self):
        return self.fault != "second_reader"

    def sole_reader_selfcheck_ok(self):
        return self.fault != "share_not_enforced"

    def leak_fired(self):
        return self.fault == "leak_event" and self.board.hooks.armed

    def console_open_ok(self):
        return self.fault != "console_busy"

    def start_leak_detector(self):
        return True

    # ---- the board's HID (W; B over Bluetooth) ----
    def _board_up(self):
        b = self.board
        if self._path == "B":
            if not self.board_ble_connected():
                raise Stall(1167, "the board's Bluetooth device is not there")
            return b
        if b.dead or b.mode != "W":
            raise Stall(1167, "the board is not there")
        return b

    def set_race_tick(self, tick):
        """keepalive_race: the runner's keep-alive tick (it takes the link lock, as the
        keep-alive thread does), run from a real thread right after a test SET."""
        self._race_tick = tick

    def _race(self):
        """keepalive_race: a keep-alive that wants to run between a test SET and its GET. With
        the link lock held by the runner it waits (50 ms here) and runs after the GET; without
        the lock it would store its STATUS over the runner's request."""
        t = threading.Thread(target=self._race_tick, name="sim-race", daemon=True)
        t.start()
        t.join(0.05)
        if not t.is_alive():
            self.race_slipped += 1

    def feature_set(self, buf):
        b = self._board_up()
        # S5/S7 where the bytes go out, with the live Context, as win/hid.py does
        safety.check_feature_buffer(bytes(buf), "ble" if self._path == "B" else "usb",
                                    self.safety_ctx)
        if self._path == "B":
            # Windows writes the 63-byte value (no report ID) to the feature characteristic; a
            # refusal is an ATT error, which Windows turns into a failed request
            rc = b.ble_feature_set(bytes(buf[1:64]))
            self.after_request()
            if rc:
                raise Stall(31, "ATT error (%d)" % rc)
            return
        if self.fault == "keepalive_race" and self._race_tick and self._races < 25 and \
                threading.current_thread().name != "sim-race" and buf[1] == C.TEST_OPCODE and \
                b.hooks.armed:
            self._races += 1
            rc = b.usb_feature_set(bytes(buf))
            self.after_request()
            self._race()
            if rc:
                raise Stall(31, "STALL (%d)" % rc)
            return
        if self.fault == "answer_dropped" and buf[1] == C.TEST_OPCODE and \
                not getattr(self, "_dropped", False) and b.hooks.armed and \
                T.is_request(bytes(buf[3:3 + buf[2]])) and buf[3 + 6] == C.OP_STATUS:
            self._dropped = True
            b.hooks.end_arming()   # the lease runs out between this SET and its GET
        rc = b.usb_feature_set(bytes(buf))
        self.after_request()
        if rc:
            raise Stall(31, "STALL (%d)" % rc)

    def feature_get(self):
        b = self._board_up()
        if self._path == "B":
            rc, buf = b.ble_feature_get()
            self.after_request()
            if rc:
                raise Stall(31, "ATT error (%d)" % rc)
            return buf
        rc, buf = b.usb_feature_get()
        self.after_request()
        if rc:
            raise Stall(31, "STALL (%d)" % rc)
        return buf

    def feature_raw(self, report_id, buf=None):
        """SET (buf) or GET (None) of another feature report ID: the board stalls it."""
        self._board_up().usb["requests"] += 1
        self.board.usb["stalls"] += 1
        raise Stall(31, "STALL: only feature report 1")

    def output_raw(self, kind, buf):
        """HidD_SetOutputReport / HidD_GetInputReport: the board stalls both."""
        self._board_up().usb["requests"] += 1
        self.board.usb["stalls"] += 1
        raise Stall(31, "STALL")

    def write_output(self, report):
        b = self._board_up()
        safety.check_output_report(report)
        if report[0] not in (0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89):
            raise OSError(87, "Windows refuses an undeclared output report ID")
        if self._path == "B":
            # Windows writes the report's characteristic; the library hands on its whole
            # declared size (the bytes after a shorter write keep their value: zero here)
            size = BLE_OUTPUT_SIZE[report[0]]
            body = (bytes(report[1:]) + bytes(size))[:size]
            b.output("ble", bytes([report[0]]) + body)
        else:
            b.output("usb", bytes(report) + bytes(64 - len(report)))
        self.after_request()

    def usb_product(self):
        """The product string the simulated board's USB device carries: its stored name, or
        `hid_product` when a test sets one that differs from it."""
        return self.hid_product if self.hid_product is not None else self.identity.name

    def hid_info(self):
        if self._path == "B":
            info = self._hid_info_usb()
            info.update(vid=C.BOARD_VID, pid=C.BLE_PID, version=0x0100, manufacturer="",
                        product=C.DEFAULT_PRODUCT_NAME, serial="")
            return info
        return self._hid_info_usb()

    def _hid_info_usb(self):
        october = not self.cleared_build_flags & (1 << C.CAP_USB_PROFILE_OCTOBER)
        return {"vid": 0x28DE, "pid": 0x1302, "version": 0x0404 if october else 0x0307,
                "manufacturer": "Valve Software", "product": self.usb_product(),
                "serial": self.unit_serial, "usage_page": 0xFF00, "usage": 1, "input_len": 54,
                "output_len": 64, "feature_len": 64,
                "value_caps": {"input": {0x42: 53, 0x44: 5, 0x79: 1, 0x43: 14, 0x7B: 12, 0x45: 45},
                               "output": {0x80: 9, 0x81: 7, 0x82: 3, 0x83: 9, 0x84: 8, 0x85: 3,
                                          0x86: 3, 0x87: 63, 0x88: 63, 0x89: 63},
                               "feature": {0x01: 63, 0x02: 63}},
                "collections": ["COL01", "COL02", "COL03"], "composite": False}

    # ---- the console (R) ----
    def console_port(self):
        b = self.board
        if b.dead or b.mode != "R":
            raise OSError(2, "no console")
        self._port = _Port(self)
        return self._port

    def puck_writer(self):
        self.writes += 1
        return _PuckWriter(self)

    # ---- the radio as the puck sees it ----
    def radio_stop(self, b):
        if b.ble is not None:
            b.ble.stop()
            return
        if b.mode != "R" or b.link_state == 0:
            return
        b.link_state = 0
        b.last_reason = 2
        b.hooks.note(C.RING_LINK, 0, bytes([0, 2, b.slot, self.channel, 2]))
        b.link_down()
        self._puck_reports.append((self.clock.monotonic() + 0.3, bytes([C.RPT_LINK_STATUS, 1])))

    def radio_start(self, b):
        if b.ble is not None:
            b.ble.start()
            return
        if b.mode != "R" or b.link_state == 4:
            return
        if self._no_reconnect or not self.puck_present:
            b.link_state = 3
            b.hooks.note(C.RING_LINK, 0, bytes([3, 0, b.slot, 0, 0]))
            return
        b.link_state = 3
        b.hooks.note(C.RING_LINK, 0, bytes([3, 0, b.slot, 0, 0]))
        self._connect(b, 0)

    def _connect(self, b, reason):
        b.link_state = 4
        b.hooks.note(C.RING_LINK, 0, bytes([4, reason, b.slot, self.channel, 2]))
        ident = self.identity
        git = ident.git.encode("ascii")[:12]
        b.hooks.note(C.RING_IDENTITY, 0, struct.pack("<HIII", ident.pid, ident.build,
                                                     ident.blbuild, ident.rev) +
                     git + bytes(12 - len(git)))
        b.hooks.note(C.RING_VERSION, 0, bytes([1]))
        self._puck_reports.append((self.clock.monotonic(), bytes([C.RPT_LINK_STATUS, 2])))
        if self.steam and self._steam_next is not None:
            self._steam_log_connect()

    def radio_deaf(self, b, ms):
        """RADIO_DEAF: the radio thread stops receiving; the link times out through its normal
        path when the deaf time ends, then the puck reconnects."""
        self._deaf = (self.clock.monotonic() + ms / 1000.0, b)

    def _radio_events(self):
        deaf = getattr(self, "_deaf", None)
        if deaf and self.clock.monotonic() >= deaf[0]:
            self._deaf = None
            b = deaf[1]
            if b is self.board and b.link_state == 4:
                b.hooks.deaf_ms = 0
                b.link_state = 3
                b.last_reason = 1
                b.hooks.note(C.RING_LINK, 0, bytes([3, 1, b.slot, 0, 0]))
                b.link_down()
                self._puck_reports.append((self.clock.monotonic(),
                                           bytes([C.RPT_LINK_QUALITY]) + struct.pack(
                                               "<HHHHbBBB", 500, 0, 0, 500, -90, 3,
                                               self.channel, 47)))
                self._reconnect_at = self.clock.monotonic() + 0.6
        at = getattr(self, "_reconnect_at", None)
        if at and self.clock.monotonic() >= at:
            self._reconnect_at = None
            if self.board.link_state == 3 and not self._no_reconnect:
                self._connect(self.board, 1)

    # ---- the stream ----
    def read_stream(self, path, duration_s=None, count=None):
        """Reports from the board's Col03 (W) or its slot (R) for `duration_s`, or until `count`
        state reports arrived."""
        if path == "R" and not getattr(self, "_reading_slots", False) and \
                self.proven_slot != self.board_slot:
            raise Stall(1167, "the board's slot on the puck is not proven")
        if path == "B":
            return self._read_ble(duration_s, count)
        out = []
        period = C.STATE_PERIOD_MS / 1000.0 if path == "W" else 0.0033
        end = self.clock.monotonic() + (duration_s if duration_s is not None else 3600.0)
        states = 0
        first_board = self.board
        while self.clock.monotonic() < end and (count is None or states < count):
            self.clock.sleep(period)
            b = self.board
            if b is not first_board and not first_board.dead:
                first_board = b
            if b is not first_board:
                break                 # the device read from went away: its reader ends
            if self.fault == "board_reset" and self._reset_armed and not self._reset_done:
                self._reset_done = True
                b.end_session(None)
                b.reboot_to = "plain"
                b.dead = True
                b.reset_cause_next = 0x10   # a watchdog
            if b.dead:
                for ts, q in b.queue:
                    out.append((ts, R.pad_input(q)))
                b.queue = []
                if duration_s is None:
                    break
                continue
            if path == "W" and b.mode != "W":
                break
            if path == "R":
                self._radio_events()
                for at, rep in list(self._puck_reports):
                    if at <= self.clock.monotonic():
                        self._puck_reports.remove((at, rep))
                        out.append((self.clock.perf_ns(), R.pad_input(rep)))
                if self.clock.monotonic() - self._last_7b >= 0.5 and b.link_state == 4:
                    self._last_7b = self.clock.monotonic()
                    out.append((self.clock.perf_ns(), R.pad_input(
                        bytes([C.RPT_LINK_QUALITY]) + struct.pack(
                            "<HHHHbBBB", 500, 120, 3, 12, -42, 0, self.channel, 47))))
                if b.link_state != 4:
                    continue
            rep = b.frame()
            if self.fault == "seq_gap" and path == "W" and b.sequence == 20:
                b.sequence = (b.sequence + 1) & 0xFF
            for ts, q in b.queue:
                if path == "R" and not _puck_passes(q):
                    continue
                out.append((ts, R.pad_input(q)))
                if q[0] == C.RPT_STATE:
                    states += 1
            b.queue = []
        return out

    def _read_ble(self, duration_s, count):
        """B: one state report per connection event (7.5 ms), sent as 0x45 (cut after the angular
        rate), the battery and the application's reports as they are, all padded as Windows
        pads them; nothing while the link is down."""
        out = []
        period = simble.INTERVAL_US / 1e6
        end = self.clock.monotonic() + (duration_s if duration_s is not None else 3600.0)
        states = 0
        first_board = self.board
        while self.clock.monotonic() < end and (count is None or states < count):
            self.clock.sleep(period)
            b = self.board
            if b is not first_board or b.dead:
                break
            if not self.board_ble_connected():
                b.queue = []
                if duration_s is None:
                    break
                continue
            b.frame()
            for ts, q in b.queue:
                if q[0] == C.RPT_STATE:
                    q = bytes([C.RPT_BT_STATE]) + q[1:1 + C.BLE_STATE_LEN]
                    states += 1
                out.append((ts, R.pad_input(q)))
            b.queue = []
        return out

    def flush(self, path=None):
        """Drop what arrived but was not read (old reports would look like new ones)."""
        if path == "R" and self.proven_slot != self.board_slot:
            return                        # no slot open: nothing to drop (WinBackend too)
        self.board.queue = []
        self._puck_reports = [(at, r) for at, r in self._puck_reports
                              if at > self.clock.monotonic()]

    def read_slots(self, duration_s, before=None):
        """Every slot read for `duration_s`, `before()` run once they are open: {slot number:
        [(ns, report)]}. The board's slot carries the board's stream; MI_04 streams another
        controller."""
        self._prove_calls += 1
        if self._puck_gone() or not self.puck_present:
            return None                   # the puck re-enumerates (or is not there)
        if before:
            before()
        self._reading_slots = True
        try:
            board = self.read_stream("R", duration_s=duration_s)
        finally:
            self._reading_slots = False
        other = []
        for i in range(max(1, int(duration_s / 0.004))):
            s = R.make_state52(buttons=self.rng.getrandbits(16), lstick_x=self.rng.randint(-99, 99),
                               imu_timestamp=self.rng.getrandbits(32))
            other.append((self.clock.perf_ns(), bytes([C.RPT_STATE, i & 0xFF]) + s))
        return {self.board_slot: board, 4: other, 3: [], 5: []}

    def set_board_slot(self, slot):
        self.proven_slot = slot

    # ---- reboots ----
    def find_board(self, timeout):
        """After an unplanned reset: the board on either path, looked for every 200 ms."""
        self.proven_slot = None
        start = self.clock.monotonic()
        while self.clock.monotonic() - start < timeout:
            if self.discover().get("mode") is not None:
                self._reboots_seen = self.reboots_requested
                return self.board.mode
            self.clock.sleep(C.POLL_INTERVAL_S)
        raise TimeoutError("the board did not come back within %.0f s" % timeout)

    def wait_reboot(self, from_mode, to_mode, timeout, gone_timeout=None):
        """The board rebooted (a reboot asked for since the last wait) and came back. The
        simulator's reboots come when asked for: `gone_timeout` is not needed here."""
        self.proven_slot = None
        start = self.clock.monotonic()
        while self.clock.monotonic() - start < timeout:
            if self.reboots_requested > self._reboots_seen and not self.board.dead and \
                    self.boots >= self.reboots_requested:
                self._reboots_seen = self.reboots_requested
                return self.board.mode
            self.clock.sleep(0.05)
        raise TimeoutError("the board did not come back within %.0f s" % timeout)


# the output reports' declared sizes (the Bluetooth characteristics)
BLE_OUTPUT_SIZE = {0x80: 9, 0x81: 7, 0x82: 3, 0x83: 9, 0x84: 8, 0x85: 3, 0x86: 3, 0x87: 63,
                   0x88: 63, 0x89: 63}


def _puck_passes(report):
    """The puck relays 0x40..0x45 from the board only at their exact lengths."""
    sizes = {0x40: 6, 0x41: 9, 0x42: 54, 0x43: 15, 0x44: 6, 0x45: 46}
    want = sizes.get(report[0])
    return want is None or len(report) == want
