# Contributing

How to report a problem, add a hardware result or send a change, and what every contribution must
keep: for anyone who wants to help.

Bug reports, hardware reports and pull requests are welcome. The
[issue forms](https://github.com/ded811/OpenSC2-Controller/issues/new/choose) ask for what helps:
a bug, a hardware report (what you wired and what worked in Steam), or a new board. Protocol
questions belong to the
[research repository](https://github.com/ded811/steam-controller-2-reverse-engineered).

## Provenance

OpenSC2 is original code written from the published protocol documentation, plus black-box tests
and USB captures of Steam with a real controller ([How it was made](README.md#how-it-was-made)).
A few facts come from the same authors' study of the controller's firmware and are not yet on the
published pages: the ranges of seven settings, details of the report descriptor, and lizard mode's
key-slot rule and scales ([docs/protocol.md](docs/protocol.md#where-this-library-had-to-fill-gaps)
lists each). It is not a clean-room implementation, and it says so. Contributions keep it that
way:

- Write your change yourself, from public documentation: the research repository's docs,
  [Counterpuck](https://github.com/ded811/Counterpuck)'s docs, SDL's public driver (its names and
  declared values, not its code), the nRF Connect SDK and Zephyr. Black-box tests with Steam and
  the puck are fine.
- No code, firmware, binaries or captures from Valve, and nothing taken from Valve's firmware or
  from a reconstruction of it.
- If you are not sure where something comes from, say so in the pull request.

## Privacy

`opensc2 bonds` shows your puck's serial, and `opensc2 status` and `opensc2 id` show the board's
serials: mask them before you post them. `opensc2 usb log` and the test suite's reports are masked
and safe to share.

Never post serial numbers, bond IDs, your puck's serial, Steam IDs, your user folders, or unmasked
lines of Steam's own logs, in an issue or in a commit. The test suite's `report.md` and
`opensc2 usb log` are masked; anything else, check before you paste it.

## Build and test

| What | Command |
| --- | --- |
| Build (Windows) | `powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project app -OutDir C:\b\osc2 -Pristine` (any project folder: `app`, `samples\minimal`, `template`, ...) |
| Build (Linux, macOS) | `west build -b promicro_nrf52840/nrf52840/uf2 -d build/app app -p always`, in the SDK's toolchain environment ([getting started](docs/getting-started.md#build)) |
| Host tests | `powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\run.ps1`, or `sh tests/host/run.sh` |
| The test suite's own checks | `python -I tools/hwtest/run.py --selftest` and `python -I tools/hwtest/run.py --dry-run` |
| Links in the documentation | `python -I tools/check_links.py` |

A change is ready when every build you touched has no compiler warnings and leaves no
`partitions.yml`, and the host tests, the self-tests, the dry run and the link check pass. There is no CI: run them
locally before you open a pull request (the firmware builds need the nRF Connect SDK). If your change touches what a board does, run the
hardware test suite on one ([testing.md](docs/testing.md)) and say which tests ran.

## Style

- C: the Zephyr style (tabs, 100 columns), an SPDX header in every new file:

  ```c
  /*
   * Copyright (c) 2026 the OpenSC2-Controller authors
   * SPDX-License-Identifier: MIT
   */
  ```

- Kconfig fragments, CMake, scripts: the same two lines as `#` comments.
- Documentation: plain, short sentences; working commands; tables where they help. Never claim
  more than was tested: say what ran on which board, with which parts and versions (the hardware
  report form asks for exactly that), and keep [What works](README.md#what-works) true.
- Update `CHANGELOG.md` under "Unreleased", and the documentation the change touches.

## Licence

Contributions are under the MIT licence, the licence of this repository ([LICENSE](LICENSE)).

The release process is in [docs/releasing.md](docs/releasing.md).

Back to the [documentation map](docs/README.md).
