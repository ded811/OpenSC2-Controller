# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# test_writers.py: the 16-hex-digit backstop in the writers, and that no serial,
# bond or chip-ID bytes reach any output file. Synthetic secrets are planted and searched for in
# plain text and in hex.
import json
import os
import tempfile
import unittest

from hwtest import mask as mask_mod
from hwtest import results as results_mod


class Writers(unittest.TestCase):
    def test_hex_backstop_and_bytes(self):
        m = mask_mod.Masker()
        obj = {"a": "00112233445566778899aabbccdd", "b": b"\x01\x02\x03", "c": {1, 2}}
        out = results_mod._hex_backstop(obj, m)
        self.assertIn("<hex:", out["a"])
        self.assertEqual(out["b"], [1, 2, 3])        # bytes -> list, never hex
        self.assertEqual(out["c"], [1, 2])           # set -> sorted list
        self.assertGreaterEqual(m.hex_dropped, 1)

    def test_no_secret_in_files(self):
        m = mask_mod.Masker()
        m.add_secret("OSC0123456789", "<unit>")
        m.add_secret("11111111", "<id>")
        res = results_mod.Results(m, quiet=True)
        r = results_mod.Result("cmd.strings", "cmd", "W")
        # a result that (wrongly) carries a serial and a bond id: masking must remove them
        r.set(results_mod.PASS, notes="serial OSC0123456789 bond 11111111",
              observed={"serial": "OSC0123456789", "raw": b"\x11\x11\x11\x11"})
        res.add(r)
        with tempfile.TemporaryDirectory() as d:
            report = res.write(d, {"suite_version": "x", "started": "t", "groups": None},
                               {"os": "x"}, {"serial": "OSC0123456789"}, {"present": True},
                               {"seen_open": False}, 0)
            blobs = []
            for name in ("report.json", "report.md", "console.log", "ring.jsonl"):
                with open(os.path.join(d, name), encoding="utf-8") as f:
                    blobs.append(f.read())
            whole = "\n".join(blobs)
        self.assertNotIn("OSC0123456789", whole)
        self.assertNotIn("11111111", whole)
        self.assertIn("<unit>", whole)

    def test_custom_name_not_in_the_transcript(self):
        # a product name stored on the board may be personal: the transcript keeps its length only
        dec = results_mod.decode_transcript_line
        self.assertEqual(dec("< name      My Bench Pad"),
                         "< name      <a custom name, 12 characters>")
        self.assertEqual(dec("< name      Steam Controller"), "< name      Steam Controller")
        self.assertEqual(dec("> opensc2 id"), "> opensc2 id")
        self.assertEqual(dec("< unit      OSC0123456789"), "< unit      OSC0123456789")

    def test_chosen_paths_not_in_the_report(self):
        # a --results or --fw-emu path is the user's choice and may hold a name anywhere in it
        default = os.path.join(os.path.dirname(os.path.abspath(results_mod.__file__)), "results")
        mine = os.path.join(tempfile.gettempdir(), "carols-bench", "results")
        emu = os.path.join(tempfile.gettempdir(), "carols-bench", "opensc2_fwemu.exe")
        opts = results_mod._report_options({"results_dir": mine, "fw_emu": emu, "lizard": True,
                                            "started": "t", "suite_version": "x"})
        self.assertEqual(opts["results_dir"], "(--results)")
        self.assertEqual(opts["fw_emu"], "opensc2_fwemu.exe")
        self.assertTrue(opts["lizard"])
        self.assertNotIn("started", opts)
        self.assertNotIn("carols-bench", json.dumps(opts))
        self.assertEqual(results_mod._report_options({"results_dir": default})["results_dir"],
                         "tools/hwtest/results")


if __name__ == "__main__":
    unittest.main()
