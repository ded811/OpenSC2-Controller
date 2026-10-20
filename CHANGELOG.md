# Changelog

Every change to OpenSC2 that matters to people who use it, release by release. The form follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html): while the version is 0.x, the API may
still change between minor versions.

## [Unreleased]

### Added

- Calibration as the real controller keeps it (`CONFIG_OPENSC2_CAL`, on by default; built, host
  tests only): the `cal/` keys (`cal/joy_l`, `cal/trg_l`, `cal/prs_l`, the LED trims, the battery
  offset, the gyro bias and the rest) under `opensc2/cal/` in the board's storage, read with `0xED`,
  changed with `0xEE`, `0xEF` and `0xF0`, kept by the factory reset; the recordings `0xD8` (sticks),
  `0xC0` (triggers) and `0xC3` (pad pressure); `0xE2` saves one side's values and `0xFE` erases
  them, only with its key. The console's `opensc2 cal` does the same.
- Raw inputs (`opensc2_set_stick_raw()`, `_trigger_raw()`, `_pressure_raw()`, `_touch_raw()`,
  `opensc2_set_raw()`): the library processes them at every state report as the real controller
  does, with the stored calibration and controller settings 30, 31, 34, 35, 46, 52, 53, 60 and 62
  (millivolts or counts, pressure and click by pressure, stick touch and grip).
- The pad rules (`CONFIG_OPENSC2_PAD_RULES`, on by default, inactive with the default settings):
  the pad rotation of settings 2 and 24 and the touch stabiliser of setting 80 = 2, on report
  `0x42` and lizard mode.
- Pure helpers for applications: `<opensc2/cal.h>` (the calibration's arithmetic),
  `<opensc2/sensor_helpers.h>` (capacitive touch, the trackpad's touch, filter, rotation and
  stabiliser, IMU scaling and mounting, a gyro-bias estimate, ADC bursts) and `<opensc2/gauge.h>`
  (battery voltage and state of charge), each with host tests.
- The blank canvas's options, all off by default and untested on hardware, built together by
  `app/sensors.conf` (with `app/sensors.overlay`): sticks and triggers read right before each state
  report with four conversions averaged and a switched sensor supply, calibrated axes through the
  library, an LSM6DSV16X IMU with a mounting matrix (`imu-mounting-matrix`) and a rest-based gyro
  bias (settings 84 and 85), the trackpad's processing (settings 65, 72, 73), stick touch and
  grips from a CAP1206 capacitive sensor (any input device named by the chosen node
  `opensc2,touch-keys`), the battery level, and I2C bus recovery at boot. `app/imu-stream.conf`
  streams the LSM6DSV16X's FIFO with its own fusion instead, as the real controller reads it.
  `app/expander.conf` adds L4, L5, R4 and R5 through an I2C port expander (a PCA9555) polled every
  16 ms, with every button debounced 16 ms. A Kconfig fragment of `app/` brings the overlay of the
  same name.
- The test hooks' raw injection (`RAW_INJECT`, `RAW_OFF`, `CAL_STATUS`, caps bit 21) and the
  hardware test suite's group `cal`, checked against the firmware emulator.

### Changed

- Without a stored calibration nothing changes for applications that give finished values. With
  `CONFIG_OPENSC2_CAL`, `0xD8`, `0xC0`, `0xC3` and `0xFE` are answered by the library instead of
  the application's `vendor_command`, `0xEE`, `0xEF` and `0xF0` accept the `cal/` keys, and `0xE2`
  reads its side and mask whatever its length byte says, as the real controller does.
- The hardware test suite never writes, saves or deletes a `cal/` key (safety rule S5), and
  `cmd.save_refusals` checks a refused `0xEF` on `cal/joy_l` instead of `0xF0`.

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
