# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# stats.py: cadence and sequence-gap analysis of a stream of state reports (the stream.* tests).
# Pure: it takes arrival timestamps (nanoseconds) and sequence bytes, so it tests on synthetic data.


def cadence(timestamps_ns):
    """Mean period (first to last arrival / count), the largest gap, and reports per second."""
    n = len(timestamps_ns)
    if n < 2:
        return {"count": n, "mean_period_ms": None, "max_gap_ms": None, "per_s": None}
    span_ms = (timestamps_ns[-1] - timestamps_ns[0]) / 1e6
    mean_period = span_ms / (n - 1)
    gaps = [(timestamps_ns[i + 1] - timestamps_ns[i]) / 1e6 for i in range(n - 1)]
    return {
        "count": n,
        "mean_period_ms": mean_period,
        "max_gap_ms": max(gaps),
        "per_s": (1000.0 / mean_period) if mean_period > 0 else None,
        "gaps_ms": gaps,
    }


def sequence_gaps(seq_bytes):
    """Analyse a list of 8-bit sequence numbers. Returns counts of missing reports (the sum of the
    holes, mod 256), duplicates (a repeat), and the number of transitions seen."""
    missing = 0
    duplicates = 0
    transitions = 0
    for a, b in zip(seq_bytes, seq_bytes[1:]):
        transitions += 1
        step = (b - a) & 0xFF
        if step == 0:
            duplicates += 1
        elif step != 1:
            missing += step - 1
    return {"missing": missing, "duplicates": duplicates, "transitions": transitions,
            "count": len(seq_bytes)}


def gap_verdict(max_gap_ms, warn_ms, fail_ms):
    """A cadence gap verdict: 'pass', 'warn' or 'fail' against the per-path thresholds (fail_ms may
    be None: then a large gap only warns)."""
    if max_gap_ms is None:
        return "pass"
    if fail_ms is not None and max_gap_ms > fail_ms:
        return "fail"
    if warn_ms is not None and max_gap_ms > warn_ms:
        return "warn"
    return "pass"


def sequence_verdict(missing, explained, transitions, warn_frac, fail_frac):
    """Verdict for unexplained sequence gaps as a fraction of transitions (R on the radio): up to
    `warn_frac` pass, up to `fail_frac` warn, more fail. On W (warn_frac None) any unexplained gap
    fails."""
    unexplained = max(0, missing - explained)
    if transitions <= 0:
        return "pass", unexplained
    if warn_frac is None:
        return ("pass" if unexplained == 0 else "fail"), unexplained
    frac = unexplained / transitions
    if frac > fail_frac:
        return "fail", unexplained
    if frac > warn_frac:
        return "warn", unexplained
    return "pass", unexplained
