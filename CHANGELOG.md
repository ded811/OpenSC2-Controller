# Changelog

Every change to OpenSC2 that matters to people who use it, release by release. The form follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html): while the version is 0.x, the API may
still change between minor versions.

## [Unreleased]

### Added

- Bluetooth LE, an opt-in build (`CONFIG_OPENSC2_BLE`; for the SuperMini the fragment
  `app/ble.conf`): the board as a HID-over-GATT Steam Controller 2 (PnP ID `28DE:1303`) with the
  real controller's name, advertising and pairing (Just Works, LE Secure Connections), the USB
  report map, the vendor commands on feature report 1 (refusals as ATT errors), output reports, the
  battery and device information services, October's gate (encryption and an ATT MTU of 64), a
  state report `0x45` per connection event and lizard mode. Built; not yet tested on hardware.
- Wireless transports: the real controller's four `user/wireless_transport` values (Bluetooth slots
  1 and 2, the puck with bond 1 or 2), `opensc2_select_transport()`, `opensc2_selected_transport()`
  and the shell's `opensc2 transport`; the puck's ESB and Bluetooth hand the radio over to each
  other at run time. With Bluetooth, `opensc2 ble status` and `opensc2 ble pair`.
- Attribute 11 of `0x83` follows the link (the connection interval over Bluetooth, 64000 µs for the
  dock link to come); the library's links and the test hooks' ring know Bluetooth (4) and the dock
  (5); ring entries of kind `0x40` for Bluetooth events; caps bit 17 for a Bluetooth build.
- Bluetooth, as the real controller does it: five identities, two per slot, with the rule that
  picks one and a pairing request that resets the slot's other identity (so the old host keeps its
  bond until a new one has paired); the bond rules (a new host deletes the slot's other bond, a
  deleted bond resets its identity, a host bonded on another identity loses that bond); high-duty
  directed advertising to the bonded host, then allow-listed, else pairing mode; a 60 s advertising
  window from every start (`CONFIG_OPENSC2_BLE_ADV_WINDOW_S`); "wireless off" when the window runs
  out or the host ends the link (`0x13`, `0x15`), with the reasons `LISTEN_TIMEOUT` and
  `HOST_DISCONNECTED` and the power request `OPENSC2_POWER_WIRELESS_OFF`; advertising again 500 ms
  after any other disconnect; a factory reset deletes every Bluetooth bond and identity.
  `opensc2_select_transport_pairing()` for a transport chord held 4 s. Built; not yet tested on
  hardware.
- Valve's vendor GATT service (`CONFIG_OPENSC2_BLE_VENDOR_SERVICE`, on in a Bluetooth build): the
  command channel, inputs and outputs as plain characteristics for SDL's Android and Apple back
  ends, sharing their values with the HID service; reports go to the vendor characteristic first
  when the host subscribed there.
- The blank canvas shows the Bluetooth link with the real controller's patterns in blue, and
  `opensc2 ble status` shows the identity, the bonds, the advertising form and its window.
- The hardware test suite's Bluetooth path (`--bluetooth`, path B): the board found through its
  console and the HID device Windows made of it, only that device ever opened; the groups hooks,
  desc, cmd, inputs, battery, stream, reports, outputs, settings, lizard and modes over Bluetooth,
  and the new group `ble` (the link, the `0x45` cut, a lost link, wireless off, with `--flash` the
  advertising window). Test ops `0x60` BLE_STATUS, `0x61` BLE_DROP and `0x62` BLE_WINDOW, and ring
  events for advertising, identities, bonds, disconnects and wireless off. Run only against the
  simulator so far.

### Changed

- A transport or puck slot chosen at run time (`opensc2_select_slot()`, the shell's `opensc2
  slot`) is stored once its link is up, as the October 2026 firmware does, instead of at once. It
  is stored as `opensc2/transport`, with `opensc2/slot` still written for a puck transport.
- Host tests: 65 (2,601 checks), and 94 (5,149 checks) with the hardware test hooks; the runner's
  self-tests: 233.

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
