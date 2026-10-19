# The protocol, in brief

What OpenSC2 implements, in short, with links to the full description: for the curious, and for
anyone who wants to check the library against the documentation.

The library implements the controller's end of the 2.4 GHz link to Valve's puck, and the USB device
a Steam Controller 2 is on its cable. It was written from the protocol documentation the same
authors published in
[steam-controller-2-reverse-engineered](https://github.com/ded811/steam-controller-2-reverse-engineered),
plus black-box tests and USB captures of Steam with a real controller
([How it was made](../README.md#how-it-was-made)). A few facts come from the same authors' study of
the controller's firmware and are not yet on the published pages; the gaps below say which.

| Topic | Page |
| --- | --- |
| The controller's end of the radio link: set-up, listening, connecting, keep-alives, records, link loss, channels, bonds, the identity packet, protocol version, turn-off | [docs/controller/04-radio.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/04-radio.md) |
| The puck's end: slots, pairing window, connecting, channel choice, suspend | [docs/puck/04-radio.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/04-radio.md) |
| The wired USB device: descriptors, strings, report timing | [docs/controller/03-usb-and-hid.md, The device](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-device) |
| The report descriptor and its collections | [The report descriptor](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-report-descriptor) |
| Report `0x42`, the button bits, the vendor commands | [docs/controller/03-usb-and-hid.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md) |
| The controller settings | [Controller settings](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#controller-settings) |
| Lizard mode | [Lizard mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#lizard-mode) |
| Steam's wired session | [The wired session as Steam runs it](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it) |
| Pairing over the cable, and the reboot into wireless mode | [Pairing a controller with a puck over the cable](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#pairing-a-controller-with-a-puck-over-the-cable), [Reboot into wireless mode](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#reboot-into-wireless-mode) |
| The July and October 2026 forms of the USB device | [What changed since July](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#what-changed-since-july) |
| Wired or wireless at boot, the warm restart, the LEDs | [docs/controller/09-system.md, The state machine](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/09-system.md#the-state-machine) |
| Output reports `0x80`..`0x8A` | [docs/controller/08-haptics.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/08-haptics.md) |
| The dock (not implemented here) | [docs/controller/06-dock.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/06-dock.md) |
| What the puck shows Steam (feature report pass-through, attribute cache) | [docs/puck/03-usb-and-hid.md](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/puck/03-usb-and-hid.md) |
| The puck side as an open firmware | [Counterpuck](https://github.com/ded811/Counterpuck), [wire-formats.md](https://github.com/ded811/Counterpuck/blob/main/docs/wire-formats.md) |

## Overview: the radio link

- **Radio**: Nordic Enhanced ShockBurst, dynamic payloads up to 128 bytes, 2 Mbit/s on the BLE 2M
  PHY, 16-bit CRC, +6 dBm, selective auto-acknowledgement, fast ramp-up. The puck transmits; the
  controller is the receiver and says everything in **acknowledgement payloads**, so each reply
  rides with the acknowledgement of the puck's next packet.
- **Pairing** (channel 2): the puck's pairing window (which Steam can open with the puck's vendor
  command `0xAD`) beacons a hello to address `91 A2 A7 93`. The controller answers `0xF0 {T,
  serial}`; the puck sends `0xE0 {puck ID, puck serial}` to address T; the controller confirms with
  `0xF4 {C, serial}`; both store `{puck ID, C}`.
- **Connecting** (channel 2, address `ibex`, prefix `0x10`): the puck broadcasts `0xE1 {puck ID,
  controller ID, channel, its address, a pipe prefix}` without acknowledgement. The controller with
  that bond moves to the private pipe and acknowledges the hello that follows 1 ms later.
- **Connected**: the puck opens with `0xE7 {0, 1}` (protocol version 1); the controller answers
  `0xF3 {1}` and once per connection its identity `0xF5` (57 bytes). Then the puck sends `0xE3`
  keep-alives about every 3 ms, carrying records `{length, type, data}`: SET feature (1), GET feature
  (3), output report (5). The controller answers `0xF1`: one SET result (2) or GET answer (4), the
  state report `0x42` as an input record (6), and any queued input reports, at most 127 bytes.
  A GET record is answered with the last SET's command, as on USB: its one data byte is not the
  opcode (Valve's puck leaves it unset), and the puck drops an answer whose opcode is not its SET's.
  Seen on hardware: with that byte taken as the opcode, Steam's bond read through Valve's puck
  failed ("Failed to read triton info").
- **Channels**: `0xE4` gives a new channel list (for version 1: the new channel plus 2 and 80). The
  controller waits 10 ms per turn on its current channel, then 13 ms on each alternate; 500 ms
  without a packet is a lost link, and it listens on channel 2 again.
- **The PC sleeps**: the puck sends `0xE5` (turn off); the controller answers `0xF2` to every packet
  until the puck stops. `0xE6` says the PC is asleep while the link stays up.

## Overview: the cable

From the published USB page, its sections on Steam's wired session, pairing over the cable and the
reboot into wireless mode (linked above), and this project's own tests with Steam and Valve's puck:

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

## Where this library had to fill gaps

The documentation describes one real controller firmware. Where it leaves something open, the
library does what should work with Steam and the puck, and says so here:

1. **Over-the-air pairing.** Pairing over the air (the puck's pairing window) is implemented but
   untested with Steam. Steam pairs controllers over a wired connection, and pairing over the USB-C
   cable is the tested way
   ([the published sequence](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#pairing-a-controller-with-a-puck-over-the-cable)).
   The documented controller firmware contains an over-the-air pairing routine that nothing in it
   ever starts
   ([pairing is unreachable](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/04-radio.md#pairing-is-unreachable)),
   but the puck still has its pairing window, so the library implements the documented exchange
   from the controller's side, with the puck's documented timings (and keeps its confirmation queued
   if the puck repeats its offer). A wireless board answers an open window when its pairing button
   is pressed; with [Counterpuck](https://github.com/ded811/Counterpuck) as the puck, its shell
   command `pair 0` opens a window without Steam (also untested with OpenSC2).
2. **Rumble report `0x80`** (closed). The published page and SDL's `MsgHapticRumble` agree: `[0]`
   the type, `[1..2]` the level, `[3..4]` the left speed, `[6..7]` the right speed. The library also
   decodes the gain bytes `[5]` and `[8]`, which the July 2026 firmware used.
3. **Report `0x43` charge state.** The library sends the application's charge state as given, in
   SDL's `EChargeState` numbering (the type SDL gives this byte; SDL reads 1 as on battery, 2 as
   charging, 4 as charged). The real controller can add a flag bit `0x80` to it (the published page
   says when); the library has no charger logic of its own and never adds it.
4. **Answer layouts not given byte for byte**: `0xF2` selector 2 (uptime u32, then the reason as a
   u32), `0xA1` (reset cause u32, device ID, zeros; the reset-cause flags are Zephyr's, not the
   original's), and `0xAE` (the tag, then the string, 20 bytes in all, as Counterpuck answers for
   the puck).
5. **Which commands Steam sends through the puck** is not fully documented. The library answers the
   identity, settings, LED, user-store and settings-by-name commands and passes the rest to the
   application. On the cable the commands Steam sends are known
   ([Steam's wired session](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-wired-session-as-steam-runs-it)),
   and each gets the real answer or the real STALL (Steam's `0xC1` is stalled, as by the real
   controller).
6. **Controller settings** have published defaults; `0x87` clamps each value to its range. The
   published page gives the ranges of settings 36 and 68; the ranges of 9, 11, 12, 14, 45, 70 and 71
   come from the same authors' study of the controller's firmware (not yet on the published pages).
   These are the ranges the library knows. Other settings are stored as sent. Settings 48, 84 and
   85 are stored too (the real controller hands them to its IMU and `0x89` answers their defaults);
   the library answers what was written.
7. **The quaternion's W** (offset 45): one page calls it always 0, another the quaternion's W (since
   October). The library sends whatever the application provides; the fusion helper fills W.
8. **Payload limit**: the controller's payloads are "within 127 bytes", the puck accepts 128; the
   library stays at 127.
9. **The October firmware's change to the ESB driver** (the processor, not the radio, starts each
   reception and acknowledgement) is not reproduced: stock nRF Connect SDK ESB is used.
10. **Board revision.** What Steam does with the hardware ID is not documented; the default is
    `0x48` (72), what a real controller reports and the value Steam has been seen working with
    ("Controller Info: HWID: 72"); `0x45` and `0x49`..`0x4B`, the other values real controllers
    accept, stay selectable.
11. **Lizard mode** follows the published key map and mouse mapping, the release reports when it
    turns off, and the 10 s Steam watchdog. The key-slot rule, the scales (128 pad units per pointer
    count, 1024 per scroll step, pad up scrolls with a negative wheel value) and the rule that a
    mouse report goes out every frame while anything is non-zero come from the same authors' study
    of the controller's firmware (not yet on the published pages). Not reproduced: the touch-down
    stabiliser, the motion average, the glide after a flick (settings 11 and 12) and the haptic
    ticks (setting 14). The keyboard is evaluated once per report frame.
12. **A bond written with `0xA2`** takes effect at once here; the documented controller only reads
    it at the next boot.
13. **The report descriptor's bytes.** Each detail has one source:

    | Detail | Source |
    | --- | --- |
    | 372 bytes; the three collections in this order: mouse, keyboard, vendor; every report's ID, direction and size; the vendor collection's item rule (every field is a byte with a usage equal to its report ID) | the published page, [The report descriptor](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-report-descriptor) |
    | the fields of the mouse and keyboard reports; the vendor fields' range (8 bits, 0..255); usage 1 for both feature reports | Counterpuck's [wire-formats.md](https://github.com/ded811/Counterpuck/blob/main/docs/wire-formats.md#usb-hid-reports) |
    | the sizes of the mouse and keyboard collections (67 and 41 bytes); the form of the vendor items (every input and output report states its ID, logical minimum 0, a two-byte logical maximum of 255, report size 8, its count, usage and main item; the feature reports only ID, count, usage and main item); the order of the vendor reports (inputs `0x42`, `0x44`, `0x79`, `0x43`, `0x7B`, `0x45`; outputs `0x80` to `0x87`, then `0x89`, `0x88`; features `0x01`, `0x02`) | the same authors' study of the controller's firmware (not yet on the published pages) |

    None of them gives every byte of the mouse and keyboard collections: the library builds them in
    the usual layouts at those sizes, and two item choices that give the sizes (a repeated Report
    Count before the mouse's horizontal pan, a restated Logical Minimum in the keyboard) are
    guesses. Windows and Steam accept the result in the tested setup
    ([What works](../README.md#what-works)); no host is known to compare the bytes.
14. **Two forms of the device.** The library has both:
    `CONFIG_OPENSC2_USB_PROFILE_JULY` (the default), the July 2026 controller firmware's form: USB
    2.00, class `EF/02/01`, release 3.07, each GET answered with 64 bytes; and
    `CONFIG_OPENSC2_USB_PROFILE_OCTOBER`, the October 2026 firmware's form: USB 2.01 with a USB 2.0
    extension BOS descriptor, class `00/00/00`, release 4.04, 63-byte GET replies
    ([The device](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#the-device),
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
