#!/usr/bin/env python3
# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# fake_puck.py: Valve's puck's end of the dock link, played by the PC, for work on OpenSC2's dock
# without the puck. It sends what the puck sends over its charging pads (a probe, GET input polls,
# a vendor command's SET and GET), checks every answer (its length, CRC, type and result) and times
# it against the puck's 64 ms wait. The frames are those of docs/protocol.md, "The dock".
#
#   python -I tools\fake_puck.py --fw-emu <the firmware emulator>   # offline: the library's own dock link
#   python -I tools\fake_puck.py --test-op                          # a board on this PC, through its test hooks
#   python -I tools\fake_puck.py --port COM9 [--pilot dtr]          # a USB-UART adapter (UNTESTED)
#   python -I tools\fake_puck.py --selftest                         # its own tests: no device
#
# Options: --polls N GET inputs, 64 ms apart (default 10); --command OP[:HEX], repeatable, a vendor
# command's SET and then its GET (only commands that tools/hwtest may send on its test link: no
# flash write, no reset, no bootloader); --errors adds a short frame and one with a bad CRC, each of
# which ends the controller's session.
#
# --test-op uses tools/hwtest's rules and backends: the board found by its OSC serial (wired) or
# its console (wireless), its test hooks armed and disarmed again; the answers come back with the
# serials masked by the board. The times are the board's own handling times.
#
# --port talks to the dock UART of a board built with app/dock.conf through a 3.3 V USB-UART
# adapter, 115200 baud 8N1: the adapter's TX and RX joined through resistors into one line, as the
# puck's P pad (inferred), or on two wires with --separate-wires (no echo then). With --pilot dtr or
# rts that line, through a resistor divider, presents the pilot while the tool runs. It refuses a
# port whose USB ID is Valve's (28DE), the board's console (2FE3:0004) or a UF2 bootloader. The
# times include the adapter's latency (set its latency timer to 1 ms).
#
# NEVER CONNECT THE ADAPTER, OR ANYTHING ELSE, TO VALVE'S PUCK'S PADS. The pads' voltages and loads
# have not been measured, and the + pad carries about 5 V once the puck switches it on: any contact
# with the puck needs the owner's step-by-step go-ahead (docs/testing.md, The dock).
#
# Standard library only (ctypes on Windows, termios elsewhere). Nothing identifying is printed:
# serials appear as their length only.
import argparse
import os
import struct
import sys
import time

_TOOLS = os.path.dirname(os.path.abspath(__file__))
if _TOOLS not in sys.path:
    sys.path.insert(0, _TOOLS)

from hwtest import config as C          # noqa: E402
from hwtest import safety                # noqa: E402
from hwtest.proto import dock as D       # noqa: E402

ECHO_WAIT_MS = 100        # the puck throws away the first frame within 100 ms: its own echo
ANSWER_WAIT_MS = 64       # then waits up to 64 ms for the answer
AFTER_MS = 10             # and 10 ms more before it stops listening
POLL_GAP_MS = 64          # an idle poll after 64 ms without a request from the PC


class LinkError(Exception):
    pass


# ---- the links -------------------------------------------------------------------------------

class TestOpLink:
    """Through a board's test hooks (or the firmware emulator): DOCK_LOAD, DOCK_RUN, DOCK_READ."""

    masked = True
    timing = "the board's handling time"

    def __init__(self, rig):
        self.rig = rig
        self.last_crc_ok = None   # the board's verdict on its own answer (the read-back is masked)

    def exchange(self, frame, length=None):
        r = self.rig.dock_frame(frame, length=length, start_session=True)
        if r["status"] != C.ST_OK:
            raise LinkError("DOCK_RUN status %s" % C.STATUS_NAMES.get(r["status"], r["status"]))
        if r["outcome"] != D.RUN_ANSWERED:
            return None, r.get("us", 0)
        self.last_crc_ok = r["crc_ok"]
        return r["answer"], r["us"]

    def close(self):
        try:
            self.rig.dock_end()
        finally:
            self.rig.disarm()


class SerialLink:
    """A USB-UART adapter. `port` has write(bytes), read(n, first_ms, gap_ms) -> bytes,
    flush_input(), pilot(on) and close()."""

    masked = False
    timing = "the time on the PC from the request's end to the answer's (adapter latency included)"

    def __init__(self, port, echo=True):
        self.port = port
        self.echo = echo

    def exchange(self, frame, length=None):
        data = bytes(frame[:length] if length is not None else frame)
        self.port.flush_input()
        self.port.write(data)
        t0 = time.perf_counter()
        if self.echo:
            # one shared line hears its own request first: the puck drops the first frame
            self.port.read(D.FRAME_LEN, ECHO_WAIT_MS, D.GAP_TIMEOUT_MS)
        ans = self.port.read(D.FRAME_LEN, ANSWER_WAIT_MS, D.GAP_TIMEOUT_MS)
        us = int((time.perf_counter() - t0) * 1e6)
        time.sleep(AFTER_MS / 1000.0)
        return (ans if ans else None), us

    def close(self):
        self.port.close()


def open_serial(name):
    """The adapter's port, refused when it is a device the tool must never write to."""
    if sys.platform == "win32":
        _refuse_windows_port(name)
        return _WinPort(name)
    return _PosixPort(name)


def _refuse_windows_port(name):
    from hwtest.win import devices
    for node in devices.enumerate_nodes():
        if node.get("kind") != "port" or (node.get("com") or "").upper() != name.upper():
            continue
        iid = (node.get("instance_id") or "").upper()
        if "VID_28DE" in iid:
            raise LinkError("%s is a Valve device (the puck or a controller): refused" % name)
        if "VID_2FE3&PID_0004" in iid:
            raise LinkError("%s is the board's own console, not an adapter: refused" % name)
        if "VID_239A" in iid:
            raise LinkError("%s is a UF2 bootloader: refused" % name)
        return
    raise LinkError("no serial port %s" % name)


class _WinPort:
    def __init__(self, name):
        import ctypes
        from ctypes import wintypes
        self._ct, self._wt = ctypes, wintypes
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.CreateFileW.restype = wintypes.HANDLE
        k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.HANDLE]
        for fn in ("ReadFile", "WriteFile"):
            getattr(k, fn).argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                       ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        self.k = k
        self.h = k.CreateFileW("\\\\.\\" + name, 0xC0000000, 0, None, 3, 0, None)
        if self.h in (None, ctypes.c_void_p(-1).value):
            raise LinkError("cannot open %s (error %d)" % (name, ctypes.get_last_error()))

        class DCB(ctypes.Structure):
            _fields_ = [("DCBlength", wintypes.DWORD), ("BaudRate", wintypes.DWORD),
                        ("fFlags", wintypes.DWORD), ("wReserved", wintypes.WORD),
                        ("XonLim", wintypes.WORD), ("XoffLim", wintypes.WORD),
                        ("ByteSize", ctypes.c_ubyte), ("Parity", ctypes.c_ubyte),
                        ("StopBits", ctypes.c_ubyte), ("XonChar", ctypes.c_char),
                        ("XoffChar", ctypes.c_char), ("ErrorChar", ctypes.c_char),
                        ("EofChar", ctypes.c_char), ("EvtChar", ctypes.c_char),
                        ("wReserved1", wintypes.WORD)]
        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        k.GetCommState(self.h, ctypes.byref(dcb))
        dcb.BaudRate = D.BAUD
        dcb.fFlags = 0x0001        # binary; no flow control, DTR and RTS off until pilot()
        dcb.ByteSize, dcb.Parity, dcb.StopBits = 8, 0, 0
        if not k.SetCommState(self.h, ctypes.byref(dcb)):
            raise LinkError("cannot set %s to 115200 8N1" % name)

    def _timeouts(self, first_ms, gap_ms):
        ct, wt = self._ct, self._wt

        class TO(ct.Structure):
            _fields_ = [(n, wt.DWORD) for n in ("ReadIntervalTimeout",
                                                "ReadTotalTimeoutMultiplier",
                                                "ReadTotalTimeoutConstant",
                                                "WriteTotalTimeoutMultiplier",
                                                "WriteTotalTimeoutConstant")]
        # the gap counts once bytes flow; the total bounds the wait for the whole frame
        to = TO(gap_ms, 0, first_ms + 7 + gap_ms, 0, 100)
        self.k.SetCommTimeouts(self.h, ct.byref(to))

    def write(self, data):
        n = self._wt.DWORD(0)
        buf = self._ct.create_string_buffer(bytes(data), len(data))
        if not self.k.WriteFile(self.h, buf, len(data), self._ct.byref(n), None) or \
                n.value != len(data):
            raise LinkError("write failed")

    def read(self, count, first_ms, gap_ms):
        self._timeouts(first_ms, gap_ms)
        buf = self._ct.create_string_buffer(count)
        n = self._wt.DWORD(0)
        self.k.ReadFile(self.h, buf, count, self._ct.byref(n), None)
        return buf.raw[:n.value]

    def flush_input(self):
        self.k.PurgeComm(self.h, 0x0008)    # PURGE_RXCLEAR

    def pilot(self, line, on):
        # EscapeCommFunction: SETRTS 3, CLRRTS 4, SETDTR 5, CLRDTR 6
        code = {("rts", True): 3, ("rts", False): 4, ("dtr", True): 5, ("dtr", False): 6}
        self.k.EscapeCommFunction(self.h, code[(line, on)])

    def close(self):
        self.k.CloseHandle(self.h)


class _PosixPort:
    def __init__(self, name):
        import fcntl
        import termios
        self._termios, self._fcntl = termios, fcntl
        self.fd = os.open(name, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0                                        # iflag: raw
        attrs[1] = 0                                        # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0                                        # lflag
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def write(self, data):
        os.write(self.fd, bytes(data))
        self._termios.tcdrain(self.fd)

    def read(self, count, first_ms, gap_ms):
        import select
        out = b""
        wait = first_ms
        while len(out) < count:
            r, _w, _x = select.select([self.fd], [], [], wait / 1000.0)
            if not r:
                break
            out += os.read(self.fd, count - len(out))
            wait = gap_ms
        return out

    def flush_input(self):
        self._termios.tcflush(self.fd, self._termios.TCIFLUSH)

    def pilot(self, line, on):
        bit = {"dtr": self._termios.TIOCM_DTR, "rts": self._termios.TIOCM_RTS}[line]
        req = self._termios.TIOCMBIS if on else self._termios.TIOCMBIC
        self._fcntl.ioctl(self.fd, req, struct.pack("I", bit))

    def close(self):
        os.close(self.fd)


# ---- the puck's end -------------------------------------------------------------------------

class FakePuck:
    def __init__(self, link, out=print):
        self.link = link
        self.out = out
        self.answers = 0
        self.failures = 0
        self.slowest_us = 0

    def _check(self, frame, ans, us):
        """Problems with one answer (an empty list when it is good)."""
        problems = []
        if ans is None:
            return ["no answer"]
        if len(ans) != D.FRAME_LEN:
            return ["%d bytes, not 73" % len(ans)]
        a = D.parse_answer(ans)
        crc_ok = self.link.last_crc_ok if self.link.masked else a["crc_ok"]
        if not crc_ok:
            problems.append("a bad CRC")
        if a["type"] != frame[D.TYPE_OFF]:
            problems.append("type %d, not %d" % (a["type"], frame[D.TYPE_OFF]))
        if us > ANSWER_WAIT_MS * 1000 and not self.link.masked:
            problems.append("%d us, longer than the puck waits" % us)
        return problems

    def request(self, frame, what):
        """One exchange as the puck makes it: a failed one is tried once more."""
        safety.check_dock_frame(frame)
        for attempt in (1, 2):
            ans, us = self.link.exchange(frame)
            self.slowest_us = max(self.slowest_us, us)
            problems = self._check(frame, ans, us)
            if not problems:
                self.answers += 1
                return D.parse_answer(ans), us
            self.out("%s: %s%s" % (what, ", ".join(problems),
                                   " (trying once more)" if attempt == 1 else ""))
        self.failures += 1
        return None, 0

    def attach(self):
        a, us = self.request(D.build_request(D.PROBE), "probe")
        if a is None:
            return False
        p = a["payload"]
        pid, build, blbuild, rev = struct.unpack_from("<IIII", p, 0)
        git = p[44:57].split(b"\0", 1)[0].decode("ascii", "replace")
        unit = p[16:30].split(b"\0", 1)[0]
        self.out("attached: product 0x%04x, build 0x%08x, bootloader 0x%08x, revision 0x%x, "
                 "git %s, unit serial %s (%d us)" % (
                     pid, build, blbuild, rev, git,
                     "masked by the board" if not any(unit) else
                     "%d characters, not shown" % len(unit), us))
        return True

    def poll(self, count):
        seqs = []
        for _ in range(count):
            a, us = self.request(D.build_request(D.INPUT), "GET input")
            if a is not None:
                seqs.append(a["payload"][0])
                buttons = struct.unpack_from("<I", a["payload"], 1)[0]
                self.out("input: sequence %3d, buttons 0x%08x, result %d (%d us)"
                         % (a["payload"][0], buttons, a["result"], us))
            time.sleep(POLL_GAP_MS / 1000.0)
        return seqs

    def command(self, opcode, payload):
        a, _us = self.request(D.set_request(opcode, payload), "SET 0x%02x" % opcode)
        if a is None:
            return None
        self.out("SET 0x%02x: result %d" % (opcode, a["result"]))
        if a["result"] != 0:
            return a["result"]
        g, us = self.request(D.build_request(D.GET), "GET 0x%02x" % opcode)
        if g is None:
            return None
        p = g["payload"]
        n = min(p[1], 61)
        data = p[2:2 + n]
        shown = "<%d bytes, masked>" % n if self.link.masked and not any(data) and n else \
            data.hex()
        if opcode in (0xAE, 0xF2, 0xA1, 0xED) and not self.link.masked:
            shown = "<%d bytes, not shown: they may identify the board>" % n
        self.out("GET 0x%02x: result %d, opcode 0x%02x, %d bytes %s (%d us)"
                 % (opcode, g["result"], p[0], n, shown, us))
        return g["result"]

    def errors(self):
        """A short frame and a bad CRC: the controller answers neither and ends its session."""
        for what, frame, length in (("40 bytes", D.build_request(D.INPUT), 40),
                                    ("a bad CRC", _flip(D.build_request(D.INPUT)), None)):
            ans, _us = self.link.exchange(frame, length)
            self.out("%s: %s" % (what, "no answer, as expected" if ans is None else
                                 "an answer (unexpected)"))
            if ans is not None:
                self.failures += 1


def _flip(frame):
    f = bytearray(frame)
    f[30] ^= 0x01
    return bytes(f)


def parse_command(text):
    op, _, hexdata = text.partition(":")
    opcode = int(op, 16)
    payload = bytes.fromhex(hexdata) if hexdata else b""
    safety.check_host_set(opcode, payload)   # what the hwtest test link may send, no more
    return opcode, payload


# ---- the command line ------------------------------------------------------------------------

def _rig_for(backend, opt):
    from hwtest import mask
    from hwtest.board import Rig
    rig = Rig(backend, mask.Masker(), opt)
    rig.discover()
    rig.arm()
    caps = rig.caps or {}
    if not caps.get("build_flags", 0) & (1 << C.CAP_DOCK):
        rig.disarm()
        raise LinkError("this firmware has no dock link (caps bit %d)" % C.CAP_DOCK)
    return rig


def run(args, out=print):
    opt = {"dry_run": bool(args.fw_emu), "yes": True, "quiet": True, "timeout_scale": 1.0}
    backend = None
    if args.fw_emu:
        from hwtest import fwemu
        backend = fwemu.FwEmuBackend(args.fw_emu, start_mode="W")
        link = TestOpLink(_rig_for(backend, opt))
    elif args.test_op:
        if sys.platform != "win32":
            raise LinkError("--test-op needs Windows (the board through tools/hwtest's backend)")
        from hwtest.board import RealClock
        from hwtest import mask
        from hwtest.win import backend as win_backend
        backend = win_backend.WinBackend(RealClock(), opt, mask.Masker())
        link = TestOpLink(_rig_for(backend, opt))
    else:
        port = open_serial(args.port)
        if args.pilot:
            port.pilot(args.pilot, True)
            time.sleep(0.3)     # the controller checks the pilot every 19 ms or so
        link = SerialLink(port, echo=not args.separate_wires)
    out("the dock link through %s; times: %s" % (
        "the firmware emulator" if args.fw_emu else "the board's test hooks" if args.test_op
        else args.port, link.timing))
    puck = FakePuck(link, out)
    try:
        if puck.attach():
            puck.poll(args.polls)
            for opcode, payload in args.command:
                puck.command(opcode, payload)
            if args.errors:
                puck.errors()
    finally:
        if args.port and args.pilot:
            link.port.pilot(args.pilot, False)     # lifted off
        link.close()
        if backend is not None and hasattr(backend, "close"):
            backend.close()
    out("%d answers, %d failed exchanges, slowest %d us" % (puck.answers, puck.failures,
                                                           puck.slowest_us))
    return 0 if puck.failures == 0 else 1


def main(argv=None):
    p = argparse.ArgumentParser(prog="fake_puck.py", description="Valve's puck's end of the dock "
                                "link, played by the PC (never connect anything to the real "
                                "puck's pads)")
    way = p.add_mutually_exclusive_group()
    way.add_argument("--fw-emu", metavar="EXE", help="the firmware emulator (offline)")
    way.add_argument("--test-op", action="store_true", help="a board through its test hooks")
    way.add_argument("--port", help="a USB-UART adapter's serial port (untested)")
    way.add_argument("--selftest", action="store_true", help="the tool's own tests")
    p.add_argument("--polls", type=int, default=10, help="GET inputs, 64 ms apart")
    p.add_argument("--command", action="append", default=[], type=parse_command,
                   metavar="OP[:HEX]", help="a vendor command's SET and GET (repeatable)")
    p.add_argument("--errors", action="store_true", help="a short frame and a bad CRC too")
    p.add_argument("--separate-wires", action="store_true",
                   help="--port: TX and RX on two wires, so no echo")
    p.add_argument("--pilot", choices=("dtr", "rts"),
                   help="--port: present the pilot with this line while the tool runs")
    args = p.parse_args(argv)
    if args.selftest:
        return _selftest()
    if not (args.fw_emu or args.test_op or args.port):
        p.error("name a way to the board: --fw-emu, --test-op or --port")
    try:
        return run(args)
    except (LinkError, safety.SafetyError) as e:
        print("fake_puck: %s" % e)
        return 2


# ---- self-tests -----------------------------------------------------------------------------

class _LoopPort:
    """A board on a wire, in memory: answers each request as OpenSC2 does, after the echo of the
    request when the wire is shared."""

    def __init__(self, shared=True, slow_ms=0):
        self.shared = shared
        self.slow_ms = slow_ms
        self.rx = []
        self.lines = []

    def write(self, data):
        data = bytes(data)
        if self.shared:
            self.rx.append(data)
        if D.check(data) != "OK":
            return
        t = data[D.TYPE_OFF]
        if t == D.PROBE:
            ident = {"pid": 0x1302, "build": 0x6AD6E287, "blbuild": 0, "rev": 0x48,
                     "unit": "OSC0123456789", "board": "OSB0123456789", "git": "osc2-0.2.0"}
            self.rx.append(D.answer(D.PROBE, 0, D.identity_record(ident)))
        elif t == D.INPUT:
            self.rx.append(D.answer(D.INPUT, 0, bytes([len(self.lines) & 0xFF]) + bytes(52)))
            self.lines.append("input")
        elif t == D.SET:
            self.rx.append(D.answer(D.SET, 0))
        elif t == D.GET:
            self.rx.append(D.answer(D.GET, 63, bytes([0x89, 3, 45, 50, 0])))

    def read(self, count, first_ms, gap_ms):
        if self.slow_ms > first_ms:
            return b""
        return self.rx.pop(0) if self.rx else b""

    def flush_input(self):
        pass

    def pilot(self, line, on):
        self.lines.append("pilot %s %s" % (line, on))

    def close(self):
        pass


def _selftest():
    import unittest

    global POLL_GAP_MS
    POLL_GAP_MS = 0

    class Tests(unittest.TestCase):
        def _puck(self, port, echo=True):
            lines = []
            puck = FakePuck(SerialLink(port, echo=echo), lines.append)
            return puck, lines

        def test_shared_wire(self):
            puck, lines = self._puck(_LoopPort(shared=True))
            self.assertTrue(puck.attach())
            self.assertIn("unit serial 13 characters, not shown", lines[0])
            self.assertNotIn("OSC0123456789", " ".join(lines))
            self.assertEqual(puck.poll(3), [0, 1, 2])
            self.assertEqual(puck.command(0x89, b"\x2d"), 63)
            self.assertEqual((puck.answers, puck.failures), (6, 0))

        def test_separate_wires(self):
            puck, _lines = self._puck(_LoopPort(shared=False), echo=False)
            self.assertTrue(puck.attach())

        def test_echo_expected_but_missing(self):
            # two wires taken for one: the answer is taken for the echo, then nothing comes
            puck, lines = self._puck(_LoopPort(shared=False), echo=True)
            self.assertFalse(puck.attach())
            self.assertEqual(puck.failures, 1)
            self.assertIn("(trying once more)", lines[0])

        def test_too_slow(self):
            puck, _lines = self._puck(_LoopPort(slow_ms=500))
            self.assertFalse(puck.attach())

        def test_commands_held_to_the_allowlist(self):
            self.assertEqual(parse_command("89:2d"), (0x89, b"\x2d"))
            for bad in ("86:17a9c1ef", "90", "ee:657362", "95:c0baaaec"):
                with self.assertRaises(safety.SafetyError):
                    parse_command(bad)
            puck, _lines = self._puck(_LoopPort())
            with self.assertRaises(safety.SafetyError):
                puck.request(D.set_request(0x90), "a bootloader request")

        def test_emulator(self):
            exe = os.environ.get("OPENSC2_FWEMU")
            if not exe or not os.path.isfile(exe):
                self.skipTest("OPENSC2_FWEMU is not set (tests/host/run.ps1 sets it)")
            lines = []
            ns = argparse.Namespace(fw_emu=exe, test_op=False, port=None, polls=3,
                                    command=[(0x89, b"\x2d"), (0x83, b"")], errors=True,
                                    separate_wires=False, pilot=None)
            self.assertEqual(run(ns, lines.append), 0, lines)
            self.assertTrue(any(ln.startswith("attached: product 0x1302") for ln in lines))
            self.assertTrue(any("masked by the board" in ln for ln in lines))
            self.assertTrue(any(ln.startswith("GET 0x89: result 63") for ln in lines))
            self.assertEqual(sum("no answer, as expected" in ln for ln in lines), 2)

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Tests)
    return 0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
