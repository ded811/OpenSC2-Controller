# Changelog

Every change to OpenSC2 that matters to people who use it, release by release. The form follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html): while the version is 0.x, the API may
still change between minor versions.

## [Unreleased]

### Added

- The real controller's haptic stream status replies: every stop-all (`0x81` with an on time of 0,
  `0x82` command 0, a zero speed in `0x80`) and `0x86`'s stream stop send `0x44 {channel, 0x02}`
  for each channel they reach (`CONFIG_OPENSC2_HAPTIC_STOP_STATUS`, on by default). Steam's pair
  of zero pulses now gets `44 04 02 ...` and `44 03 02 ...`, as from the real controller. The
  test hooks record each in the ring (kind `0x30`, STOP_STATUS); hwtest `outputs.stop_status`.
  Built and host-tested; not yet tested on hardware.
- Every settings key of the real controller in `0xED`..`0xF0`: the library now keeps
  `user/keylock`, `user/fully_charged_led_intensity`, `mte/charge_level`, `debug/resetreason` and
  `settings/haptics/...` (stored under `opensc2/k/`, read back where the real controller can, each
  exactly its size, `mte/charge_level` only while it is a valid window, deleted by the factory
  reset where it deletes them), and knows the `cal/` keys, which go to the application until the
  library keeps calibration. Any other name is still the application's, so a host cannot fill the
  storage partition. The test ring numbers the new keys 9 to 29; hwtest `cmd` and
  `flash.kept_keys` cover them. Built and host-tested; not yet tested on hardware.
- Crash records, as on the real controller (`CONFIG_OPENSC2_CRASH_RECORD`, on by default): the
  library's fatal-error handler leaves a 33-byte record in the retained RAM region and resets; the
  next boot stores it as `debug/resetreason`, and `0xF2` selector 1 answers it, then marks it
  seen. A crash at every boot writes flash once. Built and host-tested; not yet tested on
  hardware (nothing on the board can cause a crash on purpose yet).
- The settings partition's recovery (`CONFIG_OPENSC2_STORAGE_RECOVERY`, on with NVS settings): a
  partition with every sector occupied is erased at boot, as on the real controller. Built; not
  yet tested on hardware.
- The real controller's hardware watchdog (`CONFIG_OPENSC2_WATCHDOG`, off by default; the test
  build `app/watchdog.conf`): 5 s, fed every 2.5 s from the lowest priority, and a 4.5 s warning
  that crashes on purpose so a hang leaves a crash record. Built; not yet tried on hardware, nor
  with the UF2 bootloader.
- hwtest `steam.session` counts what Steam sends the outputs through the puck while it runs
  (output reports by ID, rumble, haptic commands by kind, PCM bytes, stops) and checks the board's
  `0x44` reply to each stop; the counts go into `report.json`. The test hooks' caps build flags
  have bit 16, `HAPTIC_STOP_STATUS`, and `outputs.stop_status` expects no replies without it.
  The new and changed tests have run only against the firmware emulator so far.

### Changed

- Vendor command `0x89` answers the defaults of settings 48, 84 and 85, as the real controller
  does (it hands them to its IMU); the application still sees the values Steam wrote. A number
  above 85 reads 0, also as on the real controller (it was a STALL).
- `0xE9` reads the LED colour back the way the real controller does: a channel reads 100 when it
  is exactly 100, otherwise 0. The colour itself is kept as sent.
- `0xBE`: byte 13 is 1 while the charge state is "charging" (else 0) and byte 14 is the charge
  state without its `0x80` flag, as on the real controller. The `gauge_flags` field of
  `struct opensc2_battery` is no longer sent.
- `0xA1` answers 18 bytes as the real controller does: selector 1 gives 1, one byte of reset-cause
  flags in its encoding (it was Zephyr's flags as a u32), the chip ID and 8 zero bytes; any other
  selector gives 0 and 17 zero bytes (it was an empty answer).
- None of these changed answers has been tested on hardware yet (built and host-tested).
- The test hooks' `HOST_SET` no longer takes `0xF2` with selector 1 (its GET can write flash now);
  `retained.c` is always built.

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
