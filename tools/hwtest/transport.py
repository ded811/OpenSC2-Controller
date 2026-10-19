# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# transport.py: how the runner talks to the firmware's test channel and console, the same code for
# every backend (the hardware, the simulator, the firmware emulator), so what the self-tests and
# the dry runs check is what a hardware run does:
#   - Stall: a feature request the device refused (a STALL on USB);
#   - UsbTestChannel: vendor command 0x7E on feature report 1 (a SET, then its GET), the answer
#     checked against the request;
#   - ConsoleSession: the board's Zephyr console: the handshake, the HWT line grammar, the sync
#     barrier after commands that print no end marker, lines written in short chunks;
#   - LinkLock and KeepAlive: one lock per link, so a SET and its GET (or a console command and its
#     END) are never split by the lease keep-alive.
import threading

from . import config as C
from . import safety
from .proto import console as cproto
from .proto import testproto as T
from .proto import vendor as V


class Stall(Exception):
    """A feature request that stalled (a Win32 error on the wire, or the simulated equivalent)."""

    def __init__(self, win_error=0, detail=""):
        super().__init__(detail or ("stall %d" % win_error))
        self.win_error = win_error


class ConsoleError(Exception):
    """The console did not answer as OpenSC2's does (no END in time, a reply of another id)."""


class ConsoleRefused(ConsoleError):
    """The port does not answer the OpenSC2 handshake: it is not used for the rest of the run."""


class ConsoleGone(ConsoleError):
    """The console's port went away (a reset of the board, or it was unplugged)."""


# ---- the test channel on feature report 1 -------------------------------------------------------

class UsbTestChannel:
    """0x7E on the board's feature report 1 (W) or through the puck (P): a 64-byte SET
    `01 7E <n> <request>`, then a GET whose answer `01 7E <m> <answer>` must be this request's.
    `feature_set(buffer)` raises Stall when the device refuses the SET (an unarmed board does
    for every keyed request but ARM); `feature_get()` returns the 64-byte answer or raises Stall."""

    def __init__(self, feature_set, feature_get):
        self._set = feature_set
        self._get = feature_get

    def request(self, op, args, req_id):
        req = T.build_request(op, args, req_id)
        self._set(V.build_set(C.TEST_OPCODE, req))
        opcode, _n, data = V.parse_get(self._get())
        if opcode != C.TEST_OPCODE:
            raise T.ProtocolError("the GET answered opcode 0x%02x, not the test channel" % opcode)
        T.check_answer(T.parse_answer(data), op, req_id)
        return data


# ---- the console -------------------------------------------------------------------------------

class ConsoleSession:
    """One open console port. `port` has write(text) (raw characters), drain_lines() (the lines
    received since the last call, CR and VT100 removed) and a `gone` flag. Every line goes out
    with a single CR, in chunks of CHUNK characters with a short pause between them: the shell
    copies what arrives into a small ring buffer and drops what does not fit, and a test request
    line is up to 145 characters. send_line() is the one place a line leaves the runner, and it
    checks the exact line first (S6, safety.check_shell, with the live safety Context `ctx` for
    the S7 and HOST_SET rules of a `raw` request); only the handshake's lone CR goes out
    without a line."""

    CHUNK = 32
    CHUNK_PAUSE_S = 0.002
    SYNC_ID = 1          # the handshake's id

    def __init__(self, port, clock, transcript=None, ctx=None):
        self.port = port
        self.clock = clock
        self.transcript = transcript if transcript is not None else []
        self.ctx = ctx               # the Rig's safety.Context (None: injections refused)
        self.hooks = None            # True: "opensc2 test" answers; False: passive only
        self.input_lost = 0          # the shell's "RX ring buffer full" warnings seen
        self._id = 1
        self._pending = []           # lines received but not consumed yet

    # ---- ids ----
    def next_id(self):
        self._id = self._id % 65535 + 1
        if self._id == self.SYNC_ID:
            self._id += 1
        return self._id

    # ---- raw I/O ----
    def send_line(self, text, check=None):
        """Send one line (S6 first: `check(text)`, by default safety.check_shell with this
        session's Context; the Rig passes its own check for the one --flash exception)."""
        if check is not None:
            check(text)
        else:
            safety.check_shell(text, ctx=self.ctx)
        data = text + "\r"
        try:
            for i in range(0, len(data), self.CHUNK):
                self.port.write(data[i:i + self.CHUNK])
                if i + self.CHUNK < len(data):
                    self.clock.sleep(self.CHUNK_PAUSE_S)
        except OSError as e:
            raise ConsoleGone("the console went away (%s)" % e) from e
        self.transcript.append("> " + text)

    def _lines(self):
        got = self.port.drain_lines()
        for ln in got:
            if "RX ring buffer full" in ln:
                self.input_lost += 1
            self.transcript.append("< " + ln)
        self._pending.extend(got)
        out, self._pending = self._pending, []
        return out

    def _wait_end(self, req_id, timeout):
        """Lines until `HWT <req_id> END <rc>`: (rc, [(tag, rest) of this id], [other lines])."""
        deadline = self.clock.monotonic() + timeout
        hwt, other = [], []
        while True:
            for ln in self._lines():
                found = cproto.find_hwt(ln)
                if found is None:
                    if not cproto.is_log_line(ln):
                        other.append(ln)
                    continue
                rid, tag, rest = found
                if rid != str(req_id):
                    continue           # a late line of an earlier command
                if tag == "END":
                    return cproto.parse_end(rest), hwt, other
                hwt.append((tag, rest))
            if self.port.gone:
                raise ConsoleGone("the console went away")
            if self.clock.monotonic() >= deadline:
                raise ConsoleError("no 'HWT %s END' within %.1f s" % (req_id, timeout))
            self.clock.sleep(0.01)

    # ---- commands ----
    def test(self, words, timeout=C.CONSOLE_CMD_TIMEOUT_S):
        """`opensc2 test <id> <words>`: (rc, [(tag, rest)]) of its HWT lines."""
        req_id = self.next_id()
        self.send_line("opensc2 test %d %s" % (req_id, words))
        rc, hwt, _other = self._wait_end(req_id, timeout)
        return rc, hwt

    def raw(self, op, args, timeout=C.CONSOLE_CMD_TIMEOUT_S):
        """One test-channel request as `raw`: the answer bytes, checked against the request."""
        req_id = self.next_id()
        req = T.build_request(op, args, req_id)
        self.send_line(cproto.test_raw(req.hex(), req_id))
        rc, hwt, _other = self._wait_end(req_id, timeout)
        raw = [rest for tag, rest in hwt if tag == "RAW"]
        if rc != 0 or not raw:
            errors = [rest for tag, rest in hwt if tag == "ERR"]
            raise T.ProtocolError("raw request refused by the console (END %d%s)"
                                  % (rc, ": " + errors[0] if errors else ""))
        data = bytes.fromhex(raw[0].split()[0])
        T.check_answer(T.parse_answer(data), op, req_id)
        return data

    def run(self, command, timeout=C.CONSOLE_CMD_TIMEOUT_S, check=None):
        """An existing command (they print no end marker): its output lines, up to the END of the
        `sync` sent right after it. The reboot commands get no sync (the board resets). `check`:
        the S6 check of the command line (send_line); the sync line gets the default one."""
        self.send_line(command, check=check)
        if command.strip() in cproto.REBOOT_COMMANDS:
            # Keep reading until the board has left. Closing the port at once can leave the
            # board's shell waiting for a reader in the middle of its "rebooting" message (it
            # waits for DTR); firmware that reboots from the shell thread then never reboots.
            end = self.clock.monotonic() + C.REBOOT_LEAVE_S
            while not getattr(self.port, "gone", False) and self.clock.monotonic() < end:
                self._lines()
                self.clock.sleep(0.05)
            self._lines()
            return []
        if not self.hooks:
            # without the hooks there is no barrier: wait, then take what came
            self.clock.sleep(1.0)
            return [ln for ln in self._lines() if not cproto.is_log_line(ln)]
        sync_id = self.next_id()
        self.send_line(cproto.test_sync(sync_id))
        _rc, _hwt, other = self._wait_end(sync_id, timeout)
        return other

    # ---- the session ----
    def handshake(self):
        """The handshake: a CR, then `opensc2 test 1 sync`, which must print `HWT 1 END 0` within
        2 s. Without it, `opensc2 status` must print a `mode:` line (a board without the hooks:
        passive groups only). Anything else: ConsoleRefused. Then echo, colours and the prompt
        off (the parsers expect the bare lines)."""
        self.clock.sleep(0.3)
        self._lines()                  # the banner, an old prompt
        self.port.write("\r")
        self.send_line(cproto.test_sync(self.SYNC_ID))
        try:
            rc, _hwt, _other = self._wait_end(self.SYNC_ID, 2.0)
            self.hooks = rc == 0
        except ConsoleError:
            self.hooks = False
        if not self.hooks:
            self.send_line("opensc2 status")
            self.clock.sleep(1.0)
            lines = self._lines()
            if not any(ln.strip().startswith("mode:") for ln in lines):
                raise ConsoleRefused("the port does not answer as OpenSC2's console")
            return False
        for cmd in ("shell echo off", "shell colors off", "shell prompt off"):
            self.run(cmd)
        return True

    def restore_shell(self):
        for cmd in ("shell echo on", "shell colors on", "shell prompt on"):
            try:
                self.send_line(cmd)
            except Exception:
                return


# ---- the per-link lock and the lease keep-alive -----------------------------------------------

class LinkLock:
    """One reentrant lock per link. A SET+GET pair, a console command and the keep-alive all take
    it, so none interleaves with another on the same stored request."""

    def __init__(self, name):
        self.name = name
        self._lock = threading.RLock()

    def __enter__(self):
        self._lock.acquire()
        return self

    def __exit__(self, *exc):
        self._lock.release()


class KeepAlive:
    """Calls `tick()` every `interval_s` from its own thread, under the link lock, while running:
    the lease keep-alive (a STATUS) and, while an injection runs, lizard mode kept off. The runner
    starts it after arming and stops it before a mode switch or a test that needs silence."""

    def __init__(self, lock, tick, interval_s=C.LEASE_KEEPALIVE_S):
        self._lock = lock
        self._tick = tick
        self._interval = interval_s
        self._stop = threading.Event()
        self._thread = None
        self.errors = 0

    @property
    def running(self):
        return self._thread is not None

    def start(self):
        if self._thread:
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.wait(self._interval):
            try:
                with self._lock:
                    self._tick()
            except Exception:
                self.errors += 1

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None
