# The protocol, in brief

What OpenSC2 implements, in short: for the curious, and for anyone who wants to check the library
against the protocol documentation once it is public (until then, against this page and the API
reference). The essentials are on this page; the [API reference](api.md) has every field, command
and setting the library handles.

The library implements the controller's end of the 2.4 GHz link to Valve's puck, and the USB device
a Steam Controller 2 is on its cable. It was written from the protocol documentation the same
authors wrote (to be published in
[steam-controller-2-reverse-engineered](https://github.com/ded811/steam-controller-2-reverse-engineered);
not public yet), plus black-box tests and USB captures of Steam with a real controller
([How it was made](../README.md#how-it-was-made)). A few facts come directly from the same authors'
study of the controller's firmware, not from that documentation; the gaps below say which.

> [!NOTE]
> The research repository is not public yet, so the links to it on this page do not open until it
> is. They name where each topic is described in full; what OpenSC2 relies on is stated in the
> sections the "In brief" column points to.

| Topic | In brief | Full description (research repository) |
| --- | --- | --- |
| The controller's end of the radio link: set-up, listening, connecting, keep-alives, records, link loss, channels, bonds, the identity packet, protocol version, turn-off | [The radio link](#overview-the-radio-link) | [docs/controller/04-radio.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/04-radio.md) |
| The puck's end: slots, pairing window, connecting, channel choice, suspend | [The puck's end](#overview-the-pucks-end) | [docs/puck/04-radio.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/04-radio.md) |
| The wired USB device: descriptors, strings, report timing | [The cable](#overview-the-cable), [api.md, Wired mode](api.md#wired-mode) | [docs/controller/03-usb-and-hid.md, The device](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-device) |
| The report descriptor and its collections | [The cable](#overview-the-cable), [gap 13](#where-this-library-had-to-fill-gaps) | [The report descriptor](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-report-descriptor) |
| Report `0x42`, the button bits, the vendor commands | [Reports and outputs](#overview-reports-and-outputs), [The vendor commands](#overview-the-vendor-commands), [api.md, Inputs](api.md#inputs) | [docs/controller/03-usb-and-hid.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md) |
| The controller settings | [The vendor commands](#overview-the-vendor-commands), [api.md, Controller settings](api.md#controller-settings) | [Controller settings](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#controller-settings) |
| Lizard mode | [Lizard mode](#overview-lizard-mode) | [Lizard mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#lizard-mode) |
| Steam's wired session | [The cable](#overview-the-cable), [The vendor commands](#overview-the-vendor-commands) | [The wired session as Steam runs it](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it) |
| Pairing over the cable, and the reboot into wireless mode | [The cable](#overview-the-cable) | [Pairing a controller with a puck over the cable](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#pairing-a-controller-with-a-puck-over-the-cable), [Reboot into wireless mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#reboot-into-wireless-mode) |
| The July and October 2026 forms of the USB device | [gap 14](#where-this-library-had-to-fill-gaps) | [What changed since July](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#what-changed-since-july) |
| Wired or wireless at boot, the warm restart, the LEDs | [The cable](#overview-the-cable), [Reports and outputs](#overview-reports-and-outputs), [api.md, Modes](api.md#modes) | [docs/controller/09-system.md, The state machine](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md#the-state-machine) |
| Output reports `0x80`..`0x8A` | [Reports and outputs](#overview-reports-and-outputs), [api.md, Outputs](api.md#outputs) | [docs/controller/08-haptics.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/08-haptics.md) |
| Bluetooth LE: advertising, pairing, the HID service, the gate, reports, the transport choice (an opt-in build here) | [Bluetooth LE](#overview-bluetooth-le), [api.md, Wireless transports](api.md#wireless-transports-and-bluetooth-le) | [docs/controller/05-bluetooth.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/05-bluetooth.md) |
| The dock (not implemented here) | [The puck's end](#overview-the-pucks-end) | [docs/controller/06-dock.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/06-dock.md) |
| What the puck shows Steam (feature report pass-through, attribute cache) | [The puck's end](#overview-the-pucks-end) | [docs/puck/03-usb-and-hid.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/03-usb-and-hid.md) |

The puck's side as an open firmware, public: [Counterpuck](https://github.com/ded811/Counterpuck)
and its [wire-formats.md](https://github.com/ded811/Counterpuck/blob/main/docs/wire-formats.md).

## Overview: the radio link

- **Radio**: Nordic Enhanced ShockBurst, dynamic payloads up to 128 bytes, 2 Mbit/s on the BLE 2M
  PHY, 16-bit CRC, +6 dBm, selective auto-acknowledgement, fast ramp-up. The puck transmits; the
  controller is the receiver and says everything in **acknowledgement payloads**, so each reply
  rides with the acknowledgement of the puck's next packet.
- **Bonds**: a bond, one pairing, is 24 bytes: the puck ID, the controller ID and the puck's
  serial. It is valid when both IDs are non-zero. The controller keeps two, for two pucks; the puck
  keeps one record per slot, with the controller's serial in place of the puck's.
- **Pairing** (channel 2): the puck's pairing window (which Steam can open with the puck's vendor
  command `0xAD`) beacons a hello to address `91 A2 A7 93`. The controller answers `0xF0 {T,
  serial}`; the puck sends `0xE0 {puck ID, puck serial}` to address T; the controller confirms with
  `0xF4 {C, serial}`; both store `{puck ID, C}`.
- **Connecting** (channel 2, address `ibex`, prefix `0x10`): the puck broadcasts `0xE1 {puck ID,
  controller ID, channel, its address, a pipe prefix}` without acknowledgement. The controller with
  that bond moves to the private pipe and acknowledges the hello that follows 1 ms later. The real
  controller listens for 60 s, then turns its radio off; the library listens until it is stopped,
  unless `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` sets a limit.
- **Connected**: the puck opens with `0xE7 {0, 1}` (protocol version 1); the controller answers
  `0xF3 {1}` and once per connection its identity `0xF5`, 57 bytes: the product ID, the application
  and bootloader build times and the board revision (four bytes each), then the unit serial, the
  board serial and the git revision. Then the puck sends `0xE3` keep-alives about every 3 ms,
  carrying records `{length, type, data}`: SET feature (1), GET feature (3), output report (5). The
  controller answers `0xF1`: one SET result (2) or GET answer (4), the state report `0x42` as an
  input record (6), and any queued input reports, at most 127 bytes.
  A GET record is answered with the last SET's command, as on USB: its one data byte is not the
  opcode (Valve's puck leaves it unset), and the puck drops an answer whose opcode is not its SET's.
  Seen on hardware: with that byte taken as the opcode, Steam's bond read through Valve's puck
  failed ("Failed to read triton info").
- **Channels**: `0xE4` gives a new channel list (for version 1: the new channel plus 2 and 80). The
  controller waits 10 ms per turn on its current channel, then 13 ms on each alternate; 500 ms
  without a packet is a lost link, and it listens on channel 2 again.
- **The PC sleeps**: the puck sends `0xE5` (turn off); the controller answers `0xF2` to every packet
  until the puck stops. `0xE6` says the PC is asleep while the link stays up.

## Overview: the puck's end

What OpenSC2 counts on from Valve's puck (`28DE:1304`), which it does not implement itself:

- **Slots**: the puck serves up to four controllers, one slot each. Every slot has its own USB HID
  interface, with the same 372-byte report descriptor as the controller, and its own pairing
  record. The puck's USB device also has a serial port with a shell, and on boards with a dock a
  dock interface.
- **The pairing window**: opened with the puck's vendor command `0xAD` (on its feature report 2),
  60 s by default. The slot first forgets its old pairing, then beacons every 10 ms (the exchange:
  [above](#overview-the-radio-link)). Steam pairs a Steam Controller 2 over the cable instead, and
  its log shows it writing the slot's record itself (the command it uses for that was not
  captured).
- **Connecting**: each new connection gets a new pipe prefix and a data channel. The puck sends
  `0xE1` every 4 ms (after 250 tries every 100 ms), each followed 1 ms later by a hello on the data
  channel, until the controller acknowledges one.
- **Connected**: after `0xE7` the puck exchanges packets for up to 1 s until the identity `0xF5`
  arrives (a controller that sends none is served anyway), then makes one exchange about every
  3 ms. It retries on its channel for 10 ms, then on the alternates too; 500 ms without an
  acknowledgement ends the link, and the slot starts connecting again.
- **Channel choice**: a data channel is one of the even channels 4 to 78 (2404 to 2478 MHz), scored
  by its background noise, the slot's packet error rate on it and its distance from the current
  channel. A link that keeps needing retries, or that answers only on an alternate, is moved to a
  new channel with `0xE4`.
- **Suspend**: 2 s after the PC goes to sleep the puck tells its controller to turn off (`0xE5`). A
  controller that connects while the PC sleeps makes the puck wake the PC; if it stays asleep, the
  puck says so with `0xE6` and keeps the link.
- **What Steam sees**: a slot passes the controller's input reports `0x40` to `0x45` only at exactly
  their size, and keyboard reports only with Enter, Escape, Tab or the arrows and no modifiers.
  Output reports and feature report 1 go to the controller as records. A GET of feature report 1
  works in two steps: the first one sends the request over the radio and fails ("try again"), and
  a later one returns the answer once it has arrived; an answer to another command than the SET's,
  or none within 1 s, ends the exchange. Since October 2026 the puck answers a GET after a SET of
  `0x83` or `0xAE` itself, from the identity packet (the SET still reaches the controller). Input
  report `0x79` gives the link state when it changes (1 no link, 2 connected, 3 just paired), and
  `0x7B` the link quality: every 500 ms while the link is healthy, and with a reason when it
  degrades, changes channel or drops. When a link drops, the puck sends an empty keyboard and mouse
  report, so nothing stays held.
- **The dock**: the puck is also a charging dock. A controller resting on it gets power through
  three pogo pads, and the puck talks to it over a slow serial link on the same contacts. OpenSC2
  does not implement the dock.

## Overview: the cable

From the research documentation's USB page (its sections on Steam's wired session, pairing over
the cable and the reboot into wireless mode) and this project's own tests with Steam and Valve's
puck:

- **When**: only at a boot with USB power and without the warm-restart mark. The real controller
  then plays its chime, waits for STEAM to be released and attaches; every way out of wired mode is
  a reboot or a power-off, so the device attaches at most once per boot. A controller whose radio
  link is up stays wireless on the cable (it only charges), unless STEAM is held while plugging in.
- **The device**: `28DE:1302`, strings "Valve Software", "Steam Controller" and the unit serial (the
  same serial as vendor command `0xAE` tag 1 and the radio identity record); one configuration, bus
  powered, remote wakeup, 500 mA; one HID interface without boot protocol, interrupt IN `0x81` and OUT
  `0x01`, 64 bytes every 1 ms; a 372-byte report descriptor with a mouse (`0x40`), a keyboard
  (`0x41`) and the vendor collection (everything else), which Windows shows as `Col01` to `Col03`.
  Steam opens `Col03`.
- **Reports**: `0x42` (the same 53 bytes as through the puck, one sequence counter for every link)
  every 132 ticks of the 32,768 Hz clock (4.03 ms) from the moment the host sets the configuration,
  whether or not an application has the device open, through eight buffers (in the library a state
  report that finds none free is dropped, so nothing waits for the host). `0x43` every 3.5 s. The
  mouse and keyboard reports of lizard mode. Output reports arrive on interrupt OUT.
- **Commands**: feature report 1. A 64-byte `SET_REPORT` carries `01 <opcode> <n> <payload>` and runs
  the command; if it succeeds it becomes the stored request, which `GET_REPORT` answers (64 bytes in
  the July form, 63 in the library's October form). Each link keeps its own stored request. A
  refused command, an unknown opcode, a SET of the wrong length and a GET of a command without an
  answer stall the control request.
- **Steam's session**: identity reads (`0x83`, `0xAE`), lizard mode off (`0x81`, repeated), its
  settings (`0x87`), the LED, and two zero pulses that stop the trackpad haptics.
- **Keeping lizard mode off through the puck** (seen with this board and the test suite): once
  logged in, Steam writes setting 9 = 0 with `0x87` every 3.0 s (the period SDL uses too) and sends
  no `0x81` meanwhile. Before its logon it wrote 9 = 0 once and then nothing, so the 10 s watchdog
  turned lizard mode back on, as on the real controller.
- **Pairing over the cable**, with a puck on the same PC: Steam reads the stored transport and the
  current bond (`0xED`), writes the puck's slot on the puck, then writes the controller's new 24-byte
  bond (`0xEE esb/bond`) and saves it (`0xEF esb/bond`), reads it back, reads the identity, and sends
  `0x95` with `0xA427AF52`: "reboot into wireless mode". Both IDs are new at every pairing.
- **Reboot into wireless mode**: the controller acknowledges; 20 ms later it sends one all-zero
  `0x42`, waits 10 ms, leaves the bus and reboots with the warm-restart mark set. At the next boot
  the mark (read and cleared at once) makes it start its radio with the stored bond although USB
  power is there, and the puck, already calling the new controller ID, connects within a second.

## Overview: the vendor commands

Steam's command channel is feature report 1, with the same commands on every link. A command is
`{opcode, length, payload}`: a SET runs it, and a GET reads the answer to the last SET on the same
link (each link keeps its own; before the first SET a GET answers `0x83`). How the cable and the
puck's records carry them is above. What Steam was seen sending:

| Opcode | Does | When Steam sends it |
| --- | --- | --- |
| `0x83` | reads six attributes: the product ID `0x1302`, capabilities, the bootloader and application build times, the hardware ID, the report interval | to identify the controller: on the cable, and through the puck (which since October 2026 answers it itself) |
| `0xAE` | reads a string: tag 1 the unit serial (0 and 4 the board serial, 3 the git revision) | the same |
| `0x81` | turns lizard mode off | on the cable, at the start of a session and again later |
| `0x87` | writes controller settings, `{number, value}` each | on the cable in a few groups (the IMU off, both pad click thresholds to 100 %, calibrated sticks, the LED brightness to 100, ...); through the puck setting 9 = 0 every 3 s |
| `0xDC` | stores the power-on chime level | on the cable |
| `0xE2` | persists calibration; Steam's form (side 1, mask `0x20`) saves nothing and is acknowledged | on the cable |
| `0xC1` | nothing: the controller does not know it and stalls it, and Steam carries on | on the cable |
| `0xED` | reads a setting by name: `user/wireless_transport`, `esb/bond` | when it pairs the controller over the cable |
| `0xEE`, `0xEF` | sets, then saves, `esb/bond`: the new 24-byte bond | the same |
| `0x95` with `0xA427AF52` | reboots into wireless mode | at the end of that pairing |

**Controller settings** are 86 numbered 16-bit values, in RAM only and back to their defaults at
every boot. `0x87` writes any number of them (each clamped to its range where the range is known),
`0x89` reads one, `0x8E` restores the defaults. Setting 9 is lizard mode, 37 the LED colour
override, 45 the LED brightness, 48 the IMU stream and 71 the Steam watchdog.

The controller also takes `0x90`, and `0x95` with `0xECAABAC0`, as a request to reboot into its
bootloader, for Steam's firmware update; the library hands those to the application. What the
library answers to each command, and every setting it knows: [api.md](api.md#vendor-commands).

## Overview: lizard mode

Until Steam takes over, a Steam Controller 2 also works as a plain mouse and keyboard, so that it is
usable without Steam. OpenSC2 does the same, on every link:

- **Mouse** (`0x40`): the right trackpad moves the pointer and the left one scrolls (up and down as
  the wheel, left and right as horizontal pan); the right trigger or a right-pad click is the left
  button, the left trigger the right one.
- **Keyboard** (`0x41`): A is Enter, B and VIEW are Escape, the D-pad gives the arrow keys and MENU
  is Tab; with more than six keys held the report carries the roll-over code. These are the only
  keys the puck lets through.
- **On** at every boot (setting 9 = 1). **Off** with `0x81` or with setting 9 = 0; the first `0x81`
  sends an empty keyboard report and then an all-zero mouse report, so nothing stays held.
- **The Steam watchdog**: unless setting 71 is 0, the host has to repeat one of those within 10 s,
  or lizard mode comes back and the settings return to their defaults. How Steam keeps it off
  through the puck: [the cable](#overview-the-cable).
- Not reproduced: the real controller's smoothing, its glide after a flick and its haptic ticks
  ([gap 11](#where-this-library-had-to-fill-gaps)). Every rule, with its scales:
  [api.md](api.md#lizard-mode).

## Overview: reports and outputs

- **Input reports**, the same on every link: `0x42`, the state (53 bytes after the ID: a sequence
  number, the button word, both triggers, both sticks, both trackpads with their pressure, and the
  IMU's timestamp, acceleration, angular rate and orientation; [api.md](api.md#inputs) has every
  offset and button bit); `0x43`, the battery, every 3.5 s; `0x44`, the haptic stream status;
  `0x40` and `0x41` in lizard mode.
- **Output reports**: `0x80` rumble (a level and a speed for each side; it lasts 500 ms, so the
  host repeats it), `0x81` a pulse train, `0x82` a stop or one of two clicks, `0x83` a tone, `0x84`
  a frequency sweep, `0x85` one of the built-in haptic scripts, `0x86` to `0x89` PCM streamed to
  the actuators (`0x86` starts or stops it and sets the format, the others carry the samples);
  `0x8A` does nothing. Most of them start with a target byte that picks the pads or the grips. The
  library decodes each one for the application ([api.md](api.md#outputs)).
- **The LED**: Steam sets its brightness (setting 45) and can override its colour (`0xC5`, shown
  while setting 37 is on); the library hands both to the application. The real controller's
  status LED shows its mode by colour: green while wired, white with a little blue on the puck's
  link, amber on the dock.

## Overview: Bluetooth LE

The controller can also connect straight to a PC, phone or console over Bluetooth LE, without the
puck. OpenSC2 has it as an opt-in build (`CONFIG_OPENSC2_BLE`), built and **not yet tested on
hardware**; [api.md](api.md#the-bluetooth-build) has every detail and what is not built yet.

- **One transport at a time**: the stored `user/wireless_transport` decides (0 and 1 Bluetooth slots
  1 and 2, 2 and 3 the puck with bond 1 or 2); October saves a new choice only once its link is up.
- **Advertising**: "Steam Ctrl (BT) " and the unit serial; connectable legacy advertising every 100
  to 150 ms; flags, the gamepad appearance (`0x03C4`), the HID and battery service UUIDs, the TX
  power and manufacturer data of company ID `0x055D` with no payload; the name in the scan response.
  Without a bond the slot is open to any host (pairing mode); with one, advertising is directed to
  its host (1.28 s), then allow-listed for it. Every start of advertising arms a 60 s window, after
  which wireless goes off.
- **Identities and pairing**: Just Works, LE Secure Connections only, bonding; every HID value needs
  encryption. Each slot has two identities (1 and 2, 3 and 4) with addresses of their own; a
  pairing request resets the slot's other identity and advertises on it, so the old host keeps its
  bond until a new one has paired, and each pairing appears to hosts as a new device.
- **Disconnects**: the host ending the link (`0x13`) or powering off (`0x15`) turns wireless off;
  any other loss advertises again 500 ms later on the same identity.
- **Services**: HID over GATT with the USB report map byte for byte (inputs `0x40` to `0x45`,
  outputs `0x80` to `0x89`, feature report 1); Device Information with PnP ID `28DE:1303`, model
  "Steam Controller", an empty manufacturer name, the unit serial and the build time; Battery; and
  Valve's vendor service (`100F6Cxx-1735-4313-B402-38567131E5F3`), the same reports as plain
  characteristics, which SDL's Android and Apple back ends use. Vendor command `0x83` still reports
  product ID `0x1302`.
- **The gate**: no report until the link is encrypted and its ATT MTU is 64 or more; then Bluetooth
  is the active transport and attribute 11 of `0x83` is the connection interval in µs.
- **Reports**: one state report per connection event, as `0x45` (the first 45 bytes of `0x42`, the
  quaternion cut); lizard mode, battery and haptic status on their own characteristics.
- **Commands and outputs**: feature report 1 is the command channel (refusals are ATT errors);
  output reports go to the same dispatcher as on every other link.

## Where this library had to fill gaps

The research documentation describes one real controller firmware. Where it leaves something
open, the library does what should work with Steam and the puck, and says so here:

1. **Over-the-air pairing.** Pairing over the air (the puck's pairing window) is implemented but
   untested with Steam. Steam pairs controllers over a wired connection, and pairing over the USB-C
   cable is the tested way (the sequence in brief: [the cable](#overview-the-cable); in full:
   [the documented sequence](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#pairing-a-controller-with-a-puck-over-the-cable)).
   The documented controller firmware contains an over-the-air pairing routine that nothing in it
   ever starts
   ([pairing is unreachable](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/04-radio.md#pairing-is-unreachable)),
   but the puck still has its pairing window, so the library implements the documented exchange
   from the controller's side, with the puck's documented timings (and keeps its confirmation queued
   if the puck repeats its offer). A wireless board answers an open window when its pairing button
   is pressed; with [Counterpuck](https://github.com/ded811/Counterpuck) as the puck, its shell
   command `pair 0` opens a window without Steam (also untested with OpenSC2).
2. **Rumble report `0x80`** (closed). The research documentation and SDL's `MsgHapticRumble` agree:
   `[0]` the type, `[1..2]` the level, `[3..4]` the left speed, `[6..7]` the right speed. The
   library also decodes the gain bytes `[5]` and `[8]`, which the July 2026 firmware used.
3. **Report `0x43` charge state.** The library sends the application's charge state as given, in
   SDL's `EChargeState` numbering (the type SDL gives this byte; SDL reads 1 as on battery, 2 as
   charging, 4 as charged). The real controller adds a flag bit `0x80` to it while its charger has
   given up on a supply that does not charge the battery (it tries again 60 s later); the library
   has no charger logic of its own and never adds it.
4. **Answer layouts not given byte for byte**: `0xF2` selector 2 (uptime u32, then the reason as a
   u32), `0xA1` (reset cause u32, device ID, zeros; the reset-cause flags are Zephyr's, not the
   original's), and `0xAE` (the tag, then the string, 20 bytes in all, as Counterpuck answers for
   the puck).
5. **Which commands Steam sends through the puck** is not fully documented. The library answers the
   identity, settings, LED, user-store and settings-by-name commands and passes the rest to the
   application. On the cable the commands Steam sends are known
   ([the vendor commands](#overview-the-vendor-commands);
   [Steam's wired session](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it)),
   and each gets the real answer or the real STALL (Steam's `0xC1` is stalled, as by the real
   controller).
6. **Controller settings** have documented defaults; `0x87` clamps each value to its range. The
   research documentation gives the ranges of settings 36 and 68; the ranges of 9, 11, 12, 14, 45,
   70 and 71 come from the same authors' study of the controller's firmware (not in that
   documentation yet). These are the ranges the library knows ([api.md](api.md#controller-settings)
   lists them). Other settings are stored as sent. Settings 48, 84 and 85 are stored too (the real
   controller hands them to its IMU and `0x89` answers their defaults); the library answers what
   was written.
7. **The quaternion's W** (offset 45): one page of the research documentation calls it always 0,
   another the quaternion's W (since October). The library sends whatever the application
   provides; the fusion helper fills W.
8. **Payload limit**: the controller's payloads are "within 127 bytes", the puck accepts 128; the
   library stays at 127.
9. **The October firmware's change to the ESB driver** (the processor, not the radio, starts each
   reception and acknowledgement) is not reproduced: stock nRF Connect SDK ESB is used.
10. **Board revision.** What Steam does with the hardware ID is not documented; the default is
    `0x48` (72), what a real controller reports and the value Steam has been seen working with
    ("Controller Info: HWID: 72"); `0x45` and `0x49`..`0x4B`, the other values real controllers
    accept, stay selectable.
11. **Lizard mode** follows the documented key map and mouse mapping, the release reports when it
    turns off, and the 10 s Steam watchdog. The key-slot rule, the scales (128 pad units per pointer
    count, 1024 per scroll step, pad up scrolls with a negative wheel value) and the rule that a
    mouse report goes out every frame while anything is non-zero come from the same authors' study
    of the controller's firmware (not in that documentation yet). Not reproduced: the touch-down
    stabiliser, the motion average, the glide after a flick (settings 11 and 12) and the haptic
    ticks (setting 14). The keyboard is evaluated once per report frame.
12. **A bond written with `0xA2`** takes effect at once here; the documented controller only reads
    it at the next boot.
13. **The report descriptor's bytes.** Each detail has one source:

    | Detail | Source |
    | --- | --- |
    | 372 bytes; the three collections in this order: mouse, keyboard, vendor; every report's ID, direction and size; the vendor collection's item rule (every field is a byte with a usage equal to its report ID) | the research documentation, [The report descriptor](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-report-descriptor) |
    | the fields of the mouse and keyboard reports; the vendor fields' range (8 bits, 0..255); usage 1 for both feature reports | Counterpuck's [wire-formats.md](https://github.com/ded811/Counterpuck/blob/main/docs/wire-formats.md#usb-hid-reports) |
    | the sizes of the mouse and keyboard collections (67 and 41 bytes); the form of the vendor items (every input and output report states its ID, logical minimum 0, a two-byte logical maximum of 255, report size 8, its count, usage and main item; the feature reports only ID, count, usage and main item); the order of the vendor reports (inputs `0x42`, `0x44`, `0x79`, `0x43`, `0x7B`, `0x45`; outputs `0x80` to `0x87`, then `0x89`, `0x88`; features `0x01`, `0x02`) | the same authors' study of the controller's firmware. The research documentation gives the two sizes too, for the puck's slot interfaces ([docs/puck/03-usb-and-hid.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/03-usb-and-hid.md#report-descriptors)), whose descriptor it calls the controller's byte for byte; the form of the items and the order of the reports are not in it yet |

    None of them gives every byte of the mouse and keyboard collections: the library builds them in
    the usual layouts at those sizes, and two item choices that give the sizes (a repeated Report
    Count before the mouse's horizontal pan, a restated Logical Minimum in the keyboard) are
    guesses. Windows and Steam accept the result in the tested setup
    ([What works](../README.md#what-works)); no host is known to compare the bytes.
14. **Two forms of the device.** The library has both:
    `CONFIG_OPENSC2_USB_PROFILE_JULY` (the default), the July 2026 controller firmware's form: USB
    2.00, class `EF/02/01`, release 3.07, each GET answered with 64 bytes; and
    `CONFIG_OPENSC2_USB_PROFILE_OCTOBER`, the October 2026 firmware's form: USB 2.01 with a USB 2.0
    extension BOS descriptor, class `00/00/00`, release 4.04, 63-byte GET replies (both described
    in the research documentation:
    [The device](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-device),
    [What changed since July](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#what-changed-since-july)).
    The July form is the one Steam was seen pairing over the cable with a real controller, and the
    one tested with this library. Whether Steam treats the two differently is untested.
15. **Standard requests** are answered by Zephyr's USB stack and HID class (`SET_IDLE` stored, then
    stalled; `SET_PROTOCOL` and `GET_PROTOCOL` stalled), the stack the October 2026 firmware is built
    on too. `SET_REPORT` of an output report on the control endpoint and feature report 2 stall, as
    with the October firmware.
16. **Suspend.** The October firmware does not react to a suspended bus; neither does the library,
    except that it does not build state reports while the bus is suspended (Zephyr's HID class would
    keep them waiting for the host), so those use no sequence number. Whether the bus is suspended
    is always the USB stack's own state, which a resume and a bus reset both end. July's STEAM-press
    remote wakeup is an option (`CONFIG_OPENSC2_USB_REMOTE_WAKEUP`).
17. **USB power that appears later.** The real controller reacts to its charger's power events. The
    library checks the chip's own USB power detection at boot and, in wireless mode, every 100 ms;
    with the link down, or STEAM held, it asks the application to reboot, which brings the board back
    wired (only while the policy is auto).
18. **STEAM while waiting for a host.** The real controller attaches only once STEAM is released and
    reboots into wireless mode when STEAM is pressed before a host configures it. The library
    attaches at once and counts a STEAM press only after it has seen STEAM released, so a button
    held through power-up does not count.
19. **Haptic stream status after Steam's zero pulses.** The real controller answers each pair with
    two `0x44` reports (right pad, then left pad, flag `0x02`); Steam does not appear to need them
    ([Steam's wired session](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it)).
    The library does not send them; an application can (`opensc2_send_haptic_status()`).
20. **Identity on the cable.** The USB strings are the real ones by default and the serial string is
    the identity's unit serial, kept equal to `0xAE` tag 1 and the radio identity record. A
    development board reports its own "OSC..." serial, which no real controller has. With the
    default identity (an "OSC..." serial, hardware ID `0x48`, no bootloader build time) Steam paired
    the board in the tested setup ([What works](../README.md#what-works)); other values are
    untested. A build time at or before `0x6A18D057` makes Steam's updater force an update, and one
    older than the controller firmware Steam ships makes Steam offer one
    ([troubleshooting](troubleshooting.md#steam-asks-to-update-the-controller)). The product string
    is the identity's product name, the real "Steam Controller" unless a name is stored on the
    board; a stored one makes the wired device differ from the real controller.
21. **The cable pulled while wired.** The real controller shuts down. The library waits 0.2 s for
    USB power to come back (a shorter dip changes nothing), then asks the application to power off
    or reboot (`OPENSC2_POWER_USB_UNPLUGGED`); without a callback it reboots, which comes back
    wireless on a battery, and the blank canvas does the same, having no power-off of its own. A
    board without a battery has lost its power anyway.
22. **The wireless transport** (`user/wireless_transport`). The library keeps the real controller's
    four values and October's rule that a choice is saved once its link is up. Not documented, and
    the library's choices: a host's `0xEE` of a value between the puck and Bluetooth is not run at
    once (saved with `0xEF`, it counts from the next boot; a new puck slot counts at once, as
    before); without Bluetooth, `0xEE` with 0 or 1 is taken and changes nothing; a board that stored
    only `opensc2/slot` (older firmware) starts on that puck slot. What Steam does when it reads 0
    or 1 during a cable pairing is not known.
23. **Bluetooth LE** (an opt-in build; built, not yet tested on hardware). Where it differs from the
    documented controller, or the documentation is silent:

    | Point | The documented controller | OpenSC2 |
    | --- | --- | --- |
    | Sharing the radio with the puck's ESB | MPSL is shut down when the puck link stops, and started again by Bluetooth | MPSL stays initialised (in this build its clock driver also serves the USB console's crystal); ESB takes the radio interrupt while it runs and the library hands it back to MPSL; switching between the two within one boot is supported both ways |
    | The advertising window | 60 s | 60 s by default, a Kconfig option (`CONFIG_OPENSC2_BLE_ADV_WINDOW_S`; 0: advertise until a host connects) |
    | "Wireless off" | on battery the controller turns off; on USB power wireless goes off | the library stops advertising and asks the application (`OPENSC2_POWER_WIRELESS_OFF`); the blank canvas stays on, as on USB power, until a button or `opensc2 start` |
    | Turning wireless off while Bluetooth advertises, or before the gate | Bluetooth is left advertising until its window ends (inferred, not observed) | `opensc2_stop()` stops it in any state |
    | The bond rule | a host that pairs on an identity that had no bond at the start deletes the bond on the slot's other identity, once the link reaches security level 2 | the same, once the host has bonded: a host that pairs without bonding leaves the slot's host alone |
    | A pairing request while a host is connected on the slot | not documented (a chord asks for it while wireless is off) | the link ends (the zero report, `0x13`), then the slot's other identity advertises in pairing mode |
    | The subscription flags | a quirk can send state reports to the vendor characteristic whatever the host subscribed to (after an odd puck disconnect reason in the same boot; inferred, not observed) | not copied: a report goes where the host enabled notifications, the vendor characteristic first |
    | Haptic cues | "pairing" at each start of pairing mode, "paired" when a new host has bonded | none (the library has no haptic output of its own; an application can play them from `link_changed`) |
    | LED patterns | blue: the pairing double flash, the searching fade, connected steady or fading on external power | the blank canvas shows them in blue on an RGB LED; a one-colour LED shows connected steady |
    | The ATT MTU | the last value is remembered across connections | per connection, as the specification has it (hosts raise it at every connection) |
    | Reading feature report 1 | 64 bytes, the answer then left-overs; a read blob gets the start again | the 63 bytes the report map declares; a read blob gets the rest of the same answer |
    | No connection-event trigger | no state reports (inferred) | a timer at the connection interval paces them |
    | Report pacing | the slot less the worst processing time of the last 65 frames | the slot less a fixed 500 µs (`CONFIG_OPENSC2_BLE_REPORT_LEAD_US`) |
    | A report that cannot be sent | logged as an error with its ID | logged at debug level (at the default MTU every state report would be) |

    Valve's Bluetooth company ID and the PnP ID `28DE:1303` are advertised for interoperability, as
    the USB IDs are, and the vendor service uses Valve's UUIDs for the same reason
    ([NOTICE.md](../NOTICE.md)); the IDs are Kconfig options and the vendor service can be left
    out (`CONFIG_OPENSC2_BLE_VENDOR_SERVICE`).
24. **Attribute 11 on the dock** is documented as 64000 µs; the library answers that for its dock
    link, which has no code yet and is never active.

## What this library adds for development boards

These have no counterpart on the real controller, and are there because a development board has no
case, no battery gauge and no other way to tell what happened:

- **Escapes into wireless mode**: the pairing button held at power-up (this boot only), the pairing
  button pressed before a host configured the board, or held for 3 s while wired (acted on when it
  is released), an optional mode switch, and a stored policy (`opensc2 mode wireless`: never
  wired). `opensc2 reboot wired` comes back wired once. A wired boot whose USB device cannot be set
  up goes on wireless.
- **The USB session log**: a ring in RAM that a reboot keeps, with every control request, its result
  and its timing, so a wired session can be read afterwards in wireless mode. Bond values and serials
  are left out.
- **A debug serial port in wireless mode** (non-Valve IDs `2FE3:0004`), where the real controller
  shows nothing on USB; optionally also while wired, as a second interface (a composite device,
  unlike the real controller; for debugging only).
- **LED patterns** for a single LED: a fast blink while no host has configured the board, on with a
  short dark blink every 2 s once it has. An RGB LED shows the real colours (green on a PC, amber on
  a charger).
- **A product name of the board's own** (`opensc2 id set name`), optional: the real controller's
  "Steam Controller" by default.
- **A charger test build** (`CONFIG_OPENSC2_NO_HOST_WIRELESS_MS`, off by default; the blank canvas's
  `test-charger.conf`): a testing aid for a board with no buttons, which goes wireless when no host
  sends a USB request within 5 s of a wired boot. The real controller waits for a host or for STEAM;
  this differs from it on purpose and is never part of a default build.
- **The hardware test hooks** (`CONFIG_OPENSC2_TEST_HOOKS`): input injection and output capture for
  the PC test suite, inert until it arms them ([api.md](api.md#hardware-test-hooks)).

Back to the [documentation map](README.md).
