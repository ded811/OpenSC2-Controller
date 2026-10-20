#!/usr/bin/env python3
# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# gen_haptics_tables.py: write lib/opensc2/haptics_tables.c, the constant tables of the haptics
# synthesiser (CONFIG_OPENSC2_HAPTICS_SYNTH). The synthesiser renders in integers only, so a block
# comes out bit for bit the same on the board and on a PC (the host tests and the hardware test
# suite compare CRCs of rendered blocks); the few values that need a real function are computed
# here once, with Python's double precision, and rounded:
#
#   osc2_hap_sine_q15    a quarter of a sine, 257 points, 32767 = 1.0 (linear interpolation in between)
#   osc2_hap_gain_q16    10^(dB/20) for -30..+30 dB in 1 dB steps, 65536 = 1.0
#   osc2_hap_exp2_q16    2^(i/64) for i = 0..64, 65536 = 1.0 (the exponential sweep)
#   osc2_hap_log2_q16    log2(1 + i/64) for i = 0..64, 65536 = 1.0
#   osc2_hap_click_a     OpenSC2's click A: 70 signed 8-bit samples at 4 kHz (17.5 ms)
#   osc2_hap_click_b     OpenSC2's click B: 93 signed 8-bit samples at 4 kHz (23.25 ms)
#
# The two clicks are OpenSC2's own shapes, damped sine bursts made from the formulas below; they
# have the lengths of the real controller's clicks, not their sound.
#
#   python -I tools/gen_haptics_tables.py            # rewrite the file
#   python -I tools/gen_haptics_tables.py --check    # exit 1 if the file differs from what it writes
#
# Standard library only.
import argparse
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "lib", "opensc2", "haptics_tables.c")


def rnd(x):
    """Round half away from zero (Python's round() rounds half to even)."""
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


def sine_quarter():
    return [rnd(32767 * math.sin(i * math.pi / 512)) for i in range(257)]


def gains():
    return [rnd(65536 * 10 ** (db / 20.0)) for db in range(-30, 31)]


def exp2_table():
    return [rnd(65536 * 2 ** (i / 64.0)) for i in range(65)]


def log2_table():
    return [rnd(65536 * math.log2(1 + i / 64.0)) for i in range(65)]


def click(n, hz, attack, tau, taper):
    """A damped sine burst at 4 kHz: a linear attack over `attack` samples, an exponential decay of
    time constant `tau` samples, a linear taper to 0 over the last `taper`; peak scaled to 127."""
    raw = []
    for i in range(n):
        env = min(1.0, (i + 1) / attack) * math.exp(-i / tau)
        if i >= n - taper:
            env *= (n - i) / taper
        raw.append(env * math.sin(2 * math.pi * hz * i / 4000.0))
    peak = max(abs(v) for v in raw)
    return [rnd(127 * v / peak) for v in raw]


def c_array(ctype, name, values, per_line):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("\t" + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    return "const %s %s[%d] = {\n%s\n};\n" % (ctype, name, len(values), "\n".join(lines))


def render():
    parts = [
        "/*\n"
        " * Copyright (c) 2026 the OpenSC2-Controller authors\n"
        " * SPDX-License-Identifier: MIT\n"
        " *\n"
        " * haptics_tables.c: the constant tables of the haptics synthesiser (haptics_dsp.h). Written\n"
        " * by tools/gen_haptics_tables.py: change that script and run it, not this file.\n"
        " */\n"
        "#include \"haptics_dsp.h\"\n",
        "/* sin(i * pi / 512) for i = 0..256, 32767 = 1.0. */\n" +
        c_array("int16_t", "osc2_hap_sine_q15", sine_quarter(), 12),
        "/* 10^(dB / 20) for dB = -30..+30, 65536 = 1.0. */\n" +
        c_array("uint32_t", "osc2_hap_gain_q16", gains(), 8),
        "/* 2^(i / 64) for i = 0..64, 65536 = 1.0. */\n" +
        c_array("uint32_t", "osc2_hap_exp2_q16", exp2_table(), 8),
        "/* log2(1 + i / 64) for i = 0..64, 65536 = 1.0. */\n" +
        c_array("uint32_t", "osc2_hap_log2_q16", log2_table(), 8),
        "/* OpenSC2's click A: a 250 Hz sine burst, 2-sample attack, 3 ms decay (70 samples at 4 kHz). */\n" +
        c_array("int8_t", "osc2_hap_click_a", click(70, 250.0, 2, 12.0, 8), 14),
        "/* OpenSC2's click B: a 160 Hz sine burst, 4-sample attack, 6 ms decay (93 samples at 4 kHz). */\n" +
        c_array("int8_t", "osc2_hap_click_b", click(93, 160.0, 4, 24.0, 16), 14),
    ]
    return "\n".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else None)
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if lib/opensc2/haptics_tables.c differs from what this writes")
    args = ap.parse_args()
    text = render()
    if args.check:
        try:
            with open(OUT, "r", newline="") as f:
                same = f.read().replace("\r\n", "\n") == text
        except OSError:
            same = False
        print("haptics_tables.c is %s" % ("up to date" if same else "OUT OF DATE"))
        return 0 if same else 1
    with open(OUT, "w", newline="\n") as f:
        f.write(text)
    print("wrote %s" % os.path.relpath(OUT, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
