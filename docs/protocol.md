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
| The inputs from sensor to report: sticks, triggers, trackpads, stick touch and grips, the IMU; the calibration keys and commands | [gaps 29, 31 to 33](#where-this-library-had-to-fill-gaps), [api.md, Calibration and raw inputs](api.md#calibration-and-raw-inputs) | [docs/controller/07-input-and-sensors.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md) |
| The controller settings | [The vendor commands](#overview-the-vendor-commands), [api.md, Controller settings](api.md#controller-settings) | [Controller settings](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#controller-settings) |
| Lizard mode | [Lizard mode](#overview-lizard-mode) | [Lizard mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#lizard-mode) |
| Steam's wired session | [The cable](#overview-the-cable), [The vendor commands](#overview-the-vendor-commands) | [The wired session as Steam runs it](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it) |
| Pairing over the cable, and the reboot into wireless mode | [The cable](#overview-the-cable) | [Pairing a controller with a puck over the cable](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#pairing-a-controller-with-a-puck-over-the-cable), [Reboot into wireless mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#reboot-into-wireless-mode) |
| The July and October 2026 forms of the USB device | [gap 14](#where-this-library-had-to-fill-gaps) | [What changed since July](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#what-changed-since-july) |
| Wired or wireless at boot, the warm restart, the LEDs | [The cable](#overview-the-cable), [Reports and outputs](#overview-reports-and-outputs), [api.md, Modes](api.md#modes) | [docs/controller/09-system.md, The state machine](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md#the-state-machine) |
| The battery voltage and charge level, what erases the stored settings | [api.md, Battery helpers](api.md#battery-helpers), [gap 31](#where-this-library-had-to-fill-gaps) | [Power and charging](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md#power-and-charging), [What erases what](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md#what-erases-what) |
| Power, the chords, the keylock, the idle timeout, low battery, shutdown, the status LED's modes and patterns, the cues | [Power, chords and the status LED](#overview-power-chords-and-the-status-led), [api.md, Power and the system state machine](api.md#power-and-the-system-state-machine), [api.md, Status LED](api.md#status-led), [gap 34](#where-this-library-had-to-fill-gaps) | [docs/controller/09-system.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md) |
| Output reports `0x80`..`0x8A` | [Reports and outputs](#overview-reports-and-outputs), [api.md, Outputs](api.md#outputs) | [docs/controller/08-haptics.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/08-haptics.md) |
| Bluetooth LE: advertising, pairing, the HID service, the gate, reports, the transport choice (an opt-in build here) | [Bluetooth LE](#overview-bluetooth-le), [api.md, Wireless transports](api.md#wireless-transports-and-bluetooth-le) | [docs/controller/05-bluetooth.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/05-bluetooth.md) |
| The dock: the frames over the puck's charging pads, the pilot, dock power, the docked states (the protocol here; its UART build has never run on hardware) | [The dock](#overview-the-dock), [api.md, The dock](api.md#the-dock) | [docs/controller/06-dock.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/06-dock.md), [docs/puck/05-dock.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/05-dock.md) |
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
  controller listens for 60 s, then turns its radio off (and waits 10 s for a bond when the
  selected slot is empty); the library listens until it is stopped, unless
  `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` (and `CONFIG_OPENSC2_NO_BOND_TIMEOUT_S`) set a limit, as they do
  in a battery build.
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
  has the controller's end of that protocol ([The dock](#overview-the-dock)); its UART build has
  never run on hardware.

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
- **The motion** as the real controller's: a new touch is held still until the thumb really
  moves, the motion is averaged over 8 frames, a flick keeps gliding (settings 11 and 12), and
  moving across a pad makes haptic ticks (setting 14), which the library hands to the application
  as haptic events. A few details are choices of this library
  ([gap 11](#where-this-library-had-to-fill-gaps)). Every rule, with its scales:
  [api.md](api.md#lizard-mode).

## Overview: reports and outputs

- **Input reports**, the same on every link: `0x42`, the state (53 bytes after the ID: a sequence
  number, the button word, both triggers, both sticks, both trackpads with their pressure, and the
  IMU's timestamp, acceleration, angular rate and orientation; [api.md](api.md#inputs) has every
  offset and button bit); `0x43`, the battery, every 3.5 s; `0x44`, the haptic stream status;
  `0x40` and `0x41` in lizard mode. The library applies the real controller's rules to every `0x42`
  ([api.md](api.md#the-rules-on-every-report)): a stick deflected beyond 12000 counts as touched, a
  pad is blank while the thumb rests on its stick (setting 82), no trigger bit with raw trigger
  output (setting 62), the trigger bits from setting 68, and optionally the pads' clicks from their
  pressure (settings 52 and 53).
- **Output reports**: `0x80` rumble (a level and a speed for each side; it lasts 500 ms, so the
  host repeats it), `0x81` a pulse train, `0x82` a stop or one of two clicks, `0x83` a tone, `0x84`
  a frequency sweep, `0x85` one of the built-in haptic scripts, `0x86` to `0x89` PCM streamed to
  the actuators (`0x86` starts or stops it and sets the format, the others carry the samples);
  `0x8A` does nothing. Most of them start with a target byte that picks the pads or the grips. The
  library decodes each one for the application ([api.md](api.md#outputs)) and, with its optional
  haptics synthesiser, also plays them as the real controller's two sequencers do: sample blocks for
  the four channels, outputs the application provides, the streams' flow control `0x44`
  ([api.md](api.md#the-haptics-synthesiser); gap 35).
- **The LED**: Steam sets its brightness (setting 45) and can override its colour (`0xC5`, shown
  while setting 37 is on); the library hands both to the application. The real controller's
  status LED shows its mode by colour: green while wired, white with a little blue on the puck's
  link, amber on the dock. The library selects that mode too ([below](#overview-power-chords-and-the-status-led)).

## Overview: power, chords and the status LED

From the research documentation's page on the controller's system state machine. OpenSC2 runs the
same machine (`CONFIG_OPENSC2_SYSTEM`, on by default; built, not yet tested on hardware); every
rule, state and timing is in [api.md](api.md#power-and-the-system-state-machine):

- **Power-on**: a wired boot plays the power-on chime and waits for a host; a wireless boot on USB
  power (after the warm restart) plays the stored transport's cue and starts its link; on the dock
  the radio stays off (after a warm restart the stored link starts); on battery the board first
  checks its level (0 %: off; 2 % or less: off with the low-battery signal) and the keylock (off),
  then plays the stored transport's cue and waits 1 s for USB power or the dock before it starts
  the link.
- **STEAM**: pressed 20 ms it is a press, held 7 s it turns the board off on battery, or switches
  wireless off on USB power or the dock (the amber LED); on USB power a press then reboots the
  board, which comes back wired (on the dock the chords choose a link again instead).
- **The transport chords**: A + right bumper the main puck, A + left bumper the second, B + right
  or left bumper Bluetooth slot 1 or 2, STEAM alone the stored transport. Held at a battery power-on
  (or pressed on the dock with the radio off) a chord chooses its transport; held 4 s it also asks
  for pairing. **The keylock** (STEAM + L4 + L5 + R4 + R5 held 3 s) makes the next battery
  power-on turn straight off.
- **Turning off**: the idle timeout (setting 50, 15 minutes), 0 % battery, the turn-off command
  (`0x9F`), a link that gives up (60 s listening, 10 s without a bond) and the puck's turn-off end
  battery operation with the power-off chime. On USB power the turn-off command, a link that gives
  up and the puck's turn-off only switch wireless off, and the idle timeout and the level do
  nothing. Setting 77 makes the shutdown reboot instead.
- **The status LED**: blue for Bluetooth, white with a little blue for the puck, green while wired,
  amber on USB power without a host or with wireless off, red flashes for low battery, white
  flashes for the keylock, each with its pattern (searching, connected, pairing, breathing on
  external power, a dimmer display once charged).
- **Cues**: the power-on and power-off chimes, a cue per transport, pairing and paired, and one when
  the puck says the PC is asleep; the chime level Steam stores (`0xDC`) sets their gain or mutes
  them.

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

## Overview: the dock

From the research documentation's dock pages (the controller's and the puck's); nothing of it has
been seen on a wire. The puck's three pogo pads are `+` (a rail at about USB voltage the puck
switches on itself), `−` (ground) and `P`, the **pilot**: an analog level that tells the puck a
controller is there, and (inferred) the line of a half-duplex serial link too.

- **The puck's side**: idle, it keeps `+` off and reads the pilot every 100 ms; once the pilot has
  been inside its window for 200 readings in a row, it switches `+` on, checks both voltages every
  5 ms, and after 1 s sends a probe. An answer means attached (the puck reports it to the PC as
  input report `0x79` = 2 on its fifth HID interface); no answer is a fault (`+` off for 10 s). While
  attached it relays the PC's feature report 1 and, with nothing pending for 64 ms, polls the
  controller's input; a failed poll that a probe 10 ms later does not forgive sends it back to idle.
  Each exchange: send, throw away the first frame back within 100 ms (its own echo), wait up to
  64 ms for the answer, wait 10 ms; one retry. The puck answers Steam's `0x83` and `0xAE` for a
  docked controller itself, from the probe's record (attribute 11 = 64000).
- **The link**: 115200 baud, 8N1, no flow control. Every message is a 73-byte frame: the start byte
  (`0x5A` from the puck, `0xAC` from the controller, checked by nobody), the request type, two unused
  bytes, the result (s32), 63 payload bytes and a CRC-16/KERMIT of bytes 0 to 70, low byte first.
  The receiver runs the CRC over all 73 bytes and accepts 0.
- **The requests**: 0 probe (the answer: the 57-byte identity record, as radio packet `0xF5`); 1 GET
  of a vendor command (`{opcode, length, data}` of the command last set on the dock, result 63 or a
  negative error); 2 SET (`{opcode, length, data}` through the same command channel as every link,
  result 0 or a negative error); 3 GET input (the latest state report `0x42`, its 53 bytes after the
  ID). The dock keeps its own stored request, `0x83` after a boot. `0x81` and `0x85` from the dock
  are acknowledged and do nothing.
- **The controller's session**: it checks the pilot (three readings 3 ms apart, present when two lie
  inside 2000 to 3300 mV at its pin), then serves one request at a time with no time limit for the
  first byte; a 64 ms silence inside a frame, a bad length or CRC, or a receive error ends the
  session and it checks the pilot again. It arms its receiver only after its answer has left.
- **Input over the dock**: with another link up, the dock gets a copy of that link's latest state
  report; with the dock alone, each GET input takes one sample. Nothing else goes to the dock (no
  lizard mode, no `0x43` or `0x44`), and nothing comes back but vendor commands: no output reports.
- **Dock power**: the controller tells the dock from USB power by the pilot at the moment the supply
  appears; its state machine then has docked states: with the radio off (charging; a transport
  chord or STEAM starts the link), with the link up (turn-off goes back to radio-off with the
  power-off chime), and lifted off (5 s, then off). A warm reboot on the dock starts the stored
  link again.

What OpenSC2 does with it, and what it leaves out: [api.md, The dock](api.md#the-dock) and gaps 36
to 41 below.

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
   has no charger logic of its own and never adds it, but an application may
   (`OPENSC2_CHARGE_GAVE_UP`), and then, as on the real controller, the power rules and the LED are
   told 100 % while the level is above 0.
4. **Answer layouts not given byte for byte**: `0xF2` selector 2 (uptime u32, then the reason as a
   u32) and `0xAE` (the tag, then the string, 20 bytes in all, as Counterpuck answers for the
   puck). `0xA1` follows the documentation byte for byte (18 bytes; the real controller's flag
   encoding of Zephyr's reset causes); whether the UF2 bootloader, which runs before the
   application at every reset, leaves the chip's reset-reason register alone has not been
   checked.
5. **Which commands Steam sends through the puck** is not fully documented. The library answers the
   identity, settings, LED, user-store and settings-by-name commands and passes the rest to the
   application. On the cable the commands Steam sends are known
   ([the vendor commands](#overview-the-vendor-commands);
   [Steam's wired session](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it)),
   and each gets the real answer or the real STALL (Steam's `0xC1` is stalled, as by the real
   controller).
6. **Controller settings** follow the documentation: every one of the 86 has its documented default
   and range, and `0x87` clamps each value to its range ([api.md](api.md#controller-settings)),
   also for the 49 settings nothing reads (0.2.0 knew nine ranges and stored the rest as sent, with
   0 as the default where it knew none). Settings 48, 84 and 85 are stored as sent, for the
   application (the real controller hands them to its IMU unclamped), and `0x89` answers their
   defaults, as on the real controller. Whether Steam reads them back is not known. `0x89` of a
   number above 85 reads 0, as on the real controller (0.2.0 stalled it).
7. **The quaternion** (offsets 45 to 52): the real controller never sends W = 0. July sent the
   identity (W 32767, X = Y = Z = 0); October sends its fused rotation and keeps the last sample
   while the stream is off. The library sends the application's quaternion, and its "none" (all
   four 0, the state of a board without an IMU) as the identity; the fusion helper fills all four.
   The blank canvas's polled IMU option sends "none", so the identity, while setting 48 is 0, where
   the real controller keeps its last sample. (0.2.0 sent all four 0 without an IMU.)
8. **Payload limit**: the controller's payloads are "within 127 bytes", the puck accepts 128; the
   library stays at 127.
9. **The October firmware's change to the ESB driver** (the processor, not the radio, starts each
   reception and acknowledgement) is not reproduced: stock nRF Connect SDK ESB is used.
10. **Board revision.** What Steam does with the hardware ID is not documented; the default is
    `0x48` (72), what a real controller reports and the value Steam has been seen working with
    ("Controller Info: HWID: 72"); `0x45` and `0x49`..`0x4B`, the other values real controllers
    accept, stay selectable.
11. **Lizard mode** follows the documented key map and mouse mapping, the release reports when it
    turns off, the 10 s Steam watchdog, and the documented motion: the touch stabilisers, the
    8-frame average, the glide after a flick (settings 11 and 12) and the haptic ticks and clicks
    (14, 52, 53, 70)
    ([Lizard mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#lizard-mode),
    [the touch stabiliser](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md#the-touch-stabiliser),
    [haptics in lizard mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/08-haptics.md#haptics-in-lizard-mode)).
    Where it leaves a detail open, the library chose:

    | Detail | The library |
    | --- | --- |
    | The travel a glide needs (more than 700): which length each movement adds | the controller's distance approximation, as for the stabiliser and the ticks |
    | The flick speed and the glide's factor: whole numbers or not | whole numbers: the speed is its mean rounded toward zero, each frame's motion speed × (setting 12 − frames gone) / setting 12, rounded toward zero |
    | Setting 12 changed during a glide | read when the glide starts |
    | "If the thumb moved at all" in the stabiliser | on either axis; then both axes close their gap |
    | When ticks and clicks are worked out | once per report frame, on the pad as the report carries it; the real controller does it per trackpad sample |
    | A pad's click turning off because setting 82 blanks the pad | a release like any other: click B plays |
    | Setting 80 = 2 | the stabiliser of the reported position (the pad rules, gap 32), so lizard mode's right pad has none of its own and moves with the held position; the left pad keeps its own too |
    | The haptic events' sound | the library hands over click A or B and the documented level ([api.md](api.md#outputs)); with the haptics synthesiser it plays them itself, while lizard mode is on, setting 70 is exactly 1 and the current link is not the dock (gap 35) |
    | The keyboard | evaluated once per report frame; the real controller sends a key report as soon as its button driver sees the change, so on the cable a key can come up to one frame (4 ms) later here; through the puck both ride the same acknowledgement |
    | A dropped link | the trackers are not started over (as documented): a pad held across a reconnect moves the pointer by the distance it moved meanwhile |

    The key-slot rule, the scales (128 pad units per pointer count, 1024 per scroll step, pad up
    scrolls with a negative wheel value) and the rule that a mouse report goes out every frame
    while anything is non-zero first came from the same authors' study of the controller's
    firmware; the documentation has them now too (the ranges of the settings: gap 6). All of this
    is built and tested on the host (the library against the runner's model, and in the firmware
    emulator); it is not yet tested on hardware, and its feel needs a trackpad, which no test has
    had.
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
    library checks the chip's own USB power detection at boot and, in wireless mode, every 100 ms
    (in a battery build the system state machine does, every 50 ms, in its battery state); with the
    link down, or STEAM held, it asks the application to reboot, which brings the board back wired
    (only while the policy is auto).
18. **STEAM while waiting for a host.** The real controller attaches only once STEAM is released and
    reboots into wireless mode when STEAM is pressed before a host configures it. The library
    attaches at once and counts a STEAM press only after it has seen STEAM released, so a button
    held through power-up does not count.
19. **Haptic stream status after a stop** (closed). The real controller answers each pair of
    Steam's zero pulses with two `0x44` reports (right pad, then left pad, flag `0x02`)
    ([Steam's wired session](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it));
    the documentation of its haptics explains them: every stop-all stops the channel's stream,
    which reports `0x02`, whether or not anything was streaming. The library sends the same
    replies for the same stops, in the documented order, with `CONFIG_OPENSC2_HAPTIC_STOP_STATUS`
    (on by default, and always with the haptics synthesiser; [api.md](api.md#other-input-reports)). They go out as soon as the output is
    decoded; the real controller sends them when its haptics thread takes the stop, within about a
    millisecond while idle and up to one 8 ms block later while something plays. Steam does not
    appear to need them.
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
    USB power to come back (a shorter dip changes nothing); then the system state machine runs the
    shutdown (the power-off cue, 0.5 s, off: a power-off in a battery build, a reboot otherwise,
    [gap 34](#where-this-library-had-to-fill-gaps)). Without it (`CONFIG_OPENSC2_SYSTEM=n`) the
    library asks the application to power off or reboot (`OPENSC2_POWER_USB_UNPLUGGED`), and without
    a callback it reboots, which comes back wireless on a battery. A board without a battery has
    lost its power anyway.
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
    | LED patterns | blue: the pairing double flash, the searching fade, connected steady or fading on external power | the system state machine selects them; the blank canvas shows them in blue on an RGB LED, as brightness alone on its one-colour LED |
    | The ATT MTU | the last value is remembered across connections | per connection, as the specification has it (hosts raise it at every connection) |
    | Reading feature report 1 | 64 bytes, the answer then left-overs; a read blob gets the start again | the 63 bytes the report map declares; a read blob gets the rest of the same answer |
    | Feature report 1's stored request | kept from one connection to the next: a new connection's first read answers what the last SET of an earlier one selected (`0x83` until the first SET after boot) | starts again at `0x83` at every connection, as the radio's does; a host that sends a SET before its GET, as Steam and SDL do, sees no difference |
    | No connection-event trigger | no state reports (inferred) | a timer at the connection interval paces them |
    | Report pacing | the slot less the worst processing time of the last 65 frames | the slot less a fixed 500 µs (`CONFIG_OPENSC2_BLE_REPORT_LEAD_US`) |
    | A report that cannot be sent | logged as an error with its ID | logged at debug level (at the default MTU every state report would be) |

    Valve's Bluetooth company ID and the PnP ID `28DE:1303` are advertised for interoperability, as
    the USB IDs are, and the vendor service uses Valve's UUIDs for the same reason
    ([NOTICE.md](../NOTICE.md)); the IDs are Kconfig options and the vendor service can be left
    out (`CONFIG_OPENSC2_BLE_VENDOR_SERVICE`).
24. **`0xE9`'s read-back, copied.** The real controller converts the stored colour in a way that
    truncates before it scales, so each channel of `0xE9` reads 0 below 100, 100 from 100 to 199
    and 200 from 200. A host can see that answer, so the library gives the same one; the colour
    itself is kept as sent, clamped to 100, and the application gets it unchanged (`led` callback,
    `opensc2_led_get()`). The clamp is OpenSC2's: the real controller keeps `0xC5`'s bytes
    unclamped (a channel above 100 is brighter, up to full duty), so a channel sent above 100 is
    brighter there and reads back 100 or 200, where the library keeps and reads back 100. Steam is
    not known to send such values, nor to read `0xE9` at all.
25. **`0xBE`'s bytes 13 and 14.** The documentation gives byte 13 as "1 while charging" and byte 14
    as `0x43`'s charge state without its `0x80` flag. The library derives byte 13 from the
    application's charge state (1 for state 2, charging, with or without the flag), its simplest
    reading; on the real controller it may come from the charger's own status, which no host has
    been seen to compare.
26. **Settings by name.** The library knows exactly the real controller's keys
    ([api.md](api.md#settings-by-name)); other names go to the application. It keeps `user/keylock`,
    `user/fully_charged_led_intensity`, `mte/charge_level` and the `settings/haptics/` keys as plain
    values: set in RAM, saved and read back where the real controller has a read function, deleted
    by the factory reset where it deletes them. The system state machine acts on the keylock and the
    fully-charged LED level (gap 34); nothing acts on the factory charge window and the haptics
    settings yet. As on the real controller, `0xED` answers `mte/charge_level` only while it holds a
    valid window (both values 0, or 3400 to 4120 mV in 10 mV steps), and `0xEF` saves what `0xED`
    would read back, as the real controller saves what the key's read function gives: it fails for
    the `settings/haptics/` keys (no read function) and for an invalid charge window, and writes
    nothing then. A choice where the documentation says nothing: a readable key that was never set
    answers its whole size in zeros (the real controller's RAM value). The calibration keys are the
    calibration store's (gap 31); a build without `CONFIG_OPENSC2_CAL` gives them to the
    application, so without one `0xED` answers length 0 and the others are refused.
27. **Crash records.** The record and `0xF2`'s rules follow the documentation
    ([api.md](api.md#crash-records)). OpenSC2's own: where the record waits for the next boot (the
    retained RAM region, with a magic word and a CRC-32 of its own), the build time and revision it
    names (the firmware's own, not a build time stored on the board for Steam), that a crash
    repeated at every boot writes flash once until a host reads it (the documentation names no
    such guard), and that `0xF2` saves the record it marked seen from the system work queue rather
    than inside the request.
28. **The hardware watchdog** follows the documented timings (5 s, a feed every 2.5 s from the
    lowest priority, the 4.5 s warning that raises a kernel oops, only a log with a debugger
    attached), but is off by default (`CONFIG_OPENSC2_WATCHDOG`): the real controller's own
    bootloader is not there, and how the UF2 bootloader copes with a watchdog left running has
    not been checked ([api.md](api.md#the-hardware-watchdog)).
29. **The rules on every state report** are the documented ones
    ([pad processing](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md#pad-processing),
    [raw output](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md#raw-output-settings-46-and-62)),
    applied to the values the application reports, which the real controller would have worked
    out itself from its sensors:

    | Rule | The library |
    | --- | --- |
    | Stick-touch fallback | on the application's stick values, with the documented approximation and radius; not with raw sticks (46). The real controller reads its calibrated values, which are 0 in raw mode: the same result |
    | Setting 82 | the documented blanking; raw pressure (60 = 1) passes, though the library has no raw pressure of its own |
    | Raw triggers (62) | both bits 0. The raw values themselves are the application's to give; the blank canvas has no analog triggers |
    | Click by pressure (52, 53) | opt-in (`CONFIG_OPENSC2_PAD_CLICK_AUTO`), so boards with a click switch keep it. The application's pressure 0..32767 is taken as the documented normalised press (calibrated, gain 1): at -1 (100 %) a press would have to pass 108 %, which a value capped at 32767 never does, where the real controller's uncapped value can on a very firm press; below 9 the click can only end with a new threshold, as with the documented formula |
    | Raw sticks (46) | the blank canvas gives counts and millivolts the documented way (the library cannot: it has no ADC) |

    The pressure of an untouched pad is sent as the application reports it, where the real
    controller sends 0; the trigger bits' on point is setting 68 plus 5 points, and a fully pulled
    trigger always counts, where the real one never sets the bit from 68 = 95 on.
30. **Reports waiting through the puck.** The real controller's input records wait for a free
    buffer. The library queues up to 6 extra reports behind each state report as before, and lets
    lizard mode's and the application's reports wait in up to 8 more
    (`CONFIG_OPENSC2_REPORT_WAIT_DEPTH`) instead of dropping them, while lizard mode waits with its
    next frame (its motion is a difference of positions, so the next frame takes it up; a button
    pressed and let go within the frames it waits is not seen, where the real controller's
    keyboard would still send it). The battery report is still dropped when the queue is full.
31. **Calibration** (`CONFIG_OPENSC2_CAL`, on by default). The keys, their sizes and formats, the
    commands `0xD8`, `0xC0`, `0xC3`, `0xE2` and `0xFE`, the recordings' steps and the arithmetic of
    sticks, triggers and pad pressure follow the protocol documentation
    ([input and sensors](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/07-input-and-sensors.md#calibration-keys)).
    Where it leaves something open, or a part is Valve's hardware, OpenSC2 chose:
    - the factors 0.9, 0.98 and 0.05 are exact fractions in integer arithmetic, so a value that
      lands exactly on a whole number is not off by one the way a floating-point one can be;
    - a recording saves only the sides that got raw samples since its step 1: a board that gives
      no raw input for a side keeps that side's stored key;
    - the raw inputs are processed once per state report: the pressure filter and the two-word
      means of the capacitive words step per report, where the real controller steps them per
      trackpad frame (about 250 a second, close to the report rate on the cable);
    - a pressure calibration whose range is empty (B equal to A) counts as uncalibrated;
    - `0xED` answers `cal/volt_offset` always, the LED trims while they lie within 1..199, a
      stick, trigger, pressure or touch key (`cal/touch_l`, `cal/touch_r`: 10 bytes, used by
      nothing) while its format word is not 0, and never the gyro bias, as the documentation
      says; `0xEF` saves only what `0xED` would read back (the real controller saves what the
      key's read function gives), so it refuses an analog or touch key whose format word is 0,
      an LED trim outside 1..199 and the gyro bias; the LED itself uses any trim, 0 leaving the
      channel dark, as the real controller does;
    - the flash writes that a finished recording, `0xE2` and `0xFE` cause run from the system work
      queue, in the order they were asked for (`0xFE` drops the saves still waiting from before
      it); `0xEF` and `0xF0` of a `cal/` key first wait for those, then write before they answer,
      as for the bonds. The real controller's `0xEF` does not wait: there an erase still running
      on its work queue can delete a key that a later `0xEF` saved, so a host's "erase, then store
      the new calibration" can lose the new keys at the next boot. OpenSC2 keeps them (a
      deliberate difference, in timing only when nothing is waiting);
    - a settings partition with every sector occupied at boot: the real controller erases all of
      it, calibration included, and starts over; so does OpenSC2 with
      `CONFIG_OPENSC2_STORAGE_RECOVERY`, on by default with NVS settings
      ([api.md](api.md#the-settings-partition)); without it Zephyr's NVS does not start, and the
      board runs on its built-in values with nothing loaded;
    - `0xE2` and `0xFE` read their bytes whatever the length byte says, as the real controller
      does, so an empty request reads the report's zero padding;
    - a trigger direction byte other than 0 counts as 1 (the voltage falls when pulled);
    - not reproduced, being Valve's hardware: the stick offsets while docked, the scaling of the
      capacitive words without external power, the trackpad chip's electrode processing (an
      application gives positions), and the IMU's runtime settings keys.
    The hardware test suite never writes, saves or deletes a `cal/` key and never sends the
    calibration commands (its safety rule S5): calibration is tested by the host tests only.
32. **The pad rules** (`CONFIG_OPENSC2_PAD_RULES`, on by default, inactive with the default
    settings). The rotation (settings 2 and 24) and the touch stabiliser (setting 80 = 2) run on
    the application's finished positions, after the hardware test hooks' injection: the library
    halves them back to the pad scale, turns Y back, applies the documented integer arithmetic and
    doubles them again, so an odd position loses its lowest bit while a rule is on. The rotation's
    sine and cosine are the exact values × 8192, truncated (30° gives 4096). The position filter
    (setting 65, on by default on the real controller) is a helper for applications
    (`<opensc2/sensor_helpers.h>`), not a library rule: as a rule it would change every board's
    pads. Its speed is the change since the previous output, as in the usual "one euro" filter
    (the documentation leaves open whether the previous input is meant).
33. **Setting 85.** The real controller hands it to its IMU's fusion block, where its role is not
    known. OpenSC2's rest-based gyro-bias estimate (`<opensc2/sensor_helpers.h>`, for IMUs
    without one of their own) takes it as its rest threshold in report units. The blank canvas's
    LSM6DSV16X stream option (`app/imu-stream.conf`) takes the stored gyro bias as the real
    controller does: a NaN goes in as 0, and a value of 2^31 or more in magnitude (an infinity
    included) is refused, so the stream does not start at boot and waits for a host to write
    setting 48. A bias the IMU's driver cannot program is left out and the stream starts
    without it.
34. **The system state machine** (power, chords, the status LED and cues; built, not yet tested on
    hardware). OpenSC2 follows the documented machine ([in brief](#overview-power-chords-and-the-status-led);
    [api.md](api.md#power-and-the-system-state-machine)), with these differences and choices:
    - **No battery, no power-off.** Without `CONFIG_OPENSC2_BATTERY` (the default) a board that
      runs without USB power is taken to be on an external supply, which counts as USB power, and
      every shutdown that would turn it off reboots instead (what setting 77 does on the real
      controller), so a board that could not be woken again is never switched off. The give-up
      timers (60 s of listening, 10 s without a bond) are the battery build's defaults; without a
      battery the radio keeps listening, as before.
    - **The wired or wireless decision stays before the machine** (the boot rule in
      [api.md](api.md#modes)), so USB power that appears during the 1 s battery wait, or after the
      board was lifted off the dock, reboots the board, which comes back wired; the real controller
      enables USB in place.
    - **The wired start** keeps the library's STEAM rule (gap 18) and attaches at once; the power-on
      chime is raised at the start, and the LED turns amber after 0.5 s without a host.
    - **Dropping the link** also stops a radio that is still listening; the real controller lets it
      search on until its own timer ends.
    - **Wireless on again**: a link started from outside the machine (`opensc2 start`, the
      application's `opensc2_start()`, pairing) while wireless is off on USB power counts as
      wireless on. A radio stopped on the board (`opensc2_stop()`) is not "wireless off": only a
      link that gave up, a protocol version the library does not speak, and the puck's turn-off are.
    - **Power that goes away** must stay away 0.2 s before it counts (the wired unplug rule's
      debounce); the real controller acts on its charger's events. The dock exists only as a power
      source an application names (OpenSC2 has no dock).
    - **Host requests**: the states that do not act on a host's reboot, bootloader or warm-reboot
      request ignore it, as documented; the library's own reboots (STEAM before a host, the shell,
      the pairing button) are never held back.
    - **The Bluetooth chords** exist only in a build with Bluetooth; without one, B with a bumper
      is no chord, and a stored Bluetooth transport starts the main puck.
    - **Cues 6 and 7** (pairing, paired) are raised by the over-the-air pairing (gap 1); the real
      controller's pairing routine has them, but nothing there reaches it. Cue 8 (the factory
      charge window) is never raised. Cue 0 is also raised at a remote wakeup
      (`CONFIG_OPENSC2_USB_REMOTE_WAKEUP`), as on the real controller's wake-up path (unreachable
      in its October firmware). OpenSC2 has none of the real scripts: the cue event names the real
      script it stands for, and the sound is OpenSC2's own, played by the haptics synthesiser when
      it is built in (gap 35), else by the application.
    - **The keylock chord** saves the lock for the next power-on and leaves the value in RAM as it
      is, so `0xED user/keylock` reads the old value until the next boot, as documented (a quirk
      Steam could see, copied).
    - **The LED**: the documentation gives the brightness curve for setting 38 = 1 (the default) and
      a linear brightness for 0; OpenSC2 uses the curve for every value that is not 0. The blank
      canvas scales its channels by the per-unit `cal/rgbw_*` keys, as the real controller does
      (`led-trims` in its overlay can fix them for a board instead). The over-the-air pairing (gap
      1), which the real controller never shows, flashes like Bluetooth's pairing mode. None of the
      patterns has been compared with a real controller's LED.

35. **The haptics synthesiser** (`CONFIG_OPENSC2_HAPTICS_SYNTH`, off by default; built, not yet
    tested on hardware). Written from the research documentation's description of the real
    controller's sequencers, generators, scripts, cues, streams and rumble emulation
    ([docs/controller/08-haptics.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/08-haptics.md)),
    with these choices:

    | Point | OpenSC2 |
    | --- | --- |
    | Valve's data | not used: the two clicks are damped sine bursts of OpenSC2's own at the real clicks' lengths; scripts 1 to 17, the cues' sounds and the two drop sounds are short sounds of OpenSC2's own (tones, sweeps, pulses), not the real controller's step data, melodies or recordings; the 18th script, which nothing plays, is left out |
    | Copied as the real controller plays it | the rumble's bursts at a constant 12 Hz (its rate formula divides by the 32-bit maximum); the left grip's LFO dipping to 8.8 Hz just above a fifth of full speed; `0x81` with a count of 0 playing nothing; formats 12 and above answered "set" (`0x20`) while the previous format stays; the rumble's random burst gains from the documented generator and seed; the left pad and the right grip rendered with the gain negated |
    | Kept correct where only a sound would differ | gains added without wrapping round (the real controller adds in 8 bits, so `0x85` near -128 dB plays at +30 dB); a pulse train shorter than one sample plays nothing (the real arithmetic plays -1 for 65,536 samples); a sweep with 0 Hz at an end plays nothing and one with equal ends a plain tone with the fades (the real arithmetic produces invalid numbers); long sweeps do not wrap at 16 bits; a generator that ends leaves the mix at once (the real one lingers 1 or 4 silent samples); setting 75 going to 0 does not cut a playing sound (the real one drops the enable at once); a fall's drop sound is not cancelled for good when the IMU stops |
    | Arithmetic | integers only, a sine table of OpenSC2's own (peak 1.0; the real polynomial peaks at about 1.0045), so blocks are bit for bit reproducible; the PWM DAC plays the pads at exactly 4 kHz (the real one at about 3906 Hz) |
    | Not documented, chosen | an 8 kHz format on a pad is refused (`0x10`) before a format change while playing (`0x40`); a channel that never had a format plays formats 12 and above as s16le at 8 kHz; a tone's first sample is sin(0); a clip's last sample holds through its interpolated steps; the free-fall helper's thresholds (0.35 g for 150 ms; the real controller uses its IMU's detector); the DRV2605's amplitude is the louder path's block peak |
    | Stops and `0x44` | the synthesiser sends `0x01`, `0x08`, `0x04`, `0x02` when a stream runs dry, `0x10`, `0x20` and `0x40`, in the order they happen; it does not send the `0x02` the real controller sends after every stop: those are gap 19's replies (`CONFIG_OPENSC2_HAPTIC_STOP_STATUS`, which the synthesiser selects), so they never come twice |
    | Setting 70 | does not silence the synthesiser (on the real controller only lizard mode's pad feedback reads it) |
    | `0x88` | with more than 31 bytes a side the report does nothing, as on the real controller (in every build: the decoder; v0.2.0 clamped it to 31) |
36. **The dock's answers are whole frames.** The real controller leaves every answer byte it does
    not write as it was in its previous answer: a probe's type and result, a SET's payload, the bytes
    after a shorter answer, and the whole frame for a request type it does not know. OpenSC2 writes
    zeros there, answers a probe with type 0 and result 0, and an unknown type with its type and
    `-22`. The puck reads only what each answer defines and never sends another type, so neither it
    nor Steam can tell.
37. **GET input with the dock alone** answers with the report built for that request. The real
    controller (inferred from its thread priorities) answers with the one its previous request
    built, one poll old (about 90 ms).
38. **Result codes on the dock** are the real controller's numbers whatever this platform's errno
    values are: `-22` for an opcode nobody knows or a length above 61, `-95` for a GET whose
    command has nothing to read.
39. **One command at a time.** The real controller runs each link's commands in that link's own
    thread, with no lock between links. OpenSC2 runs one command at a time across links, so a command
    from the dock waits for one from USB or the radio and the other way round; a flash write on one
    link can delay another link's command by its length. A host sees no other difference.
40. **Dock power and the docked states.** On the real controller the dock's `+` reaches the chip's
    USB supply input; on OpenSC2 it must not (a board would boot wired on the dock), so the supply
    rule takes `+` on a sense input of its own (in the UART build), and the dock and USB at once
    count as the dock, as on the real controller. The rule gives the system state machine its power
    source, and the docked states are the machine's ([api.md](api.md#the-docks-rules)). The
    SuperMini dock build has no `+` input, so it never counts as powered by the dock. Without the
    machine (`CONFIG_OPENSC2_SYSTEM=n`) a cold boot on the dock starts the radio when the
    application asks, where the real controller charges with the radio off.
41. **The dock's wire is untried.** The pilot's window is at the controller's pin with no divider;
    how the pilot and the serial link share the `P` pad, the pad's idle level and its load are not
    known. The SuperMini dock build (`app/dock.conf`) is a guess at the controller's side (an
    open-drain TX, RX without a pull, the pilot on an analog pin), built and never run; nothing may
    be connected to Valve's puck's pads without the owner's go-ahead.

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
- **The status LED on a one-colour LED**: the real controller's patterns as their rhythm (on while
  a pattern is at least half as bright as its peak), and amber, which only a colour can tell from
  green, adds a 100 ms dark blink every second, so "no host" and "wired" differ. An RGB(W) LED shows
  the real colours ([api.md](api.md#status-led)). Without the system state machine the blank canvas
  shows the puck's patterns while wireless, and green or amber while wired.
- **A product name of the board's own** (`opensc2 id set name`), optional: the real controller's
  "Steam Controller" by default.
- **A charger test build** (`CONFIG_OPENSC2_NO_HOST_WIRELESS_MS`, off by default; the blank canvas's
  `test-charger.conf`): a testing aid for a board with no buttons, which goes wireless when no host
  sends a USB request within 5 s of a wired boot. The real controller waits for a host or for STEAM;
  this differs from it on purpose and is never part of a default build.
- **The hardware test hooks** (`CONFIG_OPENSC2_TEST_HOOKS`): input injection and output capture for
  the PC test suite, inert until it arms them ([api.md](api.md#hardware-test-hooks)).

Back to the [documentation map](README.md).
