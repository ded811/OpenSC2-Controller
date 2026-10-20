# OpenSC2 hardware test suite (PC runner)

The code of the PC side of OpenSC2's hardware test suite, for people who change the runner. How to
run it, what it covers, its safety rules and how to read its report are in
**[docs/testing.md](../../docs/testing.md)**; the bytes it exchanges with the firmware are in
[docs/test-protocol.md](../../docs/test-protocol.md).

It checks, with no physical interaction, every feature OpenSC2 emulates, on a board with nothing
wired to it. It drives the firmware's test hooks (`CONFIG_OPENSC2_TEST_HOOKS`, on in the
blank-canvas app; an unarmed board behaves exactly as without them), reads the board's own reports
while wired, the puck's slot reports (read-only) while wireless, and the board's console. With
`--bluetooth` it reads and writes the board's Bluetooth HID device instead (a Bluetooth build paired
with Windows; never any other Bluetooth device).

```powershell
python -I tools\hwtest\run.py --selftest      # the runner's own tests (no device)
python -I tools\hwtest\run.py --dry-run       # the whole suite against the simulator (no device)
python -I tools\hwtest\run.py --list          # what would run, per test
python -I tools\hwtest\run.py                 # the full run on hardware
python -I tools\hwtest\run.py --dry-run --bluetooth   # the Bluetooth path against the simulator
```

Python 3 with the standard library and `ctypes` only; hardware runs need Windows. Everything but
`win/` imports on any OS, so `--selftest` and `--dry-run` run anywhere.

## Layout

```text
tools/hwtest/
  run.py            entry point (puts tools/ on sys.path, then hwtest.cli.main)
  cli.py            options, the backend (hardware, simulator or firmware emulator), exit codes
  config.py         every shared constant (IDs, the test channel's key and ops, timeouts)
  safety.py         device classes, the vendor denylist and flash tokens, the shell allowlist,
                    the puck-writer gate, the injection preconditions: the only place that decides
  mask.py           masking of serials, IDs, chip IDs, paths, Steam IDs; the 16-hex backstop
  transport.py      the test channel on feature report 1, the console session (the HWT grammar,
                    the sync barrier, short chunks), the per-link lock and the lease keep-alive
  board.py          the Rig: one test session on top of a backend (the same code for all three)
  runner.py         the order of a run, what stops it, how it recovers when the board resets, the
                    exit code
  results.py        the console table, report.json, report.md, console.log, ring.jsonl
  stats.py          cadence and sequence-gap analysis
  sim.py, simfw.py  the offline simulator (--dry-run): the devices, the puck, Steam, the console,
                    and a model of the firmware; --sim-fault makes one thing misbehave
  simble.py         the simulator's Bluetooth link (--dry-run --bluetooth): identities, bonds,
                    advertising forms, the window, disconnects, and a Windows host that connects
                    a bonded board again; its HID nodes as Windows names them
  fwemu.py          the backend for the firmware emulator (tests/host/fwemu.c: the library's own
                    code on the PC, --dry-run --fw-emu EXE)
  proto/            the wire formats, pure: reports, model (the expected-report and lizard
                    models), vendor, testproto (the 0x7E channel), console (parsers), shellfmt
                    (shell.c's line formats), steamlog
  groups/           one module per test group (hooks, desc, cmd, inputs, battery, stream,
                    reports, outputs, settings, flash, lizard, wireless, puck, ble, steam, modes,
                    restore, cal) and base.py (checks, results, what ends a group)
  win/              Windows access through ctypes, imported lazily: backend (WinBackend),
                    devices (listing without opening; the Bluetooth HID nodes too), hid (the
                    board, on USB or over Bluetooth), serial (its console),
                    puck_read (read-only, binds no sending function), puck_write (gated), rawinput
                    (the leak detector), power, procs
  selftest/         the runner's unit tests (python -I tools\hwtest\run.py --selftest)
  results/          the results folders (git-ignored)
```

## The safety rules in the code

Comments in the code name the safety rules by label. Each rule is code with its own self-test
([docs/testing.md](../../docs/testing.md#the-safety-rules-it-enforces) explains them):

| Label | Rule |
| --- | --- |
| S1 | device classes: the writers take only a classified device, never a path (over Bluetooth: only the board's vendor collection, by the address and name its console reports) |
| S2 | the puck is read-only: its reader binds no function that can send |
| S3 | writes to the puck only through the gated writer (`--through-puck`, Steam closed, the slot proven to be the board's) |
| S4 | never feature report 2 (the puck's own commands) to the puck |
| S5 | the vendor-command denylist and the one-shot tokens of `--flash`; a `cal/` key is never written, saved or deleted, not even with a token |
| S6 | the console's allowlist of commands |
| S7 | the conditions for every injection (armed, the guard canary passed, Steam closed, the only reader, lizard mode off) |
| S8 | Steam appearing during a group stops its injection |
| S9 | everything printed or written is masked (Bluetooth addresses too) |
| S10 | leaving: the restore always runs, and the lease ends arming within 15 s |
| S11 | the UF2 bootloader appearing stops the run |
| S12 | the runner is the only reader of the collection it injects into |
| S13 | the leak detector |

Other labels in comments and self-test docstrings, such as `[R7]`, `R1`, `H2`, `I8`,
`safety-HW-3` or `correctness-keepalive-splits-vendor-set-get`, name findings of earlier reviews of
the code. Those reviews are not part of the repository; the comment or docstring next to each label
says what the code does and why.

## The shared vectors

`tests/vectors/hwtest/` (`state.txt`, `lizard.txt`, `ring.txt`, `testproto.txt`, `settings.txt`)
holds the byte-level rules both sides must agree on. The firmware's host tests and the runner's
self-tests read the same files; a change of a rule changes the file, and both sides follow.
`proto/shellfmt.py` holds the console lines of `lib/opensc2/shell.c`, and a self-test checks that
every one of them is still a format string of shell.c.

## Notes

- The hardware backend (`win/`) has run on hardware:
  [docs/testing.md](../../docs/testing.md#results-so-far). The simulator, the firmware emulator and
  the unit tests cover the runner's logic; the emulator also proves that the runner and the
  library's code agree.
- There is no recording and replay of a hardware run; the firmware emulator covers the offline
  cross-check against the real code.
- `--lizard`, `--flash`, `--through-puck`, `--with-steam` and `--bluetooth` are opt-in and off by
  default. The Bluetooth path (B) has run only against the simulator; the firmware emulator has no
  Bluetooth link.

Back to the [documentation map](../../docs/README.md).
