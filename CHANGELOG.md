# Changelog

Every change to OpenSC2 that matters to people who use it, release by release. The form follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html): while the version is 0.x, the API may
still change between minor versions.

## [Unreleased]

Built and tested on the host, in the firmware emulator and in the simulator; not yet tested on
hardware.

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
  simulator faults `pad_blank_ignored`, `glide_ignored` and `ticks_ignored`.
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
- The dock, the controller's end of Valve's puck's charging pads (built; not yet tested on
  hardware): the 73-byte frames with their CRC-16/KERMIT (`<opensc2/dock.h>`); the dock link
  (`CONFIG_OPENSC2_DOCK_LINK`, on with the test hooks): sessions that a failed frame ends, the
  identity record for the probe, Steam's vendor commands on a stored request of the dock's own
  (`0x81` and `0x85` acknowledged and ignored there), GET input from the copy of the latest state
  report or, with the dock alone, from a new one, attribute 11 = 64000 while the dock is the only
  link; the dock's rules (the pilot's window, dock or USB power, the docked states), which the
  library does not act on yet.
- `CONFIG_OPENSC2_DOCK`, the dock's UART and pilot sense from the devicetree alias `opensc2-dock`
  (`dts/bindings/opensc2,dock.yaml`), and `app/dock.conf` with `app/dock.overlay`, a SuperMini
  variant: built, never run on hardware. Nothing may be connected to Valve's puck's pads without
  the owner's go-ahead.
- Test hooks: the dock ops `0x90` to `0x93` (a frame loaded in two parts, run, its answer read back
  with serials and bonds masked; the pilot check, the supply rule and the docked states on injected
  values), the dock's ring entries `0x70` to `0x73` on link 5, caps bit 20.
- The hardware test suite's opt-in `dock` group (`--dock`), with its safety rule (a dock frame that
  runs a command is held to the test link's allowlist); `tools/fake_puck.py`, the puck's end of the
  dock link through the firmware emulator, a board's test hooks or a USB-UART adapter (untested).

### Changed

- A transport or puck slot chosen at run time (`opensc2_select_slot()`, the shell's `opensc2
  slot`) is stored once its link is up, as the October 2026 firmware does, instead of at once. It
  is stored as `opensc2/transport`, with `opensc2/slot` still written for a puck transport.
- Host tests: 65 (2,610 checks), and 94 (5,164 checks) with the hardware test hooks; the runner's
  self-tests: 233.
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
- Through the puck, lizard mode's and the application's reports wait for room in the radio's
  queue (`CONFIG_OPENSC2_REPORT_WAIT_DEPTH`, 8) instead of being dropped, and lizard mode waits
  with its next frame meanwhile; the battery report is still dropped when the queue is full.
- The hardware test suite's lizard group runs its mapping tests with lizard mode's processing
  switched off, and follows every sequence until lizard mode goes quiet (a glide keeps the reports
  going); its model and the shared vectors cover the input rules and the processing.
- The host tests are now 62 (5,221 checks), and 90 (7,700 checks) with the hardware test hooks;
  the runner has 213 self-tests.
- Without a stored calibration nothing changes for applications that give finished values. With
  `CONFIG_OPENSC2_CAL`, `0xD8`, `0xC0`, `0xC3` and `0xFE` are answered by the library instead of
  the application's `vendor_command`, `0xEE`, `0xEF` and `0xF0` accept the `cal/` keys, and `0xE2`
  reads its side and mask whatever its length byte says, as the real controller does.
- `0xED` answers the `cal/` keys as the real controller does even on a board without
  calibration, where 0.2.0 answered them all with length 0: the LED trims with their defaults (90,
  35, 120, 80), `cal/volt_offset` (2 bytes) and `cal/touch_l`, `cal/touch_r` (10 bytes) always;
  the stick, trigger and pressure keys still with length 0 until one is stored.
- The hardware test suite never writes, saves or deletes a `cal/` key (safety rule S5), and
  `cmd.save_refusals` checks a refused `0xEF` on `cal/joy_l` instead of `0xF0`.
- With the system state machine, `opensc2_init()` leaves starting the link to it (still only when
  `config.start` is set); the turn-off command and the cable pulled are its events, and the
  application gets `OPENSC2_POWER_OFF` or a reboot at the end of a shutdown; a host's reboot and
  bootloader requests reach `power_request` only in the states that act on them.
- Wireless on USB power, STEAM held 7 s, the turn-off command, a link that gave up and the puck's
  turn-off now switch wireless off (the amber LED), and a STEAM press then reboots the board, which
  comes back wired, as on the real controller; `opensc2 start` turns wireless on again. In v0.2.0
  the blank canvas only stopped the radio, and any button started it again.
- The blank canvas's LED shows the real controller's patterns (on its one-colour LED: on once set
  up or connected, a slow blink while it looks for the puck, two quick flashes while it pairs over
  the air, a short dark blink every second for amber).
- `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` defaults to 60 s in a battery build.
- The retained RAM area's layout is version 2 (the test hooks' system words), so the first boot of
  this firmware starts with an empty USB session log.
- Output report `0x88` with more than 31 bytes a side does nothing, as on the real controller (it
  was clamped to 31).
- With the synthesiser, `app/`'s optional vibration motor follows the rendered blocks' loudness.
- The host tests are now 80 (6,409 checks), and 109 (8,871 checks) with the hardware test hooks built
  in; the runner has 215 self-tests.
- Vendor commands run one at a time across links (a mutex in the command channel), now that the
  dock can run them beside USB or the radio.
- The host tests are now 65 (3,121 checks), and 100 (5,964 checks) with the hardware test hooks,
  and each test also checks that every mutex the library took was let go again; the runner has
  216 self-tests and its dry run includes `--dock`.

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
