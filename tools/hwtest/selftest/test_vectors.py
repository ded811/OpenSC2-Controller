# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_vectors.py: every line of the shared vector files (tests/vectors/hwtest/) through proto/ and
# the models. The same files run through the C library in tests/host, so the runner's model and the
# firmware's own code are held to the same records. Grammar: one record per line, '#' starts a
# comment, hex in either case, and a hex value may be split into several tokens (joined up to the
# next word that is not hex).
import os
import re
import unittest

from hwtest import config as C
from hwtest.proto import model as M
from hwtest.proto import reports as R
from hwtest.proto import testproto as T

VDIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "tests",
                                     "vectors", "hwtest"))
_HEX = re.compile(r"^[0-9A-Fa-f]+$")


def records(name):
    """The token lists of the non-empty, non-comment lines of one vector file."""
    with open(os.path.join(VDIR, name), encoding="utf-8") as f:
        for line in f:
            tok = line.split("#", 1)[0].split()
            if tok:
                yield tok, line.rstrip("\n")


def hex_after(tok, i):
    """The hex value that starts at tok[i] (several tokens joined), and the index after it."""
    parts = []
    while i < len(tok) and _HEX.match(tok[i]):
        parts.append(tok[i])
        i += 1
    return bytes.fromhex("".join(parts)), i


def hex_field(tok, word):
    return hex_after(tok, tok.index(word) + 1)[0]


class Files(unittest.TestCase):
    def test_every_file_is_there(self):
        for name in ("state.txt", "lizard.txt", "ring.txt", "testproto.txt", "settings.txt"):
            self.assertTrue(os.path.isfile(os.path.join(VDIR, name)), name)


class StateVectors(unittest.TestCase):
    def test_state_txt(self):
        count = 0
        for tok, line in records("state.txt"):
            t68 = int(tok[tok.index("T68") + 1]) if "T68" in tok else 90
            prev = tok[tok.index("PREV") + 1] if "PREV" in tok else "00"
            state_in = hex_field(tok, "IN")
            want = hex_field(tok, "OUT")
            self.assertEqual(len(state_in), C.STATE52_LEN, line)
            self.assertEqual(len(want), C.STATE52_LEN, line)
            got, _l, _r = M.expected_report(state_in, prev[0] == "1", prev[1] == "1",
                                            setting68=t68)
            self.assertEqual(got.hex(), want.hex(), line)
            count += 1
        self.assertGreaterEqual(count, 28)


class LizardVectors(unittest.TestCase):
    def test_lizard_txt(self):
        lz = M.LizardModel()
        count = 0
        for tok, line in records("lizard.txt"):
            if tok[0] == "START":
                lz.reset()
                continue
            arrow = tok.index("->")
            if tok[arrow + 1] == "-":
                want = b""
            else:
                want, _ = hex_after(tok, arrow + 1)
            made = self._run(lz, tok[:arrow])
            self.assertEqual(b"".join(made).hex(), want.hex(), line)
            count += 1
        self.assertGreater(count, 60)

    @staticmethod
    def _run(lz, action):
        verb = action[0]
        if verb == "ENTER":
            return [r for _k, r in lz.enter()]
        if verb == "LEAVE":
            return [r for _k, r in lz.leave()]
        if verb == "SWITCHES":
            return [r for _k, r in lz.switches(action[1] == "up")]
        if verb == "RESYNC":
            return [r for _k, r in lz.resync()]
        if verb != "FRAME":
            raise AssertionError("unknown lizard action %r" % verb)
        kw = {"buttons": 0, "lpad": (0, 0), "rpad": (0, 0), "s82": 3}
        for part in action[1:]:
            k, _, v = part.partition("=")
            if k == "buttons":
                kw["buttons"] = int(v, 0)
            elif k in ("lpad", "rpad"):
                x, y = v.split(",")
                kw[k] = (int(x), int(y))
            elif k == "s82":
                kw["s82"] = int(v, 0)
            else:
                raise AssertionError("unknown FRAME field %r" % k)
        return [r for _k, r in lz.step(**kw)]


class RingVectors(unittest.TestCase):
    def test_ring_round_trip(self):
        kinds = set()
        for tok, line in records("ring.txt"):
            self.assertEqual(tok[0], "ENTRY", line)
            entry, _ = hex_after(tok, 1)
            self.assertEqual(len(entry), 7 + entry[6], line)
            decoded = T.decode_entry(entry)
            self.assertEqual(T.encode_entry(decoded).hex(), entry.hex(), line)
            kinds.add(decoded["kind"])
        # every ring entry kind has a line
        for kind in (C.RING_ARMED, C.RING_MARK, C.RING_SEQ_STEP, C.RING_SEQ_END,
                     C.RING_INJECT_STOP, C.RING_OUTPUT, C.RING_RUMBLE, C.RING_HAPTIC, C.RING_LED,
                     C.RING_SETTING, C.RING_LIZARD, C.RING_LIZARD_RPT, C.RING_POWER,
                     C.RING_VENDOR, C.RING_HOST_ASLEEP, C.RING_LINK, C.RING_USB, C.RING_VERSION,
                     C.RING_IDENTITY, C.RING_CHANNELS, C.RING_BLE):
            self.assertIn(kind, kinds, "ring.txt has no entry of kind 0x%02x" % kind)


class TestProtoVectors(unittest.TestCase):
    def test_testproto_txt(self):
        count = 0
        for tok, line in records("testproto.txt"):
            if tok[0] == "REQ":
                self._req(tok, line)
            elif tok[0] == "BATTERY":
                self._battery(tok, line)
            else:
                raise AssertionError("unknown record %r" % tok[0])
            count += 1
        self.assertGreater(count, 8)

    def _req(self, tok, line):
        req, _ = hex_after(tok, 1)
        armed = tok[tok.index("ARMED") + 1] == "1"
        claim = tok[tok.index("CLAIM") + 1] == "1"
        self.assertEqual(T.claims(req, armed), claim, line)
        if claim:
            self.assertEqual(req[6], int(tok[tok.index("OP") + 1], 16), line)
            self.assertEqual(len(req) - 7, int(tok[tok.index("ARGS") + 1]), line)
            # the runner builds the same bytes from the op and its arguments
            self.assertEqual(T.build_request(req[6], req[7:], req[5]), req, line)

    def _battery(self, tok, line):
        req, _ = hex_after(tok, 1)
        self.assertEqual(req[:4], C.TEST_KEY_BYTES, line)
        self.assertEqual(req[6], C.OP_BATTERY, line)
        args = req[7:]
        self.assertEqual(len(args), 16, line)
        b = R.decode_battery_args(args)
        self.assertEqual(R.encode_battery_args(send_now=bool(args[15] & 1), **b), args, line)
        self.assertEqual(R.encode_battery(b).hex(), hex_field(tok, "R43").hex(), line)
        self.assertEqual(R.encode_be(b).hex(), hex_field(tok, "RBE").hex(), line)


class SettingsVectors(unittest.TestCase):
    def test_defaults_and_ranges(self):
        listed = set()
        for tok, line in records("settings.txt"):
            if tok[0] == "DEFAULT":
                num, value = int(tok[1]), int(tok[2])
                listed.add(num)
                self.assertEqual(M.setting_default(num), value, line)
            elif tok[0] == "RANGE":
                num, lo, hi = int(tok[1]), int(tok[2]), int(tok[3])
                self.assertEqual(M.setting_range(num), (lo, hi), line)
                for v, want in ((lo, lo), (hi, hi), (-32768, lo), (32767, hi)):
                    self.assertEqual(M.setting_clamp(num, v), want, line)
                if lo > -32768:
                    self.assertEqual(M.setting_clamp(num, lo - 1), lo, line)
                if hi < 32767:
                    self.assertEqual(M.setting_clamp(num, hi + 1), hi, line)
            else:
                raise AssertionError("unknown record %r" % tok[0])
        # every number not listed defaults to 0 in the model too
        for num in range(M.SETTINGS_COUNT):
            if num not in listed:
                self.assertEqual(M.setting_default(num), 0, num)


if __name__ == "__main__":
    unittest.main()
