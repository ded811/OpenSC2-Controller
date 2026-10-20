# Changelog

Every change to OpenSC2 that matters to people who use it, release by release. The form follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html): while the version is 0.x, the API may
still change between minor versions.

## [Unreleased]

### Added

- The haptics synthesiser (`CONFIG_OPENSC2_HAPTICS_SYNTH`, off by default; `app/haptics.conf`
  builds the blank canvas with it): the library renders the haptic output reports into sample
  blocks as the real controller's two sequencers do (the pads at 4 kHz, the grips at 8 kHz, one
  thread each): `0x81`'s pulse trains and special cases, clicks, tones with an LFO, exponential
  sweeps, the October rumble emulation with settings 27, 75 and 76, scripts (one per channel), PCM
  streams with their rings, s16le, s8 and mu-law, interpolation and the `0x44` flow control.
  Sounds of OpenSC2's own (clicks, scripts 1 to 17, cues by number at the chime level, drop
  sounds after a free fall), never Valve's recordings or script data. Outputs the application
  provides (`<opensc2/haptics.h>`), a level callback for amplitude-only actuators, and three
  built-in outputs: an I2S amplifier for the grips, a PWM "DAC" on nrfx for the pads, a DRV2605 in
  real-time playback. Integer arithmetic: blocks are bit for bit the same on the board and on a
  PC. Built; not yet tested on hardware.
- `samples/haptics`: the synthesiser on the I2S and PWM DAC outputs, or a DRV2605
  (`drv2605.conf`), with a `hap` shell command to play the sounds by hand.
- The hardware test suite's `haptics` group (only on a board built with the synthesiser): CRCs of
  rendered blocks against shared vectors (`tests/vectors/hwtest/haptics.txt`), generators, the
  rumble's gains, scripts, cues, the streams' flags, the drop sounds' timing. The test hooks' ops
  `0x80`..`0x83` and ring entries `0x60`..`0x63`, caps bit 19. The firmware emulator runs the
  synthesiser, so the dry run checks the group against the library's code.
- `tools/gen_haptics_tables.py`, which writes the synthesiser's constant tables.

### Changed

- Output report `0x88` with more than 31 bytes a side does nothing, as on the real controller (it
  was clamped to 31).
- With the synthesiser, `app/`'s optional vibration motor follows the rendered blocks' loudness.
- The host tests are now 77 (6,331 checks), and 106 (8,793 checks) with the hardware test hooks built
  in; the runner has 215 self-tests.

## [0.2.0] - 2026-10-19

First public release.

### Added

- Wired mode: the board is a USB Steam Controller 2 (`28DE:1302`) with the real controller's
  descriptors, in the July 2026 form by default and the October 2026 form as an option. Steam's
  vendor commands on feature report 1; pairing with the puck over the USB-C cable, and Steam's
  "reboot into wireless mode".
- The boot-mode rule (wired or wireless, decided once at power-up) and a stored mode policy; the
  ways back to wireless; the plug-in rule.
- The USB session log, kept across a reboot and read in wireless mode.
- The debug console (a USB serial port with the shell) in wireless mode.
- Identity commands in the shell, and an optional product name.
- The hardware test hooks (`CONFIG_OPENSC2_TEST_HOOKS`) and the hardware test suite
  (`tools/hwtest`).
- `tools/match_steam_version.py`, which gives a board the build time of Steam's current controller
  firmware.
- `template/`, a project to copy as the start of your own, and `docs/integration.md`.
- Wired mode and the shell in every sample, and a README for each.
- The documentation map, getting started, troubleshooting, test protocol and release pages;
  `NOTICE.md`, `CONTRIBUTING.md`, `SECURITY.md`; `tools/check_links.py` and `tools/collect_licenses.py`.

### Changed

- The revision string Steam reads is `osc2-0.2.0`, both as the library's default and in `app/`
  (they were `opensc2-0.1` and `osc2-app-0.1`).
- The SuperMini's RC oscillator lines moved from `prj.conf` to
  `boards/promicro_nrf52840_nrf52840_uf2.conf`.
- The host tests are now 52 (2,209 checks), and 79 (4,658 checks) with the hardware test hooks
  built in.

### Proven on hardware

Tested with the v0.2.0 release image (`opensc2-supermini-nrf52840-v0.2.0.uf2`, 2026-10-19), on a
SuperMini nRF52840 with nothing wired to it, with Valve's puck on its October 2026 firmware and
Steam on a Windows PC.

- It enumerates over USB exactly like a real Steam Controller 2 (`28DE:1302`).
- Steam pairs it with Valve's puck over the USB-C cable, like a real controller.
- It then reboots into wireless mode and connects through the puck.
- Steam shows no "needs an update" prompt for a freshly flashed board: it reports a build time newer
  than Steam's current controller firmware (set to an older one, it gets the prompt).
- The automated hardware test suite passed every test on the release image with every opt-in on
  (`--through-puck --flash --lizard --with-steam --steam-control`): 165 of 165 in a run started
  wireless, and 31 of 31 for the `modes` and `cmd` groups in a run started wired, which adds the
  four tests that only run from a wired start (0 failed, 0 errors, 0 warnings). It covers inputs
  injected and checked wired and through the puck, battery, rumble and haptics, LEDs, settings,
  all vendor commands Steam sends, lizard mode with nothing reaching the PC, reboots, mode
  switches, reconnects, and a run with Steam open. Only the optional soak test (`--soak S`),
  which needs a duration, was not run.
- The charger test aid (a separate test build): on a plain USB wall charger it switches to
  wireless and connects through the puck. The suite counts one test per path (wired,
wireless, through the puck, with Steam); each test makes its own checks. In the report, the tests
of the through-puck and Steam paths carry the mode they ran in (`@R` or `@W`).

### Not proven

- Built, not yet tried with real parts: real buttons, sticks, trackpads, IMU, rumble motor, RGB LED
  and battery hardware (the drivers in `app/` and the samples have never run on such parts).
- Built, not run on hardware: the samples and the template.
- Built, untested with Steam: pairing over the air (the puck's pairing window).
- Not tested: other boards; Linux and macOS as the PC (the PC side was tested with Steam on
  Windows).
- Not implemented: Bluetooth; the dock (pogo pads); lizard mode's smoothing, glide and haptic
  ticks; PCM haptics playback in the samples.

## [0.1.0] - 2026-10-17 (not released)

The first commits: the library for the puck's 2.4 GHz link (wireless only: pairing over the air,
connecting, keep-alives, channel changes, the identity packet, the vendor commands through the puck,
lizard mode, the controller settings), the blank-canvas firmware for the SuperMini and six samples,
20 host tests (242 checks). Untested on hardware.

[Unreleased]: https://github.com/ded811/OpenSC2-Controller/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/ded811/OpenSC2-Controller/releases/tag/v0.2.0
