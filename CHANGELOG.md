# Changelog

Every change to OpenSC2 that matters to people who use it, release by release. The form follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html): while the version is 0.x, the API may
still change between minor versions.

## [Unreleased]

Built and tested on the host, in the firmware emulator and in the simulator; not yet tested on
hardware.

### Added

- Lizard mode's processing as the real controller's, each part a Kconfig option, all on by
  default: the touch stabilisers (the right pad's while setting 80 is 1, the left pad's always),
  the 8-frame motion average, the glide after a flick (settings 11 and 12; a new touch, lizard mode
  turning off or the test hooks' resync end it), and the haptic ticks and pad clicks (settings 14,
  52, 53 and 70), which reach the application as haptic events with report ID 0.
- The real controller's rules on every state report, after the test hooks' injection point: a
  stick deflected beyond 12000 counts as touched (`CONFIG_OPENSC2_STICK_TOUCH_FALLBACK`), setting
  82 blanks a pad in report `0x42` too (`CONFIG_OPENSC2_PAD_BLANK_ON_STICK`), no trigger bit while
  setting 62 asks for raw triggers, and, opt-in, the pads' clicks from their pressure with
  settings 52 and 53 (`CONFIG_OPENSC2_PAD_CLICK_AUTO`).
- The blank canvas: setting 46's raw stick output (ADC counts, or millivolts the real controller's
  way).
- Test hooks: the rule word (caps bit 15; ops `RULES` 0x17 and `RULES_SET` 0x18, which lasts until
  the end of arming). The hardware test suite: `inputs.stick_touch`, `inputs.pad_blank`,
  `inputs.raw_trigger_bits`, `inputs.pad_click`, `inputs.raw_sticks`, `lizard.stabiliser`,
  `lizard.smoothing`, `lizard.glide`, `lizard.glide_stops`, `lizard.ticks`, `lizard.default`; the
  simulator faults `pad_blank_ignored` and `glide_ignored`.

### Changed

- Through the puck, lizard mode's and the application's reports wait for room in the radio's
  queue (`CONFIG_OPENSC2_REPORT_WAIT_DEPTH`, 8) instead of being dropped, and lizard mode waits
  with its next frame meanwhile; the battery report is still dropped when the queue is full.
- The hardware test suite's lizard group runs its mapping tests with lizard mode's processing
  switched off, and follows every sequence until lizard mode goes quiet (a glide keeps the reports
  going); its model and the shared vectors cover the input rules and the processing.
- The host tests are now 62 (5,221 checks), and 90 (7,700 checks) with the hardware test hooks;
  the runner has 211 self-tests.

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
