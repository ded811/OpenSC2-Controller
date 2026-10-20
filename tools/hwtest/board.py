# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# board.py: a test session, the Rig. It is the one object the groups use. It discovers and
# classifies the devices (safety.classify), routes the test channel by path (W: feature report 1 of
# the board; R: the console; P: through the puck), arms and disarms the hooks and keeps the lease
# alive, injects inputs and reads reports, writes vendor commands and output reports, runs console
# commands, switches modes and identifies the board again after every reboot. A live
# safety.Context makes every injection and every write go through the rules of safety.py first.
#
# The backend under it is the hardware (hwtest.win.backend), the simulator (hwtest.sim) or the
# firmware emulator (hwtest.fwemu); all three expose the same methods (see Backend below), so a
# group's code is the same in a dry run and on hardware.
import struct

from . import config as C
from . import safety
from . import transport
from .proto import console as cproto
from .proto import dock as D
from .proto import model as M
from .proto import reports as R
from .proto import sysproto as SP
from .proto import testproto as T
from .proto import vendor as V

Stall = transport.Stall

# On the console, one resend with a new id for the requests that read (and MARK),
# never for one that changes anything.
CONSOLE_RETRY_OPS = {C.OP_STATUS, C.OP_RING_READ, C.OP_CNT_READ, C.OP_SETTINGS_READ, C.OP_MARK}
CONSOLE_RETRY_VERBS = {"sync", "status"}
# the test-channel ops that may go through the puck (puck.test_channel): ones that only read
PUCK_TEST_OPS = {C.OP_STATUS, C.OP_RING_READ, C.OP_CNT_READ, C.OP_SETTINGS_READ}


class RealClock:
    virtual = False

    def __init__(self):
        import time
        self._time = time

    def monotonic(self):
        return self._time.monotonic()

    def perf_ns(self):
        return self._time.perf_counter_ns()

    def sleep(self, seconds):
        if seconds and seconds > 0:
            self._time.sleep(seconds)


class RigError(Exception):
    """A precondition the group cannot work around (no board, Steam open, a forbidden request):
    turns into exit code 2."""


class BoardGone(Exception):
    """The board went away outside a planned switch (an unplanned reset), or did not come back
    from one in time. The runner looks for it and goes on."""


class DeviceError(Exception):
    """The run cannot go on with the device as it is (the UF2 bootloader appeared): exit code 3,
    nothing more is written."""


class Refused(Exception):
    """A vendor command the board refused: a STALL on USB, a negative result on the test link."""

    def __init__(self, rc, stage="set"):
        super().__init__("refused (%s, %d)" % (stage, rc))
        self.rc = rc
        self.stage = stage


class NoLink(Exception):
    """R: the board's radio link is down (it did not connect within the reconnect time, one
    `opensc2 stop`/`start` included, or a HOST_SET answered that the link is not active). What
    needs the link cannot run: the test FAILs "no link", it is no safety refusal."""


class LeaseLost(Exception):
    """An injection request met an ended lease (a STALL or NOT_ARMED while armed). The Rig armed
    again, and the request was NOT sent again under that new ARM, whose canary has not run: the
    caller runs S7 again (_ensure_injection: the canary, the 0x81, STATUS) before one retry."""


# The link state "connected" in the STATUS result (docs/test-protocol.md)
LINK_CONNECTED = 4


def _injection_request(op, args):
    """The test ops S7 guards: INJECT, SEQ_RUN, APP_SEND of a controller-state report (0x45), and
    the system ops that inject into the firmware's own rules (not SYS_BOOT's clear)."""
    return safety._injection_op(op, bytes(args)) or (
        op in safety.SYSTEM_INJECTION_OPS and not safety.system_clear(op, bytes(args)))


class Answer:
    """A vendor command's answer. `data` stays in memory: it may hold serials, bonds or the chip
    ID, so groups compare it and never write it out. `masked`: the test link answered a CRC-32 of
    the answer instead of the bytes (HOST_GET of a bond, a serial or the chip ID)."""

    __slots__ = ("opcode", "n", "data", "masked", "crc", "cut")

    def __init__(self, opcode, n, data, masked=False, crc=None, cut=False):
        self.opcode = opcode
        self.n = n
        self.data = bytes(data)
        self.masked = masked
        self.crc = crc if crc is not None else T.crc32(self.data[:n])
        self.cut = cut

    def __repr__(self):
        return "<Answer 0x%02x n=%d%s>" % (self.opcode, self.n, " masked" if self.masked else "")


class Backend:
    """What every backend provides (documentation; the three implement it).

    clock                      monotonic(), perf_ns(), sleep(); `virtual` True in a dry run
    kind                       "hw", "sim" or "fwemu"
    models                     what a dry-run backend shows (the hardware: everything)
    discover()                 {board, console, puck_slots, mode, extra_boards, uf2}
    feature_set(buf64)         the board's feature report 1 SET; raises Stall
    feature_get()              its GET (64 bytes); raises Stall
    feature_raw(report_id, buf=None), output_raw(kind, buf)   the framing probes; raise Stall
    write_output(report)       an output report on the board's interrupt OUT (W)
    hid_info()                 attributes, strings, caps and collections of the board (W)
    console_port()             the board's console as a port (R): write(), drain_lines(), gone
    read_stream(path, duration_s=None, count=None)   [(perf_ns, report)] from the board's Col03
                               (W) or its proven puck slot (R); flush(path) drops what waits
    read_slots(duration_s, before)   R: every puck slot read as the sole reader, `before()` run
                               once they are open: {slot: [(perf_ns, report)]} or None
    set_board_slot(slot)       R: keep only that slot open
    puck_writer()              P: the gated writer (set_feature1, get_feature1_answer,
                               write_output); only through safety.puck_writer()
    wait_reboot(from_mode, to_mode, timeout, gone_timeout=None)
                                               the board gone (within gone_timeout from the call,
                                               default C.GONE_TIMEOUT_S * 2), then back: its mode
    find_board(timeout)        after an unplanned reset: every handle closed, the board looked
                               for on both paths until it is back: its mode (TimeoutError)
    steam_running(), sole_reader_ok(), sole_reader_selfcheck_ok(), leak_fired(),
    console_open_ok(), steam_log_mark(), steam_log_since(mark)
    """


def _models(backend, what):
    return getattr(backend, "kind", "hw") == "hw" or what in getattr(backend, "models", ())


class Rig:
    def __init__(self, backend, masker, options):
        self.backend = backend
        self.mask = masker
        self.opt = options
        self.clock = backend.clock
        self.virtual = getattr(self.clock, "virtual", False)
        self.scale = float(options.get("timeout_scale") or 1.0)
        self.ctx = safety.Context()
        self.ctx.flash_allowed = options.get("flash", False)
        self.ctx.lizard_group = False
        self.path = "W"
        self.unit_serial = None      # the board's unit serial, in memory only
        self.ble_status = None       # B: `opensc2 ble status` at discovery (address in memory)
        self.puck_present = False    # Valve's puck's slots were listed at the last discovery
        self.identity_r = None       # `opensc2 id` of the wireless phase, in memory only
        self.hooks = None            # the firmware has the hooks (hooks.present)
        self.caps = None
        self.nonce = 0
        self.console = None
        self.transcript = []
        self.lease_lost = 0
        self.inject_active = False
        self.start_snapshot = None
        self.stats_before_switch = None
        self.did_plain_reboot = False
        self.puck_writer_used = False
        self.slot_proven_at = None
        self.board_info = {}
        self._baseline = R.APP_IDLE52
        self._req_id = 0
        self._lock = transport.LinkLock("link")
        self._keepalive = None
        self._last_tick = 0.0
        self._last_lizard_off = -1e9
        self._lease_s = C.LEASE_DEFAULT_S
        self._restore_flag = True
        self._quiet = 0
        self._rearming = False
        self._last_uptime = None     # the last STATUS uptime: one that went back is a reset
        self.injection_blocked = None   # why no injection may run any more in this run
        self.guard_failed = False    # the guard canary failed (tests that need the guard skip)
        self.hangs = 0
        self.console_retries = 0
        self.console_input_lost = 0  # the shell's "RX ring buffer full" warnings, all consoles
        self.puck_writes_total = 0
        self.steam_closed_required = False   # set by the runner around the groups of S8
        self.log = lambda line: None         # the runner's log (every through-puck write)
        self._steam_polled = -1e9
        self.leak_seen = False       # S13 fired: injection stopped and the hooks disarmed
        self.link_stuck = False      # the radio link did not come back after one retry
        self.link_unstuck = False    # the last link_up() needed its stop and start
        self.puck_serials = set()    # the puck's serial (in memory only), for steam's selection
        self._exclusive = 0          # rig.exclusive(): no keep-alive between these requests
        self.stats = {}
        self._reset_stats()
        hook = getattr(backend, "set_race_tick", None)
        if hook:
            hook(self._race_tick)     # the simulator's keepalive_race fault (--sim-fault)
        # The functions that send (the backend's feature SET, the puck writer, the console) check
        # every request again with this live Context (S5, S7): the Rig's checks are pre-checks.
        try:
            backend.safety_ctx = self.ctx
        except AttributeError:
            pass

    def _reset_stats(self):
        self.puck_writes_total += self.stats.get("puck_writes", 0)
        self.stats = {"usb_requests": 0, "usb_stalls": 0, "test_requests": 0, "puck_writes": 0}

    def puck_writes(self):
        """Every write through the puck in this run (the results' safety.puck_writes)."""
        return self.puck_writes_total + self.stats.get("puck_writes", 0)

    def block_injection(self, why, guard=False):
        """A safety layer failed: no injection for the rest of the run (S7 refuses it)."""
        if self.injection_blocked is None:
            self.injection_blocked = why
        if guard:
            self.guard_failed = True

    # ---- the board's feature requests (W): a gone device and a hang are told from a STALL ----
    def _usb(self, fn, *args):
        """A feature request on the board. A STALL stays a Stall; the device gone (Win32 2, 6,
        995, 1167) is BoardGone; a hang (no completion within 6 s, cancelled) gets one 0x83 probe,
        and a second hang is the board gone too."""
        try:
            return fn(*args)
        except Stall as e:
            if e.win_error in C.DEVICE_GONE_ERRORS:
                raise BoardGone("the board went away (Win32 error %d)" % e.win_error) from e
            raise
        except TimeoutError as e:
            self.hangs += 1
            try:
                self.backend.feature_set(V.build_set(C.VC_ATTRIBUTES, b""))
            except TimeoutError:
                raise BoardGone("two feature requests hung: the board is treated as reset") \
                    from e
            except Stall as e2:
                if e2.win_error in C.DEVICE_GONE_ERRORS:
                    raise BoardGone("the board went away (Win32 error %d)" % e2.win_error) \
                        from e
            raise T.ProtocolError("a feature request hung for %.0f s (cancelled; the 0x83 probe "
                                  "answered)" % C.FEATURE_IOCTL_TIMEOUT_S) from e

    def _usb_set(self, buf):
        return self._usb(self.backend.feature_set, buf)

    def _usb_get(self):
        return self._usb(self.backend.feature_get)

    def _console(self, fn, *args):
        """A console request: the port gone is BoardGone (the board reset or was unplugged)."""
        if self.console is None:
            raise RigError("the board's console is not open")
        try:
            return fn(*args)
        except transport.ConsoleGone as e:
            raise BoardGone(str(e)) from e

    # ---- helpers ----
    def models(self, what):
        """Can this backend show `what` (always true on hardware)?"""
        return _models(self.backend, what)

    def t(self, seconds):
        return seconds * self.scale

    def sleep(self, seconds):
        """Wait; in a dry run the keep-alive is driven from here (no thread runs there)."""
        if not self.virtual:
            self.clock.sleep(seconds)
            return
        left = seconds
        while left > 1e-9:
            step = min(left, 0.5)
            self.clock.sleep(step)
            left -= step
            self._virtual_tick()

    def _virtual_tick(self):
        if self._quiet or self._exclusive or not self.ctx.armed:
            return
        if self.clock.monotonic() - self._last_tick >= C.LEASE_KEEPALIVE_S:
            with self._lock:
                self._tick()

    class _Exclusive:
        def __init__(self, rig):
            self.rig = rig

        def __enter__(self):
            self.rig._lock.__enter__()
            self.rig._exclusive += 1
            return self.rig

        def __exit__(self, *exc):
            self.rig._exclusive -= 1
            self.rig._lock.__exit__(*exc)

    def exclusive(self):
        """`with rig.exclusive():` the link lock held across several requests, so the lease
        keep-alive (whose claimed STATUS becomes the link's stored request) cannot fall between a
        test's SET and a later, separate GET [R14]. For sequences well under a second: the lease
        is 15 s."""
        return Rig._Exclusive(self)

    class _OwnLinkDrop:
        def __init__(self, rig, grace_s):
            self.rig = rig
            self.grace_s = grace_s

        def __enter__(self):
            fn = getattr(self.rig.backend, "leak_quiet", None)
            if fn:
                fn(True)
            return self.rig

        def __exit__(self, *exc):
            fn = getattr(self.rig.backend, "leak_quiet", None)
            if fn:
                fn(False, self.grace_s)

    def own_link_drop(self, grace_s=1.0):
        """`with rig.own_link_drop():` around the runner's own link drops while armed (`opensc2
        stop`/`start`, RADIO_DEAF, a turn-off). When a link drops, the puck sends empty keyboard
        and mouse reports on the slot (so that nothing stays held): those are the puck's, not a
        leak, so the leak detector ignores the slot's events from here until `grace_s` after the
        block (and counts them as leak_ignored)."""
        return Rig._OwnLinkDrop(self, grace_s)

    def _next_id(self):
        self._req_id = self._req_id % 65534 + 1
        return self._req_id

    def baseline(self):
        """The application's own state (all zero on the unwired board, but the identity
        quaternion: R.APP_IDLE52), recorded before the first ARM of each path."""
        return self._baseline

    def record_baseline(self):
        reps = [r for _t, r in self.read_stream(duration_s=1.0) if r[0] == C.RPT_STATE]
        states = [R.state52_of_report(r) for r in reps]
        notes = []
        if states:
            last = states[-1]
            if any(s != last for s in states):
                notes.append("the application's state changed within the second")
            if last != R.APP_IDLE52:
                notes.append("the application's state is not all zero, with the identity "
                             "quaternion (an input picks up noise?)")
            self._baseline = last
        return notes

    def flush(self):
        """Drop the reports that wait unread (they would look like new ones)."""
        fn = getattr(self.backend, "flush", None)
        if fn:
            fn(self.path)

    # ---- discovery ----
    def discover(self):
        d = self.backend.discover()
        if d.get("uf2"):
            raise DeviceError("the UF2 bootloader appeared: double-tap reset or replug the board "
                              "to leave it")
        if d.get("extra_boards"):
            raise RigError("more than one board is connected; choose one with --board N")
        board, console = d.get("board"), d.get("console")
        if board:
            cls, why = safety.classify(board)
            if cls != safety.BOARD_HID:
                raise RigError("the wired device is not the board (%s)" % why)
            self.path = "W"
            serial = board.get("serial")
            if self.unit_serial and serial != self.unit_serial:
                raise RigError("a different board came back (its serial differs)")
            self.unit_serial = serial
        elif console:
            cls, why = safety.classify(console)
            if cls != safety.BOARD_CONSOLE:
                raise RigError("the console is not the board's (%s)" % why)
            self.path = "R"
            self.mask.add_secret(console.get("serial"), "<usb-serial>")
        else:
            raise RigError("no board found: neither 28DE:1302 with an OSC serial nor its console")
        if self.unit_serial:
            self.mask.add_secret(self.unit_serial, "<unit>")
        for slot in d.get("puck_slots", []):
            self.mask.add_secret(slot.get("serial"), "<puck>")
            if slot.get("serial"):
                self.puck_serials.add(slot["serial"])
        self.puck_present = bool(d.get("puck_slots"))
        self._use_path()
        if self.path == "R":
            self._open_console()
            if self.opt.get("bluetooth"):
                d = self._find_ble()
        return d

    def _use_path(self):
        fn = getattr(self.backend, "use_path", None)
        if fn:
            fn(self.path)

    def _find_ble(self):
        """--bluetooth: the board is wireless (its console is open). Its console must show
        Bluetooth as the running transport and the address of the identity it advertises on;
        then Windows' HID device of exactly that address and the board's name is the board over
        Bluetooth (safety.classify_ble, S1), waited for while Windows connects it. Path B."""
        lines = self.console_run("opensc2 ble status")
        st = cproto.parse_ble_status(lines)
        if "on" not in st:
            raise RigError("--bluetooth: this firmware has no Bluetooth (build app\\ble.conf, "
                           "docs/testing.md, Bluetooth)")
        if not st.get("transport"):
            raise RigError("--bluetooth: the board's transport is not Bluetooth: `opensc2 "
                           "transport ble` on its console, then pair it in Windows (docs/testing.md,"
                           " Bluetooth)")
        address = cproto.address_hex(st.get("address"))
        if not address:
            raise RigError("--bluetooth: the board's console shows no Bluetooth address")
        self.mask.add_ble_address(address)
        self.ble_status = st
        self.ctx.ble_expect = safety.BleExpect(address, self.unit_serial)
        deadline = self.clock.monotonic() + self.t(C.BLE_RECONNECT_TIMEOUT_S)
        while True:
            d = self.backend.discover()
            node = d.get("ble_board")
            if node is not None:
                cls, why = safety.classify_ble(node, self.ctx.ble_expect)
                if cls != safety.BOARD_BLE:
                    raise RigError("the Bluetooth device is not the board (%s)" % why)
                self.path = "B"
                self._use_path()
                return d
            if self.clock.monotonic() >= deadline:
                break
            self.sleep(0.5)
        nodes = getattr(self.backend, "ble_nodes", lambda: [])()
        if not nodes:
            why = ("Windows lists no Bluetooth 28DE:1303 device: pair the board in Settings, "
                   "Bluetooth & devices, Add device (docs/testing.md, Bluetooth), and keep it "
                   "connected")
        else:
            why = "; ".join(sorted(set(w for _mine, w in nodes)))
        raise RigError("--bluetooth: the board's Bluetooth HID device is not there (%s)" % why)

    @property
    def own_hid(self):
        """The board's own HID device carries the reports, feature report 1 and the output
        reports: on USB (W) and over Bluetooth (B); through the puck (R) the test link does."""
        return self.path in ("W", "B")

    @property
    def wireless(self):
        """A wireless boot: the puck's radio (R) or Bluetooth (B); the console is there."""
        return self.path in ("R", "B")

    @property
    def console_path(self):
        """The path of test-channel requests that must work with the link down: the console on
        B (the Bluetooth HID device is gone then), else the path itself."""
        return "R" if self.path == "B" else self.path

    @property
    def reconnect_s(self):
        """How long the link may take to come back after `opensc2 start`: over Bluetooth Windows
        connects the board again itself."""
        return C.BLE_RECONNECT_TIMEOUT_S if self.path == "B" else C.RECONNECT_TIMEOUT_S

    def reopen_console(self):
        """R, B: open the console again (after you closed the program that held it)."""
        if self.wireless and self.console is None:
            self._open_console()

    def _open_console(self):
        if not self.backend.console_open_ok():
            raise RigError("close the program that holds the board's console")
        self.console = transport.ConsoleSession(self.backend.console_port(), self.clock,
                                                self.transcript, ctx=self.ctx)
        try:
            hooks = self.console.handshake()
        except transport.ConsoleRefused as e:
            self.console = None
            raise RigError(str(e))
        self.board_info["hooks"] = hooks
        ident = cproto.parse_id(self.console.run("opensc2 id"))
        unit = ident.get("unit")
        if not unit:
            raise RigError("the console did not show the board's identity")
        if self.unit_serial and unit != self.unit_serial:
            self.console = None
            raise RigError("the console belongs to another board (its unit serial differs)")
        self.unit_serial = unit
        self.mask.add_secret(unit, "<unit>")
        self.mask.add_secret(ident.get("board"), "<board>")
        self.identity_r = ident

    def identity_git(self):
        """The git revision of the identity in use (0xAE 3 on the board's own HID, `opensc2 id`
        through the puck)."""
        if self.own_hid:
            a = self.vendor(C.VC_STRING_ATTR, b"\x03")
            return a.data[1:20].split(b"\0", 1)[0].decode("ascii", "replace")
        return (self.identity_r or {}).get("git")

    # ---- the test channel ----
    def _channel(self, op, args, rid, path):
        # W and B: feature report 1 of the board's own HID device (USB, or Bluetooth); R: the
        # console; P: through the puck
        if path in ("W", "B"):
            self.stats["usb_requests"] += 2
            try:
                return transport.UsbTestChannel(self._usb_set, self._usb_get).request(op, args,
                                                                                    rid)
            except Stall:
                self.stats["usb_stalls"] += 1
                raise
        if path == "P":
            if op not in PUCK_TEST_OPS:
                raise safety.SafetyError("only reading test-channel ops go through the puck")
            w = self._puck_writer()
            self.stats["puck_writes"] += 1
            self.log("[through-puck] feature report 1: test-channel %s (0x7E), then its GET"
                     % C.OP_NAMES.get(op, "op 0x%02x" % op))
            return transport.UsbTestChannel(w.set_feature1, w.get_feature1_answer).request(
                op, args, rid)
        timeout = self.t(C.CONSOLE_SLOW_TIMEOUT_S if op in (C.OP_RING_READ, C.OP_SETTINGS_READ)
                         else C.CONSOLE_CMD_TIMEOUT_S)
        try:
            return self._console(lambda: self.console.raw(op, args, timeout))
        except (transport.ConsoleError, T.ProtocolError):
            if op not in CONSOLE_RETRY_OPS:
                raise
            self.console_retries += 1
            return self._console(lambda: self.console.raw(op, args, timeout))

    def _steam_poll(self):
        """S8, every 2 s while a group that needs Steam closed runs: Steam started means the
        injection is stopped at once and the group ends (SteamOpened)."""
        if not self.steam_closed_required:
            return
        now = self.clock.monotonic()
        if now - self._steam_polled < 2.0:
            return
        self._steam_polled = now
        if self.steam_running():
            self._steam_stop_injection()

    def _steam_stop_injection(self):
        self.steam_closed_required = False      # nothing below may poll again
        if self.ctx.armed:
            for op in (C.OP_SEQ_STOP, C.OP_INJECT_OFF):
                try:
                    with self._lock:
                        self._channel(op, b"", self._next_id(), self.path)
                except Exception:
                    pass
        self.inject_active = False
        raise safety.SteamOpened("Steam started: the injection was stopped at once (S8)")

    def test(self, op, args=b"", stall_ok=False, path=None):
        """One test-channel request on the current path (or `path`), under the link lock. Returns
        the answer as a dict, or None for a STALL when `stall_ok` (the request was not claimed:
        unarmed, or a wrong key). A STALL or NOT_ARMED while the runner believed it was armed means
        the lease ended unexpectedly: re-arm once (new nonce, the canary again) and repeat. An
        injection (INJECT, SEQ_RUN, APP_SEND of 0x45) is not repeated: the new ARM's canary has
        not run, so LeaseLost goes to the caller, which runs S7 again before its one retry."""
        path = path or self.path
        args = bytes(args)
        self._steam_poll()
        with self._lock:
            rid = self._next_id()
            try:
                a = T.parse_answer(self._channel(op, args, rid, path))
            except Stall:
                if stall_ok:
                    return None
                if op == C.OP_ARM or not self.ctx.armed or self._rearming:
                    raise
                self._lease_lost()
                if _injection_request(op, args):
                    raise LeaseLost("the lease ended before %s: armed again, not repeated"
                                    % C.OP_NAMES.get(op, "op 0x%02x" % op))
                a = T.parse_answer(self._channel(op, args, self._next_id(), path))
            if a["status"] == C.ST_NOT_ARMED and op != C.OP_ARM and self.ctx.armed \
                    and not stall_ok and not self._rearming:
                self._lease_lost()
                if _injection_request(op, args):
                    raise LeaseLost("the lease ended before %s: armed again, not repeated"
                                    % C.OP_NAMES.get(op, "op 0x%02x" % op))
                a = T.parse_answer(self._channel(op, args, self._next_id(), path))
            self.stats["test_requests"] += 1
            if self.ctx.armed:
                self._last_tick = self.clock.monotonic()
            return a

    def _lease_lost(self):
        """Re-arm once (a new nonce; the canary runs again before the next injection).
        Then a STATUS: an uptime below the last one means the board reset (BoardGone), not that
        the lease ran out."""
        self.lease_lost += 1
        self._rearming = True
        try:
            self._arm_now(self._lease_s, self._restore_flag)
            a = T.parse_answer(self._channel(C.OP_STATUS, b"", self._next_id(), self.path))
        finally:
            self._rearming = False
        if a["status"] == C.ST_OK:
            self._check_uptime(T.decode_status(a["result"]))

    def _check_uptime(self, st):
        up = st["uptime_ms"]
        last, self._last_uptime = self._last_uptime, up
        if last is not None and up + 1000 < last:
            raise BoardGone("board reset: its uptime went back from %d to %d ms" % (last, up))

    def _fresh_nonce(self):
        n = (self.clock.perf_ns() ^ (self._req_id * 0x9E3779B1)) & 0xFFFFFFFF
        if n == 0 or n == self.nonce:
            n = ((n + 0x5A5A0001) & 0xFFFFFFFF) or 1
        self.nonce_last = n
        return n

    def _arm_now(self, lease, restore, nonce=None):
        nonce = nonce if nonce is not None else self._fresh_nonce()
        flags = C.ARM_FLAG_RESTORE if restore else 0
        fresh = not self.ctx.armed
        a = T.parse_answer(self._channel(C.OP_ARM, struct.pack("<IHB", nonce, lease, flags),
                                         self._next_id(), self.path))
        if a["status"] != C.ST_OK:
            raise RigError("ARM refused (status %s)" % C.STATUS_NAMES.get(a["status"], a["status"]))
        if fresh:
            # S13 counts only what comes while armed: lizard mode's own reports while unarmed
            # (Steam's 0x81, a reboot's link drop) are on the PC by right, and the two reports a
            # fresh ARM releases go out before its guard (R6)
            epoch = getattr(self.backend, "leak_epoch", None)
            if epoch:
                epoch(0.1)
        self.caps = T.decode_caps(a["result"])
        self.nonce = nonce
        self.ctx.armed = self.ctx.guard = True
        self.ctx.canary_passed = False
        self.ctx.lizard_off = False
        self.ctx.caps_identity_write = T.cap_flag(self.caps, C.CAP_IDENTITY_VENDOR_WRITE)
        self.ctx.caps_ble = T.cap_flag(self.caps, C.CAP_BLE)
        self._last_tick = self.clock.monotonic()
        return a

    def arm(self, lease=None, restore=True, nonce=None):
        """ARM (allowed armed or not): the caps. The keep-alive runs from here on."""
        lease = C.LEASE_DEFAULT_S if lease is None else lease
        self._lease_s, self._restore_flag = lease, restore
        with self._lock:
            self._arm_now(lease, restore, nonce)
        if not self._quiet:
            self._start_keepalive()
        return self.caps

    def disarm(self):
        """DISARM: its answer, or None when the board was not armed (W: a STALL)."""
        self._stop_keepalive()
        self.inject_active = False
        try:
            a = self.test(C.OP_DISARM, stall_ok=True)
        finally:
            self.ctx.armed = self.ctx.guard = self.ctx.canary_passed = False
        return a

    def status(self):
        """STATUS as a dict; None when the board does not answer it (W unarmed: a STALL)."""
        a = self.test(C.OP_STATUS, stall_ok=not self.ctx.armed)
        if a is None or a["status"] != C.ST_OK:
            return None
        st = T.decode_status(a["result"])
        self._check_uptime(st)
        self.ctx.armed = st["armed"]
        self.ctx.guard = st["guard"]
        return st

    # ---- the keep-alive (a thread on hardware, driven by sleep() in a dry run) ----
    def leak_stop(self):
        """S13: an input event from the board's own mouse or keyboard collection while armed:
        SEQ_STOP, INJECT_OFF and DISARM at once; no injection for the rest of the run."""
        self.leak_seen = True
        self.block_injection("the leak detector fired (S13)")
        for op in (C.OP_SEQ_STOP, C.OP_INJECT_OFF, C.OP_DISARM):
            try:
                self._channel(op, b"", self._next_id(), self.console_path)
            except Exception:
                pass
        self.inject_active = False
        self.ctx.armed = self.ctx.guard = self.ctx.canary_passed = False

    def _tick(self):
        self._last_tick = self.clock.monotonic()
        if self.ctx.armed and not self.leak_seen and self.backend.leak_fired():
            self.leak_stop()
            return
        try:
            # B: through the console, which keeps the lease while the Bluetooth link is down
            self._channel(C.OP_STATUS, b"", self._next_id(), self.console_path)
        except Exception:
            return
        # lizard mode's keep-off runs for every group but lizard, whose sequences need lizard
        # mode on (a 0x81 there would end it in the middle of a sequence)
        if self.inject_active and not self.ctx.lizard_group and \
                self.clock.monotonic() - self._last_lizard_off >= 5.0:
            try:
                self._lizard_off_now()
            except Exception:
                pass

    def _race_tick(self):
        """What the keep-alive thread does, for the keepalive_race fault: the tick under the link
        lock (unless the run is quiet or unarmed, when the thread would not be running)."""
        if self._quiet or not self.ctx.armed:
            return
        with self._lock:
            self._tick()

    def _start_keepalive(self):
        if self.virtual or self._keepalive:
            return
        self._keepalive = transport.KeepAlive(self._lock, self._tick)
        self._keepalive.start()

    def _stop_keepalive(self):
        if self._keepalive:
            self._keepalive.stop()
            self._keepalive = None

    class _Quiet:
        def __init__(self, rig):
            self.rig = rig

        def __enter__(self):
            self.rig._quiet += 1
            self.rig._stop_keepalive()
            return self.rig

        def __exit__(self, *exc):
            self.rig._quiet -= 1
            if not self.rig._quiet and self.rig.ctx.armed:
                self.rig._start_keepalive()

    def quiet(self):
        """`with rig.quiet():` no keep-alive (the lease test needs the silence)."""
        return Rig._Quiet(self)

    # ---- the guard canary (hooks.guard, S7) ----
    def guard_canary(self):
        """An all-zero 0x40 and an empty 0x41 through APP_SEND: both must come back as LIZARD_RPT
        entries, with lizard_diverted 2, before anything is injected after an ARM."""
        start = self.ring_next()
        before = self.cnt_read().get("lizard_diverted", 0)
        rc1 = self._app_send(bytes([C.RPT_MOUSE]) + bytes(5))
        rc2 = self._app_send(bytes([C.RPT_KEYBOARD]) + bytes(8))
        _f, _n, entries = self.ring_all(start)
        rpts = [e["report"] for e in entries if e["kind"] == C.RING_LIZARD_RPT]
        diverted = self.cnt_read().get("lizard_diverted", 0) - before
        ok = (rc1 == 0 and rc2 == 0 and diverted == 2 and
              rpts == [bytes([C.RPT_MOUSE]) + bytes(5), bytes([C.RPT_KEYBOARD]) + bytes(8)])
        self.ctx.canary_passed = ok
        if not ok:
            self.block_injection("the guard canary failed (hooks.guard)", guard=True)
        return ok, {"rc": [rc1, rc2], "diverted": diverted, "entries": len(rpts)}

    # ---- injection (S7) ----
    def _ensure_injection(self):
        """S7, checked again before every injection: armed and the guard on, this ARM's canary
        passed, Steam closed, the sole reader, and (outside the lizard group) lizard mode off
        as the board itself reports it, after a 0x81 within the last 5 s. When one of these
        requests finds the lease ended (the Rig arms again: a new ARM, its canary not run), the
        preparation runs once more from the start; a second time is LeaseLost."""
        lost = self.lease_lost
        self._prepare_injection()
        if self.lease_lost != lost:
            lost = self.lease_lost
            self._prepare_injection()
            if self.lease_lost != lost:
                raise LeaseLost("the lease ended twice while an injection was prepared (the "
                                "keep-alive is being starved)")
        safety.check_injection(self.ctx)

    def _prepare_injection(self):
        if self.injection_blocked:
            raise safety.InjectionBlocked(self.injection_blocked)
        if self.ctx.armed and self.backend.leak_fired():
            with self._lock:
                self.leak_stop()
            self._stop_keepalive()
            raise safety.LeakFired("the leak detector fired (S13): injection stopped, disarmed")
        if self.steam_running():
            self._steam_stop_injection()
        if self.ctx.armed and not self.ctx.canary_passed:
            ok, _info = self.guard_canary()
            if not ok:
                raise safety.SafetyError("the guard canary failed: no injection in this run")
        st = self.status() if (self.wireless or not self.ctx.lizard_group) else None
        if self.path == "R" and st is not None and st["link_state"] != LINK_CONNECTED:
            # no injection without the link (after a boot into wireless it may take a
            # moment); one stop and start inside wait_link, then "no link"
            if not self.link_up():
                raise NoLink("the board's radio link is not connected (link state %d)"
                             % st["link_state"])
            st = self.status()
        if self.path == "B" and st is not None and st["link_state"] != LINK_CONNECTED:
            raise NoLink("the board's Bluetooth link is not connected (link state %d)"
                         % st["link_state"])
        if not self.ctx.lizard_group:
            if st is None or st["lizard"] or \
                    self.clock.monotonic() - self._last_lizard_off > 5.0:
                self.lizard_off()
            else:
                self.ctx.lizard_off = True

    def ready_to_inject(self):
        """Everything S7 needs before an injection, done now (the canary after an ARM, lizard
        mode off), so that a test can read its counters afterwards."""
        self._ensure_injection()

    def _inject_op(self, op, args):
        """An injection request: S7 (_ensure_injection) first, and once more before the one retry
        when the lease turned out to have ended (LeaseLost: the new ARM's canary has not run)."""
        for attempt in (0, 1):
            self._ensure_injection()
            try:
                return self.test(op, args)
            except LeaseLost:
                if attempt:
                    raise

    def inject(self, state52, nonce=None, flags=0, keep_timestamp=False):
        """INJECT a state: (answer, nonce). Its IMU timestamp gets a fresh nonce (or `nonce`), so
        the reports that carry it are known; with `keep_timestamp` the state's own timestamp is
        the identifier (inputs.imu sets that field itself)."""
        if flags & C.INJECT_FLAG_LIZARD and not self.ctx.lizard_group:
            raise safety.SafetyError("the lizard-test flag outside the lizard group")
        if keep_timestamp:
            s = bytes(state52)
            n = R.decode_state52(s)["imu_timestamp"]
        else:
            n = self._fresh_nonce() if nonce is None else nonce
            s = R.set_field(state52, "imu_timestamp", n)
        a = self._inject_op(C.OP_INJECT, bytes([flags]) + s)
        if a["status"] == C.ST_OK:
            self.inject_active = True
        return a, n

    def probe_inject_denied(self, state52):
        """inputs.lizard_lock: INJECT without the lizard-test flag while lizard mode is on, which
        the firmware must refuse (DENIED). Every precondition of S7 but lizard mode holds; if the
        firmware took it anyway, it is stopped at once and no injection runs for the rest of the
        run."""
        if not (self.ctx.armed and self.ctx.canary_passed and self.ctx.sole_reader):
            raise safety.SafetyError("the lizard-lock probe needs the guard and the sole reader")
        if self.steam_running():
            raise safety.SafetyError("Steam is running")
        self.ctx.lizard_probe = True          # the send-time S7 check lets this one through
        try:
            a = self.test(C.OP_INJECT, bytes([0]) + R.set_field(state52, "imu_timestamp",
                                                                 self._fresh_nonce()))
        finally:
            self.ctx.lizard_probe = False
        if a["status"] == C.ST_OK:
            self.test(C.OP_INJECT_OFF)
            self.block_injection("the firmware took an injection while lizard mode was on "
                                 "(inputs.lizard_lock)")
        return a

    def inject_off(self):
        a = self.test(C.OP_INJECT_OFF)
        self.inject_active = False
        return a

    def seq_load(self, step, hold, state52):
        return self.test(C.OP_SEQ_LOAD, bytes([step, hold]) + bytes(state52))

    def seq_run(self, count, flags=0):
        if flags & C.SEQ_FLAG_LIZARD and not self.ctx.lizard_group:
            raise safety.SafetyError("the lizard-test flag outside the lizard group")
        a = self._inject_op(C.OP_SEQ_RUN, bytes([count, flags]))
        if a["status"] == C.ST_OK:
            self.inject_active = True
        return a

    def seq_stop(self):
        return self.test(C.OP_SEQ_STOP)

    def battery(self, send_now=False, **fields):
        b = dict(R.BATTERY_DEFAULT_FIELDS)
        b.update(fields)
        return self.test(C.OP_BATTERY, R.encode_battery_args(send_now=send_now, **b))

    def battery_off(self):
        return self.test(C.OP_BATTERY_OFF)

    # ---- the system state machine (proto/sysproto.py) ----
    def _sys_ready(self):
        """S7 for the system ops (safety.check_system_injection): armed with the guard, this
        ARM's canary passed, Steam closed."""
        if self.injection_blocked:
            raise safety.InjectionBlocked(self.injection_blocked)
        if self.steam_running():
            self._steam_stop_injection()
        if self.ctx.armed and not self.ctx.canary_passed:
            ok, _info = self.guard_canary()
            if not ok:
                raise safety.SafetyError("the guard canary failed: no injection in this run")
        safety.check_system_injection(self.ctx)

    def _sys_op(self, op, args=b""):
        for attempt in (0, 1):
            self._sys_ready()
            try:
                return self.test(op, args)
            except LeaseLost:
                if attempt:
                    raise

    def sys_inject(self, flags, buttons=0, level=0, source=0):
        """SYS_INJECT: buttons, a battery level and/or a power source into the firmware's rules."""
        return self._sys_op(C.OP_SYS_INJECT, SP.sys_args(flags, buttons, level, source))

    def sys_inject_off(self):
        return self.test(C.OP_SYS_INJECT_OFF)

    def sys_boot(self, flags, buttons=0, level=0, source=0, hold=0):
        """SYS_BOOT: a boot-time injection for the next boot (`hold`: the buttons' time in
        100 ms; flags 0 clears it)."""
        return self._sys_op(C.OP_SYS_BOOT, SP.sys_args(flags, buttons, level, source, hold))

    def sys_boot_clear(self):
        """SYS_BOOT with flags 0: nothing left for the next boot. It injects nothing, so it
        needs none of S7's conditions (safety.system_clear); the restore sends it."""
        return self.test(C.OP_SYS_BOOT, SP.sys_args(0))

    def sys_event(self, kind):
        return self._sys_op(C.OP_SYS_EVENT, bytes([kind]))

    def sys_status(self):
        a = self.test(C.OP_SYS_STATUS)
        if a["status"] != C.ST_OK:
            return None
        return SP.decode_sys_status(a["result"])

    def inject_and_read(self, state52, nonce=None, timeout=None, keep_timestamp=False):
        """Inject and return (the first report carrying the nonce, or None; the nonce)."""
        a, n = self.inject(state52, nonce=nonce, keep_timestamp=keep_timestamp)
        if a["status"] != C.ST_OK:
            return None, n
        got = self.wait_nonce(n, timeout)
        return (got[1] if got else None), n

    # ---- raw inputs (CONFIG_OPENSC2_CAL, caps bit 21) ----
    def inject_raw(self, raw, nonce=None):
        """RAW_INJECT: raw inputs (R.encode_raw_args' fields) replace the application's before the
        library's own processing, S7 first like every injection. The frames built with them carry
        a fresh nonce (or `nonce`) in the IMU timestamp. Returns (answer, nonce)."""
        n = self._fresh_nonce() if nonce is None else nonce
        a = self._inject_op(C.OP_RAW_INJECT, R.encode_raw_args(n, **raw))
        if a["status"] == C.ST_OK:
            self.inject_active = True
        return a, n

    def raw_off(self):
        a = self.test(C.OP_RAW_OFF)
        self.inject_active = False
        return a

    def cal_status(self):
        a = self.test(C.OP_CAL_STATUS)
        if a["status"] != C.ST_OK:
            return None
        return R.decode_cal_status(a["result"])

    def nonce_reports(self, nonce, count, timeout=None):
        """The first `count` state reports carrying `nonce`, in order (fewer on a timeout)."""
        got = []

        def want(r):
            if len(r) >= C.STATE_REPORT_LEN and r[0] == C.RPT_STATE and \
                    R.nonce_of_report(r) == nonce:
                got.append(bytes(r))
            return len(got) >= count

        timeout = timeout if timeout is not None else \
            C.NONCE_DEADLINE_MS[self.path] / 1000.0 + 0.05 * count
        self.wait_report(want, timeout)
        return got

    # ---- ring, counters, settings ----
    def ring_read(self, frm, max_entries=0, path=None):
        a = self.test(C.OP_RING_READ, struct.pack("<IB", frm, max_entries), path=path)
        if a["status"] != C.ST_OK:
            raise T.ProtocolError("RING_READ status %s" % C.STATUS_NAMES.get(a["status"]))
        first, nxt, entries = T.parse_ring_read(a["result"])
        seq = max(frm, first)
        for e in entries:
            e["seq"] = seq
            seq += 1
        return first, nxt, entries

    def ring_all(self, frm=0, path=None):
        """Every entry from sequence `frm` on: (first, next, entries). `first` > `frm` means
        entries were evicted before they were read. `path`: another way to the test channel
        (rig.console_path while the Bluetooth link is down)."""
        out = []
        first = nxt = 0
        cur = frm
        for _ in range(1024):
            first, nxt, entries = self.ring_read(cur, path=path)
            out.extend(entries)
            cur = max(cur, first) + len(entries)
            if not entries or cur >= nxt:
                break
        return first, nxt, out

    def ring_next(self):
        st = self.status()
        return st["ring_next"] if st else 0

    def ring_wait(self, frm, want, timeout=0.5):
        """Read the ring from `frm` until `want(entries)` holds or `timeout` passes (entries a host
        request causes asynchronously: an output report on USB, say)."""
        deadline = self.clock.monotonic() + self.t(timeout)
        while True:
            first, nxt, entries = self.ring_all(frm)
            if want(entries) or self.clock.monotonic() >= deadline:
                return first, nxt, entries
            self.sleep(0.02)

    def cnt_read(self):
        out = {}
        first = 0
        while first < C.COUNTER_COUNT:
            a = self.test(C.OP_CNT_READ, bytes([first, min(13, C.COUNTER_COUNT - first)]))
            out.update(T.decode_counters(a["result"]))
            first += 13
        return out

    def cnt_reset(self):
        return self.test(C.OP_CNT_RESET)

    def mark(self, marker):
        a = self.test(C.OP_MARK, struct.pack("<I", marker & 0xFFFFFFFF))
        return struct.unpack_from("<I", a["result"], 0)[0] if a["status"] == C.ST_OK else None

    def rules(self):
        """The board's rule word (proto/model.py, RULE_*): (in effect, the build's). A firmware
        without RULES (caps bit 15) has OpenSC2 0.2.0's: the trigger bits when the caps say
        TRIGGER_CLICK_AUTO, nothing else."""
        if not (self.caps and T.cap_flag(self.caps, C.CAP_RULES)):
            r = M.RULES_V0_2 if self.caps is None or \
                T.cap_flag(self.caps, C.CAP_TRIGGER_CLICK_AUTO) else 0
            return r, r
        a = self.test(C.OP_RULES)
        if a["status"] != C.ST_OK or len(a["result"]) < 8:
            raise T.ProtocolError("RULES status %s" % C.STATUS_NAMES.get(a["status"]))
        now, build = struct.unpack_from("<II", a["result"], 0)
        return now, build

    def rules_set(self, rules):
        """RULES_SET: this rule word until the end of arming (lizard mode starts over). Returns
        the word in effect; None on a firmware without it."""
        if not (self.caps and T.cap_flag(self.caps, C.CAP_RULES)):
            return None
        a = self.test(C.OP_RULES_SET, struct.pack("<I", rules & 0xFFFFFFFF))
        if a["status"] != C.ST_OK:
            raise T.ProtocolError("RULES_SET status %s" % C.STATUS_NAMES.get(a["status"]))
        return struct.unpack_from("<I", a["result"], 0)[0]

    def settings_read(self):
        out = {}
        first = 0
        while first < 86:
            a = self.test(C.OP_SETTINGS_READ, bytes([first, min(27, 86 - first)]))
            if a["status"] != C.ST_OK:
                raise T.ProtocolError("SETTINGS_READ status %s" % C.STATUS_NAMES.get(a["status"]))
            out.update(T.decode_settings(a["result"]))
            first += 27
        return out

    # ---- host emulation on the test link ----
    def host_out(self, report):
        a = self.test(C.OP_HOST_OUT, bytes(report))
        return struct.unpack_from("<b", a["result"], 0)[0] if a["status"] == C.ST_OK else None

    def host_set(self, opcode, payload=b""):
        safety.check_host_set(opcode, bytes(payload), self.ctx)
        a = self.test(C.OP_HOST_SET, bytes([opcode, len(payload)]) + bytes(payload))
        if a["status"] != C.ST_OK:
            raise Refused(-1000 - a["status"], "host_set")
        return struct.unpack_from("<i", a["result"], 0)[0]

    def host_get(self):
        a = self.test(C.OP_HOST_GET)
        d = a["result"]
        if a["status"] != C.ST_OK or len(d) < 3:
            raise T.ProtocolError("HOST_GET status %s" % C.STATUS_NAMES.get(a["status"]))
        opcode, n, flags = d[0], d[1], d[2]
        if flags & 0x01:
            return Answer(opcode, n, b"", masked=True, crc=struct.unpack_from("<I", d, 3)[0])
        return Answer(opcode, n, d[3:3 + n], cut=bool(flags & 0x02))

    def _app_send(self, report):
        a = self.test(C.OP_APP_SEND, bytes([0]) + bytes(report))
        if a["status"] != C.ST_OK:
            return -1000 - a["status"]
        return struct.unpack_from("<i", a["result"], 0)[0]

    def app_send_report(self, report):
        if report and report[0] == C.RPT_BT_STATE:   # a 0x45 state report counts as an injection
            a = self._inject_op(C.OP_APP_SEND, bytes([0]) + bytes(report))
            if a["status"] != C.ST_OK:
                return -1000 - a["status"]
            return struct.unpack_from("<i", a["result"], 0)[0]
        return self._app_send(report)

    def app_send_status(self, channel, flags):
        a = self.test(C.OP_APP_SEND, bytes([1, channel]) + struct.pack("<I", flags & 0xFFFFFFFF))
        if a["status"] != C.ST_OK:
            return -1000 - a["status"]
        return struct.unpack_from("<i", a["result"], 0)[0]

    def radio_deaf(self, ms):
        return self.test(C.OP_RADIO_DEAF, struct.pack("<H", ms))

    # ---- the dock link, through the test hooks' dock ops (proto/dock.py) ----
    def dock_frame(self, frame, length=None, start_session=False):
        """One frame to the board's dock link as if the puck had sent it: two DOCK_LOADs and a
        DOCK_RUN of `length` bytes (73 by default). Returns the run's result as a dict with its
        `status`; when the board answered, `answer` holds the answer frame as DOCK_READ shows it
        (the serials, bonds and chip ID it masks are zeros). A SET is held to the HOST_SET
        allowlist here first (S5); the firmware checks it again."""
        frame = bytes(frame)
        safety.check_dock_frame(frame)
        with self._lock:
            for args in D.load_args(frame):
                a = self.test(C.OP_DOCK_LOAD, args)
                if a["status"] != C.ST_OK:
                    raise T.ProtocolError("DOCK_LOAD status %s" % C.STATUS_NAMES.get(a["status"]))
            n = D.FRAME_LEN if length is None else length
            a = self.test(C.OP_DOCK_RUN, bytes([n, D.RUN_START_SESSION if start_session else 0]))
            if a["status"] != C.ST_OK:
                return {"status": a["status"]}
            r = D.parse_run(a["result"])
            r["status"] = C.ST_OK
            if r["outcome"] == D.RUN_ANSWERED:
                r["answer"] = self.dock_read()
            return r

    def dock_read(self):
        """The last answer frame, in two DOCK_READs (masked bytes as zeros)."""
        out = b""
        for off, n in ((0, D.READ_MAX), (D.READ_MAX, D.FRAME_LEN - D.READ_MAX)):
            a = self.test(C.OP_DOCK_READ, bytes([off, n]))
            if a["status"] != C.ST_OK or len(a["result"]) != 2 + n:
                raise T.ProtocolError("DOCK_READ status %s" % C.STATUS_NAMES.get(a["status"]))
            out += a["result"][2:]
        return out

    def dock_ctrl(self, args):
        """DOCK_CTRL: the answer dict (status, result)."""
        return self.test(C.OP_DOCK_CTRL, bytes(args))

    def dock_status(self):
        a = self.dock_ctrl([D.CTRL_STATUS])
        if a["status"] != C.ST_OK:
            raise T.ProtocolError("dock STATUS status %s" % C.STATUS_NAMES.get(a["status"]))
        return D.parse_status(a["result"])

    def dock_end(self):
        return self.dock_ctrl([D.CTRL_END])["status"]

    # ---- the vendor channel as a host uses it (W: feature report 1; R: the test link) ----
    def _feature_set(self, buf):
        self._steam_poll()
        with self._lock:
            self.stats["usb_requests"] += 1
            try:
                self._usb_set(buf)
            except Stall as e:
                self.stats["usb_stalls"] += 1
                raise Refused(-1, "set") from e

    def vendor_set(self, opcode, payload=b"", path=None):
        """A vendor SET. W: feature report 1 (Refused on a STALL); R: HOST_SET (Refused on a
        negative result)."""
        path = path or self.path
        payload = bytes(payload)
        if opcode == C.TEST_OPCODE:
            raise safety.SafetyError("0x7E goes through the test channel (Rig.test), not as a "
                                     "vendor SET")
        if path == "R":
            rc = self.host_set(opcode, payload)
            if rc != 0:
                raise Refused(rc)
            return 0
        buf = V.build_set(opcode, payload)
        # S5 pre-check; the backend's feature_set checks again and spends a flash token
        safety.check_feature_buffer(buf, "usb", self.ctx, consume=False)
        self._feature_set(buf)
        return 0

    def vendor_get(self, path=None):
        path = path or self.path
        if path == "R":
            return self.host_get()
        with self._lock:
            self.stats["usb_requests"] += 1
            try:
                ans = self._usb_get()
            except Stall as e:
                self.stats["usb_stalls"] += 1
                raise Refused(-1, "get") from e
        opcode, n, data = V.parse_get(ans)
        return Answer(opcode, n, data)

    def vendor(self, opcode, payload=b"", path=None):
        """SET then GET, as Steam does, under one lock: the Answer. Refused when either fails."""
        with self._lock:
            self.vendor_set(opcode, payload, path)
            return self.vendor_get(path)

    def refused(self, opcode, payload=b"", get=False):
        """True when the board refuses the SET (or, with `get`, the GET after it)."""
        try:
            if get:
                self.vendor(opcode, payload)
            else:
                self.vendor_set(opcode, payload)
            return False
        except Refused:
            return True

    def refused_get(self):
        """True when a GET (W) fails: a set-only command stored, or no held answer."""
        try:
            self.vendor_get()
            return False
        except Refused:
            return True

    def refused_raw(self, buf):
        """A raw 64-byte feature report 1 SET (cmd.framing): True when refused."""
        if len(buf) > 1 and buf[1] == C.TEST_OPCODE:
            raise safety.SafetyError("0x7E goes through the test channel, not as a raw SET")
        safety.check_feature_buffer(buf, "usb", self.ctx, consume=False)
        try:
            self._feature_set(bytes(buf))
            return False
        except Refused:
            return True

    def probe_test_set(self, payload):
        """W: a SET of opcode 0x7E with `payload` that must not be claimed (a wrong key or
        version, too short, or keyed while unarmed): True when it STALLs."""
        safety.check_test_probe(payload)
        try:
            self._feature_set(V.build_set(C.TEST_OPCODE, payload))
            return False
        except Refused:
            return True

    def framing_probe(self, kind):
        """cmd.framing: the report types and IDs the board must stall before it looks at the
        data (feature report 2 with `02 83 00`, an output report through the control pipe, an
        input report through the control pipe). True when refused."""
        buf = safety.check_framing_probe(kind)
        with self._lock:
            self.stats["usb_requests"] += 1
            try:
                if kind in ("set2", "get2"):
                    self._usb(self.backend.feature_raw, 2, buf if kind == "set2" else None)
                else:
                    self._usb(self.backend.output_raw, kind, buf)
                return False
            except Stall:
                self.stats["usb_stalls"] += 1
                return True

    def read_named(self, name):
        """0xED of a key name (with its NUL): the Answer. What it read is remembered for S5 (a
        rewrite of the same value, or of 24 zero bytes for an empty bond slot)."""
        a = self.vendor(C.VC_BY_NAME_READ, name)
        key = name.split(b"\0", 1)[0].decode("ascii", "replace")
        if not a.masked:
            self.ctx.read_values[key] = a.data[:a.n]
            if key in ("esb/bond", "esb/bond_2") and a.n >= 24:
                # a bond read in the clear: its two IDs, in every form a log prints them, and
                # the puck serial in it are secrets of this run from now on (masked)
                for ident in struct.unpack_from("<II", a.data, 0):
                    self.mask.add_id_secret(ident, "<id>")
                serial = a.data[8:24].split(b"\0", 1)[0].decode("ascii", "replace")
                self.mask.add_secret(serial, "<puck>")
        if a.n == 0:
            self.ctx.empty_slots.add(key)
        else:
            self.ctx.empty_slots.discard(key)
        return a

    def led_color(self):
        a = self.vendor(C.VC_LED_GET)
        return tuple(a.data[:4])

    def write_settings(self, pairs):
        """0x87 with {number, value} pairs."""
        return self.vendor_set(C.VC_SETTINGS_SET, V.settings_triplets(pairs))

    def output(self, report):
        """An output report from the host: W WriteFile on interrupt OUT, B WriteFile on the
        Bluetooth HID device (Windows writes the report's characteristic), R HOST_OUT."""
        if self.path == "R":
            return self.host_out(report)
        safety.check_output_report(report)       # again where it is written (win/hid.py)
        with self._lock:
            self.backend.write_output(bytes(report))
        return None

    # ---- lizard mode off (the third layer of S7) ----
    def _lizard_off_now(self):
        if self.own_hid:
            buf = V.build_set(C.VC_LIZARD_OFF, b"")
            safety.check_feature_buffer(buf, "usb", self.ctx, consume=False)
            self.stats["usb_requests"] += 1
            self._usb_set(buf)
        else:
            safety.check_host_set(C.VC_LIZARD_OFF, b"", self.ctx)
            a = T.parse_answer(self._channel(C.OP_HOST_SET, bytes([C.VC_LIZARD_OFF, 0]),
                                             self._next_id(), self.path))
            rc = struct.unpack_from("<i", a["result"] + bytes(4), 0)[0] \
                if a["status"] == C.ST_OK else None
            if rc is not None and rc != 0:
                # the test link stands in for the radio: -22 while the radio is not connected,
                # and lizard mode stays as it was
                raise NoLink("HOST_SET 0x81 answered %d: the radio link is not active" % rc)
        self._last_lizard_off = self.clock.monotonic()

    def lizard_off(self):
        with self._lock:
            try:
                self._lizard_off_now()
            except (Stall, T.ProtocolError):
                pass
        st = self.status()
        self.ctx.lizard_off = bool(st and not st["lizard"])
        return self.ctx.lizard_off

    # ---- the console ----
    def console_run(self, command, timeout=C.CONSOLE_CMD_TIMEOUT_S):
        safety.check_shell(command, flash_allowed=self.opt.get("flash", False))
        self._steam_poll()
        with self._lock:
            return self._console(lambda: self.console.run(command, self.t(timeout)))

    def console_run_flash(self, command, token, policy_in_use):
        """flash.policy's one exception to S6 [R22]: checked here, and the token is spent by the
        check of the line that goes out (ConsoleSession.send_line)."""
        flash = self.opt.get("flash", False)
        safety.check_shell(command, flash_allowed=flash, policy_token=token,
                           policy_in_use=policy_in_use, ctx=self.ctx, consume=False)

        def check(line):
            safety.check_shell(line, flash_allowed=flash, policy_token=token,
                               policy_in_use=policy_in_use, ctx=self.ctx)

        with self._lock:
            return self._console(lambda: self.console.run(command,
                                                          self.t(C.CONSOLE_CMD_TIMEOUT_S),
                                                          check=check))

    def console_test(self, words, timeout=C.CONSOLE_CMD_TIMEOUT_S):
        with self._lock:
            try:
                return self._console(lambda: self.console.test(words, self.t(timeout)))
            except transport.ConsoleError:
                if words.strip() not in CONSOLE_RETRY_VERBS:
                    raise
                self.console_retries += 1
                return self._console(lambda: self.console.test(words, self.t(timeout)))

    def link_status(self):
        """STATUS for the waits on the link: on B through the console, since the Bluetooth HID
        device that carries the test channel there is gone while the link is down."""
        if self.path != "B":
            return self.status()
        try:
            a = self.test(C.OP_STATUS, stall_ok=True, path="R")
        except (transport.ConsoleError, T.ProtocolError):
            return None
        if a is None or a["status"] != C.ST_OK:
            return None
        return T.decode_status(a["result"])

    def ble_link_status(self):
        """B: BLE_STATUS through the console (it works with the link down): a dict, or None."""
        try:
            a = self.test(C.OP_BLE_STATUS, path="R")
        except (transport.ConsoleError, T.ProtocolError):
            return None
        if a is None or a["status"] != C.ST_OK:
            return None
        return T.decode_ble_status(a["result"])

    def wait_link(self, name, timeout):
        """R, B: poll STATUS until the link state is `name` (off, listening, connected...)."""
        want = {"off": 0, "no-bond": 1, "pairing": 2, "listening": 3, "connected": 4}[name]
        deadline = self.clock.monotonic() + self.t(timeout)
        while True:
            st = self.link_status()
            if st and st["link_state"] == want:
                if name == "connected" and self.path == "B":
                    return self._wait_ble_device(deadline)
                return True
            if self.clock.monotonic() >= deadline:
                if name == "connected" and not self.link_stuck:
                    self._unstick_link()
                return False
            self.sleep(0.05)

    def _wait_ble_device(self, deadline):
        """B: the link is up again; Windows names the board's Bluetooth HID device again a little
        later (the handle of the last one is gone)."""
        while True:
            node = self.backend.discover().get("ble_board")
            if node is not None and \
                    safety.classify_ble(node, self.ctx.ble_expect)[0] == safety.BOARD_BLE:
                return True
            if self.clock.monotonic() >= deadline:
                return False
            self.sleep(0.2)

    def link_up(self):
        """R: the radio link connected, waited for up to the reconnect time and, failing that,
        one `opensc2 stop` and `start` (wait_link). True once it is connected; `link_unstuck`
        says that it took the stop and start."""
        if self.wait_link("connected", C.RECONNECT_TIMEOUT_S):
            return True
        st = self.status()
        if st is not None and st["link_state"] == LINK_CONNECTED:
            self.link_unstuck = True
            return True
        return False

    def _unstick_link(self):
        """A stuck link (the board stays listening): one `opensc2 stop` and `opensc2 start`
        and 5 s more. The test that waited fails either way; if the link stays down, link_stuck
        tells the runner to skip what needs it and go to the restore."""
        with self.own_link_drop():
            self.console_run("opensc2 stop")
            self.sleep(0.5)
            self.console_run("opensc2 start")
            deadline = self.clock.monotonic() + self.t(
                C.BLE_RECONNECT_TIMEOUT_S if self.path == "B" else C.RECONNECT_TIMEOUT_S)
            while self.clock.monotonic() < deadline:
                st = self.link_status()
                if st and st["link_state"] == LINK_CONNECTED:
                    if self.path != "B" or self._wait_ble_device(deadline):
                        return
                self.sleep(0.1)
        self.link_stuck = True

    # ---- the stream ----
    @staticmethod
    def ble_as_state(report):
        """B: a state report as it arrives over Bluetooth, 0x45 (its sequence byte and the first
        44 bytes of the state; the quaternion is cut), read as the 0x42 every group reads: the
        same bytes under ID 0x42, the quaternion zero (Windows pads the report with zeros)."""
        if report and report[0] == C.RPT_BT_STATE and len(report) >= 2 + C.BLE_STATE52_KEPT:
            out = bytes([C.RPT_STATE]) + bytes(report[1:2 + C.BLE_STATE52_KEPT])
            return out + bytes(max(C.STATE_REPORT_LEN, len(report)) - len(out))
        return report

    def _backend_read(self, path, raw=False, **kw):
        reps = self.backend.read_stream(path, **kw)
        if path == "B" and not raw:
            reps = [(ts, self.ble_as_state(r)) for ts, r in reps]
        return reps

    def read_stream(self, duration_s=None, count=None, path=None, raw=False):
        """Input reports from the board's Col03 (W), its Bluetooth HID device (B: 0x45 read as
        0x42, see ble_as_state; `raw` keeps them as they came) or its proven slot (R):
        [(perf_ns, report)]. In a dry run the keep-alive is driven between chunks of at most
        2 s."""
        path = path or self.path
        if not self.virtual:
            return self._backend_read(path, raw, duration_s=duration_s, count=count)
        got = []
        if duration_s is not None:
            left = duration_s
            while left > 1e-9:
                step = min(left, 2.0)
                got += self._backend_read(path, raw, duration_s=step)
                left -= step
                self._virtual_tick()
            return got
        left = count or 0
        while left > 0:
            step = min(left, 400)
            got += self._backend_read(path, raw, count=step)
            left -= step
            self._virtual_tick()
        return got

    def wait_report(self, want, timeout, path=None, raw=False):
        """Read until a report satisfies `want(report)`: (perf_ns, report, the reports read before
        it) or None."""
        deadline = self.clock.monotonic() + self.t(timeout)
        seen = []
        while True:
            chunk = self.read_stream(count=4, path=path, raw=raw) if self.virtual else \
                self._backend_read(path or self.path, raw, duration_s=0.02)
            for ts, rep in chunk:
                if rep and want(rep):
                    return ts, rep, seen
                seen.append((ts, rep))
            if self.clock.monotonic() >= deadline:
                return None

    def wait_nonce(self, nonce, timeout=None, path=None):
        """The first 0x42 carrying `nonce`: (perf_ns, report, the reports before it) or None."""
        timeout = timeout if timeout is not None else C.NONCE_DEADLINE_MS[self.path] / 1000.0
        return self.wait_report(lambda r: len(r) >= C.STATE_REPORT_LEN and r[0] == C.RPT_STATE
                                and R.nonce_of_report(r) == nonce, timeout, path)

    # ---- the puck (R, P) ----
    def prove_slot(self):
        """wireless.slot: which of the puck's slots carries the board. All four held as their only
        reader (S12), a signature injected (a fresh nonce, four distinctive stick values), the
        slots read for 2 s. Returns {slot, count}, {slot: None, count: 0, no_link: True} when the
        board's radio link did not connect (3.10: one stop and start included), or None (the puck
        went away)."""
        sticks = {"lstick_x": 0x1234, "lstick_y": -0x2345, "rstick_x": 0x3456,
                  "rstick_y": -0x4567}
        injected = {}
        unstuck = False
        if self.path == "R":
            # after a boot into wireless the link may not be up yet: wait for it before the
            # signature (an injection needs it, and its 0x81 is refused without it)
            st = self.status()
            if st is not None and st["link_state"] != LINK_CONNECTED:
                self.link_unstuck = False
                if not self.link_up():
                    return {"slot": None, "count": 0, "no_link": True}
                unstuck = self.link_unstuck

        def before():
            a, n = self.inject(R.make_state52(**sticks))
            injected["nonce"] = n if a["status"] == C.ST_OK else None

        slots = self.backend.read_slots(self.t(2.0), before)
        self.inject_off()
        if slots is None or injected.get("nonce") is None:
            return None
        n = injected["nonce"]

        def carries(reps):
            for _t, r in reps:
                if r[0] == C.RPT_STATE and R.nonce_of_report(r) == n:
                    f = R.decode_state52(R.state52_of_report(r))
                    if all(f[k] == v for k, v in sticks.items()):
                        return True
            return False

        hits = [s for s, reps in slots.items() if carries(reps)]
        if len(hits) != 1:
            return {"slot": hits[0] if hits else None, "count": len(hits)} if hits else None
        self.backend.set_board_slot(hits[0])
        self.slot_proven_at = self.clock.monotonic()
        # the kept reader still holds reports built before INJECT_OFF took effect: read up to the
        # first one without the signature's nonce, then drop what waits (the baseline that is
        # read next must not see the signature)
        self.wait_report(lambda r: len(r) >= C.STATE_REPORT_LEN and r[0] == C.RPT_STATE and
                         R.nonce_of_report(r) != n, 0.5)
        self.flush()
        return {"slot": hits[0], "count": 1, "unstuck": unstuck}

    def find_puck(self, timeout=15.0):
        """The puck re-enumerated: every slot handle is closed (the readers ended); look for
        its slots for up to `timeout` s (every 200 ms). True when they are back."""
        deadline = self.clock.monotonic() + self.t(timeout)
        while True:
            if self.backend.discover().get("puck_slots"):
                return True
            if self.clock.monotonic() >= deadline:
                return False
            self.sleep(C.POLL_INTERVAL_S)

    def prove_slot_if_stale(self):
        if self.slot_proven_at is None or self.clock.monotonic() - self.slot_proven_at > 10.0:
            return self.prove_slot()
        return {"slot": None, "count": 1}

    def _puck_writer(self):
        if self.slot_proven_at is None or self.clock.monotonic() - self.slot_proven_at > 10.0:
            found = self.prove_slot()
            if not found or found.get("count") != 1 or self.slot_proven_at is None:
                raise RigError("the board's slot on the puck is not proven%s"
                               % (" (no link)" if found and found.get("no_link") else ""))
        w = safety.puck_writer(self.backend.puck_writer,
                               through_puck=self.opt.get("through_puck", False),
                               steam_running=self.steam_running(),
                               slot_proven_s=self.clock.monotonic() - self.slot_proven_at)
        if not self.puck_writer_used:
            self.log("[through-puck] writing to the board's slot on the puck (proven %.1f s ago; "
                     "feature report 1 and output reports only)"
                     % (self.clock.monotonic() - self.slot_proven_at))
        self.puck_writer_used = True
        return w

    def puck_vendor(self, opcode, payload=b""):
        """P: SET then the puck's two-step GET (retried for 1.5 s): the Answer, or Refused."""
        buf = V.build_set(opcode, payload)
        # S5 pre-check; PuckSlotWriter.set_feature1 checks again where it writes
        safety.check_feature_buffer(buf, "puck", self.ctx, consume=False)
        with self._lock:
            w = self._puck_writer()
            self.stats["puck_writes"] += 1
            self.log("[through-puck] feature report 1: vendor command 0x%02X (%d payload "
                     "byte%s), then its GET" % (opcode, len(payload),
                                                "" if len(payload) == 1 else "s"))
            try:
                w.set_feature1(buf)
                ans = w.get_feature1_answer()
            except Stall as e:
                raise Refused(-1, "puck") from e
        op, n, data = V.parse_get(ans)
        return Answer(op, n, data)

    def puck_output(self, report):
        with self._lock:
            w = self._puck_writer()
            self.stats["puck_writes"] += 1
            self.log("[through-puck] output report 0x%02X (%d bytes)" % (report[0], len(report)))
            w.write_output(bytes(report))

    # ---- sole reader (S12) and Steam (S8) ----
    def acquire_sole_reader(self):
        if not self.backend.sole_reader_ok():
            raise RigError("another program is reading the board's collection: close it (S12)")
        if not self.backend.sole_reader_selfcheck_ok():
            raise RigError("Windows does not enforce the sharing of the board's collection (S12)")
        self.ctx.sole_reader = True

    def steam_running(self):
        running = self.backend.steam_running()
        self.ctx.steam_running = running
        return running

    # ---- mode switches ----
    def _after_boot(self):
        self._stop_keepalive()
        self.ctx.armed = self.ctx.guard = self.ctx.canary_passed = False
        self.ctx.lizard_off = False
        self.ctx.sole_reader = False
        self.inject_active = False
        self.caps = None
        if self.console is not None:
            self.console_input_lost += self.console.input_lost
        self.console = None
        self.slot_proven_at = None
        self.puck_writer_used = False     # a boot leaves every link's stored request at 0x83
        self._last_uptime = None
        self.link_stuck = False
        self._reset_stats()
        self.discover()

    def recover(self, timeout=C.RESET_LOOK_S):
        """An unplanned reset: look for the board on both paths (nothing is written until
        it is identified by its unit serial), then read why it reset. Returns that note."""
        self._stop_keepalive()
        try:
            self.backend.find_board(self.t(timeout))
        except safety.SafetyError as e:
            raise DeviceError(str(e)) from e
        self._after_boot()
        try:
            if self.own_hid:
                info = V.decode_reset_info(self.vendor(C.VC_RESET_INFO, b"\x01").data +
                                           bytes(V.RESET_INFO_LEN))
                return "reset flags (they add up until a power-on): %s" % \
                    V.reset_cause_text(info["reset_cause"])
            st = cproto.parse_status(self.console_run("opensc2 usb log"))
            if "last_wired_end" not in st or st["last_wired_end"] is None:
                return "the session log holds no wired session"
            return "the last wired session ended: %s" % st["last_wired_end"]
        except Exception as e:
            return "the reset cause could not be read (%s)" % e

    def plain_reboot(self):
        """0x95 00 00 00 00 (W) or `opensc2 reboot` (R): the path it came back on."""
        self._stop_keepalive()
        if self.path == "W":
            self.vendor_set(C.VC_POWER_REBOOT, bytes(4))
        else:
            self.console_run("opensc2 reboot")
        self.wait_back(self.path, None, self.t(C.REENUM_TIMEOUT_S))
        self._after_boot()
        return self.path

    def wait_back(self, from_mode, to_mode, timeout, gone_timeout=None):
        """A planned reboot: the board gone, then back (its mode). `gone_timeout`: how long it may
        take to leave, from this call (None: C.GONE_TIMEOUT_S * 2, for a reboot that is asked for
        right before; give more for one the board makes later by itself). Not back in time:
        BoardGone (the runner looks for it 30 s more, then stops with "unplug and replug the
        board"); the UF2 bootloader: DeviceError."""
        try:
            return self.backend.wait_reboot(from_mode, to_mode, timeout,
                                            gone_timeout=gone_timeout)
        except TimeoutError as e:
            raise BoardGone(str(e)) from e
        except safety.SafetyError as e:
            raise DeviceError(str(e)) from e

    def to_wireless(self):
        """0x95 with the reboot-into-wireless key (W): the path it came back on."""
        self._stop_keepalive()
        self.vendor_set(C.VC_POWER_REBOOT, C.KEY_REBOOT_WIRELESS.to_bytes(4, "little"))
        self.stats_before_switch = dict(self.stats)
        self.wait_back("W", "R", self.t(C.REENUM_FIRST_INSTALL_S))
        self._after_boot()
        return self.path

    def warm_wireless(self):
        """`opensc2 reboot wireless` (R, B): back on the stored transport (Bluetooth comes back
        as B once Windows has connected it again)."""
        self._stop_keepalive()
        was = self.path
        self.console_run("opensc2 reboot wireless")
        self.wait_back(was, was, self.t(C.REENUM_TIMEOUT_S))
        self._after_boot()
        return self.path

    def to_wired(self):
        self._stop_keepalive()
        was = self.path
        self.console_run("opensc2 reboot wired")
        self.wait_back(was, "W", self.t(C.REENUM_TIMEOUT_S))
        self._after_boot()
        return self.path

    # ---- leaving ----
    def restore(self):
        """Leave the board unarmed (the firmware writes its own ARM-time snapshot back) with no
        injection, and the console as it was."""
        try:
            if self.ctx.armed:
                for fn in (self.seq_stop, self.inject_off, self.battery_off):
                    try:
                        fn()
                    except Exception:
                        pass
                self.disarm()
        except Exception:
            pass
        self._stop_keepalive()
        if self.console is not None:
            try:
                self.console.restore_shell()
            except Exception:
                pass
