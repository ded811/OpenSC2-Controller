# Changelog

Every change to OpenSC2 that matters to people who use it, release by release. The form follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html): while the version is 0.x, the API may
still change between minor versions.

## [Unreleased]

### Added

- The real controller's system state machine (`CONFIG_OPENSC2_SYSTEM`, on by default): the power-on
  decision on USB power, on the dock and on a battery; STEAM pressed, and held 7 s; the transport
  chords and their 4 s pairing hold; the keylock (`user/keylock`); the idle timeout (setting 50);
  low battery; the turn-off command, a link that gives up and the puck's turn-off; the cable pulled;
  setting 77 (reboot instead of power-off); what powers the board (`opensc2_set_power_source()`,
  `opensc2_power_source()`, `opensc2_keylock()`); the shell's `opensc2 power`.
- A battery build: `CONFIG_OPENSC2_BATTERY` and `app/battery.conf`. Its shutdowns end in
  `OPENSC2_POWER_OFF` (the blank canvas: System OFF, woken by STEAM or USB power), and the radio
  gives up as the real controller does (60 s of listening; `CONFIG_OPENSC2_NO_BOND_TIMEOUT_S`, 10 s
  with an empty slot, link reason `OPENSC2_REASON_NO_BOND_TIMEOUT`). Without the option a board
  never switches itself off: every shutdown reboots.
- The status LED: the library selects the real controller's modes (the `led_status` callback,
  `opensc2_led_status()`), and `<opensc2/led.h>` has its patterns as a pure player, the brightness
  curve (setting 38), the channel trims, the fully-charged display
  (`user/fully_charged_led_intensity`) and the IR LED (setting 36). The blank canvas shows them on
  its one-colour LED, an optional RGB or RGBW LED and an optional IR LED.
- Cue events (the `cue` callback, `enum opensc2_cue`): the power-on and power-off chimes, the
  transport cues, pairing and paired, the PC asleep, each with the built-in script it stands for
  and its gain from the chime level. Playing them is the application's.
- `OPENSC2_CHARGE_GAVE_UP` (the charge state's `0x80` flag) and `opensc2_charge_state_of()`; an
  optional charger status pin in the blank canvas (`charger-status-gpios`).
- The test hooks' system injection (ops `0x70` to `0x74`: buttons, a battery level and a power
  source that reach the rules, a boot-time injection, the machine's status, simulated link events),
  its ring entries (`0x50` to `0x56`) and caps build flag 18; the hardware test suite's `system`
  group; host tests of every pure rule; the firmware emulator runs the machine.

### Changed

- With the system state machine, `opensc2_init()` leaves starting the link to it (still only when
  `config.start` is set); the turn-off command and the cable pulled are its events, and the
  application gets `OPENSC2_POWER_OFF` or a reboot at the end of a shutdown; a host's reboot and
  bootloader requests reach `power_request` only in the states that act on them.
- Wireless on USB power, STEAM held 7 s, the turn-off command, a link that gave up and the puck's
  turn-off now switch wireless off (the amber LED), and a STEAM press then reboots the board, which
  comes back wired, as on the real controller; `opensc2 start` turns wireless on again. In v0.2.0
  the blank canvas only stopped the radio, and any button started it again.
- The blank canvas's LED shows the real controller's patterns (on its one-colour LED: on once set
  up or connected, a slow blink while it looks for the puck, a short dark blink every second for
  amber).
- `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` defaults to 60 s in a battery build.
- The retained RAM area's layout is version 2 (the test hooks' system words), so the first boot of
  this firmware starts with an empty USB session log.

### Not proven

- Everything above is built, host-tested and run by the test suite against the firmware emulator;
  none of it has run on hardware yet.

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
