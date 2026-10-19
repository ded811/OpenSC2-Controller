# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_transport.py: the transport the runner uses on every backend.
#   - the link lock keeps a keep-alive STATUS from falling between a test's SET and its GET on a
#     link that stores one request, and the keep-alive thread starts and stops;
#   - UsbTestChannel refuses an answer that is not its request's (another op, another ID);
#   - ConsoleSession against a fake port that answers as test_core.c's console does: lines go
#     out in short chunks (the shell drops what overflows its RX ring buffer), the handshake with
#     and without the hooks, `raw` answers checked, ERR lines, late lines of an earlier ID, log
#     noise and VT100 debris, the shell's "RX ring buffer full" warning counted, a missing END
#     timing out on a fake clock, a port that went away, and the sync barrier after commands that
#     print no end marker.
import threading
import time
import unittest

from hwtest import config as C
from hwtest import transport
from hwtest.proto import shellfmt as SF
from hwtest.proto import testproto as T
from hwtest.proto import vendor as V


class FakeLink:
    """A link with one stored request. set() stores; get() returns what the last set stored. A
    keep-alive STATUS set() between a test's set() and get() would make the get() wrong."""

    def __init__(self):
        self.stored = None

    def set(self, value):
        time.sleep(0.0005)   # a window for a race
        self.stored = value

    def get(self):
        return self.stored


class LinkLockTest(unittest.TestCase):
    def test_lock_prevents_race(self):
        link = FakeLink()
        lock = transport.LinkLock("test")
        bad = []

        def keepalive():
            for _ in range(200):
                with lock:
                    link.set("STATUS")

        t = threading.Thread(target=keepalive, daemon=True)
        t.start()
        for _ in range(200):
            with lock:
                link.set("TEST")
                if link.get() != "TEST":
                    bad.append(link.get())
        t.join()
        self.assertEqual(bad, [], "a keep-alive slipped between SET and GET under the lock")

    def test_keepalive_runs_and_stops(self):
        calls = []
        lock = transport.LinkLock("x")
        ka = transport.KeepAlive(lock, lambda: calls.append(1), interval_s=0.01)
        ka.start()
        self.assertTrue(ka.running)
        time.sleep(0.08)
        ka.stop()
        self.assertFalse(ka.running)
        self.assertGreaterEqual(len(calls), 1)
        n = len(calls)
        time.sleep(0.05)
        self.assertEqual(len(calls), n)   # stopped

    def test_keepalive_errors_do_not_kill_it(self):
        calls = []

        def tick():
            calls.append(1)
            raise OSError("the device went away")

        ka = transport.KeepAlive(transport.LinkLock("x"), tick, interval_s=0.01)
        ka.start()
        time.sleep(0.08)
        ka.stop()
        self.assertGreaterEqual(len(calls), 2)
        self.assertEqual(ka.errors, len(calls))


class FakeBoard:
    """Feature report 1 of a board that stores one request (W)."""

    def __init__(self, answer_id_delta=0, answer_op=None, answer_opcode=C.TEST_OPCODE):
        self.stored = None
        self.delta = answer_id_delta
        self.op = answer_op
        self.opcode = answer_opcode

    def feature_set(self, buf):
        self.stored = bytes(buf)

    def feature_get(self):
        n = self.stored[2]
        req = self.stored[3:3 + n]
        rid = (req[5] + self.delta) & 0xFF
        op = req[6] if self.op is None else self.op
        answer = bytes([C.TEST_VERSION, rid, op, C.ST_OK, 7, 8])
        return V.build_set(self.opcode, answer)


class UsbChannel(unittest.TestCase):
    def test_answer_of_this_request(self):
        b = FakeBoard()
        data = transport.UsbTestChannel(b.feature_set, b.feature_get).request(C.OP_STATUS, b"",
                                                                              9)
        a = T.parse_answer(data)
        self.assertEqual((a["request_id"], a["op"], a["status"], a["result"]),
                         (9, C.OP_STATUS, C.ST_OK, bytes([7, 8])))
        self.assertEqual(b.stored[:3], bytes([C.FEATURE_ID, C.TEST_OPCODE, 7]))
        self.assertEqual(b.stored[3:7], C.TEST_KEY_BYTES)

    def test_another_request_id(self):
        b = FakeBoard(answer_id_delta=1)
        with self.assertRaises(T.ProtocolError):
            transport.UsbTestChannel(b.feature_set, b.feature_get).request(C.OP_STATUS, b"", 9)

    def test_another_op(self):
        b = FakeBoard(answer_op=C.OP_MARK)
        with self.assertRaises(T.ProtocolError):
            transport.UsbTestChannel(b.feature_set, b.feature_get).request(C.OP_STATUS, b"", 9)

    def test_another_opcode(self):
        b = FakeBoard(answer_opcode=C.VC_ATTRIBUTES)
        with self.assertRaises(T.ProtocolError):
            transport.UsbTestChannel(b.feature_set, b.feature_get).request(C.OP_STATUS, b"", 9)

    def test_too_long(self):
        with self.assertRaises(ValueError):
            T.build_request(C.OP_INJECT, bytes(T.ARGS_MAX + 1), 1)


class FakeClock:
    virtual = True

    def __init__(self):
        self.t = 100.0

    def monotonic(self):
        return self.t

    def perf_ns(self):
        return int(self.t * 1e9)

    def sleep(self, seconds):
        self.t += seconds


class FakeShell:
    """A console port that answers as test_core.c's `opensc2 test` does (sync, raw), prints
    `opensc2 status` with shell.c's formats, and can misbehave: no hooks, no answer at all, an
    answer of another request ID, an ERR line, a late line of an earlier ID, log noise and VT100
    debris before the HWT lines."""

    def __init__(self, hooks=True, mute=False, wrong_id=False, err=None, noise=False,
                 ring_warning=False):
        self.hooks = hooks
        self.mute = mute
        self.wrong_id = wrong_id
        self.err = err
        self.noise = noise
        self.ring_warning = ring_warning
        self.gone = False
        self.writes = []
        self.commands = []
        self._buf = ""
        self._out = []

    def write(self, text):
        if self.gone:
            raise OSError(1167, "the device is not connected")
        self.writes.append(text)
        self._buf += text
        while "\r" in self._buf:
            line, self._buf = self._buf.split("\r", 1)
            if line.strip():
                self.commands.append(line.strip())
                self._handle(line.strip())

    def drain_lines(self):
        out, self._out = self._out, []
        return out

    def _handle(self, cmd):
        if self.mute:
            return
        if self.noise:
            self._out += ["\x1b[1;32mopensc2:~$ \x1b[m" + cmd,
                          "[00:00:01.234,567] <inf> opensc2: a log line in between",
                          "HWT 99 END 0"]                     # a late line of an earlier ID
        if self.ring_warning:
            self._out.append("[00:00:01.300,000] <wrn> shell_uart: RX ring buffer full.")
        words = cmd.split()
        if words[:2] == ["opensc2", "test"]:
            if not self.hooks:
                self._out.append("test: command not found")
                return
            rid, verb = words[2], words[3]
            if self.err:
                self._out += ["HWT %s ERR %s" % (rid, self.err), "HWT %s END -13" % rid]
            elif verb == "sync":
                self._out.append("HWT %s END 0" % rid)
            elif verb == "raw":
                req = bytes.fromhex(words[4])
                answer_id = (req[5] + (1 if self.wrong_id else 0)) & 0xFF
                answer = bytes([C.TEST_VERSION, answer_id, req[6], C.ST_OK, 1, 2, 3])
                self._out += ["prompt debris HWT %s RAW %s" % (rid, answer.hex()),
                              "HWT %s END 0" % rid]
        elif cmd == "opensc2 status":
            self._out.append(SF.line("status_mode", "wireless", "warm restart", "auto"))
            self._out.append(SF.line("status_lizard", "off", 0))
        elif cmd.startswith("shell "):
            pass
        else:
            self._out.append("%s: command not found" % words[0])


def session(**kw):
    port = FakeShell(**kw)
    s = transport.ConsoleSession(port, FakeClock())
    s.hooks = kw.get("hooks", True)
    return s, port


class Console(unittest.TestCase):
    def test_lines_go_out_in_chunks(self):
        s, port = session()
        before = s.clock.t
        # the longest request: 61 bytes, 122 hex digits, 141 characters in all
        line = "opensc2 test 5 raw " + T.build_request(C.OP_HOST_OUT, bytes([0x80]) + bytes(53),
                                                       5).hex()
        self.assertEqual(len(line), 141)
        s.send_line(line)
        self.assertTrue(all(len(w) <= transport.ConsoleSession.CHUNK for w in port.writes))
        self.assertEqual("".join(port.writes), line + "\r")
        self.assertGreaterEqual(len(port.writes), 5)
        self.assertAlmostEqual(s.clock.t - before,
                               (len(port.writes) - 1) * transport.ConsoleSession.CHUNK_PAUSE_S)

    def test_handshake_with_hooks(self):
        s, port = session()
        s.hooks = None
        self.assertTrue(s.handshake())
        self.assertEqual(port.commands[0], "opensc2 test 1 sync")
        for cmd in ("shell echo off", "shell colors off", "shell prompt off"):
            self.assertIn(cmd, port.commands)

    def test_handshake_without_hooks(self):
        s, port = session(hooks=False)
        s.hooks = None
        self.assertFalse(s.handshake())
        self.assertIn("opensc2 status", port.commands)

    def test_handshake_refused(self):
        s, _port = session(mute=True)
        s.hooks = None
        with self.assertRaises(transport.ConsoleRefused):
            s.handshake()

    def test_ids_skip_the_sync_id(self):
        s, _port = session()
        ids = {s.next_id() for _ in range(70000)}
        self.assertNotIn(transport.ConsoleSession.SYNC_ID, ids)

    def test_raw_answer(self):
        s, port = session(noise=True)
        data = s.raw(C.OP_STATUS, b"")
        self.assertEqual(T.parse_answer(data)["result"], bytes([1, 2, 3]))
        self.assertTrue(port.commands[-1].startswith("opensc2 test "))

    def test_raw_answer_of_another_request(self):
        s, _port = session(wrong_id=True)
        with self.assertRaises(T.ProtocolError):
            s.raw(C.OP_STATUS, b"")

    def test_raw_err_line(self):
        s, _port = session(err="not armed")
        with self.assertRaises(T.ProtocolError) as e:
            s.raw(C.OP_MARK, bytes(4))
        self.assertIn("not armed", str(e.exception))
        self.assertIn("END -13", str(e.exception))

    def test_test_verb(self):
        s, _port = session(noise=True)
        rc, hwt = s.test("sync")
        self.assertEqual((rc, hwt), (0, []))

    def test_missing_end_times_out(self):
        s, _port = session(mute=True)
        t0 = s.clock.t
        with self.assertRaises(transport.ConsoleError):
            s.test("sync", timeout=2.0)
        self.assertGreaterEqual(s.clock.t - t0, 2.0)

    def test_ring_buffer_warning_counted(self):
        s, _port = session(ring_warning=True)
        s.test("sync")
        self.assertGreaterEqual(s.input_lost, 1)

    def test_port_gone(self):
        s, port = session(mute=True)
        port.gone = True
        with self.assertRaises(transport.ConsoleGone):
            s.send_line("opensc2 test 4 sync")
        s2, port2 = session(mute=True)
        s2.send_line("opensc2 test 4 sync")
        port2.gone = True
        with self.assertRaises(transport.ConsoleGone):
            s2._wait_end(4, 1.0)

    def test_run_takes_the_lines_up_to_the_sync(self):
        s, port = session(noise=True)
        lines = s.run("opensc2 status")
        self.assertIn(SF.line("status_mode", "wireless", "warm restart", "auto"),
                      [ln.strip() for ln in lines])
        self.assertTrue(port.commands[-1].endswith(" sync"))
        self.assertFalse(any(ln.startswith("HWT") for ln in lines))

    def test_reboot_gets_no_sync(self):
        s, port = session()
        self.assertEqual(s.run("opensc2 reboot wired"), [])
        self.assertEqual(port.commands, ["opensc2 reboot wired"])

    def test_transcript(self):
        s, _port = session()
        s.test("sync")
        self.assertTrue(any(ln.startswith("> opensc2 test ") for ln in s.transcript))
        self.assertTrue(any(ln.startswith("< HWT ") for ln in s.transcript))


if __name__ == "__main__":
    unittest.main()
