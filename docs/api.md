# API reference

The library's whole interface, for developers who use it in their own firmware: every function,
callback, field, rule and option.

Everything a project needs from `<opensc2/opensc2.h>` (which also includes `<opensc2/types.h>`,
`<opensc2/codec.h>`, `<opensc2/settings.h>` and `<opensc2/led.h>`, the status LED), plus the
optional `<opensc2/fusion.h>`, `<opensc2/cal.h>` (calibration and raw inputs),
`<opensc2/sensor_helpers.h>` and `<opensc2/gauge.h>` (sensor and battery helpers),
`<opensc2/haptics.h>` (the haptics synthesiser) and `<opensc2/dock.h>` (the dock's frames). The
library is hardware-agnostic: it has no pins. Your application reads its buttons, sensors and touch parts
and hands the values over; it receives the outputs through callbacks and drives whatever it has.
The same inputs and outputs work wireless (through the puck) and wired (on USB). How to bring the
library into a project: [integration.md](integration.md).

- [Functions at a glance](#functions-at-a-glance)
- [Life cycle](#life-cycle)
- [Modes](#modes): wired or wireless, the policy, reboots
- [Wired mode](#wired-mode): the USB device, what a project needs, the session log
- [Inputs](#inputs): every field, its range, and where Steam sees it
- [Calibration and raw inputs](#calibration-and-raw-inputs): the real controller's processing and
  its stored calibration
- [Outputs](#outputs): callbacks and what is in them
- [Power and the system state machine](#power-and-the-system-state-machine): power-on, STEAM, the
  chords, the keylock, the idle timeout, low battery, shutdown and power-off, the cues
- [Status LED](#status-led): the modes the library selects, and the pattern player that shows them
- [The haptics synthesiser](#the-haptics-synthesiser): the output reports rendered for actuators
  (an option)
- [Identity](#identity)
- [Bonds and pairing](#bonds-and-pairing)
- [Wireless transports and Bluetooth LE](#wireless-transports-and-bluetooth-le): the puck or
  Bluetooth, and the opt-in Bluetooth build
- [Vendor commands](#vendor-commands): what the library answers itself
- [Controller settings](#controller-settings), and the other state Steam sets
- [Kconfig options](#kconfig-options)
- [Threads and timing](#threads-and-timing)
- [Reliability](#reliability): crash records, the hardware watchdog, the settings partition's
  recovery, reset causes
- [Shell commands](#shell-commands)
- [Hardware test hooks](#hardware-test-hooks): input injection, output capture, the lizard guard
- [The dock](#the-dock): the controller's end of the puck's charging pads (the protocol; its
  UART build has never run on hardware)
- [Pure helpers](#pure-helpers), [the IMU fusion helper](#the-imu-fusion-helper), the
  [calibration](#calibration-helpers), [sensor](#sensor-helpers) and [battery](#battery-helpers)
  helpers

"In Steam" below means what Steam and SDL's open-source Steam Controller 2 driver
(`SDL_hidapi_steam_triton.c`) read from the field. How Steam's own configurator presents each one
is not documented here and has not been checked against this library.

## Functions at a glance

| Function | Does | More |
| --- | --- | --- |
| `opensc2_init(&config)` | start the library once: load what is stored, decide the mode, start USB or the radio | [Life cycle](#life-cycle) |
| `opensc2_start()`, `opensc2_stop()` | the wireless transport on, off (wireless mode): the radio, or Bluetooth | [Life cycle](#life-cycle) |
| `opensc2_pair_start(s)`, `opensc2_pair_cancel()` | pair over the air with a puck's open pairing window (with Bluetooth as the transport: pairing mode) | [Bonds and pairing](#bonds-and-pairing) |
| `opensc2_link_state()`, `opensc2_link_info(&info)` | the wireless link's state and counters | [Life cycle](#life-cycle) |
| `opensc2_select_transport(t)`, `opensc2_select_transport_pairing(t)`, `opensc2_selected_transport()` | the wireless transport: the puck with bond 1 or 2, or Bluetooth slot 1 or 2 (with the pairing flag, Bluetooth opens the slot for a new host) | [Wireless transports](#wireless-transports-and-bluetooth-le) |
| `opensc2_mode()`, `opensc2_mode_reason()` | this boot's mode (wired or wireless) and why | [Modes](#modes) |
| `opensc2_policy()`, `opensc2_policy_set(p)` | the stored rule for boots with USB power | [Modes](#modes) |
| `opensc2_reboot(how, why)`, `opensc2_reboot_later(how, why, ms)` | reboot by the rule, into wireless, or into wired once | [Modes](#modes) |
| `opensc2_usb_state()`, `opensc2_usb_info(&info)` | the wired USB device's state and counters | [Wired mode](#wired-mode) |
| `opensc2_set_buttons(mask, value)` | press and release buttons and touch bits | [Inputs](#inputs) |
| `opensc2_set_stick(side, x, y)`, `opensc2_set_trigger(side, v)` | a stick, a trigger | [Inputs](#inputs) |
| `opensc2_set_trackpad(side, &pad)`, `opensc2_set_imu(&imu)` | a trackpad, the IMU | [Inputs](#inputs) |
| `opensc2_set_state(&state)`, `opensc2_get_state(&state)` | the whole input state at once | [Inputs](#inputs) |
| `opensc2_set_battery(&battery)` | the battery report | [Battery](#battery) |
| `opensc2_set_stick_raw()`, `_trigger_raw()`, `_pressure_raw()`, `_touch_raw()`, `opensc2_set_raw(&raw)`, `opensc2_cal_raw_release(inputs)` | raw inputs, processed as the real controller does with its stored calibration | [Calibration and raw inputs](#calibration-and-raw-inputs) |
| `opensc2_cal_stick_ok(side)`, `_trigger_ok()`, `_pressure_ok()`, `opensc2_cal_recording(kind)`, `opensc2_cal_led_trim(ch)`, `opensc2_cal_volt_offset()`, `opensc2_cal_gyro_bias(b)`, `opensc2_cal_generation()` | the stored calibration, a recording running | [Calibration and raw inputs](#calibration-and-raw-inputs) |
| `opensc2_send_report(report, len)`, `opensc2_send_haptic_status(channel, flags)` | any other input report | [Other input reports](#other-input-reports) |
| `opensc2_bond_get(slot, &bond)`, `opensc2_bond_set(slot, &bond)` | read or store a pairing | [Bonds and pairing](#bonds-and-pairing) |
| `opensc2_select_slot(slot)`, `opensc2_selected_slot()` | the pairing slot in use (0 or 1) | [Bonds and pairing](#bonds-and-pairing) |
| `opensc2_identity_get(&id)`, `opensc2_identity_set(&id)`, `opensc2_identity_store(&id)`, `opensc2_identity_clear()` | who the controller says it is | [Identity](#identity) |
| `opensc2_setting_get(n)`, `opensc2_lizard_mode()`, `opensc2_led_get(&led)`, `opensc2_chime_level()` | the controller settings and the other state Steam sets | [Controller settings](#controller-settings) |
| `opensc2_setting_set(n, v)`, `opensc2_led_set_color(r, g, b, w)`, `opensc2_chime_level_set(l)` | the same changes from the board, as Steam's `0x87`, `0xC5`, `0xDC` make them | [Controller settings](#controller-settings) |
| `opensc2_setting_default(n)`, `opensc2_setting_clamp(n, v)` | a setting's default and range (pure) | [Controller settings](#controller-settings) |
| `opensc2_set_power_source(s)`, `opensc2_power_source()` | what powers the board: a battery, USB or the dock | [Power and the system state machine](#power-and-the-system-state-machine) |
| `opensc2_keylock()` | whether the keylock (travel lock) is stored | [The keylock](#the-keylock) |
| `opensc2_charge_state_of(power, charging)` | report `0x43`'s charge state from a charger's status output (pure) | [Battery](#battery) |
| `opensc2_led_status(&st)` | the status LED's mode and everything its pattern depends on | [Status LED](#status-led) |
| `opensc2_led_player_init(&p)`, `opensc2_led_player_step(&p, &st, now, rgbw)`, `opensc2_led_player_envelope(&p)` | the real controller's LED patterns as colours every 10 ms (pure) | [Status LED](#status-led) |
| `opensc2_led_brightness(s45, s38)`, `opensc2_led_duty(c, trim, b)` | the brightness curve and a channel's duty (pure) | [Status LED](#status-led) |
| `opensc2_fusion_init(&f)`, `opensc2_fusion_update(&f, a, g, dt)`, `opensc2_fusion_quat(&f, q)` | the orientation quaternion from an IMU without its own fusion | [The IMU fusion helper](#the-imu-fusion-helper) |
| `opensc2_accel_from_g(g)`, `opensc2_gyro_from_dps(dps)` | g and °/s in the report's units | [The IMU fusion helper](#the-imu-fusion-helper) |
| `opensc2_encode_state()`, `opensc2_decode_output()`, ... | every wire format as a pure function (`<opensc2/codec.h>`) | [Pure helpers](#pure-helpers) |
| `opensc2_stick_setup()`, `opensc2_trigger_value()`, `opensc2_pressure_sample()`, ... | the calibration's arithmetic as pure functions (`<opensc2/cal.h>`) | [Calibration helpers](#calibration-helpers) |
| `opensc2_cap_sample()`, `opensc2_pad_rotate()`, `opensc2_stabiliser_step()`, `opensc2_imu_mount()`, ... | sensor processing as pure functions (`<opensc2/sensor_helpers.h>`) | [Sensor helpers](#sensor-helpers) |
| `opensc2_gauge_step()`, `opensc2_gauge_divider_mv()`, ... | battery voltage and state of charge (`<opensc2/gauge.h>`) | [Battery helpers](#battery-helpers) |
| `opensc2_haptics_set_output(path, &out)`, `opensc2_haptics_play_cue(cue, level)`, ... | with `CONFIG_OPENSC2_HAPTICS_SYNTH`: the haptics synthesiser's outputs and its own sounds (`<opensc2/haptics.h>`) | [The haptics synthesiser](#the-haptics-synthesiser) |
| `opensc2_dock_crc16()`, `opensc2_dock_frame_check()`, `opensc2_dock_frame_seal()`, `opensc2_dock_build_request()` | the dock's 73-byte frames (`<opensc2/dock.h>`, pure) | [The dock](#the-frame-functions) |

The callbacks your application can register are in [Outputs](#outputs).

## Life cycle

```c
#include <opensc2/opensc2.h>

static void on_link(const struct opensc2_link_info *info, void *user)
{
    /* show info->state on an LED */
}

static const struct opensc2_callbacks callbacks = { .link_changed = on_link };

int main(void)
{
    struct opensc2_config config = {
        .callbacks = &callbacks,
        .bond_slot = OPENSC2_SLOT_STORED,
        .start = true,
    };

    if (my_pair_button_is_held()) {
        config.boot_mode = OPENSC2_BOOT_WIRELESS;   /* stay off USB this boot */
    }
    opensc2_init(&config);
    /* ... opensc2_set_buttons(), opensc2_set_stick(), ... */
}
```

| Function | Does |
| --- | --- |
| `opensc2_init(&config)` | Loads the bonds, the wireless transport, the stored identity and the mode policy from Zephyr settings, checks the identity, decides this boot's mode, then starts the USB device (wired) or the stored wireless transport (wireless: the radio thread, or Bluetooth). Once. `-EINVAL` for an invalid identity. A wired boot whose USB device cannot be set up goes on wireless (`OPENSC2_MODE_USB_FAILED`, the transport started if `start` is set). After an error the debug console (`CONFIG_OPENSC2_USB_CONSOLE`) still starts, so the log can be read. With `CONFIG_OPENSC2_SYSTEM` (the default) the [system state machine](#power-and-the-system-state-machine) then makes its power-on decision and starts the transport when its rules say so (on battery after its 1 s wait), and only if `start` is set. |
| `opensc2_start()` | The wireless transport on. The radio: listen for the bonded puck, or wait in `OPENSC2_LINK_NO_BOND` when the slot is empty. Bluetooth: advertise ([below](#the-bluetooth-build)), also after "wireless off". `-EBUSY` while wired. |
| `opensc2_stop()` | The wireless transport off. The radio: like the real controller, nothing is sent; the puck notices the silence. Bluetooth: an all-zero `0x45`, 100 ms, a disconnect, no advertising. |
| `opensc2_pair_start(seconds)` | Pair over the air with a puck whose pairing window is open. `0` = `CONFIG_OPENSC2_PAIRING_TIMEOUT_S` (60). Starts the radio if needed. With Bluetooth as the transport: pairing mode instead (the slot's other identity, reset, advertises open to a new host; the slot's old host keeps its bond until a new one has paired). `-EBUSY` while wired (Steam pairs a wired board itself). |
| `opensc2_pair_cancel()` | Stop pairing. |
| `opensc2_link_state()` | `OFF`, `NO_BOND`, `PAIRING`, `LISTENING` or `CONNECTED`, of the running transport (always `OFF` while wired). |
| `opensc2_link_info(&info)` | State, last reason, slot, channel list, protocol version, RSSI, packet and report counters, connection time, and `transport` (the radio fields are 0 for Bluetooth). |

`struct opensc2_config`:

| Field | Meaning |
| --- | --- |
| `identity` | `NULL`, or an identity; empty or zero fields fall back to Kconfig ([Identity](#identity)). |
| `callbacks`, `user_data` | The callbacks below; `user_data` is passed to each. |
| `bond_slot` | 0 or 1, or `OPENSC2_SLOT_STORED` for the slot chosen last (the stored wireless transport, [below](#wireless-transports-and-bluetooth-le)). |
| `start` | Start the wireless transport at once in wireless mode. |
| `boot_mode` | `OPENSC2_BOOT_DEFAULT` (the rule below), `OPENSC2_BOOT_WIRELESS` or `OPENSC2_BOOT_WIRED` for this boot. |

Link states and reasons (`link_changed` gets both):

| State | Means |
| --- | --- |
| `OPENSC2_LINK_OFF` | Radio off: before `opensc2_start()`, after `opensc2_stop()`, after the puck turned the controller off, after `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` or `CONFIG_OPENSC2_NO_BOND_TIMEOUT_S`, or wired. |
| `OPENSC2_LINK_NO_BOND` | Radio on, but the selected slot has no bond: pair first. |
| `OPENSC2_LINK_PAIRING` | Answering a puck's pairing beacon. |
| `OPENSC2_LINK_LISTENING` | Waiting on channel 2 for the bonded puck's connection request. |
| `OPENSC2_LINK_CONNECTED` | On the puck's private pipe; reports flow. |

With Bluetooth as the transport the same states mean: `OFF` Bluetooth stopped, or "wireless off"
(reason `LISTEN_TIMEOUT` or `HOST_DISCONNECTED`); `PAIRING` advertising open to any host (pairing
mode); `LISTENING` advertising for the bonded host only, directed and then allow-listed (and the
500 ms before advertising again after a lost link); `CONNECTED` a host is connected (reports flow
once the link is encrypted and its ATT MTU is 64 or more). The reasons `LINK_LOST`, `DISABLED` and
`PAIRED` apply too.

| Reason | Means |
| --- | --- |
| `LINK_LOST` (1) | 500 ms without a packet from the puck. |
| `DISABLED` (2) | `opensc2_stop()`, a local turn-off, pairing started while connected, or another slot selected. |
| `PUCK_TURN_OFF` (3) | The puck sent "turn off" (the PC went to sleep). The link then goes `OFF`. |
| `BAD_VERSION` (4) | The puck's protocol version is not 0 or 1. |
| `PAIRED`, `PAIRING_FAILED` | End of a pairing window. |
| `LISTEN_TIMEOUT` | `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` passed without the puck; Bluetooth: its advertising window ended without a host (wireless off). |
| `BOND_CHANGED` | A bond or the slot selection changed. |
| `HOST_DISCONNECTED` | Bluetooth: the host ended the link (HCI `0x13`) or powered off (`0x15`): wireless off. |
| `NO_BOND_TIMEOUT` | `CONFIG_OPENSC2_NO_BOND_TIMEOUT_S` passed with the selected slot still empty. |

Reasons 0 to 4 are the controller's own disconnect reasons; the last one is what vendor command
`0xF2` selector 2 reports.

## Modes

Each boot is **wireless** (the puck's radio link) or **wired** (the USB device `28DE:1302`, radio off),
decided once in `opensc2_init()` and kept until the next reboot, the way the real controller decides
at power-up. In order:

| Condition at boot | Mode | `opensc2_mode_reason()` |
| --- | --- | --- |
| built without `CONFIG_OPENSC2_USB` | wireless | `OPENSC2_MODE_NO_USB` |
| `config.boot_mode` is `OPENSC2_BOOT_WIRELESS` / `_WIRED` | as asked | `OPENSC2_MODE_APP` |
| no USB power (the chip's own VBUS detection) | wireless | `OPENSC2_MODE_NO_USB_POWER` |
| the warm-restart mark (Steam's "reboot into wireless mode", `opensc2_reboot(OPENSC2_REBOOT_WIRELESS)`) | wireless | `OPENSC2_MODE_WARM_RESTART` |
| the "wired once" mark (`opensc2_reboot(OPENSC2_REBOOT_WIRED)`) | wired | `OPENSC2_MODE_WIRED_RESTART` |
| the policy is `OPENSC2_POLICY_WIRELESS` | wireless | `OPENSC2_MODE_POLICY` |
| otherwise (USB power, policy auto) | wired | `OPENSC2_MODE_USB_POWER` |
| any wired boot above whose USB device cannot be set up | wireless instead | `OPENSC2_MODE_USB_FAILED` |

The marks live in RAM that start-up does not clear (or, with `CONFIG_OPENSC2_WARM_MARK_GPREGRET2`,
in the nRF52's second retention register); a mark is set only right before the reboot and read and
cleared first thing at the next boot, so it never outlives one reboot. A power loss clears it. Two
differences from the real controller
([protocol.md](protocol.md#where-this-library-had-to-fill-gaps), gap 34): a warm-reboot request that
the system state machine ignores leaves no mark here, and a transport chord or STEAM held at a warm
boot does not cancel the mark.

| Function | Does |
| --- | --- |
| `opensc2_mode()`, `opensc2_mode_reason()` | This boot's mode and why. |
| `opensc2_policy()`, `opensc2_policy_set(p)` | The policy for boots with USB power, stored on the board (`opensc2/mode`): `OPENSC2_POLICY_AUTO` (the real controller's rule) or `OPENSC2_POLICY_WIRELESS` (never wired). The default until one is stored is a Kconfig choice. It decides the next boot's mode; the rule for USB power that appears while wireless (below) follows it at once. |
| `opensc2_reboot(how, why)` | Reboot the way the real controller does: while wired leave the bus first (one all-zero state report, 10 ms, USB off), then set the mark for `how` and reset. `OPENSC2_REBOOT_PLAIN` comes back by the rule (on a PC: wired), `OPENSC2_REBOOT_WIRELESS` comes back wireless also on USB power, `OPENSC2_REBOOT_WIRED` comes back wired once if USB power is there. `why` goes into the USB session log (NULL: the cause of the power request being handled). Call it from a thread, not from the USB stack's thread. Does not return. |
| `opensc2_reboot_later(how, why, delay_ms)` | The same reboot `delay_ms` later from the system work queue; returns at once. Use it where the caller may block afterwards: a shell command's message waits for a terminal to read it, and with the terminal closed the shell can wait for good (the shell's `reboot`, `mode` and `uf2` commands schedule their reboot this way before they print). |

How the application controls switching: the library never reboots on its own while the application
has a `power_request` callback. It asks with `OPENSC2_POWER_REBOOT_KEEP_LINK` (Steam's reboot into
wireless mode, or STEAM pressed while no host has configured the wired device, or, in a charger test
build only, no host within `CONFIG_OPENSC2_NO_HOST_WIRELESS_MS`), `OPENSC2_POWER_REBOOT`,
`OPENSC2_POWER_USB_PLUGGED` and `OPENSC2_POWER_USB_UNPLUGGED` (USB power gone while wired); the
application calls `opensc2_reboot()` when it is ready (see [Power requests](#power-requests)).
Without the callback the library does it at once. The
application can also switch itself: the blank canvas reboots into wireless mode when its pairing
button, pressed before a host configured the board or held for 3 s while wired, is released. A
button still held at the next boot would count as the application's choice
(`OPENSC2_BOOT_WIRELESS`), which wins over the warm-restart mark; acting on the release leaves the
mark to decide.

With `CONFIG_OPENSC2_USB_PLUG_IN_REBOOT`, a wireless board that sees USB power appear while its link
is down, or with STEAM held, asks for `OPENSC2_POWER_USB_PLUGGED` (the real controller reboots and
comes back wired); with the link up it stays wireless and only charges. Only while the policy is
auto: the policy at the moment USB power appears counts, so `opensc2_policy_set()` applies to it at
once. Not when the application chose the mode at boot, nor after a failed wired start. In a battery
build (`CONFIG_OPENSC2_BATTERY`) the [system state machine](#power-and-the-system-state-machine)
applies this rule in its battery state, from its own look at the power source; otherwise the
library checks USB power every 100 ms, as before (also with the machine, while the power source in
effect is USB, so a board with a battery that was built without the option keeps the rule).

### Test builds only: no host, then wireless

> [!WARNING]
> The charger test build (`app/test-charger.conf`, `CONFIG_OPENSC2_NO_HOST_WIRELESS_MS=5000`) is a
> temporary testing aid. It is not the real controller's behaviour and it is not part of the
> default build or any release. A wired boot that no USB host talks to within 5 s reboots into
> wireless mode, so a PC that is asleep or still booting looks like a charger to it. It has not been
> tried on a real charger. Build it only on purpose, in a build folder of its own, and flash the
> default build again when you are done.

`CONFIG_OPENSC2_NO_HOST_WIRELESS_MS` is a **temporary testing aid** for a board with no buttons
wired. It is **not the real controller's behaviour** and is **off (0) by default**; the blank
canvas's default build leaves it off, and only its separate test build turns it on
([app/README.md](../app/README.md#test-build-the-charger-fallback)). The real controller, booted on
USB power, waits until a host sets it up, or until STEAM is pressed, so on a plain USB charger a
board without a STEAM button never reaches the puck. With a time set (1000 to 60000 ms, checked at
build time):

- A boot wired **by the boot rule** (`OPENSC2_MODE_USB_POWER`: USB power, no mark, policy auto)
  that no host has sent a single control request to within that time after the device attached
  asks for `OPENSC2_POWER_REBOOT_KEEP_LINK`, exactly as STEAM does while no host is there; the
  application reboots into wireless mode (without a callback the library does), and the board
  connects through the puck while it charges. Boots asked to be wired (`OPENSC2_BOOT_WIRED`,
  `opensc2 reboot wired`) wait as before.
- **The signal is a setup packet**, the first control request of a host (or the configuration):
  a PC resets the bus and sends one within a second of the attach, a charger, a power bank or a
  powered hub on a PC that is off never sends one. A bus reset alone does not count, so that a
  reset the USB controller might report before its pull-up is on cannot hide a charger. Once a host
  has spoken the board waits for it as long as it takes (a first driver installation can take a
  while).
- **A PC that is asleep, or booting without touching USB** (fast boot), sends nothing either: the
  board goes wireless there too. Replug it once the PC is up to have it wired.
- The pure rule is `osc2_no_host_rule()` (`usb_rules.h`, host-tested). The USB session log notes
  the option at every such wired boot (`test build: wireless after 5000 ms if no host talks`), when
  a host spoke (`a host talked after N ms`), and the fallback (`no request from a host in 5000 ms
  (no bus reset): to wireless`; the session then ends with "no host (charger test build)").
  `opensc2 status` prints a `test aid:` line, the boot log a warning, and the hardware test hooks'
  caps carry build flag 14, which `tools/hwtest` turns into a warning.
- The STEAM exit and the plug-in rule are unchanged; after the fallback the board is wireless with
  USB power already there, so the plug-in rule (which acts when USB power *appears*) does not send
  it back.

## Wired mode

`CONFIG_OPENSC2_USB` (on by default when the devicetree has the node below). The board becomes one
full-speed USB device with the real controller's descriptors:

| | |
| --- | --- |
| Device | `28DE:1302`, strings "Valve Software" (Kconfig), the identity's product name ("Steam Controller" unless one is stored, [Identity](#identity)) and its 13-character unit serial (the same string `0xAE` tag 1 answers); one configuration, bus powered, remote wakeup advertised, 500 mA |
| Interface 0 | HID, no boot protocol; interrupt IN `0x81` and OUT `0x01`, 64 bytes, every 1 ms |
| Report descriptor | the real controller's 372 bytes, byte for byte: a mouse collection (`0x40`), a keyboard collection (`0x41`) and the vendor collection with every other report, in that order (Windows' `Col01`, `Col02`, `Col03`); the same bytes are the Bluetooth report map |
| Form | `CONFIG_OPENSC2_USB_PROFILE_OCTOBER` (default): the October 2026 firmware's, USB 2.01 with a USB 2.0 extension BOS descriptor, class `00/00/00`, release 4.04, 63-byte GET replies. `..._JULY`: USB 2.00, class `EF/02/01`, release 3.07, 64-byte GET replies, the form Steam was seen pairing over the cable with OpenSC2 0.2.0 |
| Feature report 1 | the vendor commands: a 64-byte `SET_REPORT(Feature 1)` runs a command (`01 <opcode> <n> <payload>`) and, when it succeeds, becomes USB's stored request; `GET_REPORT(Feature 1)` answers the stored request, the form's reply length whatever wLength (Steam asks 65 first), the request's bytes and then the last SET's after the answer ([Vendor commands](#vendor-commands)). Anything refused is a STALL: an unknown opcode, a SET that is not 64 bytes or has n above 61, a GET of a command without an answer. Other report types and IDs (also output reports on the control endpoint, feature report 2) stall |
| Output reports | on interrupt OUT, at their declared length or longer, decoded as on the radio link |
| Input reports | the state report `0x42` every 132 ticks of the 32,768 Hz clock (a 4000 µs timer, 4.03 ms) from the moment the host configures the device; the battery report `0x43` every 3.5 s; lizard-mode reports; `opensc2_send_report()`. Eight IN buffers, as on the real controller: every report is copied into a free one or lost (`-ENOMEM`), nothing waits; a state report lost this way keeps its sequence number, so the host sees a gap |
| Suspend | nothing changes, as on the October 2026 controller: reports go on being built and wait in the eight buffers, so after a resume the host first reads up to eight older ones; the rest are lost. `CONFIG_OPENSC2_USB_REMOTE_WAKEUP`: STEAM held about 20 ms wakes the PC, as the July 2026 controller did |
| Leaving | only on the way to a reboot: one all-zero `0x42` (sequence 0), 10 ms, USB off. Steam's `0x95` with `0xA427AF52` is acknowledged and acted on 20 ms later |
| Turn-off | `0x9F` and `0x95` with `0xABDF9710` are acknowledged and ignored while wired (the real controller's wired state does not act on them either) |
| STEAM held at a wired boot | the device goes on the bus once STEAM is released, as on the real controller (`OPENSC2_USB_OFF` until then). The library sees a press the application reported before `opensc2_init()` (the blank canvas reports the buttons held at power-up) |
| While waiting for the host | `OPENSC2_USB_ATTACHED` for 0.5 s, then `OPENSC2_USB_NO_HOST`; STEAM pressed asks for `OPENSC2_POWER_REBOOT_KEEP_LINK` (back to wireless, as the real controller). In a charger test build only, no request from a host within `CONFIG_OPENSC2_NO_HOST_WIRELESS_MS` asks for it too ([above](#test-builds-only-no-host-then-wireless)) |
| Cable pulled | matters only to a board that keeps running (a battery). USB power gone for 0.2 s: the reports stop, the state goes `OFF`, and the library asks for `OPENSC2_POWER_USB_UNPLUGGED` (without a callback: a plain reboot, which comes back wireless). A shorter dip changes nothing. The real controller shuts down, and so does the library with `CONFIG_OPENSC2_SYSTEM` (the default): the device leaves the bus, then the [shutdown](#shutdown-and-power-off) runs and ends in `OPENSC2_POWER_OFF` on a battery build, a reboot otherwise |

`opensc2_usb_state()` and `opensc2_usb_info(&info)` give the state (`OFF`, `ATTACHED`, `NO_HOST`,
`CONFIGURED`), whether the bus is suspended, and counters. When the host deconfigures the device (a
bus reset does) the state goes back to `ATTACHED` until it configures it again; `info.active` says
whether the host has the device configured right now. The `usb_changed` callback reports each change.

What a project needs for it (the [minimal sample](../samples/minimal) and the
[template](../template) have all of it, and [integration.md](integration.md#devicetree) explains each
part):

```dts
/ {
    chosen {
        opensc2,usb-hid = &opensc2_hid;
        opensc2,retained-ram = &opensc2_retained;   /* recommended: see the session log */
    };
    opensc2_hid: opensc2_hid {
        compatible = "zephyr,hid-device";
        in-report-size = <64>;
        in-polling-period-us = <1000>;
        out-report-size = <64>;
        out-polling-period-us = <1000>;
    };
    opensc2_retained: memory@2002c000 {
        compatible = "zephyr,memory-region", "mmio-sram";
        reg = <0x2002c000 DT_SIZE_K(16)>;
        zephyr,memory-region = "OPENSC2_RETAINED";
    };
};
&sram0 { reg = <0x20000000 DT_SIZE_K(176)>; };   /* the application's RAM ends below it */
```

```text
CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n   # the library runs the USB device
CONFIG_USBD_HID_IN_BUF_COUNT=8
CONFIG_USBD_THREAD_STACK_SIZE=2048           # commands write flash in the USB thread
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048
CONFIG_LOG_PROCESS_THREAD_STACK_SIZE=1536    # with CONFIG_LOG: the log thread feeds the session log
```

and Zephyr's USB device stack (`CONFIG_USB_DEVICE_STACK_NEXT`; the SuperMini's board files enable
it). In wireless mode `CONFIG_OPENSC2_USB_CONSOLE` shows the board's CDC-ACM serial port instead
(non-Valve IDs `2FE3:0004` by default), or nothing with it off.

### The USB session log

A wired board has no console. `CONFIG_OPENSC2_USB_LOG` keeps a ring of short text lines in RAM that
start-up does not clear, with a header (magic, version, boot counter, write position, length, CRC):
the boot and its mode, every control request (`CONFIG_OPENSC2_USB_LOG_SETUP`: the setup packets,
also the standard ones the stack answers), each feature request with its opcode, length, setting
numbers or key names, OK or STALL with the reason and its hold time, output reports, bus events, the
state-report counters every second (every 10 s after the first 10), lizard mode, the reason the
session ended, and while wired the Zephyr log (`CONFIG_OPENSC2_USB_LOG_ZEPHYR`). Bond values and the
board's serials never go into it. It survives soft, pin and watchdog resets, not a power loss. Put
it in a devicetree region chosen as `opensc2,retained-ram`, outside the RAM the bootloader uses at a
reset ([porting.md](porting.md#where-the-retained-region-may-go) gives the UF2 bootloader's ranges; the SuperMini overlays'
region at `0x2002C000` is clear of them). Without one it is `CONFIG_OPENSC2_USB_LOG_SIZE` bytes of the
application's no-init RAM, which a bootloader that runs at every reset may overwrite: the build warns,
and on nRF52 the warm-restart mark then defaults to GPREGRET2. The shell prints it
(`opensc2 usb log`); `opensc2 status` shows the summary of the last wired session (requests, stalls,
reports, how it ended).

## Inputs

Set fields with the setters, or the whole `struct opensc2_state` with `opensc2_set_state()`. All
setters are thread safe, cheap, and usable from interrupts. Whatever you never set stays zero, which
reads as neutral. The library builds a state report (input report `0x42`, 53 bytes) for every packet
the puck sends, about every 3 ms, and every 4.03 ms on USB; the optional `sample` callback runs just
before each one, so a project can also sample there. One sequence number counts the reports of both
links.

| Input | Setter / field | Type, range, unit | Report `0x42` (offset after the ID) | In Steam / SDL |
| --- | --- | --- | --- | --- |
| Buttons and touch bits | `opensc2_set_buttons(mask, value)`, `state.buttons` | `uint32_t` bit mask, table below | 1, u32 | buttons, D-pad (SDL: hat), cap-sense touches |
| Left / right trigger | `opensc2_set_trigger(side, v)`, `state.trigger[2]` | 0 (released) .. 32767 (pulled) | 5, 7, u16 | trigger axes (SDL: `v * 2 - 32768`) |
| Trigger digital bits | automatic, or set yourself | bits 27 (left), 23 (right) | in the button word | not mapped by SDL |
| Left stick | `opensc2_set_stick(OPENSC2_LEFT, x, y)` | -32767 .. 32767; right and **up** positive | 9, 11, s16 | left stick (SDL negates Y) |
| Right stick | `opensc2_set_stick(OPENSC2_RIGHT, x, y)` | same | 13, 15, s16 | right stick |
| Left / right trackpad | `opensc2_set_trackpad(side, &pad)` | `x`, `y`: -32767 .. 32767, right and up positive; `pressure`: 0 .. 32767; `touched`, `clicked` | 17..22 (left), 23..28 (right): x, y s16, pressure u16 | two touchpads (SDL: position `x / 65536 + 0.5`, pressure `/ 32768`), pad clicks as buttons |
| IMU timestamp | `opensc2_set_imu(&imu)`, `imu.timestamp_us` | µs, wrapping u32 | 29, u32 | sensor timestamps |
| Acceleration | `imu.accel[3]` X, Y, Z | 16384 per g (32767 = 2 g) | 33, 35, 37, s16 | accelerometer |
| Angular rate | `imu.gyro[3]` X, Y, Z | 32767 = 2000 °/s (16.384 per °/s) | 39, 41, 43, s16 | gyroscope (gyro controls) |
| Orientation | `imu.quat[4]` W, X, Y, Z | 32767 = 1.0; all zero = none, which the reports carry as the identity (W 32767), as the real controller never sends W = 0 | 45 (W), 47, 49, 51 | not read by SDL |
| Battery | `opensc2_set_battery(&b)` | see below | input report `0x43`, every 3.5 s | battery level and charge state (SDL power info) |

Axes, as the real controller: X right, Y away from the player, Z up when it lies flat. SDL turns them
into its own frame (it uses report X, Z, -Y).

An untouched trackpad is sent as position 0, 0. The pad's `touched` and `clicked` flags set its
touch and click bits; you can also set those bits directly in the button word.

### The rules on every report

Before a state report goes out, the library applies the real controller's rules to it, in this
order (the hardware test hooks' injected state goes through them too). They change the report only;
`opensc2_get_state()` keeps what the application set.

| Rule | Kconfig (default) | What it does |
| --- | --- | --- |
| Stick-touch fallback | `CONFIG_OPENSC2_STICK_TOUCH_FALLBACK` (y) | a stick further than 12000 from its centre counts as touched: its stick-touch bit (20 right, 24 left) is set. The distance is the real controller's quick approximation of √(x² + y²) (about 12200 along an axis, 12340 on a diagonal); no hysteresis; never while setting 46 asks for raw sticks |
| Click by pressure | `CONFIG_OPENSC2_PAD_CLICK_AUTO` (n) | the pads' click bits (22 right, 26 left) come from their pressure, as on the real controller, whose pads have no switch: on above setting 53 (right) or 52 (left) plus 8 points, off below it minus 8 (percent of 32767; default 40, so on above 48 % and off below 32 %; -1 counts as 100); a new threshold clears the click; the click follows the pressure while the pad is not touched, but only a touched pad reports it. It replaces the application's own click bits. Off by default: a board with a physical click keeps it, and a pressure that is not a real press would click on every touch |
| Setting 82 | `CONFIG_OPENSC2_PAD_BLANK_ON_STICK` (y) | while a stick is touched (its sensor or the fallback), the pad on its side reports no touch, no click, position 0, 0 and pressure 0 (setting 60 = 1 keeps the pressure); bit 0 of setting 82 is the left side, bit 1 the right, default 3. The stick-touch and grip bits stay |
| Trigger bits | `CONFIG_OPENSC2_TRIGGER_CLICK_AUTO` (y) | below |
| Raw triggers | always | while setting 62 is not 0 (raw trigger output), both trigger bits are 0, also ones the application set |

### The button word

Bit names follow SDL's `TRITON_*` flags; the bit numbers are also in
`<dt-bindings/opensc2/inputs.h>` as `OPENSC2_KEY_*`, for devicetree overlays.

| Bit | Mask macro | Button | Bit | Mask macro | Button |
| --- | --- | --- | --- | --- | --- |
| 0 | `OPENSC2_BTN_A` | A | 15 | `OPENSC2_BTN_L3` | left stick click |
| 1 | `OPENSC2_BTN_B` | B | 16 | `OPENSC2_BTN_STEAM` | STEAM |
| 2 | `OPENSC2_BTN_X` | X | 17 | `OPENSC2_BTN_L4` | left upper grip |
| 3 | `OPENSC2_BTN_Y` | Y | 18 | `OPENSC2_BTN_L5` | left lower grip |
| 4 | `OPENSC2_BTN_QAM` | quick access ("...") | 19 | `OPENSC2_BTN_L` | left bumper |
| 5 | `OPENSC2_BTN_R3` | right stick click | 20 | `OPENSC2_TOUCH_RIGHT_STICK` | thumb on the right stick |
| 6 | `OPENSC2_BTN_VIEW` | VIEW (right-hand menu button) | 21 | `OPENSC2_TOUCH_RIGHT_PAD` | right pad touched |
| 7 | `OPENSC2_BTN_R4` | right upper grip | 22 | `OPENSC2_CLICK_RIGHT_PAD` | right pad clicked |
| 8 | `OPENSC2_BTN_R5` | right lower grip | 23 | `OPENSC2_CLICK_RIGHT_TRIGGER` | right trigger past its threshold |
| 9 | `OPENSC2_BTN_R` | right bumper | 24 | `OPENSC2_TOUCH_LEFT_STICK` | thumb on the left stick |
| 10 | `OPENSC2_BTN_DPAD_DOWN` | D-pad down | 25 | `OPENSC2_TOUCH_LEFT_PAD` | left pad touched |
| 11 | `OPENSC2_BTN_DPAD_RIGHT` | D-pad right | 26 | `OPENSC2_CLICK_LEFT_PAD` | left pad clicked |
| 12 | `OPENSC2_BTN_DPAD_LEFT` | D-pad left | 27 | `OPENSC2_CLICK_LEFT_TRIGGER` | left trigger past its threshold |
| 13 | `OPENSC2_BTN_DPAD_UP` | D-pad up | 28 | `OPENSC2_TOUCH_RIGHT_GRIP` | right grip held |
| 14 | `OPENSC2_BTN_MENU` | MENU (left-hand view button) | 29 | `OPENSC2_TOUCH_LEFT_GRIP` | left grip held |

The controller's own firmware calls bit 6 "MENU" and bit 14 "Left view"; SDL calls them VIEW and
MENU. The names here are SDL's. SDL maps MENU to its Back button and VIEW to Start.

With `CONFIG_OPENSC2_TRIGGER_CLICK_AUTO` (default on) the trigger bits follow the trigger values as on
the real controller: on above controller setting 68 plus 5 points (95 % by default), off below it
minus 5 (85 %); a fully pulled trigger always counts. A digital trigger button can therefore just
call `opensc2_set_trigger(side, pressed ? 32767 : 0)`. While setting 62 asks for raw trigger output
both bits are 0 ([the rules](#the-rules-on-every-report)).

### Battery

`struct opensc2_battery`, sent as input report `0x43` every `CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS`
from boot on whatever link is up (dropped while there is none), and answered to vendor command
`0xBE`. Until set: 100 %, "charging done".

| Field | Unit | Note |
| --- | --- | --- |
| `charge_state` | `OPENSC2_CHARGE_DISCHARGING` (1), `_CHARGING` (2), `_DONE` (4), `_RESET` (0), `_SRC_VALIDATE` (3) | SDL's `EChargeState`, the numbering the real controller uses; SDL shows 1 as on battery, 2 charging, 4 charged |
| `level` | percent | the battery level Steam shows |
| `battery_mv`, `system_mv`, `input_mv` | mV | |
| `current_ma`, `input_current_ma` | mA | battery current signed |
| `temperature` | raw | the charger's raw value on the real controller |
| `gauge_flags` | | not sent (it was `0xBE`'s byte 13 in 0.2.0); `0xBE` now derives that byte from `charge_state`, as the real controller does |

`0xBE` answers 15 bytes: temperature, battery, system and input mV, current, input current, the
level, then 1 while `charge_state` is `OPENSC2_CHARGE_CHARGING` (else 0) and `charge_state` without
a `0x80` flag. `0x43` sends `charge_state` as given (the real controller adds `0x80` while its
supply check has given up; [protocol.md, gap 3](protocol.md#where-this-library-had-to-fill-gaps)).

`OPENSC2_CHARGE_GAVE_UP` (`0x80`) may be added to `charge_state`: the real controller sets it while
its charger has given up on a supply that does not charge the battery (it tries again 60 s later).
Report `0x43` carries it as given; while it is set and the level is above 0, the
[system state machine](#low-battery) and the [status LED](#status-led) are told 100 %, and the
report keeps the measured level. The library never adds it itself (it has no charger logic).

`opensc2_charge_state_of(input_power, charging)` (pure) gives the state for a charger with a status
output, such as a TP4056 module's CHRG pin: without input power 1 (on battery), with it 2 while the
charger charges and 4 (done; report a level of 100) otherwise. The blank canvas uses it for an
optional charger status pin ([app/README.md](../app/README.md#optional-parts); untested).

With the system state machine the level also counts for the power rules ([Low battery](#low-battery)),
from the first `opensc2_set_battery()` on: a board that never sets a battery is never low.

### Other input reports

`opensc2_send_report(report, len)` sends any input report (ID first, at most 64 bytes) on the link in
use: through the puck it rides with the next state reports, on USB it goes out on the interrupt
endpoint. The puck only passes IDs `0x40` to `0x45` with exactly their descriptor length plus one:
`0x40` mouse (5), `0x41` keyboard (8; only Enter, Escape, Tab and the arrows, no modifiers), `0x42`
state (53), `0x43` battery (14), `0x44` haptic stream status (5), `0x45` (45).
`opensc2_send_haptic_status(channel, flags)` builds `0x44`: channel left grip 0, right grip 1, left
pad 3, right pad 4; flags `0x01` bytes dropped, `0x02` ran dry or stopped, `0x04` below the water
mark (send more), `0x08` at the water mark (pause), `0x10` the format needs an 8 kHz channel, `0x20`
format set, `0x40` format cannot change while playing.

The library sends one kind of `0x44` itself, as the real controller does
(`CONFIG_OPENSC2_HAPTIC_STOP_STATUS`, on by default): every stop-all, that is output report `0x81`
with an on time of 0, `0x82` with command 0 or a speed of 0 in `0x80` (for that grip), and `0x86`'s
stream stop, gets `0x44 {channel, 0x02}` for each channel it reaches, whether or not anything was
streaming, on the current link (the one a host's output came on), right after the `haptic` or
`rumble` callback. The order is the real controller's: the left channel first for `0x81` and
`0x82`, the right first for `0x86`, the right grip before the left for `0x80`. Steam's pair of
all-zero `0x81` reports (target 0, then 1) gets `44 04 02 00 00 00`, then `44 03 02 00 00 00`.
An application whose own haptics engine sends these can turn the option off; the library's
[haptics synthesiser](#the-haptics-synthesiser) selects it, as it sends no `0x02` for a stop.
Turn the option off when your haptics engine sends its own `0x44` reports.

### Lizard mode

Lizard mode: until Steam takes over, a Steam Controller 2 acts as a plain mouse and keyboard (the
right trackpad moves the pointer, A is Enter, B is Escape, the D-pad gives the arrow keys). Steam
switches it off while it runs; about 10 seconds after Steam stops, it comes back. OpenSC2 does the
same, on every link. With `CONFIG_OPENSC2_LIZARD_MODE` (default on) the library builds the mouse and
keyboard reports from the input state:

- **On** at every boot (controller setting 9 = 1; settings live in RAM). Coming back on (setting 9
  = 1, `0x8E`, the Steam watchdog) sends one empty keyboard report `0x41`, then one all-zero mouse
  report `0x40`, in the real controller's order.
- **Off** with vendor command `0x81` (the first one sends one empty keyboard report `0x41`, then one
  all-zero mouse report `0x40`, releasing whatever was held) or setting 9 = 0. The release reports
  always come after every report lizard mode made before it turned off, also on USB, where the
  command runs in the USB stack's thread and the reports in the report thread. `0x85` only brings
  the keyboard and mouse buttons back when setting 9 is on. `0x8E` (all defaults) turns it back on.
- **The Steam watchdog** (`CONFIG_OPENSC2_STEAM_WATCHDOG`, controller setting 71): a 10 s timer,
  armed at boot and re-armed by every `0x81` and every write of setting 9 = 0, independent of the
  link. When it runs out with lizard mode off, lizard mode comes back and the settings return to
  their defaults (the IMU settings 48, 84 and 85 keep theirs). Setting 71 = 0 stops it.
- **Mouse** `0x40`: the right trackpad moves the pointer (128 pad units per count; pad up = pointer
  up), the left one scrolls (1024 per step; moving up gives a negative wheel value, right a positive
  pan); the right trigger or a right-pad click is button 1, the left trigger button 2. A pad is
  ignored while its stick is touched (setting 82). A report goes out every frame while anything is
  non-zero (a held button repeats, a glide keeps moving), then one zero report.
- **Keyboard** `0x41`: A is Enter, B and VIEW are Escape, the D-pad the arrows, MENU is Tab. When one
  of those changes, every held button in bit order takes the next key slot (its key, or 0), and more
  than six held buttons give the roll-over code in every slot.
- **The motion**, as the real controller works it out, once per frame (each state report), each part
  an option (all on by default):
  - the touch stabilisers (`CONFIG_OPENSC2_LIZARD_STABILISER`): a new touch does not move until the
    thumb has left a hold radius around the touch-down point, then the motion follows the thumb and
    closes the gap gradually; the right pad's radius is 4369 pad units while setting 80 is 1 (the
    default), the left pad's 8738 always;
  - the 8-frame average (`CONFIG_OPENSC2_LIZARD_SMOOTHING`): each frame's motion is the mean of the
    newest 8 movements of the touch (or of as many as there are), about 32 ms on the cable;
  - the glide after a flick (`CONFIG_OPENSC2_LIZARD_GLIDE`): when a touch of more than 20 frames and
    700 pad units of travel ends faster than setting 11 (default 200 pad units per frame, the mean
    of the 15 oldest of its last 20 movements), the motion goes on, slowing down to nothing over
    setting 12 frames (default 100, about 0.4 s on the cable); a new touch on that pad or lizard
    mode turning off stops it; while setting 82 blanks the pad its motion is dropped, not saved
    for later, and the slowing down goes on; the left pad glides too;
  - the haptic ticks and clicks (`CONFIG_OPENSC2_LIZARD_TICKS`), while setting 70 is 1: a step each
    time the thumb has moved more than 1500 pad units from the last one, and a tick once the steps
    times 1500 exceed setting 14 (5500: every fourth step); click B when a pad's click turns on or
    off, unless its threshold (52, 53) is -1. Each is a [haptic event](#outputs) from the library.
- **Through the puck**, lizard mode's reports wait for room in the radio's queue instead of being
  dropped (`CONFIG_OPENSC2_REPORT_WAIT_DEPTH`, 8 beyond the queue's 6), and lizard mode waits with
  its next frame while they do; the next frame takes up the motion. A dropped link does not start
  the trackers over, as on the real controller: no frames run meanwhile, and the first frame after
  the gap compares the pads with the frame before it.

## Calibration and raw inputs

`<opensc2/cal.h>`, with `CONFIG_OPENSC2_CAL` (on by default). The real controller turns raw sensor
readings into the report's values itself, with a calibration stored on the controller. The
library can do the same: an application that gives it **raw inputs** gets the real controller's
processing, and the calibration commands work as on the real controller. An application that gives
finished values (the setters above) is not affected; raw and finished inputs can be mixed, input by
input.

| Raw input | Setter | Unit | Becomes |
| --- | --- | --- | --- |
| a stick | `opensc2_set_stick_raw(side, x, y)` | 12-bit ADC counts, the average of a burst (0..4095), right and up the higher values | the stick, from `cal/joy_l` / `cal/joy_r`; setting 46 = 1 gives millivolts, 2 the counts |
| a trigger | `opensc2_set_trigger_raw(side, counts)` | 12-bit counts | the trigger, from `cal/trg_l` / `cal/trg_r`; setting 62 = 1 millivolts, 2 counts |
| a pad's pressure | `opensc2_set_pressure_raw(side, word)` | the sensor's own signed 16-bit word | the pad's pressure (filtered, from `cal/prs_l` / `cal/prs_r`, 0 while not touched) and, with `CONFIG_OPENSC2_CAL_PRESSURE_CLICK`, its click bit (settings 52, 53); setting 60 = 1 gives the raw word |
| capacitive words | `opensc2_set_touch_raw(side, stick_word, grip_word)` | the controller's units | the stick-touch bit (settings 30, 31) and the grip bit (settings 34, 35) |
| all at once | `opensc2_set_raw(&raw)` | `struct opensc2_raw_inputs`, the inputs given in `present` (`OPENSC2_RAW_*` bits) | |

The library processes them once per state report, before the hardware test hooks' injection;
give them from the `sample` callback to have them sampled at the last moment. An input stays raw
until `opensc2_cal_raw_release(OPENSC2_RAW_*)` gives it back to the finished-value setters
(`OPENSC2_RAW_ALL` for every input). A pad
given raw pressure keeps its touch flag and position from `opensc2_set_trackpad()`.

The "millivolts" are the real controller's: counts × 3300 / 4096 for a stick (full scale VDD) and
counts × 1650 / 4096 for a trigger (full scale VDD / 2), whatever the supply. A board whose trigger
is read with another gain still works: its stored calibration is in the same units.

**Uncalibrated, a raw stick, trigger or pressure reads 0** (and a pad without pressure calibration
never clicks), as on the real controller. A new board has no calibration: record one with the
commands below, or keep giving finished values until it has one (`opensc2_cal_stick_ok(side)`,
`_trigger_ok()`, `_pressure_ok()`), and raw ones while its recording runs
(`opensc2_cal_recording(OPENSC2_CAL_REC_STICKS)`, `_TRIGGERS`, `_PRESSURE`): a recording takes its
samples from raw inputs only. The blank canvas's options do both. How each is processed:

- **Sticks.** Each axis split at the middle of its rest band into two halves, each scaled so that
  full deflection comes 90 % of the way to the stored extreme, truncated toward zero; no dead zone,
  no filter. Usable with format 2 or `0x81` and each extreme at least 200 counts from its end of
  the rest band. `CONFIG_OPENSC2_CAL_STICK_DEAD_ZONE` adds a centre dead zone of OpenSC2's own (for
  potentiometer sticks), `CONFIG_OPENSC2_CAL_STICK_GROW_RANGE` July 2026's run-time range.
- **Triggers.** From the released end to the pulled end moved 2 % toward it: 0 over the first 5 %,
  full from 95 % on; the direction comes from the calibration. Usable with A and B at least 100
  counts apart.
- **Pad pressure.** A filter (p = (3 p + raw) / 4, stepped only while the calibration is usable;
  without one p keeps its value), a 5 % band at both ends of the calibrated range, the calibration's
  gain; the click on above (setting 52 or 53 + 8) %, off below (that − 8) %, with −1 counting as
  100.
- **Stick touch and grip.** The mean of the last two words against setting 30 (on, default 500)
  and 31 (held, 400), and against (setting 34 / 2 + 12.5) % of 1800 (on, 1125) and setting 35 % of
  that (held, 900). A change of settings 30, 31, 33, 34, 35, 52, 53, 66, 72 or 73 starts both pads
  over (these means, the pad's click and its stabiliser of the pad rules below), and a load of a
  pad's pressure key starts that pad over.

**The stored calibration** lives under `opensc2/cal/` in Zephyr settings (the storage partition,
which a `.uf2` reflash keeps); a factory reset (`0x86`) keeps it. The keys, by the real names a host
uses with the by-name commands:

| Key | Size | Content |
| --- | --- | --- |
| `cal/joy_l`, `cal/joy_r` | 18 | format u16 (2 or `0x81`), then X minimum, maximum, rest low, rest high, then Y, u16 mV each |
| `cal/trg_l`, `cal/trg_r` | 7 | format u16, A (the higher mV) u16, B (the lower) u16, direction u8 (0: rises when pulled) |
| `cal/prs_l`, `cal/prs_r` | 8 | format u16 (1: range A..B; `0x80`: 0..B−A), A s16, B s16 (a firm press), gain u16 (2000 = 1) |
| `cal/touch_l`, `cal/touch_r` | 10 | format u16 first (the rest of the layout is unknown); kept, and readable while the format word is not 0; nothing uses them |
| `cal/rgbw_r`, `_g`, `_b`, `_w` | 1 each | LED colour trims, percent, used whatever their value as on the real controller (0 leaves the channel dark, above 100 is brighter up to full duty; `0xED` reads one only while it lies within 1..199); 90, 35, 120, 80 from a boot with none stored, 0 after `0xF0` until the next boot: `opensc2_cal_led_trim(channel)` |
| `cal/volt_offset` | 2 | battery voltage offset, s16 mV: `opensc2_cal_volt_offset()` |
| `cal/sensors/gyroscope/bias` | 12 | three floats (rad/s), a starting gyro bias, taken as the real controller takes it (a NaN as 0; 2^31 or more in magnitude, an infinity too, refused with `-ERANGE`, and the real controller then leaves its IMU stream off): `opensc2_cal_gyro_bias(bias)` |

The sizes and formats have names in `<opensc2/cal.h>`: `OPENSC2_CAL_STICK_LEN` (18), `_TRIGGER_LEN`
(7), `_PRESSURE_LEN` (8), `_TOUCH_LEN` (10) and `_GYRO_BIAS_LEN` (12); `OPENSC2_CAL_FORMAT_ANALOG` (2)
and `_ANALOG_ALT` (`0x81`) for sticks and triggers, `_PRESSURE` (1) and `_PRESSURE_SPAN` (`0x80`) for
pressure; `OPENSC2_CAL_PRESSURE_GAIN_ONE` (2000) is a gain of 1, and `OPENSC2_CAL_ADC_MAX` (4095) the
top of a 12-bit reading.

`opensc2_cal_generation()` changes whenever a stick, trigger or pressure key changes in RAM, for an
application that processes its own inputs with the [calibration helpers](#calibration-helpers); the
LED trims, the battery offset and the gyro bias do not change it (their functions read the RAM
value each time).

**The commands** (on every link, as on the real controller; the [test suite](testing.md) never
sends them):

| Opcode | Payload | Does |
| --- | --- | --- |
| `0xD8` | `[1]` the step (`[0]` is not read) | stick calibration from the raw sticks: 1 records the rest band, 2 full circles, 3 saves `cal/joy_l` and `cal/joy_r` at the next frame, 0 ends without saving |
| `0xC0` | `[0]` the step | triggers: 1 released, 2 pulled, 3 saves both (each keeps the direction whose A and B lie further apart), 0 ends |
| `0xC3` | `[0]` the step | pad pressure: 1 start, 2 the firmest press, 3 saves both at once, 0 pauses |
| `0xE2` | side, mask | saves one side's RAM values (mask bit 0 the trigger, 1 the stick, 2 the pressure); a side above 1 is refused with -1 (`-EPERM`), the real controller's code |
| `0xFE` | the 32-bit key `0xC6674885` | deletes every `cal/` key from flash; the RAM copies stay in use until the next boot; any other key is refused |
| `0xED`, `0xEE`, `0xEF`, `0xF0` | a `cal/` key | read, set in RAM (exactly the key's size), save, delete, as in [Vendor commands](#vendor-commands) |

A recording takes the samples of the raw inputs, one per state report, so only while a link is up,
and saves only the sides that gave samples. Its flash writes, and those of `0xE2` and `0xFE`, run
from the system work queue in the order they were asked for (`0xFE` drops the saves still waiting
from before it); `0xEF` and `0xF0` of a `cal/` key wait for them, then write before they answer, so
a host's "erase, then store the new calibration" keeps the new keys. Steps above 3 do nothing; a
step 2 or 3 without a step 1 does nothing (the real controller's `0xC3` step 3 saves even then,
wiping the pads' pressure calibration:
[protocol.md](protocol.md#where-this-library-had-to-fill-gaps), gap 31).

**The pad rules** (`CONFIG_OPENSC2_PAD_RULES`, on by default) act on the reported pads, raw or not,
after the hardware test hooks' injection, so report `0x42` and lizard mode both see them: with
settings 2 and 24 both non-zero the pads turn by setting 2's angle (the right pad
counter-clockwise, the left clockwise, in the real controller's angle steps), and with setting 80 =
2 the touch stabiliser holds a new touch still until the thumb moves more than 4369 units, then
follows it. Both are off with the default settings. The position filter (setting 65) and the pad
touch levels (72, 73) belong to the trackpad's own processing: [sensor helpers](#sensor-helpers).

## Outputs

All callbacks are in `struct opensc2_callbacks`; any may be `NULL`. Through the puck they run in the
library's radio thread; wired, in its USB report thread (`sample`, and `haptic` for lizard mode's
own events) or the USB stack's thread (output reports, vendor commands), so on the cable `haptic`
can come from either of those two threads. Over Bluetooth (`CONFIG_OPENSC2_BLE`) `sample` and lizard
mode's `haptic` events run in the Bluetooth report thread (`opensc2_blerpt`, priority 4, above the
radio thread), output reports, vendor commands, `setting_changed` and `led` in the Bluetooth host's
receive thread, and `link_changed` in the Bluetooth thread (`opensc2_ble`). With the dock link, the
dock's vendor commands, and `sample` while the dock is the only link, run in the dock thread (or in
the thread of a test op that hands the dock a frame). `power_request`, `usb_changed`, `led_status`,
`cue` and a rumble timeout run in the system work queue. So a callback can come from more than one
thread: guard state that two of them share. Return quickly: the radio link only lives while
acknowledgements keep up with the puck.

| Callback | Called for | Gets |
| --- | --- | --- |
| `link_changed(info)` | every state change of the wireless link, the puck's radio or Bluetooth | `struct opensc2_link_info` (`transport` says which) |
| `sample(state)` | before each state report | the state, to update in place (it is kept) |
| `rumble(r)` | output report `0x80` | `left_speed`, `right_speed` 0..65535 (0 stops), `intensity`, `left/right_gain_db`, `type`; `timeout` set when the library stops a rumble the host did not repeat (`CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS`, 500) or the link dropped (a wireless link lost, the cable pulled; a host that deconfigures the USB device leaves the rumble to its 500 ms, as on the real controller) |
| `haptic(h)` | output reports `0x81`..`0x89`; lizard mode's ticks and pad clicks | `kind`, `channels`, raw `target`, and the decoded fields below; `report_id` is the output report's ID, or 0 for an event of the library's own |
| `output_report(report, len)` | any output report, raw | ID first |
| `led(led)` | vendor command `0xC5`, settings 37 and 45 | `red`, `green`, `blue`, `white` percent; `override` (setting 37: show this instead of the status pattern); `brightness` (setting 45, 0..100, default 50) |
| `setting_changed(number, value)` | a setting changed value (vendor command `0x87`, `0x81` for setting 9), or all of them went back to their defaults (`0x8E`, the Steam watchdog: `OPENSC2_SETTINGS_ALL`) | |
| `host_asleep()` | the puck says the PC is asleep (the link stays up) | the real controller plays a short cue: with the system state machine the library raises it (`cue` 10, once per boot) |
| `power_request(req)` | see below | |
| `vendor_command(cmd, answer, answer_len)` | vendor commands the library does not answer itself | see [Vendor commands](#vendor-commands) |
| `usb_changed(info)` | wired: the USB device's state changed | `struct opensc2_usb_info` |
| `led_status(status)` | `CONFIG_OPENSC2_SYSTEM`: the status LED's mode, its flags (`external_power`, `steady`, `shutting_down`) or the battery level it is told changed. The other fields can change without a call | `struct opensc2_led_status` ([Status LED](#status-led)); `opensc2_led_status()` gives all of it at any time |
| `cue(cue)` | `CONFIG_OPENSC2_SYSTEM`: a cue to play; only cues that are not silent at the chime level come here | `struct opensc2_cue_event`: `cue`, the built-in `script` it stands for, `gain_db` ([Cues](#cues)) |

Haptic channels: `OPENSC2_HAPTIC_LEFT_PAD`, `_RIGHT_PAD`, `_LEFT_GRIP`, `_RIGHT_GRIP` (the real
controller's four actuators). `channels` is the target byte decoded the way the real controller
decodes it, including its quirks: report `0x81` swaps the two pads for the exact targets 0 and 1
(while `0x80` and `0x81` there are the left and the right pad, as in the other reports), and the PCM
reports number their targets differently again (in `0x87`, target 1 selects nothing and 4 the right
grip).

| `kind` | Report | Fields |
| --- | --- | --- |
| `OPENSC2_HAPTIC_PULSE` | `0x81` | `pulse.on_us`, `off_us`, `count`. The real controller: on time 0 stops everything, off time 0 plays click A, a count of 3 or 4 click B. |
| `OPENSC2_HAPTIC_COMMAND` | `0x82` | `command.command`: 0 stop all, 1 click A, 2 click B; `command.gain_db` |
| `OPENSC2_HAPTIC_TONE` | `0x83` | `tone.gain_db`, `freq_hz`, `duration_ms`, `lfo_hz`, `lfo_depth` (percent) |
| `OPENSC2_HAPTIC_SWEEP` | `0x84` | `sweep.gain_db`, `duration_ms`, `start_hz`, `end_hz`, `antiphase` (targets 6 and 7) |
| `OPENSC2_HAPTIC_SCRIPT` | `0x85` | `script.id` (the real controller's built-in scripts 1..17), `script.gain_db` |
| `OPENSC2_HAPTIC_PCM_CONTROL` | `0x86` | `pcm_control.operation` (0 nothing, 1 stop, 2 set format), `format`, `encoding` (s16le, s8, mu-law), `rate_hz` (8000, 4000, 2000, 1000) |
| `OPENSC2_HAPTIC_PCM_DATA` | `0x87`, `0x88` (twice: left grip, right grip; a `0x88` with more than 31 bytes a side does nothing, as on the real controller), `0x89` | `pcm.data`, `pcm.len` (valid during the callback only) |

Report `0x8A` does nothing, as on the real controller.

**The library's own events** (`report_id` 0): lizard mode's pad feedback
([Lizard mode](#lizard-mode)), as `OPENSC2_HAPTIC_COMMAND` for one pad (`target` 0 left, 1 right, as
`0x82` numbers them, and the matching channel): a tick is `OPENSC2_HAPTIC_CMD_CLICK_A` with
`gain_db` 8, a pad click (on or off) `OPENSC2_HAPTIC_CMD_CLICK_B` with `gain_db` 6. Their gain is
the level the sound plays at, without the extra 6 dB click A gets in `0x82`. They come from the
thread that builds the state reports, after the frame's reports, and never once Steam has turned
lizard mode off.

The callbacks hand over what Steam asked for; playing it on actuators is the application's job,
unless the library's [haptics synthesiser](#the-haptics-synthesiser) (an option) renders it.

### Power requests

`power_request(req)`, in the system work queue; vendor commands are acted on about 20 ms after they
arrive, so their answer still goes out. Without the callback the library does the suggested action
itself where it is a reboot, and ignores the others. With `CONFIG_OPENSC2_SYSTEM` (the default) the
[system state machine](#power-and-the-system-state-machine) takes the turn-off command and the
cable pulled itself, and a host's reboot or bootloader request reaches the application only in a
state that acts on it (in the others it is logged and ignored, as on the real controller); at the
end of a shutdown the application gets `OPENSC2_POWER_OFF` or a reboot.

| Request | From | Suggested action |
| --- | --- | --- |
| `OPENSC2_POWER_TURN_OFF_PUCK` | the puck's "turn off" (the PC sleeps) | power down if on battery; the link goes `OFF` by itself once the puck stops. A button press can then `opensc2_start()` again, which also lets the puck wake the PC. With the system state machine, for information only: the link going `OFF` is its "wireless off" |
| `OPENSC2_POWER_TURN_OFF` | `0x9F`, or `0x95` with `0xABDF9710` (never while wired) | `opensc2_stop()` or power down. Not sent with the system state machine, which handles it |
| `OPENSC2_POWER_OFF` | the system state machine: the end of a shutdown on a battery build (the link dropped, the power-off cue played, STEAM let go) | turn the board off with a wake-up pin, as the blank canvas does ([Shutdown and power-off](#shutdown-and-power-off)); without a callback the library calls `sys_poweroff()`, and only a new USB supply or a reset wakes the chip |
| `OPENSC2_POWER_REBOOT` | `0x95` with another value | `opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL)` |
| `OPENSC2_POWER_REBOOT_KEEP_LINK` | `0x95` with `0xA427AF52` (Steam's "reboot into wireless mode", after it paired the board over the cable); STEAM pressed while no host configured the wired device; in a charger test build only, no host within `CONFIG_OPENSC2_NO_HOST_WIRELESS_MS` | `opensc2_reboot(OPENSC2_REBOOT_WIRELESS, NULL)` |
| `OPENSC2_POWER_BOOTLOADER` | `0x90`, or `0x95` with `0xECAABAC0`: Steam's firmware update | your bootloader cannot take Steam's update; ignoring it is usually right (Steam's update then fails, and the firmware stays as it is) |
| `OPENSC2_POWER_FACTORY_RESET` | `0x86` with `0xEFC1A917` | the bonds, slot and chime level are already erased; `opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL)` |
| `OPENSC2_POWER_USB_PLUGGED` | USB power appeared while wireless with the link down or STEAM held (policy auto) | `opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL)`: back wired |
| `OPENSC2_POWER_WIRELESS_OFF` | Bluetooth gave up: its advertising window (`CONFIG_OPENSC2_BLE_ADV_WINDOW_S`, 60 s) ended without a host, or the host ended the link (`0x13`) or powered off (`0x15`); the link is `OFF` with reason `LISTEN_TIMEOUT` or `HOST_DISCONNECTED` | the library has already stopped advertising. Without the system state machine: power down if on battery (the real controller turns off); on USB power nothing more (the real controller stays on with wireless off); a button can `opensc2_start()` again. With `CONFIG_OPENSC2_SYSTEM` (the default) the machine takes it instead, as for the puck's link giving up: wireless off on USB power, a shutdown on a battery build ([Power and the system state machine](#power-and-the-system-state-machine)) |
| `OPENSC2_POWER_USB_UNPLUGGED` | USB power gone for 0.2 s while wired (only a board with a battery or another supply still runs to see it) | power off if the board can (the real controller shuts down), or `opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL)`, which comes back wireless without USB power (the default, and what the blank canvas does). Ignoring it leaves the board wired with neither link until a reset or a re-plug. Not sent with the system state machine, which runs its shutdown instead |

## Power and the system state machine

> [!NOTE]
> Built; not yet tested on hardware. The rules are host-tested, and the hardware test suite's
> `system` group runs them against the firmware emulator ([testing.md](testing.md)); none of this
> section has run on a board yet.

`CONFIG_OPENSC2_SYSTEM` (on by default) runs the real controller's system state machine, as the
protocol documentation describes it (not public yet; [protocol.md](protocol.md#where-this-library-had-to-fill-gaps)
lists where OpenSC2 differs): what a power-on does on USB power, on the dock and on a battery;
STEAM pressed, and held for 7 s; the transport chords and their 4 s pairing hold; the keylock; the
idle timeout; low battery; the turn-off command, a link that gives up and the puck's turn-off; the
cable pulled; the status LED's mode; and the cues. It all runs in the system work queue.

What the application does for it:

- **The buttons**, as always (`opensc2_set_buttons()`, `opensc2_set_state()`): the machine reads
  the same button word for STEAM, the chords and the idle timeout. The test hooks' report injection
  never reaches it; their system injection does ([Hardware test hooks](#hardware-test-hooks)).
- **The battery** (`opensc2_set_battery()`), before `opensc2_init()` for the power-on checks. The
  level counts from the first call on ([Low battery](#low-battery)).
- **The power source**, optionally (`opensc2_set_power_source()`, below).
- **The outputs**: it shows the LED (the `led_status` callback, [Status LED](#status-led)), plays the
  cues (the `cue` callback, [Cues](#cues)) and turns the board off on `OPENSC2_POWER_OFF`
  ([Shutdown and power-off](#shutdown-and-power-off)). The blank canvas does all of it
  ([app/README.md](../app/README.md#led)).

**A board without a battery is never switched off.** Without `CONFIG_OPENSC2_BATTERY` (the default)
a board that runs without USB power is taken to be on an external supply, which counts as USB
power, and every shutdown that would turn the board off reboots it instead. On USB power the machine
changes only what the real controller changes there: STEAM held 7 s, the turn-off command, a link
that gave up and the puck's turn-off (the PC went to sleep) switch wireless off and show the amber
LED; a STEAM press then reboots the board, which comes back wired, and `opensc2 start` (or the
application's `opensc2_start()`) turns wireless on again. A battery build (`app/battery.conf`, or
`CONFIG_OPENSC2_BATTERY=y`) runs on its battery without USB power, with every rule below.

With `CONFIG_OPENSC2_SYSTEM=n` the library behaves as v0.2.0 did: `opensc2_init()` starts the link,
every power request goes to the application, `opensc2_led_status()` selects no mode and
`opensc2_keylock()` is false.

### What powers the board

| `enum opensc2_power_source` | Means |
| --- | --- |
| `OPENSC2_POWER_SOURCE_AUTO` | the default: the library's own view. The dock while [the dock's supply rule](#the-docks-rules) says so (only the UART build with a `+` sense input can); USB power while the chip's VBUS detection sees it; otherwise a battery with `CONFIG_OPENSC2_BATTERY`, or else an external supply (USB power, for the rules) |
| `OPENSC2_POWER_SOURCE_BATTERY` | no input power: running on the battery |
| `OPENSC2_POWER_SOURCE_USB` | USB power, or another supply that is not the dock |
| `OPENSC2_POWER_SOURCE_DOCK` | the puck's dock contacts: an application that senses their power can name it; with `AUTO` the dock's supply rule decides |

`opensc2_set_power_source(s)` sets it (`-EINVAL` for another value), for example from a charger's
input type; `opensc2_power_source()` returns the source in effect. The library looks at it every
`CONFIG_OPENSC2_POWER_POLL_MS` (50 ms, as often as the real controller polls its USB power
detection). Power that goes away counts once it has stayed away 0.2 s; power that comes counts at
once. Only a change of "power present" is an event: USB to the dock with power all along changes
nothing. Set it before `opensc2_init()` for the power-on decision: a boot with VBUS whose source is
the battery or the dock is not wired (`OPENSC2_MODE_NO_USB_POWER`).

### The power-on decision

Once `opensc2_init()` has decided the mode ([Modes](#modes)), in this order:

| At power-on | State | What happens |
| --- | --- | --- |
| a wired boot | `wired-wait` | cue 0 (the power-on chime); the LED stays as it is for 0.5 s, then amber until a host configures the device (`wired`, green). The keylock is deleted |
| wireless on USB power (the warm restart, the policy, the application's choice) | `usb-link-on` | the stored transport's cue; its link starts. The keylock is deleted |
| on the dock, right after the warm restart | `dock-on` | the stored transport's link starts, without a cue. The keylock is deleted |
| on the dock otherwise | `dock-off` | the radio stays off; amber. The keylock is deleted |
| on battery, level 0 % | `shutdown-silent` | off, without a cue |
| on battery, the keylock stored | `shutdown-locked` | the keylock pattern for 0.5 s, then off |
| on battery, level 2 % or less | `shutdown-low` | the power-off cue and the low-battery pattern for 0.5 s, then off |
| on battery, STEAM alone or a transport chord held | `chord` | its transport, its cue, its colour steady, 4 s ([Chords](#chords)), then `battery` |
| on battery otherwise | `battery-wait` | the stored transport's cue, then 1 s for USB power (a reboot, which comes back wired) or the dock (`dock-off`); then its link starts: `battery` |

The stored transport is `user/wireless_transport` as this boot runs it: 2 the main puck (cue 3),
3 the second puck (cue 4), 0 and 1 Bluetooth slots 1 and 2 (cues 1 and 2). Starting a link selects
that transport when it differs and starts it, and only if `config.start` is set: the radio for a
puck, Bluetooth on that slot in a build with Bluetooth (without one the main puck stands in).

### The states while it runs

| State | LED mode | Acts on |
| --- | --- | --- |
| `wired-wait`, `wired` | amber after 0.5 s without a host; 3 (green) once configured | USB power gone (the cable pulled, 0.2 s): `shutdown`. A host's reboot and bootloader requests reach the application while `wired` |
| `usb-link-on` | the transport's, shown as on external power | STEAM held 7 s, the turn-off command, a link that gave up or the puck's turn-off: `usb-link-off`; USB power gone: `battery`, the link kept; a host's requests reach the application |
| `usb-link-off` | 4 (amber) | STEAM pressed: a reboot, which comes back wired; USB power gone: `shutdown-silent`; the link started from outside (`opensc2 start`, `opensc2_start()`, pairing): `usb-link-on` |
| `battery` | the transport's | STEAM held 7 s, the idle timeout, the turn-off command, a link that gave up or the puck's turn-off: `shutdown`; 0 %: `shutdown-low`; the keylock chord held 3 s: the keylock stored, then `shutdown`; USB power: with STEAM held or the link down a reboot that comes back wired (policy auto, `CONFIG_OPENSC2_USB_PLUG_IN_REBOOT`), with the link up `usb-link-on`; dock power: `dock-on`; a host's requests reach the application |
| `dock-off` | 4 (amber) | a transport chord: `dock-chord`; lifted off (power gone): `lifted`; Steam's "reboot into wireless mode" reaches the application |
| `lifted` | 5 (off) | 5 s: `shutdown-silent`; back on the dock: `dock-off`; USB power: a reboot; a transport chord: `chord` |
| `dock-chord` | the chord's transport, steady | as `chord`, then `dock-on` |
| `dock-on` | the transport's, shown as on external power | STEAM held 7 s, the turn-off command, a link that gave up or the puck's turn-off: the power-off cue, `dock-off`; power gone: `battery`, the link kept; a host's requests reach the application |
| `shutdown`, `shutdown-silent`, `shutdown-low`, `shutdown-locked`, `reboot` | 5, 5, 6, 7, 5 | nothing else: [Shutdown and power-off](#shutdown-and-power-off) |

"A link that gave up" is the radio going `OFF` after `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` (60 s in a
battery build), after `CONFIG_OPENSC2_NO_BOND_TIMEOUT_S` with the selected slot empty (10 s in a
battery build), or for a protocol version it does not speak; the puck's turn-off counts the same. A
radio stopped on the board (`opensc2_stop()`, the shell's `opensc2 stop`) is not "wireless off".
The turn-off command is `0x9F`, or `0x95` with `0xABDF9710`; wired, it is acknowledged and ignored,
as before. A host's other requests (`0x95` reboots, the bootloader requests) reach `power_request`
only in the states the table names; elsewhere they are logged and ignored. Requests of the library's
own (STEAM before a host, the shell's reboots) are never held back.

### Chords

| Chord | The buttons, exactly | Does |
| --- | --- | --- |
| STEAM alone | STEAM | the stored transport; pressed (20 ms): "STEAM pressed", the reboot out of `usb-link-off`; held (7 s): "STEAM held", the shutdown or wireless off above |
| the main puck | A + right bumper, with or without STEAM | bond slot 1 (transport 2), cue 3 |
| the second puck | A + left bumper, with or without STEAM | bond slot 2 (transport 3), cue 4 |
| Bluetooth 1 | B + right bumper, with or without STEAM | Bluetooth slot 1 (transport 0), cue 1: only in a build with Bluetooth |
| Bluetooth 2 | B + left bumper, with or without STEAM | Bluetooth slot 2 (transport 1), cue 2: only in a build with Bluetooth |
| the keylock | STEAM + L4 + L5 + R4 + R5 | 5 ms: deletes the keylock (in any state); 3 s on battery: stores it, then `shutdown` ([The keylock](#the-keylock)) |

A chord matches when the button word, with the stick-touch, trigger-click and grip-touch bits left
out, equals its buttons exactly, and releases when it stops matching; the trackpad touch and click
bits stay in, so a thumb resting on a pad spoils a chord. One chord matches at a time.

The transport chords count when the board powers up on battery with one held, and on the dock
with the radio off (`dock-off`, `lifted`): the chord's transport becomes the choice, its cue plays
and its colour shows steady. Released within 4 s, its link starts; held 4 s, it starts with the
pairing flag (Bluetooth then opens pairing mode; the puck's radio ignores the flag). For 500 ms
after the chord was recognised the machine takes no other event. STEAM alone held at a battery
power-on is the stored transport's chord.

### The keylock

A travel lock. Held 3 s on battery, the keylock chord stores the lock and shuts the board down. It
counts from the next power-on: on battery the board then turns straight off again, showing the
keylock pattern. A power-on on USB power or the dock deletes it, and so does a 5 ms tap of the
chord. `opensc2_keylock()` says whether it is stored (as read at boot or written since; the chord's
own store counts from the next boot).

It is the settings-by-name key `user/keylock`, 4 bytes, `0x41FB0AC3` while locked, one of the
values the library keeps ([Settings by name](#settings-by-name); Zephyr settings
`opensc2/k/keylock`): `0xED` reads it, `0xEE` sets the value in RAM, `0xEF` saves it and `0xF0`
deletes it; a factory reset (`0x86`) deletes it too.

### The idle timeout

Controller setting 50, in seconds (900, 15 minutes, by default; 0 or more than 9998: never). Only
battery operation acts on it: the board shuts down. The timeout is cut into 16 periods; a period
with input in it leaves a mark in a 16-bit history, and the idle event comes when all 16 are empty.
Input is a button pressed (a change that leaves a button down), a stick beyond 4000 of 32767, a
trigger above 0, or a touched pad or stick (a touch still there when a period ends counts too).
Sticks and triggers are seen in the state reports, so only while a link is up, as on the real
controller. A new value of setting 50 (the defaults coming back included) starts the monitor again,
and an idle history fires again at the end of every further idle period, so a board that stayed idle
on USB power turns off within one period of being unplugged. The status LED's "input recent" comes
from it.

### Low battery

Each battery report (`0x43`, every 3.5 s) also tells the machine the level: the application's, or
100 while the charge state has `OPENSC2_CHARGE_GAVE_UP` and the level is above 0. On battery, 0 %
starts the low-battery shutdown; at a battery power-on 0 % turns the board off silently and 2 % or
less with the low-battery signal. At 5 % or less the status LED shows red ([Status LED](#status-led)).
Only a level the application set counts: until its first `opensc2_set_battery()` there is none, so
a board that measures nothing (and reports the default 100 %) is never low.

### Shutdown and power-off

| State | Shows | Ends |
| --- | --- | --- |
| `shutdown` | LED off, the power-off cue | after 0.5 s, once STEAM is up: off, or a reboot while controller setting 77 is not 0 |
| `shutdown-silent` | LED off | off, once STEAM is up |
| `shutdown-low` | the low-battery pattern, the power-off cue | after 0.5 s, once STEAM is up: off |
| `shutdown-locked` | the keylock pattern | after 0.5 s, once STEAM is up: off |
| `reboot` | LED off | a reboot at once, by the boot rule (wired on USB power) |

Each of them drops the link first (wired: one all-zero `0x42`, 10 ms, USB off; wireless: the radio
stops, and the puck notices the silence) and turns the IR LED off. Waiting for STEAM to be up holds
everything else, so the press that turned the board off cannot wake it again. "Off" is:

- in a battery build (`CONFIG_OPENSC2_BATTERY`): `OPENSC2_POWER_OFF` to the application, which turns
  the board off; without a `power_request` callback the library calls `sys_poweroff()` itself;
- without a battery: a plain reboot (`OPENSC2_POWER_REBOOT`), so a board that could not be woken
  again is never switched off;
- while the test hooks are armed, or in a boot their boot-time injection started: a reboot too.

Setting 77 (default 0; SDL's name for it is `THUMB_TOUCH_THRESH`) makes the ordinary shutdown
reboot instead, read when the shutdown ends. Settings return to their defaults at every boot, so one
write covers one shutdown. The blank canvas turns the board off the way Zephyr's `system_off` sample
does: the STEAM key's pin (without one, every key's) as a level-sensing wake-up input
(`GPIO_INT_LEVEL_ACTIVE`), then `sys_poweroff()` (`CONFIG_POWEROFF`, which the battery option
selects). A new USB supply, or a reset, also wakes the nRF52840.

```c
static void on_power(enum opensc2_power_request request, void *user)
{
    if (request == OPENSC2_POWER_OFF) {
        gpio_pin_interrupt_configure_dt(&steam_pin, GPIO_INT_LEVEL_ACTIVE);   /* STEAM wakes */
        my_leds_off();
        sys_poweroff();
    }
    /* ... the reboots, as before */
}
```

### Cues

| Cue | `enum opensc2_cue` | Raised | Script |
| --- | --- | --- | --- |
| 0 | `OPENSC2_CUE_POWER_ON` | the wired start; a remote wakeup (`CONFIG_OPENSC2_USB_REMOTE_WAKEUP`) | 1 |
| 1, 2 | `OPENSC2_CUE_BT_1`, `_BT_2` | Bluetooth slot 1 or 2 chosen, at power-on or by a chord | 1, 2 |
| 3, 4 | `OPENSC2_CUE_PUCK_1`, `_PUCK_2` | the main or the second puck chosen, at power-on or by a chord | 1, 2 |
| 5 | `OPENSC2_CUE_POWER_OFF` | `shutdown`, `shutdown-low`; wireless switched off on USB power or the dock | 5 |
| 6 | `OPENSC2_CUE_PAIRING` | pairing starts (over the air: the link enters `PAIRING`) | 8 |
| 7 | `OPENSC2_CUE_PAIRED` | a puck (or a Bluetooth host) has paired | 9 |
| 8 | `OPENSC2_CUE_CHARGE_WINDOW` | the real controller's factory charge window: never raised here | 10 |
| 9 | `OPENSC2_CUE_UNUSED` | nothing raises it | 3 |
| 10 | `OPENSC2_CUE_HOST_ASLEEP` | the puck says the PC is asleep (`0xE6`), once per boot | 4 |

The `cue` callback gets `struct opensc2_cue_event`: the cue, the real controller's built-in script
it stands for (the `0x85` script numbers) and a gain from the chime level Steam stores
(`opensc2_chime_level()`, `user/haptic_boot_level`): 1 (quiet) −18 dB, 2 (normal, the default)
−12 dB, one dB less for cues 6 and 7; any other level is silent, and the callback is not called.
Cue 8 would play at +2 dB whatever the level. OpenSC2 has no copy of the real controller's scripts:
with the [haptics synthesiser](#the-haptics-synthesiser) (`CONFIG_OPENSC2_HAPTICS_SYNTH`) the library
plays OpenSC2's own sound for each cue itself (`opensc2_haptics_play_cue()`), and the callback still
reports it; without it the sound is the application's, played from the callback. The blank canvas
logs each cue.

## Status LED

> [!NOTE]
> Built; not yet tested on hardware. The patterns follow the protocol documentation's description
> of the real controller's LED; none of them has been compared with a real controller.

With `CONFIG_OPENSC2_SYSTEM` the library selects the status LED's mode and the application shows
it. `opensc2_led_status(&st)` gives everything a pattern depends on, at any time. The `led_status`
callback comes when the mode, its flags or the battery level change; the link, pairing, recent
input, the brightness, Steam's colour and the fully-charged level change without one, so read the
status at every step, as below. `<opensc2/led.h>` also has the real controller's patterns as a
pure **pattern player**, so an application only drives its LED:

```c
static struct opensc2_led_player player;     /* opensc2_led_player_init(&player) once */

static void every_10_ms(void)                /* OPENSC2_LED_STEP_MS */
{
    struct opensc2_led_status st;
    uint16_t rgbw[4];                        /* R, G, B, W in thousandths */

    opensc2_led_status(&st);
    opensc2_led_player_step(&player, &st, k_uptime_get_32(), rgbw);
    for (int i = 0; i < 4; i++) {
        set_my_pwm(i, opensc2_led_duty(rgbw[i], my_trim[i], st.brightness));   /* thousandths */
    }
}
```

| `struct opensc2_led_status` | |
| --- | --- |
| `mode` | `enum opensc2_led_mode`, below |
| `external_power` | modes 1 and 2: shown as on USB power or the dock |
| `steady` | a chord state: the transport's colour, steady as if connected |
| `connected`, `pairing` | a wireless link is connected; pairing: Bluetooth advertises in pairing mode, or the radio pairs over the air (`opensc2_pair_start()`, OpenSC2's own: the real controller has no such pairing) |
| `input_recent` | the idle monitor saw input within its timeout |
| `shutting_down` | a shutdown or reboot runs (the IR LED is off too) |
| `level`, `below_full_ms` | the battery level the LED was told (255: none yet, which counts as full); the uptime when it last read below 100 % |
| `full_level` | `user/fully_charged_led_intensity`: 0 off, 1 dim, 2 medium, 3 (the default) full |
| `brightness` | percent: setting 45 through the curve of setting 38 |
| `override`, `override_rgbw[4]` | setting 37: show Steam's `0xC5` colour (percent) instead |

| Mode | `enum opensc2_led_mode` | Colour (R, G, B, W in thousandths) | Pattern |
| --- | --- | --- | --- |
| 0 | `NONE` | dark | nothing selected yet |
| 1 | `BLUETOOTH` | blue (0, 0, 200, 0) | connected on battery, or a chord state: steady (red at 5 % or less), chosen again every 2 s. Connected on external power: a breath from dark, 1.8 s up and 1.8 s down, or at 100 % the fully-charged display. Pairing: 75 ms on, 75 ms off, 75 ms on, 300 ms off. Otherwise (searching): 100 ms up, 900 ms on, 100 ms down, 900 ms dark, cut short when the link comes up |
| 2 | `PUCK` | white with a little blue (0, 0, 50, 500) | as mode 1 |
| 3 | `WIRED` | green (0, 250, 0, 0) | the breath from dark; at 100 % the fully-charged display |
| 4 | `AMBER` | amber (200, 200, 0, 0) | from dark, 200 ms to a dim amber (15, 20, 0, 0), 1.6 s up to amber, 1.6 s down, 200 ms to dark; at 100 % the fully-charged display. USB power without a host or with wireless off, the dock with the radio off |
| 5 | `OFF` | dark | shutdowns, reboots, lifted off the dock |
| 6 | `LOW_BATTERY` | red (100, 0, 0, 0) | at 5 % or less 75 ms on, 75 ms off, 75 ms on, 1,075 ms off; above 5 % it keeps what it showed, for 1 s a cycle |
| 7 | `KEYLOCK` | white (0, 0, 0, 200) | 75 ms on, 75 ms off, 75 ms on, 10,075 ms off |

**The fully-charged display** (modes 1 and 2 connected on external power, 3 and 4, at 100 %): a 2 s
fade to one of four levels, chosen again every 2 s: full for 15 s after the level last read below
100 %, full while there was recent input (modes 1 to 3), otherwise the stored
`user/fully_charged_led_intensity`. The levels in thousandths, full, medium and dim: blue 200, 100,
40 (B); the puck's 50 B + 500 W, then 100 and 40 W; green 250, 100, 50 (G); amber (200, 200),
(80, 100), (30, 40) (R, G). The key is one byte, one of the values the library keeps ([Settings
by name](#settings-by-name); Zephyr settings `opensc2/k/fcled`): `0xED` reads it, `0xEE`, `0xEF` and
`0xF0` set, save and delete it (a deletion brings back 3 at once), and a factory reset deletes it.

**Steam's colour**: while setting 37 is on, `0xC5`'s colour shows steady instead of any pattern.

**How the player plays**: one cycle of a pattern at a time; when it ends, the next cycle comes from
the status as it is then, as on the real controller. So a new status shows within 0.1 s from the
dark display and Steam's colour; from the steady link display (modes 1 and 2 connected on battery,
or a chord state) a mode other than 1 or 2 shows at its next 100 ms check, but the other link mode,
a power change, a lost link, the red at 5 % or less and Steam's colour wait for the end of its 2 s
cycle; up to 10.3 s from the keylock pattern. Fades are linear in 10 ms steps; the breathing
patterns (modes 1 and 2 connected on external power, 3 and 4 below 100 %) start every cycle from
dark, the searching and fully-charged displays from the colour shown last.

| Function | Does |
| --- | --- |
| `opensc2_led_player_init(&p)` | a player at rest (dark) |
| `opensc2_led_player_step(&p, &st, now_ms, rgbw)` | the colour to show at `now_ms`: R, G, B, W in thousandths of full duty, before trims and brightness |
| `opensc2_led_player_envelope(&p)` | for a one-colour LED: how bright the pattern is against the cycle's peak, 0 to 1000; switch the LED on from 500 |
| `opensc2_led_brightness(s45, s38)` | the brightness in percent: with setting 38 not 0 (the default 1) the curve b = 100 × (6^(s/100) − 1) / 5, cut to a whole number (setting 45's default 50 gives 28); with 0, b = s |
| `opensc2_led_duty(colour, trim, brightness)` | a channel's duty in thousandths: colour × trim / 100 × brightness / 100, at most 1000 (`OPENSC2_LED_FULL_SCALE`). The real controller's factory trims are `OPENSC2_LED_TRIM_RED` 90, `_GREEN` 35, `_BLUE` 120 and `_WHITE` 80 (percent) |

**The IR LED**: controller setting 36 (percent; 0 by default), off during every shutdown and reboot
(`st.shutting_down`; its setting stays). The blank canvas drives one from a PWM channel named `ir`.

## The haptics synthesiser

`CONFIG_OPENSC2_HAPTICS_SYNTH` (off by default; `app/haptics.conf` builds the blank canvas with it)
makes the library play the haptic output reports itself, as the real controller's two sequencers
do: it renders them into sample blocks for the controller's four haptic channels and hands the
blocks to outputs the application provides, or to built-in ones. It also plays sounds of
OpenSC2's own (the scripts of report `0x85`, the cues, the drop sounds) and sends the PCM streams'
flow control, input report `0x44`. The `rumble` and `haptic` callbacks are still called as before.
Everything is in `<opensc2/haptics.h>`; without the option its functions answer `-ENOTSUP`. Built;
not yet tested on hardware: the rendering is checked by the host tests and, on a board with
nothing wired, by the hardware test suite's `haptics` group ([testing.md](testing.md)).

```c
#include <opensc2/haptics.h>

static int amp_write(const struct opensc2_haptics_output *out, const void *block, size_t frames)
{
    /* 64 frames of two int16_t, left grip first: queue them for 8 kHz playback, and wait while
     * two blocks are already queued */
    return 0;
}

static const struct opensc2_haptics_output_api amp_api = { .write = amp_write };
static const struct opensc2_haptics_output amp = { .api = &amp_api };

opensc2_haptics_set_output(OPENSC2_HAPTICS_GRIPS, &amp);
/* the cues: the system state machine (on by default) plays them itself; a sound of your own: */
opensc2_haptics_play_script(OPENSC2_HAPTIC_LEFT_GRIP | OPENSC2_HAPTIC_RIGHT_GRIP, 4, 0);
```

### The two paths

| `enum opensc2_haptics_path` | Pads (`OPENSC2_HAPTICS_PADS`) | Grips (`OPENSC2_HAPTICS_GRIPS`) |
| --- | --- | --- |
| Channels | 0 left pad, 1 right pad | 2 left grip, 3 right grip |
| Rate | 4000 Hz (`OPENSC2_HAPTICS_PAD_RATE_HZ`) | 8000 Hz (`OPENSC2_HAPTICS_GRIP_RATE_HZ`) |
| Block | 32 frames, 8 ms (`OPENSC2_HAPTICS_PAD_FRAMES`) | 64 frames, 8 ms (`OPENSC2_HAPTICS_GRIP_FRAMES`) |
| Samples | `uint8_t`, 128 = silence | `int16_t`, -32767..32767 |

A frame is two samples, the left channel's first. Each path has its own thread: idle until
something plays; then a first block that begins with 4 ms of silence, and block after block, one
block ahead of what plays, until nothing plays any more, when it lets the output drain. A sound
therefore starts about 4 ms after its report on an idle path, 8 to 16 ms after it while something
plays (one block sooner than on the real controller:
[protocol.md](protocol.md#where-this-library-had-to-fill-gaps), gap 35). Each channel runs up to eight generators at once, one of each type (click A, click B, pulse
train, tone, rumble, sweep, stream, clip); their samples, each scaled by its own gain, are summed,
multiplied by the channel gain and clipped. The channel gain is the master gain (controller setting
76, default -3 dB), negated for the left pad and the right grip as on the real controller. Every
gain in dB goes through 10^(dB/20), from -30 to +30 dB (values beyond play at the nearest end, so a
gain never mutes). The pads refuse the rumble generator and the grips the clicks. Each path holds 20
commands (`CONFIG_OPENSC2_HAPTICS_QUEUE_DEPTH`); one that finds the queue full is lost.

The arithmetic is in integers only, so a block comes out bit for bit the same on the board and on a
PC (the host tests and the suite compare CRCs of blocks). Its sine is a table of OpenSC2's own, not
the real controller's polynomial.

### What each report plays

| Report | Plays |
| --- | --- |
| `0x80` | the October rumble emulation. The right grip: a rumble generator, a 50 Hz sine started downwards in bursts of two periods, one burst every 83.5 ms (668 samples, a constant 12 Hz, as the real one computes it; the second of a fresh start 667 samples after the first, as there), each burst after the first a random -4 to +3 dB, at trunc(right speed × 0.0001) - 14 dB (-20 dB below 1000). The left grip: a 50 Hz tone with a full-depth LFO (10 Hz up to a fifth of full speed, then 10 + 6 × (speed / 65535 - 0.4) Hz, so 8.8 Hz just above a fifth, as the real one plays it) at trunc(left speed × 0.00024) + 2 dB + setting 27 (default -10) + the level step (16, 60, 100, 200, 1000, 8000, 16000: +16, +12, +8, +4, -4, -8, -12 dB) - 12 dB (-18 below 1000). Each lasts 500 ms; a report over a running rumble keeps its bursts and the tone its phase. A speed of 0 stops everything on that grip, the right one first. The library's own rumble stop (`timeout`) plays nothing here: the sound ends by itself. |
| `0x81` | on each channel, in this order: on time 0 stops everything; off time 0 plays click A at +8 dB; a count of 3 or 4 click B at +4 dB; a count of 0 nothing; otherwise a full-scale square wave of `count` periods, each the on plus the off time in whole samples (250 µs on a pad, 125 µs on a grip): +1 for the on time's whole samples, -1 for the rest of the period, as the real controller rounds them (an on time under one sample gives a constant -1). Clicks on the pads only. |
| `0x82` | 0 stops everything on the channels; 1 click A at the gain + 6 dB; 2 click B at the gain (pads only); any other command is ignored |
| `0x83` | a sine at the gain, optionally modulated: (1 - d) + d × sin(LFO) with d = depth / 200 (100 % swings the amplitude between 0 and 1), the LFO starting at -π/4; a duration of 0 plays until stopped or replaced; a tone started over a running one keeps its phases |
| `0x84` | an exponential sweep from the start to the end frequency, with an 80 ms fade-in and a 5 ms fade-out; targets 6 and 7 play the right channel inverted; 0 Hz at either end plays nothing, equal ends a plain tone with the fades |
| `0x85` | built-in script 1..17 (OpenSC2's own sounds, below) on each channel at the gain; a channel whose script still runs refuses the next one |
| `0x86`..`0x89` | PCM streams, below |

### PCM streams and input report `0x44`

Each channel has a 512-byte ring. `0x86` operation 2 sets a channel's format (format / 4: s16le, s8,
mu-law; format % 4: 8, 4, 2, 1 kHz); the format stays set across stops, and a stream never starts
before one is set. `0x87`, `0x88` and `0x89` write their bytes into the rings (the right channel
first for two-channel targets, the right grip's bytes of `0x88` before the left's; `0x88` with more
than 31 bytes a side is ignored). A write starts the stream once the ring already held more than 16
ms of audio before it (8 ms on a pad); lower rates are interpolated linearly up to the channel's
rate, samples scaled to ±1 (s16le by 1/32767, s8 by 1/127, mu-law through its 16-bit G.711 value).
The stream stops when it runs dry. `0x86` operation 1, and every stop of everything on the channel,
empties its ring. The data's length is the report's own on every link: `0x87` takes the bytes after
its target, `0x89` needs its n + 2 bytes. Through the puck the real controller is handed the
record's length, which counts the report ID too, so there its `0x87` takes one byte from beyond the
report and its `0x89` accepts an n one larger (with a junk byte); OpenSC2 does not copy that, since
the extra byte would put a 16-bit stream out of step
([protocol.md](protocol.md#where-this-library-had-to-fill-gaps), gap 35).

The synthesiser sends `0x44` {channel, flags} (channel ids: left grip 0, right grip 1, left pad 3,
right pad 4) on the link in use:

| Flag | Sent |
| --- | --- |
| `0x01` | at once, by a write that did not fit (the bytes that fit are kept) |
| `0x08` | at once, by a write that leaves less than 16 ms of room |
| `0x04` | once, when the queued audio falls below 16 ms while playing (after an `0x08`) |
| `0x02` | when the stream runs dry (with `0x04` as `0x06` when both fall due) |
| `0x10` | for an 8 kHz format asked for on a pad (refused) |
| `0x20` | when a format is set |
| `0x40` | for a format change while the stream plays (refused) |

The synthesiser does not send the `0x02` the real controller also sends after every stop, streaming
or not (Steam's zero pulses get two): those replies to a stop are the library's business
([protocol.md](protocol.md#where-this-library-had-to-fill-gaps), gap 19), so they can never come
twice, and where the library does not send them, a stop gets no reply.

### Its outputs

```c
struct opensc2_haptics_output_api {
    int (*start)(const struct opensc2_haptics_output *out);            /* optional */
    int (*write)(const struct opensc2_haptics_output *out, const void *block, size_t frames);
    int (*stop)(const struct opensc2_haptics_output *out, bool drop);  /* optional */
    void (*enable)(const struct opensc2_haptics_output *out, bool on); /* optional */
};
```

For each playback a path calls `enable(true)`, `write()` with the first block, `start()`, then
`write()` for every block; `write()` must block until the output can take the block (an output with
two buffers returns at once for the first two, then when one has played). When nothing plays any
more: `stop(false)` (return once what was written has played) and `enable(false)`. `write()`
returning `-EPIPE` reports an underrun: the path calls `stop(true)`, writes the block again and
starts again. Controller setting 75 = 1 keeps the enable on between sounds. All calls come from the
path's own thread. `opensc2_haptics_set_output(path, &out)` takes effect from the path's next
playback; `NULL` removes it.

For an actuator that only has an amplitude, `opensc2_haptics_set_level_output(path, fn, user)`
hands over each block's loudness (`opensc2_haptics_block_level()`, 0..255) every 8 ms and 0 when
nothing plays, as `fn(path, level, user)` (an `opensc2_haptics_level_fn`); the blocks are then
paced by a timer. A path with neither renders anyway, paced by a timer.

Built-in outputs, each set up from the devicetree and registered at start-up (built; not yet tested
on hardware; `samples/haptics` has all three):

| Option | Node | Plays |
| --- | --- | --- |
| `CONFIG_OPENSC2_HAPTICS_OUT_I2S` | `opensc2,haptics-i2s` (`i2s`, optional `enable-gpios`) | the grips on Zephyr's I2S driver as master, 16-bit stereo at 8 kHz, for an I2S amplifier per grip |
| `CONFIG_OPENSC2_HAPTICS_OUT_PWM_DAC` | `opensc2,haptics-pwm-dac` (`pwm`, `left-gpios`, `right-gpios`, optional `enable-gpios`) | the pads as duty cycles of an nRF PWM instance through nrfx's sequence playback: 250 steps of the 16 MHz clock, each sample 16 periods, exactly 4 kHz; idle at half duty. The instance's node stays disabled |
| `CONFIG_OPENSC2_HAPTICS_OUT_DRV2605` | the `ti,drv2605` node chosen as `opensc2,haptics-drv2605` | one ERM or LRA in the chip's real-time playback: the louder path's block level every 8 ms (no pitch) |

`app/` drives its optional vibration motor (the overlay's `rumble` PWM channel) from the level
outputs when it is built with the synthesiser.

### Sounds of its own

The real controller's clicks, script contents and drop-sound recordings are Valve's data and are not
in OpenSC2. The synthesiser has sounds of its own instead:

- **Clicks** A (70 samples, 17.5 ms) and B (93 samples, 23.25 ms) at 4 kHz: damped sine bursts made
  by `tools/gen_haptics_tables.py`, at the real clicks' lengths.
- **Scripts 1 to 17** (`opensc2_haptics_play_script()`, report `0x85`): short sequences of tones,
  sweeps and pulses. Each step runs after its delay from the step before (at least 1 ms), at the
  script's gain plus its own offset (added without wrapping round); pulse steps play at full scale.
  One script per channel at a time; a stop does not stop a script.
- **Cues** (`opensc2_haptics_play_cue(cue, level)`), always on both grips, at the chime level Steam
  stores (`opensc2_chime_level()`: 1 quiet -18 dB, 2 normal -12 dB, anything else silent):

  | Cue | | Script | Gain |
  | --- | --- | --- | --- |
  | 0 | USB start-up (wired) | 1 | the level |
  | 1, 2 | Bluetooth slot 1, 2 | 1, 2 | the level |
  | 3, 4 | the main puck, the second puck | 1, 2 | the level |
  | 5 | turning off | 5 | the level |
  | 6, 7 | Bluetooth pairing, paired | 8, 9 | the level - 1 dB |
  | 8 | the factory charge window | 10 | +2 dB, even when silent |
  | 9 | (nothing plays it on the real controller) | 3 | the level |
  | 10 | the PC is asleep | 4 | the level |

  The [system state machine](#power-and-the-system-state-machine) plays them when it raises
  them ([Cues](#cues)); an application can play them too. The numbers are `enum opensc2_cue`'s;
  `<opensc2/haptics.h>` also names them `OPENSC2_HAPTICS_CUE_USB_START` (0), `_BT_SLOT_1`,
  `_BT_SLOT_2`, `_PUCK`, `_PUCK_2`, `_POWER_OFF`, `_BT_PAIRING`, `_BT_PAIRED`, `_CHARGE_WINDOW`,
  `_SPARE` and `_HOST_ASLEEP` (10).
- **The drop sounds** (`CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS`, on with the synthesiser):
  `opensc2_haptics_free_fall()` reports a fall; 250 ms later a random byte picks script 16 (below
  48), script 17 (48 to 127) or nothing, on the left grip at 0 dB whatever the chime level; falls
  count again 5 s after the one taken. `opensc2_freefall_update()` is a detector for IMUs without
  one: the acceleration below 0.35 g for 150 ms (OpenSC2's own thresholds).
- **Lizard mode's pad feedback**: the real controller plays click A at +8 dB per tick and click B at
  +6 dB per pad press and release. The synthesiser plays lizard mode's events
  ([Lizard mode](#lizard-mode)) itself, as the real controller does: while lizard mode is on and
  setting 70 is exactly 1 and the current link is USB, the puck or Bluetooth, never the dock;
  `opensc2_haptics_click()` plays a click of the application's.

### Functions

| Function | Does |
| --- | --- |
| `opensc2_haptics_set_output(path, &out)` | the output of a path (NULL: none) |
| `opensc2_haptics_set_level_output(path, fn, user)` | a path's block levels for an amplitude-only actuator |
| `opensc2_haptics_play_cue(cue, level)` | a cue on both grips: 0, `-ENODATA` (silent at that level), `-EBUSY` (both grips' scripts run) |
| `opensc2_haptics_play_script(channels, id, dB)` | script 1..17 on the channels (`OPENSC2_HAPTIC_*` bits): how many channels it started on |
| `opensc2_haptics_click(channels, click, dB)` | click A or B (`OPENSC2_HAPTIC_CMD_CLICK_A`, `_B`) on the pads: how many it queued |
| `opensc2_haptics_stop(channels)` | stop everything on the channels: how many it queued |
| `opensc2_haptics_play_clip(channel, data, samples, format, dB)` | a recording of your own in a stream format (0..11); `data` stays valid until it has played |
| `opensc2_haptics_free_fall()` | a free fall: maybe a drop sound. From any context, an IMU's free-fall interrupt included: it notes the time, the rest runs on the system work queue |
| `opensc2_haptics_info(&info)` | what plays, blocks, underruns, lost commands, stream fill |
| `opensc2_haptics_block_level(path, block, frames)`, `opensc2_freefall_init(&ff)`, `opensc2_freefall_update(&ff, accel, ms)` | pure helpers, with or without the synthesiser |

Every function above but `opensc2_haptics_free_fall()` and the pure helpers takes the synthesiser's
mutex: call it from a thread, never from an interrupt.

### Settings, threads and memory

- Setting 76 (master gain) applies from the next block; setting 75 (amplifier mode) at the next
  idle check, within 100 ms, but never cuts a sound short; setting 27 at each rumble report.
  Setting 70 ("haptics enabled") does not silence the synthesiser: on the real controller only
  lizard mode's pad feedback reads it.
- Two threads, `osc2_hap_pads` and `osc2_hap_grips` (priority
  `CONFIG_OPENSC2_HAPTICS_THREAD_PRIORITY`, 6; stack `CONFIG_OPENSC2_HAPTICS_THREAD_STACK_SIZE`,
  1536); the scripts and drop sounds run on the system work queue; one mutex guards the
  synthesiser, never held while an output plays or a report goes out, and a second one lets one
  thread at a time send what it made, so its `0x44` reports keep their order. Output reports reach it from
  the radio or USB thread after the callbacks, and only queue commands and copy stream bytes there.
- About 10 KB of RAM and 15 KB of flash (the blank canvas with `app/haptics.conf` against without).

## Identity

`struct opensc2_identity`, all of it but the product name sent once per connection in the identity
packet `0xF5`, used for vendor commands `0x83`, `0xAE` and `0xF2`, and on USB the unit serial is the
serial string and the product name the product string. The October 2026 puck firmware answers
Steam's attribute queries from the identity packet; older puck firmware passes the queries to the
controller, which the library answers too.

| Field | Rule | Default (Kconfig) | Where it shows |
| --- | --- | --- | --- |
| `unit_serial` | exactly 13 characters, `0-9` and `A-Z` | `CONFIG_OPENSC2_UNIT_SERIAL` "OSC2DEV000001", or with `CONFIG_OPENSC2_SERIAL_FROM_CHIP` (default) "OSC" + 10 hex digits of the chip ID | the serial Steam reads (`0xAE` tag 1, the USB serial string); pairing packets |
| `board_serial` | same | "OSC2PCB000001", or "OSB" + chip ID | `0xAE` tags 0 and 4 |
| `git_revision` | 1 to 12 printable characters, no spaces | `CONFIG_OPENSC2_GIT_REVISION` "osc2-0.2.0" (the library's release, "osc2-X.Y.Z"; a project of your own may set its own) | `0xAE` tag 3, `0xF2` |
| `product_id` | non-zero; Steam knows the Steam Controller 2 as `0x1302` | `CONFIG_OPENSC2_PRODUCT_ID` | attribute 1 |
| `board_revision` | any; real controllers use `0x45`, `0x48`..`0x4B` | `CONFIG_OPENSC2_BOARD_REVISION` `0x48` (72, what a real controller reports) | attribute 9 |
| `build_time` | Unix seconds | `CONFIG_OPENSC2_BUILD_TIME`, 0 = when the build was configured | attribute 4 |
| `bootloader_build_time` | Unix seconds, 0 = none | `CONFIG_OPENSC2_BOOTLOADER_BUILD_TIME` | attribute 10 |
| `product_name` | 1 to 31 printable ASCII characters (`0x20`..`0x7E`), no space at either end | `CONFIG_OPENSC2_USB_PRODUCT` "Steam Controller" | the wired USB device's product string, from the next wired boot; never sent over the radio |

The "OSC"/"OSB" development serials are this project's, not a real controller's. The real controller
also has a Bluetooth name ("Steam Ctrl (BT) " + serial), which the Bluetooth build advertises the
same way ([The Bluetooth build](#the-bluetooth-build)); nothing is named over the puck's link.
Steam's updater forces an update on controllers whose build time is at or before `0x6A18D057`. Such
a build time stops the build when it is the default (`CONFIG_OPENSC2_BUILD_TIME`, or with 0 the
build machine's clock), the shell stores one only with `force`, and the library warns at boot when
one is in use.

**The product name** is optional. Windows shows it: it reads the product string when the wired
device enumerates (for example as the device's name in Settings and as its bus-reported description
in Device Manager). Steam, as far as known, names a new controller itself, by its type ("Steam
Controller"), and renaming it in Steam works and is stored by Steam, per serial; so a custom name
changes what Windows shows, not what Steam calls it. Any name other than "Steam Controller" makes the
wired device differ from the real controller's, which is why the default stays the real one.

**Steam's update prompt.** Steam offers its controller firmware update to a controller that reports
an older build time than the controller firmware Steam ships. The prompt goes away when the board
reports a build time at least as new as Steam's current controller firmware (tested). A fresh build
reports the time it was built, so it is newer than any controller firmware Steam had shipped by
then; a prebuilt release reports its own build time. Once Steam ships a newer controller firmware,
give the board Steam's value: on Windows, with the board in wireless mode and no terminal open on
its console, run `python -I tools\match_steam_version.py --dry-run` (shows what it would do: it
reads Steam's `hardwareupdater.cfg` and finds the board's console, but opens no device and sends
nothing), then `python -I tools\match_steam_version.py`, then reboot the board. Declining
the prompt is always safe. If you accept it by mistake, the board ignores Steam's request to enter a
bootloader, so the update cannot start and the firmware stays as it is (by design; not tried with
Steam).

For reference: Steam's October 2026 controller firmware has the build time `0x6AC686B3`, and a board
reporting the July 2026 controller's `0x6A628345` got the prompt against it. The build time is
attribute 4 of vendor command `0x83`; `tools/match_steam_version.py` reads Steam's value
(`TRITON_FW_TS` in its updater's `hardwareupdater.cfg`) and stores exactly that value with
`opensc2 id set build`, never one at or below Steam's forced-update value
([app/README.md](../app/README.md#steams-firmware-update)). Steam's bootloader requests (`0x90`,
`0x95` with `0xECAABAC0`) reach the application as `OPENSC2_POWER_BOOTLOADER`; the blank canvas
ignores them, and so does the library when the application has no `power_request` callback.

Three ways to set it, later ones winning field by field:

1. **Kconfig** (`CONFIG_OPENSC2_*` in `prj.conf`), checked at build time by the rules above (the
   strings in `lib/opensc2/CMakeLists.txt`, characters and length): a value that breaks one stops
   the build, since `opensc2_init()` would refuse it at boot.
2. **At run time**: `config.identity` in `opensc2_init()`. Empty strings and zero numbers keep the
   Kconfig value. An invalid identity makes `opensc2_init()` return `-EINVAL`. A board that learns
   a value only later (a hardware revision read from straps or an EEPROM, its own bootloader's build
   time) hands it over with `opensc2_identity_set(&id)` at any time after `opensc2_init()`: read
   the same way, kept in RAM only (nothing is stored, so the board supplies it again at every
   boot, as the real controller reads its own from its chip), answered at once by `0x83`, `0xAE`
   and `0xF2` on every link and by the radio's identity record at its next connection; the USB
   strings, set when the device attaches, take it at the next boot. A stored field (3) still wins.
3. **On a built board, without rebuilding**: `opensc2_identity_store(&id)` (fields left empty or
   zero keep what they are), the shell (`opensc2 id set <field> <value>`, `opensc2 id clear`), or, if
   `CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE` is on, vendor command `0xEE` with the setting names
   `opensc2/unit_serial`, `opensc2/board_serial`, `opensc2/git_revision`, `opensc2/board_revision`.
   The shell and `0xEE` store exactly the one field they name, as given, 0 included:
   `opensc2 id set blbuild 0` reports no bootloader build time, whatever was stored or configured
   before. The shell takes numbers in hex with `0x` (`0x48`, `0x6A628345`) or a lone `0`, and refuses
   anything else, so a value typed without its `0x` or with a stray character is never stored as
   another number. It refuses a build time at or before `0x6A18D057` unless `force` follows the
   value (`opensc2 id set build 0x6A18D057 force`, for a deliberate test of Steam's update rule).
   The product name is set with `opensc2 id set name <text>` or `opensc2_identity_store()` (`0xEE`
   has no key for it): a name with spaces goes in double quotes,
   `opensc2 id set name "My Steam Controller"` (inside the quotes `\"` is a quote character;
   unquoted, a backslash before a space keeps it), and the shell refuses a name of several unquoted
   words rather than store the first. `opensc2 id` shows it, `opensc2 id clear` forgets it with the
   rest.
   Stored under the settings keys `opensc2/id` and `opensc2/name` (the name has its own, so firmware
   without names still reads the first); validated before it is stored; kept by a factory reset.
   The USB strings take a new identity at the next boot.

| Function | Does |
| --- | --- |
| `opensc2_identity_get(&id)` | the identity in use: Kconfig, then `config.identity`, then what is stored, field by field |
| `opensc2_identity_set(&id)` | change the configured identity in RAM, nothing stored (fields left empty or zero keep their value; stored fields still win); validated first (`-EINVAL`, nothing changes), `-EACCES` before `opensc2_init()`; the command channel and new connections use it at once, the USB strings from the next boot |
| `opensc2_identity_store(&id)` | store fields on the board (empty or zero fields are not stored and keep their value); validated first (`-EINVAL`); new connections use it at once, the USB strings from the next boot |
| `opensc2_identity_clear()` | forget the stored identity, the product name too (the settings keys `opensc2/id` and `opensc2/name`): back to the `opensc2_init()` and Kconfig values. Returns 0 or the settings subsystem's negative errno |
| `opensc2_identity_check(&id)` | pure: 0 for a valid identity, or a mask of `OPENSC2_ID_BAD_*` problems |

## Bonds and pairing

A bond is one pairing with one puck, 24 bytes: `puck_id`, `controller_id` and the puck's serial. Like
the real controller there are two slots, so one board can be paired with two pucks;
`opensc2_select_slot(0|1)` chooses: it selects the wireless transport "the puck with that bond",
stored once the puck connects ([Wireless transports](#wireless-transports-and-bluetooth-le)). The
shell numbers the two pairing slots 1 and 2; the API numbers them 0 and 1.

| Function | Does |
| --- | --- |
| `opensc2_bond_get(slot, &bond)` | read one |
| `opensc2_bond_set(slot, &bond)` | store one (both IDs zero: delete). Listening restarts with it. |
| `opensc2_select_slot(slot)`, `opensc2_selected_slot()` | the slot in use (`opensc2_select_transport()` with `OPENSC2_TRANSPORT_PUCK_1` or `_2`) |

**Over the cable** (how Steam pairs a real controller): with the board wired and a puck on the PC,
Steam writes the puck's slot, then the board's bond with the settings-by-name commands (`0xED` read,
`0xEE` set, `0xEF` save on `esb/bond`), and sends `0x95` with `0xA427AF52`. The board comes back
wireless and finds the puck with the new bond. Both IDs are chosen on the PC side, new at every
pairing.

**Over the air**: pairing over the air (the puck's pairing window) is implemented but untested with
Steam. Steam pairs controllers over a wired connection, and pairing over the USB-C cable is the
tested way. While a puck's pairing window is open (60 s), `opensc2_pair_start()` answers the puck's
beacon, exchanges IDs (the controller ID is this board's choice), stores the bond in the selected
slot (replacing what was there) and starts listening; the puck then connects it. A wireless board
answers an open window when its pairing button is pressed; with
[Counterpuck](https://github.com/ded811/Counterpuck) as the puck, its shell command `pair 0` opens a
window without Steam (also untested with OpenSC2).

Bonds can also arrive through `0xA2` (the first slot, API slot 0). Treat the stored IDs as private:
they identify the puck.

Storage keys (Zephyr settings): `opensc2/bond0`, `opensc2/bond1`, `opensc2/transport` (the
wireless transport, 0 to 3), `opensc2/slot` (the puck's slot, written with a puck transport;
firmware before the transport key reads only this one), `opensc2/chime`, `opensc2/id`,
`opensc2/name`, `opensc2/mode`, the values the library keeps for settings by name under
`opensc2/k/` ([Settings by name](#settings-by-name)), and with `CONFIG_OPENSC2_CAL` the calibration
keys under `opensc2/cal/` (`opensc2/cal/joy_l` for `cal/joy_l`, and so on;
[Calibration and raw inputs](#calibration-and-raw-inputs)). A Bluetooth build also keeps Zephyr's
`bt/` keys (bonds, identities, the name). A factory reset (`0x86`) deletes the bonds, the transport,
the chime level, the kept values the real controller's reset deletes and every `bt/` key; it keeps
the identity, the mode policy and the calibration.

## Wireless transports and Bluetooth LE

A wireless boot runs one transport, as on the real controller: the puck's 2.4 GHz radio with one
of the two bonds, or, in a build with `CONFIG_OPENSC2_BLE`, Bluetooth LE in one of two slots. The
choice is the real controller's `user/wireless_transport`, which Steam reads with `0xED` when it
pairs a controller over the cable:

| Value | `enum opensc2_transport` | Shell word | Transport |
| --- | --- | --- | --- |
| 0 | `OPENSC2_TRANSPORT_BLE_1` | `ble` | Bluetooth, slot 1 |
| 1 | `OPENSC2_TRANSPORT_BLE_2` | `ble2` | Bluetooth, slot 2 |
| 2 | `OPENSC2_TRANSPORT_PUCK_1` | `puck` | the puck with bond slot 0 (the default) |
| 3 | `OPENSC2_TRANSPORT_PUCK_2` | `puck2` | the puck with bond slot 1 |

| Function | Does |
| --- | --- |
| `opensc2_select_transport(t)` | Run `t` from now on: the running transport stops and hands over the radio (only one runs at a time). Stored once its link is up, as the October 2026 firmware does (the puck: connected; Bluetooth: connected, encrypted, an ATT MTU of 64 or more), so a choice whose link never comes up is not kept; in wired mode it is stored at once, for the next wireless boot. With the system state machine it is also the machine's choice from then on (the LED's colour, the battery wait's start, `opensc2 power`). `-ENOTSUP` for Bluetooth without `CONFIG_OPENSC2_BLE`, `-EINVAL` for another value. |
| `opensc2_select_transport_pairing(t)` | The same with the pairing flag that the real controller's transport chords give when held 4 s: Bluetooth opens the slot for a new host (pairing mode on the slot's other identity, which is reset first, so the old host's bond stays until a new host has paired), even when that slot is connected; the puck's radio ignores the flag, as the real controller's does; in wired mode the choice is stored and the flag dropped. |
| `opensc2_selected_transport()` | the transport this boot runs |

A boot runs the stored transport (a board from before the transport key: the slot it stored).
A host can also write the key: `0xEE` sets the choice in RAM (a new puck slot counts at once, as a
new bond does; a change between the puck and Bluetooth is not run at once: saved with `0xEF`, it
counts from the next boot) and `0xEF` saves it. Without Bluetooth, `0xEE` with 0 or 1 is taken and
changes nothing, as before.

### The Bluetooth build

`CONFIG_OPENSC2_BLE` (off by default) makes the board a Steam Controller 2 over Bluetooth LE: a
HID-over-GATT peripheral that Windows, phones and SDL can use without the puck. **Built; not yet
tested on hardware.** The blank canvas's opt-in build is `app/ble.conf`
([app/README.md](../app/README.md#test-build-bluetooth-le)). What a host sees, following the
protocol documentation's Bluetooth page ([protocol.md](protocol.md#overview-bluetooth-le) has the
summary and the gaps):

- **Name and advertising**: "Steam Ctrl (BT) " and the unit serial (29 characters). Three forms of
  connectable advertising, chosen at each start from the identity in use: **pairing mode** when it
  has no bond (undirected every 100 to 150 ms, anyone may connect; the flags, the gamepad
  appearance, the HID and battery service UUIDs, the TX power and the manufacturer data of company
  ID `0x055D` with no payload, and the name in the scan response); **directed** when it has one
  (high duty cycle to the bonded host, 1.28 s at most); and **allow-listed** after a directed try
  or a connection that failed (undirected every 100 to 150 ms, only the bonded host on the filter
  accept list; only the flags, the TX power and the manufacturer data).
- **The advertising window**: every start of advertising arms `CONFIG_OPENSC2_BLE_ADV_WINDOW_S`
  (60 s, as on the real controller; 0: none). When it ends without a connection, advertising stops:
  "wireless off", the link `OFF` with reason `LISTEN_TIMEOUT`; with the system state machine the
  machine takes it (wireless off on USB power, a shutdown on a battery build), without it the
  application gets `OPENSC2_POWER_WIRELESS_OFF`. `opensc2_start()` advertises again.
- **Identities and slots**: five identities (0, never used to advertise, and 1 to 4), each with its
  own static random address. Slot 1 (`OPENSC2_TRANSPORT_BLE_1`) uses identities 1 and 2, slot 2
  identities 3 and 4. A start uses the slot's first identity if it has a bond or neither has one,
  otherwise the second; a pairing request (`opensc2_pair_start()`,
  `opensc2_select_transport_pairing()`, the shell's `opensc2 ble pair`) resets the slot's other
  identity (a fresh address, no keys) and advertises on it in pairing mode, so a request no host
  answers loses nothing.
- **Pairing and bonds**: Just Works with LE Secure Connections only, bonding; every HID and vendor
  value needs an encrypted link. A host that bonds on an identity that had no bond when advertising
  started is the slot's new host: the bond on the slot's other identity is deleted, so each slot has
  one host at a time and each new pairing appears to hosts as a new device. A bond the stack deletes
  by any route resets its identity (only logged while that identity is in use). A host that bonds
  on a second identity loses its bond on the first (`CONFIG_BT_ID_UNPAIR_MATCHING_BONDS`); a host
  that asks to pair again on the identity it is bonded to is refused (SMP error `0x03`): it needs a
  pairing request first. A factory reset (`0x86` with its key) deletes the whole `bt/` subtree:
  bonds, identities and the name; the next start creates four new identities.
- **Disconnects**: when the host ends the link (HCI `0x13`) or powers off (`0x15`): "wireless off",
  the link `OFF` with reason `HOST_DISCONNECTED`, and, without the system state machine,
  `OPENSC2_POWER_WIRELESS_OFF` (with it, the machine takes it as above). Any other reason
  (a supervision timeout, `0x08`): 500 ms later the same identity advertises again, directed to its
  host first, with a fresh window. When the board ends the link itself (`opensc2_stop()`, another
  slot, a pairing request) it stops its state reports and sends an all-zero `0x45` if the link is
  past the gate, so that is the last state the host sees, waits 100 ms and disconnects with `0x13`;
  nothing more follows. A reboot just resets (the host sees the link time out).
- **Services**: the library's own HID service with the USB report map byte for byte (inputs `0x40`
  to `0x45`, outputs `0x80` to `0x89`, feature report 1), the library's own Device Information
  Service (model "Steam Controller", an empty manufacturer name, PnP ID `28DE:1303` version
  `0x0100`, the unit serial, the build time as the firmware and software revision), Zephyr's
  Battery Service, and Valve's vendor service (`CONFIG_OPENSC2_BLE_VENDOR_SERVICE`, on by default),
  in the real controller's order after Zephyr's GATT and GAP services (battery, vendor, device
  information, HID), which with this build's configuration also gives its handles (vendor 21 to 61,
  device information 62 to 74, HID 75 to 140).
- **Valve's vendor service**, for hosts that talk GATT themselves (SDL's Android and Apple back
  ends): UUIDs `100F6Cxx-1735-4313-B402-38567131E5F3`, the service `xx` = `32`, the command channel
  `34` (feature report 1, read and write), inputs `0x40` to `0x45` as `75` to `7A` (read, notify)
  and outputs `0x80` to `0x89` as `B5` to `BE` (read, write, write without response): 41
  attributes, every value encrypted. The characteristics share their values and handlers with the
  HID service's, so a SET written on one is the stored request of the other; a write shorter than
  the report is taken (SDL leaves out the last byte). An input report goes out on the vendor
  characteristic when the host enabled its notifications there, else on the HID service's.
- **The gate**: reports flow once the link is encrypted and its ATT MTU is 64 or more; then
  Bluetooth is the active transport (`0x81` and `0x85` act, attribute 11 of `0x83` is the connection
  interval in µs) and the choice is stored.
- **Reports**: one state report per connection event, built one connection interval after the event
  less `CONFIG_OPENSC2_BLE_REPORT_LEAD_US` (500 µs) and sent as `0x45` (the first 45 bytes of
  `0x42`); lizard mode's `0x40` and `0x41`, `0x43` (which also sets the battery service's level) and
  `0x44` go out on their characteristics, each only where the host enabled notifications (the
  vendor service's first).
- **Outputs and commands**: output reports written with or without response go to the same
  dispatcher as on every link; feature report 1 is the vendor command channel (a write is a SET, a
  read a GET), with refusals as ATT error `0x04` and a write past 64 bytes as `0x07`. Power and
  reboot commands act as on the other links; a reboot into wireless mode comes back on Bluetooth when
  Bluetooth is the stored transport.
- **Connection**: 7.5 ms, latency 0 and 420 ms asked for 5 s after connecting, every valid host
  request accepted (the stack refuses invalid ones; a longer interval is logged as suboptimal), the
  2M PHY asked for; no data-length update of its own.
- **The radio**: the puck's ESB and Bluetooth share it, one at a time. Both take their interrupts at
  run time; MPSL (Nordic's radio scheduler, in this build also the clock driver for the radio and the
  USB console) stays initialised. Bluetooth starts once ESB is off and gives the radio back by
  disabling its stack.
- **The low-frequency clock**: the SuperMini has no 32.768 kHz crystal, so its board file chooses
  the internal RC oscillator with calibration (500 ppm); in the Bluetooth build MPSL calibrates it
  every 4 s, whichever transport runs.

With the [system state machine](#power-and-the-system-state-machine) (on by default) Bluetooth is
one of its transports: B + right bumper and B + left bumper choose slot 1 and 2, held 4 s they ask
for pairing mode ([Chords](#chords)); pairing mode's start and a new host raise cues 6 and 7, and
advertising that runs out or a host that leaves end in "wireless off" as on the puck (the machine's
drop that follows keeps Bluetooth's wireless-off flag and its reason for `opensc2 ble status`).
Without the machine the chords are an application's (`opensc2_select_transport()`,
`opensc2_select_transport_pairing()`) and the cues its own, from `link_changed`. The shell's
`opensc2 ble status` shows the link, and the hardware test suite's `--bluetooth` path checks it on a
board paired with Windows ([testing.md](testing.md#bluetooth)); nothing of it has run on
hardware yet.

What a project needs, as `app/ble.conf` sets it:

| Line | Why |
| --- | --- |
| `CONFIG_BT=y`, `CONFIG_BT_PERIPHERAL=y`, `CONFIG_BT_MAX_CONN=1` | the host and the SoftDevice Controller (its peripheral library) |
| `CONFIG_OPENSC2_BLE=y` | this (it selects `CONFIG_ESB_DYNAMIC_INTERRUPTS`, `CONFIG_MPSL_DYNAMIC_INTERRUPTS` and `CONFIG_BT_CTLR_SDC_EVENT_TRIGGER`) |
| `CONFIG_BT_SMP=y`, `CONFIG_BT_SMP_SC_PAIR_ONLY=y`, `CONFIG_BT_BONDABLE=y` | Just Works bonding, LE Secure Connections only |
| `CONFIG_BT_SETTINGS=y`, `CONFIG_BT_ID_MAX=5`, `CONFIG_BT_MAX_PAIRED=5`, `CONFIG_BT_FILTER_ACCEPT_LIST=y` | bonds and identities kept; five identities; advertising for the bonded host only (the first two checked at build time) |
| `CONFIG_BT_ID_UNPAIR_MATCHING_BONDS=y`, `CONFIG_BT_ID_ALLOW_UNAUTH_OVERWRITE=y` | a host that bonds on another identity loses its older bond (checked); an unauthenticated pairing may replace keys held on another identity, never those of the identity in use. Leave `CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE` off: a host pairing again on its own identity is refused, as on the real controller |
| `CONFIG_BT_DEVICE_NAME_DYNAMIC=y`, `CONFIG_BT_DEVICE_NAME_MAX=31`, `CONFIG_BT_DEVICE_APPEARANCE=964` | the name with the unit serial; the gamepad appearance (the first two checked) |
| `CONFIG_BT_BUF_ACL_RX_SIZE=71`, `CONFIG_BT_BUF_ACL_TX_SIZE=71`, `CONFIG_BT_L2CAP_TX_MTU=67`, `CONFIG_BT_ATT_PREPARE_COUNT=4` | an ATT MTU of 67 (64 or more checked); long writes of the 64-byte feature report at the default MTU |
| `CONFIG_BT_PERIPHERAL_PREF_MIN_INT=6`, `_MAX_INT=6`, `_LATENCY=0`, `_TIMEOUT=42`, `CONFIG_BT_AUTO_PHY_PERIPHERAL_2M=y`, `CONFIG_BT_AUTO_DATA_LEN_UPDATE=n` | the connection parameters and the PHY |
| `CONFIG_BT_BAS=y` | the battery service (checked) |
| `CONFIG_BT_RX_STACK_SIZE=3072` | vendor commands run in the host's receive thread, and some write flash |
| `CONFIG_BT_UNINIT_MPSL_ON_DISABLE` left off | MPSL must stay initialised (checked) |

The EGU whose interrupt carries the connection-event trigger is `egu3` (SWI3); a devicetree can
choose another as `opensc2,ble-egu` (ESB uses `egu0`, MPSL `egu5`).

## Vendor commands

Steam's feature report 1 reaches the controller through the puck, as records in its packets, on
USB, as a 64-byte SET and a GET, and over Bluetooth as a write and a read of the feature report's
characteristic. All work the same way, as on the real controller: a SET runs a command and, if it
succeeds, stores it; a GET answers the stored one. Each link keeps its own stored request, `0x83`
until a SET (Bluetooth's starts again at every connection, as the radio's does). A GET record's one
byte is not the opcode: Valve's puck leaves it unset, and drops an answer whose opcode is not its
SET's. A refused command is a result code through the puck, a STALL on USB and ATT error `0x04`
over Bluetooth; a GET of a command that has no answer is `{opcode, 0}` through the puck, a STALL on
USB and ATT error `0x04` over Bluetooth.

The rules every command follows, the real controller's:

- **The payload is read whatever the length says.** A request is `{opcode, length, payload}`, the
  payload read from a 61-byte buffer: the bytes the link carried after the length, zeros after
  them (a puck record shorter than its length reads zeros). So `0x95` with a length of 0 still
  reads its key, `0xDC` its type and level, `0xC5` its four channels.
- **The result codes** through the puck (and the dock, and the test hooks' host emulation) are the
  real controller's numbers: `-22` (`EINVAL`) for an opcode nobody knows, a length over 61, a
  malformed name and the handlers' refusals; `-95` (`EOPNOTSUPP`) for a GET of a command that only
  acts; `-19` (`ENODEV`) and `-134` (`ENOTSUP`) from the settings by name (below). On USB every
  refusal is a STALL. (The numbers are the nRF Connect SDK C library's, which are the real
  controller's; a host build of the library has its platform's.)
- **A GET's reply** starts as the stored request (its length + 2 bytes) with the answer written
  over it, so the bytes after the answer are the request's, then the reply buffer's: on USB the
  rest of the last `SET_REPORT` the board received (the USB stack reuses that buffer), elsewhere
  zeros. A command reads its parameters in the reply too: a `0xED` name sent without its NUL runs
  on into those bytes (Steam's zero padding ends it).
- **The USB reply length** is the device form's: 63 bytes in the October form (the default: the
  61st byte of a 61-byte answer is not sent; no answer of the real controller is that long), 64 in
  July's. The hardware test channel's own opcode (`0x7E`, answered only while the hooks are armed)
  always gets 64.
- **What runs after the answer:** the power requests 20 ms later (one waits at a time: a later one
  takes its place but keeps the first one's time; a turn-off is dropped then while wired), the
  factory reset 10 ms later, a `0xA2` bond as soon as the system work queue runs it.

The library answers these itself:

| Opcode | Kind | Answer / action |
| --- | --- | --- |
| `0x83` | get | six `{tag, u32}`: 1 product ID, 2 capabilities (0), 10 bootloader build time, 4 build time, 9 board revision, 11 the current link's report interval, whichever link asks: 4000 µs on USB and through the puck, the connection interval over Bluetooth once it is active (0 before), 64000 µs while the [dock](#the-dock) is the only link. Every value comes from the [identity](#identity) except 11 |
| `0xAE` | get | the tag, then a string zero padded to 20 bytes: 1 unit serial, 0 and 4 board serial, 3 git revision, others "NA" (the tag byte is read whatever the length; the byte after the answer is written 0, the place of a twentieth character) |
| `0xF2` | get | selector 0: build time, board revision, git revision (16), unit serial (16); 1: the last crash record as it is (33 bytes, zeros when there is none), after which a new record is marked seen and saved again ([Crash records](#crash-records)); 2: uptime in ms (u32) and the last disconnect reason (u32); any other: length 0 |
| `0xA1` | get | 18 bytes. Selector 1: 1, a byte of reset-cause flags in the real controller's encoding (`0x01` power-on, `0x02` brown-out, `0x08` debug, a low-power wake, a CPU lock-up or any other cause, `0x10` the reset pin, `0x20` the watchdog, `0x40` software), the chip's device ID (`hwinfo_get_device_id()`, 16 bytes of room: 8 bytes and 8 zeros on an nRF52); any other selector: 0 and 17 zero bytes. The flags come from Zephyr's `hwinfo_get_reset_cause()`; nothing clears the chip's register, so the causes of several resets add up until a power-on, as on the real controller. A cause that cannot be read refuses the GET, as on the real controller (a chip without the function, `-ENOSYS`, reports none) |
| `0xBE` | get | battery data, 15 bytes (from `opensc2_set_battery()`; byte 13 is 1 while charging, byte 14 the charge state without a `0x80` flag: [Battery](#battery)) |
| `0xE9` / `0xC5` | get / set | LED colour, four percentages. `0xC5` keeps each as sent, 0..255, as the real controller does (above 100 is brighter, up to full duty: the system state machine's LED rules render it so) and calls `led`; `0xE9` reads it back as the real controller does, cut to whole hundreds: 0 below 100, 100 from 100 to 199, 200 from 200 ([protocol.md, gap 24](protocol.md#where-this-library-had-to-fill-gaps)) |
| `0xDB` / `0xDC` | get / set | user store type 1: the power-on chime level. `0xDC` type 1 saves it and then uses it (a save that fails leaves the level in use as it was); other types do nothing; both are acknowledged (`opensc2_chime_level()`) |
| `0x89` / `0x87` / `0x8E` | get / set / set | controller settings (calls `setting_changed`); `0x87` takes length ÷ 3 triplets, ignoring bytes after the last whole one; `0x89` answers the defaults of settings 48, 84 and 85, and 0 for a number above 85, as the real controller does ([Controller settings](#controller-settings)) |
| `0x81` / `0x85` | set | lizard mode off / its keyboard and buttons back ([Lizard mode](#lizard-mode)); refused without an active link (Bluetooth counts once past its gate); from the dock acknowledged and ignored |
| `0xED` / `0xEE` / `0xEF` / `0xF0` | get / set / save / delete by name | [below](#settings-by-name) |
| `0xE2` | set | persist calibration: side 0 or 1 (read whatever the length says), mask bit 0 the trigger, 1 the stick, 2 the pressure key of that side saved from RAM (with `CONFIG_OPENSC2_CAL`), then `vendor_command`, which may do something with it; refused above 1 (-1, as on the real controller) |
| `0xD8`, `0xC0`, `0xC3`, `0xFE` | set | with `CONFIG_OPENSC2_CAL`: the stick, trigger and pressure recordings and the calibration erase ([Calibration and raw inputs](#calibration-and-raw-inputs)) |
| `0xA2` | set | the bond of the first slot (API slot 0, shell slot 1), exactly 24 bytes, stored for the next boot (an ID of zero deletes the stored one); the bond in use, and `0xED esb/bond`, stay as they are until then, as on the real controller. Steam pairs with `0xEE` and `0xEF` instead |
| `0x86` | set | factory reset, only with its key `0xEFC1A917` (another is acknowledged and ignored): 10 ms later the bonds, the transport choice, the chime level, the kept keys of the `user`, `mte` and `debug` groups and the Bluetooth keys are deleted from flash (calibration, the identity and the mode policy stay), then `power_request(OPENSC2_POWER_FACTORY_RESET)` 100 ms later. The values in RAM stay as they are until that reboot |
| `0x90`, `0x95`, `0x9F` | set | bootloader, reboot, turn off: `power_request`, 20 ms later; turn-off is ignored while wired. With the system state machine the turn-off is its event, and the bootloader and reboot requests reach `power_request` in the states that act on them ([The states while it runs](#the-states-while-it-runs)) |

Everything else (anything new, and without `CONFIG_OPENSC2_CAL` the calibration commands `0xD8`,
`0xC0`, `0xC3` and `0xFE`) goes to `vendor_command`; a command nobody knows is refused with `-22`
(on USB a STALL, as the real controller stalls Steam's `0xC1`):

```c
static int on_vendor(const struct opensc2_vendor_cmd *cmd, uint8_t *answer, uint8_t *answer_len,
             void *user)
{
    if (cmd->opcode == 0xB0 && !cmd->is_get) {   /* a command of your own */
        do_my_thing(cmd->payload, cmd->len);
        return 0;                 /* SET result 0 */
    }
    return -ENOTSUP;              /* not mine: refused as an opcode nobody knows (-22) */
}
```

For a GET, write at most 61 bytes to `answer` and set `*answer_len`; `answer` is the reply in
place, so bytes you do not write keep the request's, as the library's own answers do.
`cmd->payload` of a GET is the payload of the SET before it. Return `-ENOTSUP` for a command you do
not know (the library turns it into the real controller's `-22`); any other error is passed on as
it is. Commands may take their time (a flash write): Steam waits for each. While wired the
callback runs in Zephyr's USB stack thread, inside the host's control request: only its return
value counts. (The stack would stall a GET that leaves `errno` set, which C library calls may do;
the library clears it before it answers.)

### Settings by name

`0xED` reads a key, `0xEE` sets it in RAM, `0xEF` saves the RAM value and `0xF0` deletes it (from
flash, then the RAM value is what the key holds when nothing is stored). The payload is the key's
name with its NUL, then for `0xEE` the value. The rules are the real controller's, whose commands
hand the name to its settings handlers:

| Command | Refused (`-22`) | Otherwise |
| --- | --- | --- |
| `0xED` | an empty name (the GET) | the value, at most 61 bytes; length 0 for a name nobody has, a key without a read function, a read function with nothing valid to give, and a name with no NUL in the 61 bytes |
| `0xEE` | an empty name, no value, a value of another size than the key's, a name nobody has | the value in RAM only |
| `0xEF` | the NUL not the last byte, a name nobody has | saves what the key's read function gives; `-134` (`ENOTSUP`) for a key without one, `-19` (`ENODEV`) when it has nothing valid to give; nothing is written then |
| `0xF0` | the NUL not the last byte | deletes it from flash and gives the RAM value what a deleted key holds; a name nothing stores is acknowledged |

A name goes, in this order, to the library's keys (the table below: every key of the real
controller), the calibration store (with `CONFIG_OPENSC2_CAL`), the application's `vendor_command`
(return `-ENOTSUP` to pass), and then to **any other Zephyr settings handler of the build**, as on
the real controller: Bluetooth's `bt/...` keys in a Bluetooth build, and every handler your
application registers (`settings_runtime_set()`, `_get()`, `settings_save_one()`,
`settings_delete()`; needs `CONFIG_SETTINGS_RUNTIME`, which `CONFIG_OPENSC2_STORAGE` implies). A custom board
can so expose keys of its own to Steam-style tools; keep anything a host must not change out of the
settings handlers, or answer its names in `vendor_command`. The library's own subtree (`opensc2/`)
is never reached this way.

| Key | Size | `0xED` | Kept as | The factory reset (`0x86`) |
| --- | --- | --- | --- | --- |
| `esb/bond`, `esb/bond_2` | 24 | the bond, length 0 when empty (`0xEF` of an empty one: `-19`) | the two bonds (`opensc2/bond0`, `bond1`) | deletes |
| `user/wireless_transport` | 1 | the byte in RAM: the transport stored, or chosen at boot when nothing is ([Wireless transports](#wireless-transports-and-bluetooth-le)) | `opensc2/transport`, and `opensc2/slot` for a puck transport. `0xEE` takes any byte into RAM and changes nothing else; once saved, the next boot runs it (a value that names no transport, or Bluetooth in a build without it, starts the main puck); `0xF0` gives back 2 | deletes |
| `user/haptic_boot_level` | 1 | the chime level | `opensc2/chime` | deletes |
| `user/keylock` | 4 | the value, zeros until set | `opensc2/k/keylock` | deletes |
| `user/fully_charged_led_intensity` | 1 | the level, 3 until set | `opensc2/k/fcled` | deletes |
| `mte/charge_level` | 8 | the value, zeros until set, while it is a valid window (both values 0, or a lower one of at least 3400 and an upper one of at most 4120, u32 mV each, in 10 mV steps); otherwise length 0 (`0xEF`: `-19`) | `opensc2/k/mte` | deletes |
| `debug/resetreason` | 33 | the crash record, zeros when there is none | `opensc2/k/crash` | deletes |
| `settings/haptics/haptic_master_gain_db`, `amplifier_mode`, `enabled` | 2 each | length 0 (no read function; `0xEF`: `-134`) | `opensc2/k/hgain`, `hamp`, `hen` | keeps |
| `cal/joy_l`, `_r` (18), `cal/trg_l`, `_r` (7), `cal/prs_l`, `_r` (8), `cal/touch_l`, `_r` (10), `cal/rgbw_r`, `_g`, `_b`, `_w` (1), `cal/volt_offset` (2), `cal/sensors/gyroscope/bias` (12) | as listed | with `CONFIG_OPENSC2_CAL` the library's calibration store ([Calibration and raw inputs](#calibration-and-raw-inputs)): a stick, trigger, pressure or touch key while its format word is not 0, the battery offset always, an LED trim while it lies within 1..199, the gyro bias never; without it the application's `vendor_command`, length 0 without one | `opensc2/cal/...` with `CONFIG_OPENSC2_CAL`, else the application's | keeps |

The system state machine acts on `user/keylock` ([The keylock](#the-keylock)) and
`user/fully_charged_led_intensity` ([Status LED](#status-led)); `mte/charge_level` and the haptics
keys are kept, but nothing in the library acts on them yet; `debug/resetreason` holds the
[crash record](#crash-records). With `CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE`, `0xEE` also takes
OpenSC2's identity keys ([Identity](#identity)).

## Controller settings

86 numbered 16-bit values that Steam writes with `0x87` and reads with `0x89`, in RAM only (defaults
at every boot), as on the real controller. `opensc2_setting_get(n)`; constants in
`<opensc2/settings.h>` (`OPENSC2_SETTING_*`, with SDL's names in the comments). The ones a project is
most likely to act on:

| # | Constant | Default | Meaning |
| --- | --- | --- | --- |
| 9 | `OPENSC2_SETTING_LIZARD_MODE` | 1 | lizard mode; Steam sends `0x81`, SDL writes 0 |
| 11, 12 | `OPENSC2_SETTING_MOMENTUM_MIN_VEL`, `_DECAY` | 200, 100 | lizard mode's glide: the flick speed it needs (pad units per frame), its length in frames (used by the library) |
| 14 | `OPENSC2_SETTING_HAPTIC_INCREMENT` | 5500 | lizard mode's tick spacing (used by the library) |
| 27 | `OPENSC2_SETTING_RUMBLE_GAIN_DB` | -10 | rumble gain offset, dB: on the real controller the left grip's only (the haptics synthesiser does the same) |
| 36 | `OPENSC2_SETTING_IR_LED_BRIGHTNESS` | 0 | the IR LED, percent ([Status LED](#status-led)) |
| 37 | `OPENSC2_SETTING_LED_OVERRIDE` | 0 | show `0xC5`'s colour |
| 38 | `OPENSC2_SETTING_LED_CURVE` | 1 | not 0: setting 45 goes through the brightness curve ([Status LED](#status-led)) |
| 45 | `OPENSC2_SETTING_LED_BRIGHTNESS` | 50 | status LED brightness, percent |
| 46 | `OPENSC2_SETTING_RAW_STICKS` | 0 | raw stick output: 1 millivolts, 2 ADC counts (the application's to give; the blank canvas does; the library's stick-touch fallback is off meanwhile) |
| 48 | `OPENSC2_SETTING_IMU_MODE` | 0 | IMU stream: `OPENSC2_IMU_MODE_SEND_RAW_ACCEL` \| `_SEND_RAW_GYRO` from SDL when sensors are on |
| 50 | `OPENSC2_SETTING_SLEEP_TIMEOUT_S` | 900 | idle power-off on battery, seconds (used by the [system state machine](#the-idle-timeout)) |
| 52, 53 | `OPENSC2_SETTING_LEFT/RIGHT_CLICK_PRESSURE` | 40 | pad click threshold, percent (-1: 100 %), with `CONFIG_OPENSC2_PAD_CLICK_AUTO`; -1 also silences lizard mode's click sound (used by the library) |
| 60, 62 | `OPENSC2_SETTING_RAW_PRESSURE`, `_RAW_TRIGGERS` | 0 | raw pad pressure; raw triggers (1 millivolts, 2 counts): both the application's to give, and no trigger bit while 62 is not 0 (used by the library) |
| 68 | `OPENSC2_SETTING_TRIGGER_THRESHOLD` | 90 | trigger digital threshold, percent (used by the library) |
| 70, 76 | `OPENSC2_SETTING_HAPTICS_ENABLED`, `_HAPTIC_GAIN_DB` | 1, -3 | 70: lizard mode's ticks and clicks only while it is exactly 1 (used by the library); 76: the master gain |
| 71 | `OPENSC2_SETTING_STEAM_WATCHDOG` | 1 | the 10 s lizard watchdog (used by the library) |
| 77 | `OPENSC2_SETTING_REBOOT_ON_OFF` | 0 | not 0: the ordinary shutdown reboots instead of turning off ([Shutdown and power-off](#shutdown-and-power-off)) |
| 80 | `OPENSC2_SETTING_PAD_STABILIZER` | 1 | 1: lizard mode's right pad has the touch stabiliser; 2: both pads' reported positions have it, with `CONFIG_OPENSC2_PAD_RULES` ([Calibration and raw inputs](#calibration-and-raw-inputs)), and lizard mode's right pad none of its own (used by the library) |
| 82 | `OPENSC2_SETTING_PAD_IGNORE_ON_STICK` | 3 | ignore a pad while its stick is touched, in report `0x42` and lizard mode (used by the library) |

With `CONFIG_OPENSC2_CAL` the library also acts on 30, 31, 34, 35, 46, 52, 53, 60 and 62 for raw
inputs, and with `CONFIG_OPENSC2_PAD_RULES` on 2, 24 and 80 for the reported pads
([Calibration and raw inputs](#calibration-and-raw-inputs)).

`0x87` clamps every value to its setting's range (`opensc2_setting_clamp()`), as the real controller
does: for example 2: 0..360, 24: 0..30, 27: −24..12, 46: 0..2, 52 and 53: −1..100, 60: 0..1, 62:
0..2, 68: 40..99, 70: 0..2, 75: 0..1, 76: −24..6, 80: 0..2, 82: 0..3, and Steam's `{8, 7}` is stored
as 1, the top of 8's range. Only 48, 84 and 85 keep any value (the real controller hands them to its
IMU unclamped). Every default and range is in the protocol documentation the same authors wrote
([controller settings](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#controller-settings),
not public yet), also for the 49 settings nothing reads (stored and read back only); the defaults of
28 and 29 (16500, 15000) lie above their range and read back so until a write limits them.
`setting_changed` is called only when a value changes.
`0x8E` and the Steam watchdog keep settings 48, 84 and 85 (the real controller passes those to its
IMU instead of storing them), and `0x89` answers their defaults (0, 1, 100) whatever was written,
as the real controller does; `opensc2_setting_get()` and the `setting_changed` callback give the
application the values Steam wrote. `0x89` of a number above 85 reads 0, also as the real
controller does.

The functions for the settings and the other state Steam sets:

| Function | Returns |
| --- | --- |
| `opensc2_setting_get(n)` | setting `n` as it is now (Steam's last write, or its default); 0 for a number above 85 |
| `opensc2_lizard_mode()` | `true` while lizard mode is on (setting 9 is not 0): from every boot until Steam turns it off, and again when the Steam watchdog brings it back |
| `opensc2_led_get(&led)` | the LED colour and settings Steam set, as the `led` callback gets them: `red`, `green`, `blue`, `white` in percent (`0xC5`, 0..255: above 100 is brighter, up to full duty), `override` (setting 37) and `brightness` (setting 45) |
| `opensc2_chime_level()` | the power-on chime level Steam stores with `0xDC`: 1 quiet, 2 normal (the default), anything else silent |
| `opensc2_setting_set(n, v)` | set setting `n` from the board, exactly as one triplet of `0x87` (clamped; setting 9 switches lizard mode, 71 the Steam watchdog, 37 and 45 call `led`; `setting_changed` on a change); -EINVAL above 85 |
| `opensc2_led_set_color(r, g, b, w)` | the LED override colour from the board, as `0xC5` (0..255 percent each; shown while setting 37 is not 0) |
| `opensc2_chime_level_set(l)` | the chime level from the board, as `0xDC` type 1: saved, then in use; a failed save is returned and changes nothing |
| `opensc2_setting_default(n)` | pure: the default of setting `n` (0 above 85) |
| `opensc2_setting_clamp(n, v)` | pure: `v` clamped to setting `n`'s range, as `0x87` stores it (unchanged for 48, 84, 85 and a number above 85) |

## Kconfig options

| Option | Default | |
| --- | --- | --- |
| `CONFIG_OPENSC2` | n | the library (selects ESB and REBOOT) |
| `CONFIG_ESB_MAX_PAYLOAD_LENGTH` | 128 (128 or more) | checked at build time |
| `CONFIG_ESB_TX_FIFO_SIZE` | 2 recommended | the real controller keeps two replies queued |
| `CONFIG_OPENSC2_THREAD_PRIORITY`, `_STACK_SIZE` | 5, 2048 | the radio thread |
| `CONFIG_OPENSC2_TX_POWER_DBM` | 6 | as the real controller |
| `CONFIG_OPENSC2_REPORT_DELAY_US` | 2000 | sample a state report this long after a keep-alive |
| `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` | 0 (never); 60 with `CONFIG_OPENSC2_BATTERY` | the real controller gives up after 60 s |
| `CONFIG_OPENSC2_NO_BOND_TIMEOUT_S` | 0 (never); 10 with `CONFIG_OPENSC2_BATTERY` | with the selected slot empty, the radio goes `OFF` after this long (the real controller waits 10 s for a bond); 0 to 3600 |
| `CONFIG_OPENSC2_SYSTEM` | y | the [system state machine](#power-and-the-system-state-machine): power, chords, the status LED's mode, cues; off: as v0.2.0 |
| `CONFIG_OPENSC2_BATTERY` | n | the board runs on a battery without USB power: shutdowns turn it off (`OPENSC2_POWER_OFF`; selects `CONFIG_POWEROFF`); off, every shutdown reboots |
| `CONFIG_OPENSC2_POWER_POLL_MS` | 50 | how often the system state machine looks at the power source; 10 to 1000 |
| `CONFIG_OPENSC2_PAIRING_TIMEOUT_S` | 60 | |
| `CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS` | 500 | |
| `CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS` | 3500 | 0: never |
| `CONFIG_OPENSC2_TRIGGER_CLICK_AUTO` | y | |
| `CONFIG_OPENSC2_LIZARD_MODE`, `_STEAM_WATCHDOG` | y, y | |
| `CONFIG_OPENSC2_LIZARD_STABILISER`, `_SMOOTHING`, `_GLIDE`, `_TICKS` | y, y, y, y | lizard mode's processing ([Lizard mode](#lizard-mode)) |
| `CONFIG_OPENSC2_STICK_TOUCH_FALLBACK`, `CONFIG_OPENSC2_PAD_BLANK_ON_STICK`, `CONFIG_OPENSC2_PAD_CLICK_AUTO` | y, y, n | [the rules on every report](#the-rules-on-every-report) |
| `CONFIG_OPENSC2_STORAGE` | y with `CONFIG_SETTINGS` | bonds, identity and policy in Zephyr settings |
| `CONFIG_OPENSC2_SHELL` | y with `CONFIG_SHELL` | |
| `CONFIG_OPENSC2_FUSION` | n | the IMU fusion helper |
| `CONFIG_OPENSC2_CAL` | y | the calibration keys and commands, raw inputs ([Calibration and raw inputs](#calibration-and-raw-inputs)) |
| `CONFIG_OPENSC2_CAL_PRESSURE_CLICK` | y | raw pad pressure also sets the pad's click bit |
| `CONFIG_OPENSC2_CAL_STICK_DEAD_ZONE` | 0 | a centre dead zone of raw sticks, report units (the real controller has none) |
| `CONFIG_OPENSC2_CAL_STICK_GROW_RANGE` | n | July 2026's run-time stick range |
| `CONFIG_OPENSC2_PAD_RULES` | y | the pad rotation (settings 2, 24) and stabiliser (setting 80 = 2) |
| `CONFIG_OPENSC2_HAPTICS_SYNTH` | n | the [haptics synthesiser](#the-haptics-synthesiser) |
| `CONFIG_OPENSC2_HAPTICS_THREAD_PRIORITY`, `_THREAD_STACK_SIZE`, `_QUEUE_DEPTH` | 6, 1536, 20 | its two threads and their command queues |
| `CONFIG_OPENSC2_HAPTICS_DROP_SOUNDS` | y with the synthesiser | the drop sounds after `opensc2_haptics_free_fall()` |
| `CONFIG_OPENSC2_HAPTICS_OUT_I2S`, `_OUT_PWM_DAC`, `_OUT_DRV2605` | n | its built-in outputs, each with its devicetree node (untested on hardware) |
| `CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE` | n | identity through `0xEE` |
| `CONFIG_OPENSC2_HAPTIC_STOP_STATUS` | y | `0x44 {channel, 0x02}` after every stop, as the real controller ([Other input reports](#other-input-reports)) |
| `CONFIG_OPENSC2_CRASH_RECORD` | y (n with `CONFIG_RESET_ON_FATAL_ERROR`) | the fatal-error handler: a crash record, then a reset ([Crash records](#crash-records)) |
| `CONFIG_OPENSC2_WATCHDOG` | n (not yet tried with the UF2 bootloader) | the real controller's hardware watchdog with its warning crash ([The hardware watchdog](#the-hardware-watchdog)) |
| `CONFIG_OPENSC2_STORAGE_RECOVERY` | y with NVS settings | erase a settings partition with every sector occupied ([The settings partition](#the-settings-partition)) |
| `CONFIG_OPENSC2_RX_QUEUE_DEPTH`, `_REPORT_QUEUE_DEPTH`, `_REPORT_WAIT_DEPTH` | 8, 6, 8 | the radio's queues; lizard and application reports wait for room in the last 8 instead of being dropped |
| `CONFIG_OPENSC2_DEFAULT_POLICY_AUTO` / `_WIRELESS` | auto | the mode policy until one is stored |
| `CONFIG_OPENSC2_USB` | y with the `opensc2,usb-hid` node | wired mode |
| `CONFIG_OPENSC2_USB_PROFILE_JULY` / `_OCTOBER` | October | the USB device's form |
| `CONFIG_OPENSC2_USB_MANUFACTURER`, `_PRODUCT` | "Valve Software", "Steam Controller" | the USB strings (`_PRODUCT`: the default of the identity's product name) |
| `CONFIG_OPENSC2_USB_THREAD_PRIORITY`, `_STACK_SIZE` | 5, 1536 | the USB report thread |
| `CONFIG_OPENSC2_USB_REMOTE_WAKEUP` | n | STEAM wakes a suspended PC |
| `CONFIG_OPENSC2_USB_PLUG_IN_REBOOT` | y | USB power with the link down: back to wired |
| `CONFIG_OPENSC2_NO_HOST_WIRELESS_MS` | 0 (wait, as the real controller) | **test builds only**, not the real controller's behaviour: a wired boot no host talks to within this time goes wireless ([above](#test-builds-only-no-host-then-wireless)); 1000 to 60000 |
| `CONFIG_OPENSC2_USB_CONSOLE` (`_VID`, `_PID`, `_MANUFACTURER`, `_PRODUCT`) | y with a CDC-ACM node (`2FE3:0004`, "OpenSC2 console") | the serial port in wireless mode; the PC tools only talk to a console with the default IDs and product string ([NOTICE.md](../NOTICE.md)) |
| `CONFIG_OPENSC2_USB_WIRED_CONSOLE` | n | debug: the serial port also while wired (composite) |
| `CONFIG_OPENSC2_USB_LOG`, `_LOG_SIZE`, `_LOG_ZEPHYR`, `_LOG_SETUP` | y, 8192, y, y | the USB session log (`_LOG_ZEPHYR` needs `CONFIG_LOG_PROCESS_THREAD_STACK_SIZE=1536`, checked at build time) |
| `CONFIG_OPENSC2_WARM_MARK_RAM` / `_GPREGRET2` | RAM; GPREGRET2 on nRF52 when no `opensc2,retained-ram` region is chosen | where the warm-restart mark is kept |
| `CONFIG_OPENSC2_TEST_HOOKS`, `CONFIG_OPENSC2_TEST_RING_SIZE`, `CONFIG_OPENSC2_TEST_SEQ_DEPTH` | n (the blank canvas turns it on), 2048, 8 | the [hardware test hooks](#hardware-test-hooks); with them, Zephyr's `CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE` defaults to 256 |
| `CONFIG_OPENSC2_DOCK_LINK` | y with the test hooks | the [dock link](#the-dock-link): the dock's protocol without pins |
| `CONFIG_OPENSC2_DOCK` (`_THREAD_PRIORITY`, `_THREAD_STACK_SIZE`) | n (9, 2048) | the dock's UART and pilot sense, from the devicetree alias `opensc2-dock`; **built, never run on hardware** ([The UART build](#the-uart-build-untested)) |
| identity: `_UNIT_SERIAL`, `_BOARD_SERIAL`, `_SERIAL_FROM_CHIP`, `_GIT_REVISION`, `_PRODUCT_ID`, `_BOARD_REVISION`, `_BUILD_TIME`, `_BOOTLOADER_BUILD_TIME` | see [Identity](#identity) | |
| `CONFIG_OPENSC2_BLE` | n | Bluetooth LE, built and not yet tested on hardware ([the Bluetooth build](#the-bluetooth-build)) |
| `CONFIG_OPENSC2_BLE_NAME_PREFIX` | "Steam Ctrl (BT) " | the name before the unit serial |
| `CONFIG_OPENSC2_BLE_COMPANY_ID` | 0x055D | the advertised manufacturer data's company ID (Valve's, for interoperability: [NOTICE.md](../NOTICE.md)) |
| `CONFIG_OPENSC2_BLE_PNP_VENDOR`, `_PNP_PRODUCT`, `_PNP_VERSION` | 0x28DE, 0x1303, 0x0100 | the Device Information Service's PnP ID (vendor ID source: the USB-IF) |
| `CONFIG_OPENSC2_BLE_MODEL`, `_MANUFACTURER` | "Steam Controller", "" | its model number and manufacturer name |
| `CONFIG_OPENSC2_BLE_VENDOR_SERVICE` | y | Valve's vendor GATT service, for SDL on Android and Apple platforms |
| `CONFIG_OPENSC2_BLE_ADV_WINDOW_S` | 60 | the advertising window, then "wireless off"; 0 to 3600, 0: advertise until a host connects |
| `CONFIG_OPENSC2_BLE_THREAD_PRIORITY`, `_STACK_SIZE` | 6, 2048 | the Bluetooth thread |
| `CONFIG_OPENSC2_BLE_REPORT_THREAD_PRIORITY`, `_STACK_SIZE` | 4, 1536 | the Bluetooth report thread |
| `CONFIG_OPENSC2_BLE_TX_QUEUE_DEPTH` | 8 | input reports from other threads waiting for it |
| `CONFIG_OPENSC2_BLE_REPORT_LEAD_US` | 500 | build the state report this long before the next connection event |

## Threads and timing

- One radio thread (`opensc2_radio`, priority `CONFIG_OPENSC2_THREAD_PRIORITY`). Keep long-running
  application threads below it (a higher number). It stays idle while wired.
- Wired: a USB report thread (`opensc2_usb`, `CONFIG_OPENSC2_USB_THREAD_PRIORITY`) woken by a
  periodic 4000 µs timer (132 ticks of the 32,768 Hz clock: 4.03 ms, about 248 state reports per
  second); vendor commands and output reports run in Zephyr's USB stack thread, and a command that
  writes flash holds its request until the write is done (Steam waits).
- The puck sends a keep-alive about every 3 ms; each reply goes out with the acknowledgement of the
  puck's next packet. State reports are sampled `CONFIG_OPENSC2_REPORT_DELAY_US` after a keep-alive.
- Link loss: 10 ms on the current channel, then 13 ms on each alternate, 500 ms in all.
- The Steam watchdog, the battery report and the rumble timeout run in the system work queue,
  whatever the link, and so does the system state machine: it looks at the buttons when they
  change, at the power source every `CONFIG_OPENSC2_POWER_POLL_MS`, and at its own timers when they
  are due; the events of the rest of the library wait in a queue of ten.
- Lizard mode runs one frame after each state report, in the thread that built it; through the puck
  a frame waits while reports wait for room in the radio's queue.
- With the haptics synthesiser: two more threads, one per haptic path
  ([its threads](#settings-threads-and-memory)).
- Vendor commands run one at a time, whichever link they come on (a mutex in the command
  channel): a command from the dock waits for one from USB or the radio, and the other way round.
  Callbacks a command calls (`vendor_command`, `setting_changed`, `led`) run with it held: do not
  wait there for another thread that sends a command. The real controller has no such lock.
- With `CONFIG_OPENSC2_DOCK`, one dock thread (`osc2_dock_tid`, priority
  `CONFIG_OPENSC2_DOCK_THREAD_PRIORITY`, 9) checks the pilot and serves the dock's requests; it runs
  the dock's vendor commands itself.
- A flash write (storing a bond) stalls the CPU for a few milliseconds; the radio link tolerates
  500 ms without a packet, so it should ride that out (not measured); on USB state reports pause.
- The library needs the 32 MHz crystal while the radio is on and requests it itself; the USB driver
  does the same while wired.
- Bluetooth (`CONFIG_OPENSC2_BLE`): a Bluetooth thread (`opensc2_ble`,
  `CONFIG_OPENSC2_BLE_THREAD_PRIORITY`, 6, below the radio thread) does all Bluetooth work in order;
  the host's callbacks only post to its queue (10 deep; a poster waits up to 200 ms for room). A
  report thread (`opensc2_blerpt`, 4) builds a state report at every connection event, one interval
  later less `CONFIG_OPENSC2_BLE_REPORT_LEAD_US`: the SoftDevice Controller's event-start trigger
  raises an EGU interrupt (if the trigger cannot be set, a timer at the connection interval paces
  instead). Vendor commands and output reports run in the host's receive thread. At most
  `CONFIG_BT_ATT_TX_COUNT` − 2 notifications are in flight; a state or battery report that finds no
  room is dropped; the others wait in the report queue, in order, until a notification completes
  (the report thread itself never waits for room: it can hold lizard mode's or the haptics' lock
  while it sends, and the room is freed from the system work queue).

## Reliability

### Crash records

With `CONFIG_OPENSC2_CRASH_RECORD` (on by default) the library is the application's fatal-error
handler (Zephyr's `k_sys_fatal_error_handler()`), as the real controller's firmware is its own: a
fault, `k_oops()`, `k_panic()` or a failed stack check writes a 33-byte record into RAM that a reset
keeps, and the chip resets at once (no log flush, no flash write). `opensc2_init()` then stores the
record as the settings key `debug/resetreason` and logs it ("New crash record: reason ..., PC ...,
LR ..."); a crash that repeats at every boot writes flash only once, until a host has read it.

| Offset | Field |
| --- | --- |
| 0 | 1 for a new record, 0 once a host has read it with `0xF2` selector 1 |
| 1 | the firmware's build time (u32; `CONFIG_OPENSC2_BUILD_TIME` or the configure time, not a build time stored on the board) |
| 5 | its revision (`CONFIG_OPENSC2_GIT_REVISION`, 16 bytes, zero padded) |
| 21 | Zephyr's fatal-error reason (u32: 0 a CPU exception, 2 a stack check, 3 a kernel oops, 4 a panic) |
| 25, 29 | the program counter and the link register at the error (u32 each; find them in `zephyr.map` or with `addr2line`) |

Vendor command `0xF2` with selector 1 answers the selector and the record as it is (a host sees
whether it is new), then marks it seen and saves it again from the system work queue (a flash
write: give that queue 2048 bytes of stack, as the template does); `0xED debug/resetreason` reads
it without changing it, and the factory reset deletes it. The RAM copy takes 44 bytes of the
devicetree region chosen as `opensc2,retained-ram` (the USB session log keeps the rest), which the
UF2 bootloader leaves alone; without such a region it is in no-init RAM, and the build warns. The
option is off when `CONFIG_RESET_ON_FATAL_ERROR` (the nRF Connect SDK's own handler) is on; turn it
off too if your application has a fatal-error handler of its own (the link fails with a multiple
definition of `k_sys_fatal_error_handler`). Without it a fatal error halts the board (Zephyr's
default).

### The hardware watchdog

`CONFIG_OPENSC2_WATCHDOG` (off by default; `app/watchdog.conf` builds the blank canvas with it)
runs the real controller's watchdog: the hardware watchdog (the devicetree alias `watchdog0`)
resets the chip 5 s after the last feed and pauses while a debugger halts the CPU; a work queue at
the lowest application priority (`opensc2_wdt`) feeds it every 2.5 s, and each feed re-arms a 4.5 s
warning on the system work queue. When the feeds stop (a thread keeps the CPU busy at the
feeder's priority or above), the warning comes first: it logs that the watchdog went unfed and
raises a kernel oops, so the [crash record](#crash-records) names the hang (reason 3); a hang that
also stops the system work queue ends in a plain watchdog reset, seen only in `0xA1`'s reset
causes. With a debugger attached the warning only logs. `opensc2_init()` starts it; it needs
`CONFIG_OPENSC2_CRASH_RECORD`.

> [!WARNING]
> Not yet tried on hardware. The nRF52's watchdog cannot be stopped once it runs and may keep
> running through a soft reset, so it may also run in the UF2 bootloader after the shell's `uf2` or
> Steam's bootloader request; whether the Adafruit UF2 bootloader feeds it is not known. If it does
> not, the bootloader resets 5 s later and a `.uf2` copy may not finish. A power cycle stops the
> watchdog: unplug the board, plug it back in and right away short RST to GND twice quickly.

### The settings partition

With `CONFIG_OPENSC2_STORAGE_RECOVERY` (on by default with NVS settings) a settings partition in
which NVS finds every sector occupied (corrupt, or left by another firmware: nothing could be
stored) is erased and started again at boot, as on the real controller; Zephyr's
`CONFIG_NVS_INIT_BAD_MEMORY_REGION` does it, for exactly that case. Any other error leaves the data
alone: the board runs without its bonds until the next boot. The erase loses the bonds, the stored
identity and the mode policy. Zephyr's option applies to every NVS file system of the
application, not only the settings.

### Reset causes

`0xA1` answers the reset causes in the real controller's encoding ([Vendor
commands](#vendor-commands)). The library never clears the chip's reset-reason register, so on the
nRF52 the causes of several resets add up until a power-on, as on the real controller; an
application that wants one reset's cause can read and clear it itself (Zephyr's
`hwinfo_get_reset_cause()` and `hwinfo_clear_reset_cause()`).

## Shell commands

With `CONFIG_SHELL` (on the SuperMini: the USB serial port, in wireless mode):

| Command | Does |
| --- | --- |
| `opensc2 status` | mode and why, link, channels, counters, lizard and IMU mode, identity, the last wired session; in a charger test build also a `test aid:` line |
| `opensc2 mode [auto\|wireless]` | show the mode, or store the policy (a changed policy reboots at once when it changes the next boot's mode) |
| `opensc2 reboot [wireless\|wired]` | reboot by the rule, into wireless, or into wired once |
| `opensc2 usb`, `opensc2 usb log [clear]` | the USB state, the USB session log |
| `opensc2 pair [seconds]`, `opensc2 cancel` | pairing over the air |
| `opensc2 start`, `opensc2 stop` | the wireless transport on / off |
| `opensc2 slot [1\|2]`, `opensc2 bonds`, `opensc2 forget <1\|2>` | bonds |
| `opensc2 transport [puck\|puck2\|ble\|ble2] [save]` | show the transport (running, chosen, stored), or run another; stored once its link is up, or at once with `save` |
| `opensc2 ble [status]`, `opensc2 ble pair` | with `CONFIG_OPENSC2_BLE`: the Bluetooth link (on or off, slot, identity and its address, the name, the bonds of the four identities, advertising and its form and the seconds left of its window, or connected with security, ATT MTU, interval, pacing and counters, or wireless off and why; the last disconnect reason); pairing mode for the slot (its other identity, reset, open to a new host; the old host's bond goes once a new one has paired) |
| `opensc2 id`, `opensc2 id set <unit\|board\|git\|pid\|rev\|build\|blbuild\|name> <value> [force]`, `opensc2 id clear` | identity: `set` stores that one field as given (0 too); numbers in hex with `0x`, or `0`; a name with spaces in double quotes; `force` for a build time at or before `0x6A18D057` ([Identity](#identity)) |
| `opensc2 state` | the current input state |
| `opensc2 cal`, `opensc2 cal sticks\|triggers\|pressure <0-3>`, `opensc2 cal save <0\|1> <mask>`, `opensc2 cal erase <key>` | with `CONFIG_OPENSC2_CAL`: what is stored and in use; a recording's step, as `0xD8`, `0xC0` and `0xC3` give it; one side's RAM values saved, as `0xE2` (mask 1 the trigger, 2 the stick, 4 the pressure); every `cal/` key deleted from flash with the key in hex, as `0xFE` ([Calibration and raw inputs](#calibration-and-raw-inputs)). The hardware test suite never sends it |
| `opensc2 power` | with the system state machine: the power source, the state, the transport, the LED mode, the battery level it was told, the keylock, the fully-charged LED level and the idle monitor |
| `opensc2 test [<id>] <verb> ...` | with `CONFIG_OPENSC2_TEST_HOOKS`: the [hardware test hooks](test-protocol.md#the-console-opensc2-test) |

The shell numbers the two pairing slots 1 and 2; the API numbers them 0 and 1. `opensc2 bonds` shows
your puck's serial, and `opensc2 status` and `opensc2 id` show the board's serials: mask them before
you post them. `opensc2 usb log` and the test suite's reports are masked and safe to share.

## Hardware test hooks

`CONFIG_OPENSC2_TEST_HOOKS` (off by default; the blank canvas turns it on) is the board's side of
OpenSC2's hardware test suite (`tools/hwtest`), which checks every feature the library emulates on a
board with nothing wired to it. While the PC has the hooks **armed**:

- **input injection**: the state reports carry a state the PC sends instead of the application's
  (every button bit, both sticks, triggers and pads, the IMU fields), statically or as a frame-locked
  sequence; a battery the PC sends replaces the application's in `0x43` and `0xBE`; with
  `CONFIG_OPENSC2_CAL`, raw inputs the PC sends replace the application's before the library
  processes them (`RAW_INJECT`), so the processing itself can be checked;
- **output capture**: a RAM ring records what the library decoded or called back (rumble, haptics,
  the LED, settings, lizard mode, power requests, vendor commands, link, Bluetooth and USB events),
  entry by entry, so outputs with no hardware behind them can be checked byte for byte;
- **the lizard guard**: lizard mode's mouse and keyboard reports (`0x40`, `0x41`) never leave the
  board; they go into the ring instead;
- **counters** of everything that flows (frames, reports sent and dropped, packets, commands);
- **the rule word**: which of [the rules on every report](#the-rules-on-every-report) and of lizard
  mode's processing run can be read, and changed until the end of arming (ops `RULES`,
  `RULES_SET`), so the suite can test a rule the build has off, such as the click by pressure.
- **Bluetooth** (a build with `CONFIG_OPENSC2_BLE`): the link's state (`BLE_STATUS`), a link ended
  as if by the host with a given reason (`BLE_DROP`), a shorter advertising window once
  (`BLE_WINDOW`), and ring entries for advertising, its end, identities, bonds, disconnects and
  wireless off.
- **system injection** (with `CONFIG_OPENSC2_SYSTEM`): buttons, a battery level and a power source
  that reach the [system state machine](#power-and-the-system-state-machine)'s rules (the chords,
  STEAM, the idle timeout, low battery, the power source) and never a report; a **boot-time
  injection** kept in the retained RAM area for the next boot, so the power-on decision itself can
  be tested; the machine's status; and ring entries for its states, the LED mode, the cues, the
  chords, the power source and how a shutdown ended (which survives the reboot it causes). While
  the hooks are armed, or in a boot a boot-time injection started, a shutdown never turns the board
  off: it reboots.
- **the dock** (with the [dock link](#the-dock-link)): frames handed to the dock link as if the puck
  had sent them, their answers read back, and the pilot check, the supply rule and the docked
  states fed values of the PC's own, so the dock's protocol is tested on a board with nothing
  wired.

Unarmed, the board behaves exactly as without the option, byte for byte, including the STALL of every
unknown opcode (the host tests compare the two builds request by request). Arming is RAM only: it ends
at every reboot, with `DISARM`, or when its lease runs out because the PC went quiet. The one
exception is a boot-time injection of the system state machine (`SYS_BOOT`), which the armed hooks
store in the retained RAM area for the next boot: a reset before the arming ends makes that boot
decide its power on the injected source and run on it unarmed (its shutdowns reboot) until a power
cycle or an `ARM` and `SYS_INJECT_OFF`; the end of arming drops one that no reboot has used yet
([test-protocol.md](test-protocol.md#system-injection)). About 3 KB of RAM with the defaults (the
ring, the sequence steps, the settings snapshot).

Safety rules the firmware enforces itself:

- **Injection needs lizard mode off.** `INJECT`, `SEQ_RUN` and `RAW_INJECT` answer DENIED while
  setting 9 is not 0 (unless the request carries the lizard-test flag, which only the suite's opt-in
  lizard group sets; `RAW_INJECT` has none). An injection stops in the very frame lizard mode comes
  back (the Steam watchdog, `0x8E`, setting 9 = 1): that frame is built from the application's state
  again, an `INJECT_STOP` entry is written, and lizard mode never sees an injected state. The guard
  is a second, independent layer.
- **Arming releases first, the guard comes off last.** `ARM` first releases whatever lizard mode
  holds towards the host (an empty keyboard and an all-zero mouse report, only when it holds
  something), then turns the guard on. At the end of arming the guard stays on 50 ms more, lizard mode
  resyncs (it takes what it sees next as its starting point, so nothing of the armed session produces
  a report later), and only then the guard comes off.
- **The settings come back.** `ARM` with flag bit 0 snapshots the 86 settings and the LED colour; the
  end of arming (also the lease running out, so also when the PC side died) writes back whatever
  differs, settings 9 and 71 last. A rule word changed with `RULES_SET` goes back to the build's
  at the end of arming and at the next `ARM`.
- **The host emulation is an allowlist.** `HOST_SET` runs only commands that write no flash and reach
  no bootloader ([the list](test-protocol.md#the-test-channel-vendor-command-0x7e)); everything else
  answers DENIED without running. A dock frame that is a SET is held to the same list.
- **Nothing identifying leaves.** The ring never keeps bond values, serials or answers; `HOST_GET`
  replaces bond, serial and chip-ID answers with their CRC-32; the dock's answers come back with
  their serials, bonds and chip ID as zeros (and a CRC-32 of what was hidden); the console prints
  no serial.

The key (`0x7E57AB1E`) is published with the code. It stops accidents, not an attacker: anything that
can send feature reports to the board or to the puck can arm the hooks and inject controller state
reports. That adds no capability a local program or a radio impersonating the controller does not
already have, and the guard and the lizard rule keep injected inputs out of the PC's mouse and
keyboard.

The default `app/` build has the hardware test hooks on (`CONFIG_OPENSC2_TEST_HOOKS=y`), so the test
suite checks the very image you run. They do nothing until the suite arms them; arming lives in RAM
and ends at every reboot (but for a boot-time injection stored for the next boot, above), or 15 s
after the PC goes quiet. The key that arms them is published with the code: it prevents accidents,
it is not a lock. For a board you give to someone else, build with `CONFIG_OPENSC2_TEST_HOOKS=n`.
The samples and the template build without them.

Kconfig: `CONFIG_OPENSC2_TEST_HOOKS` (off by default), `CONFIG_OPENSC2_TEST_RING_SIZE` (2048 bytes,
512 to 16384) and `CONFIG_OPENSC2_TEST_SEQ_DEPTH` (8 steps, 2 to 32). With the hooks, Zephyr's
`CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE` defaults to 256, and `CONFIG_SHELL_CMD_BUFF_SIZE`
must be 160 or more (checked at build time). The bytes of the test channel, the ring and the
console: [test-protocol.md](test-protocol.md). How the suite uses them: [testing.md](testing.md).

## The dock

Valve's puck is also a charging dock. A controller resting on it gets power through three pogo
pads (`+`, `−` and `P`, the "pilot"), and the puck talks to it over a slow serial link on the same
contacts: it asks for the controller's identity, relays Steam's vendor commands, and polls the
latest state report. OpenSC2 has the controller's end of that protocol without pins (the dock
link), the frames as pure functions, the dock's rules (the pilot check, dock or USB power, the
docked states), and a UART build for the SuperMini. The protocol is checked by the host tests,
the firmware emulator and, on a board with nothing wired, the hardware test suite's `dock` group;
**the UART build has never run on hardware**.

> [!WARNING]
> Nothing may be connected to Valve's puck's pads without the owner's step-by-step go-ahead. The
> pads' voltages and loads have not been measured, and the `+` pad carries about 5 V once the puck
> switches it on ([testing.md, The dock](testing.md#the-dock)).

### The dock link

`CONFIG_OPENSC2_DOCK_LINK`, on by default with the hardware test hooks. It needs no pins: the
UART build feeds it frames, and so do the test hooks. While no session runs, nothing changes on USB
or the radio.

- **Sessions.** A session starts when the pilot check passes (the UART build) or when the test hooks
  start one; while it runs the dock is an active link and the controller counts as docked. The
  controller serves one request at a time, with no time limit for the first byte; any frame that
  fails (fewer than 73 bytes before a 64 ms silence, a bad CRC, a receive error) ends the session,
  and the pilot is checked again. A frame that never comes keeps the session waiting.
- **Frames.** 73 bytes both ways: the start byte (`0x5A` from the puck, `0xAC` from the controller;
  nobody checks it), the request type, two unused bytes, the result (s32), 63 payload bytes, and a
  CRC-16/KERMIT of bytes 0 to 70, low byte first ([the frame functions](#the-frame-functions)).
- **Requests.**

  | Type | Request | The answer |
  | --- | --- | --- |
  | 0 | probe | the 57-byte identity record (as radio packet `0xF5`), type and result 0 |
  | 1 | GET | `{opcode, length, data}` of the dock's stored request; result 63, `-95` when the command has nothing to read (after a SET of `0x87`), or the command's negative error |
  | 2 | SET | runs `{opcode, length, data}` through the same command channel as USB and the radio; result 0 or a negative error (`-22` for an opcode nobody knows or a length above 61) |
  | 3 | GET input | the latest state report `0x42` without its ID (53 bytes), result 0 |
  | other | | its type and `-22` (the puck never sends one) |

- **Commands.** The dock keeps its own stored request (`0x83` after a boot). `0x81` and `0x85` are
  acknowledged and do nothing there, as on the real controller; everything else, `0x87` included,
  acts as on the other links. Attribute 11 of `0x83` is 64000 µs while the dock is the only link,
  otherwise the other link's interval.
- **Input.** With another link up (USB configured while wired, the radio or Bluetooth connected
  while wireless) GET input answers the copy of that link's latest state report, so the dock takes
  no sequence number from its stream. With the dock alone, each GET input builds one new report
  (the application's `sample` callback runs, from the thread that serves the dock).
- **Nothing else goes to the dock**: no lizard mode reports, no battery report `0x43`, no `0x44`.
  `0xBE` answers over the dock as on any link.
- **Timing.** An answer is built at once (well under a millisecond) unless the command writes flash;
  the puck waits up to 64 ms until the whole answer is in, so an answer has to start within about
  57 ms of the request. The dock link logs a warning for an answer that took over 50 ms, and the
  test hooks report the slowest.

### The dock's rules

Pure functions (`lib/opensc2/dock_rules.c`), from the protocol documentation (not public yet;
[protocol.md](protocol.md#overview-the-dock) sums it up):

- **The pilot**: one check is three readings 3 ms apart, present when two lie inside 2000 to
  3300 mV at the pin (no divider; a failed conversion counts as outside); 10 ms between checks that
  fail.
- **Dock or USB power**: the first reading and every change of "a supply is present" posts an event;
  the supply is the dock when the dock's `+` is there and the pilot passes as it appears, USB
  otherwise. On OpenSC2 the `+` rail must stay off the chip's VBUS pin (a board would otherwise boot
  wired on the dock), so the rule takes it on an input of its own: `plus-gpios` of the dock's
  devicetree node, in the UART build. There the rule runs on USB power, that input and the pilot
  (checked once before the application starts, then between sessions), and while the application
  leaves the power source on `AUTO` the [system state machine](#what-powers-the-board) takes "the
  dock's power" as `OPENSC2_POWER_SOURCE_DOCK`. Without a `+` input (`app/dock.overlay` has none)
  the board never counts as powered by the dock.
- **The docked states**: on the dock with the radio off (charging), a transport chord held (up to
  4 s), on the dock with the link up, and lifted off with the radio off (5 s, then a silent
  power-off), with what each does on each event (turn-off on the dock means back to radio-off with
  the power-off chime, never a power-off).

The docked states are also the system state machine's own (`dock-off`, `lifted`, `chord`,
`dock-on`: [The states while it runs](#the-states-while-it-runs)), which acts on them; these pure
rules act on nothing. The test hooks run them, and the supply rule, on the PC's values and record
each step in the ring.

### The UART build (untested)

`CONFIG_OPENSC2_DOCK` (default n) serves the dock on a UART and senses the pilot on an ADC channel,
both named by the devicetree node aliased `opensc2-dock` ([dts/bindings/opensc2,dock.yaml](../dts/bindings/opensc2,dock.yaml)):

```dts
/ {
    aliases { opensc2-dock = &opensc2_dock; };
    opensc2_dock: opensc2-dock {
        compatible = "opensc2,dock";
        uart = <&uart1>;            /* 115200 baud, 8N1, no flow control, async API */
        io-channels = <&adc 2>;     /* gain 1/6, internal reference, 12 bits, no divider */
        /* optional: plus-gpios = <&gpio0 N GPIO_ACTIVE_HIGH>;  the dock's + sensed (a divider) */
    };
};
```

It needs `CONFIG_UART_ASYNC_API` and the instance's `CONFIG_UART_<n>_ASYNC`. One thread checks the
pilot between sessions and serves the requests during one; it arms the receiver for one 73-byte
frame with a 64 ms gap timeout, answers, waits until the answer has left, and only then arms again,
so it never hears its own answer on a shared line. A receive error (framing, break, overrun) that
cuts a frame short ends the session with its own reason; an answer still not sent after 50 ms is
aborted. The TX pin should leave the line alone between answers (open drain, as the SuperMini build
has it). The UART and the test hooks' dock ops share the one dock link: the thread leaves a session
the test hooks started alone, so use one at a time. A pilot pin with nothing connected floats and
may read inside the window now and then, which starts sessions nobody asked for (harmless, but the
`dock` group expects none): give it a defined level (a resistor to ground) when nothing is attached.
`app/dock.conf` with `app/dock.overlay` is a SuperMini variant; its pins, and how they would meet the
puck's `P` pad, are untried guesses.

### The frame functions

`<opensc2/dock.h>`, pure, for tools, tests and the library:

| Function | Does |
| --- | --- |
| `opensc2_dock_crc16(crc, data, len)` | CRC-16/KERMIT (polynomial `0x1021` bit-reversed, no final XOR); `crc` 0 to start; "123456789" gives `0x2189` |
| `opensc2_dock_frame_check(frame, len)` | a received frame: `OPENSC2_DOCK_FRAME_OK`, `_BAD_SIZE` (not 73 bytes) or `_BAD_CRC` (the CRC over all 73 bytes is not 0) |
| `opensc2_dock_frame_seal(frame, start)` | the start byte and the CRC of bytes 0 to 70 at 71 and 72 |
| `opensc2_dock_build_request(type, payload, len, out)` | a request as the puck builds it (`0x5A`, the type, up to 63 payload bytes) |

The frame's numbers are named too: `OPENSC2_DOCK_FRAME_LEN` (73), the offsets `OPENSC2_DOCK_TYPE_OFF`
(1), `_RESULT_OFF` (4), `_PAYLOAD_OFF` (8, `_PAYLOAD_LEN` 63 bytes) and `_CRC_OFF` (71), the start bytes
`OPENSC2_DOCK_START_PUCK` (`0x5A`) and `_START_CONTROLLER` (`0xAC`), the request types
`OPENSC2_DOCK_PROBE`, `_GET`, `_SET` and `_INPUT` (0 to 3), `OPENSC2_DOCK_BAUD` (115200),
`OPENSC2_DOCK_GAP_TIMEOUT_MS` (64) and `OPENSC2_DOCK_INTERVAL_US` (64000, attribute 11 while the dock
is the only link).

## Pure helpers

`<opensc2/codec.h>` has every wire format as a pure function (encode the state, battery and identity
records; decode output reports; parse connection requests, channel changes, protocol versions,
pairing packets and records; build acknowledgement payloads), and the USB device's: the report
descriptor (`opensc2_hid_report_desc`), the two profiles, the device, configuration and HID
descriptors they give, the check of a feature SET and the build of a feature GET reply, and a text
for a setup packet. `<opensc2/dock.h>` has the dock's frames ([above](#the-frame-functions)). They
need no kernel and are what the host tests exercise.

### The IMU fusion helper

`<opensc2/fusion.h>` (`CONFIG_OPENSC2_FUSION`, which needs the FPU: `CONFIG_FPU=y`) turns
accelerometer and gyroscope readings into the orientation quaternion of report `0x42`, for an IMU
without fusion of its own: a small Mahony filter in floating point. It has no magnetometer, so the
heading drifts slowly, like any 6-axis "game rotation vector". Without it, or another source of
orientation, the quaternion stays zero, "none", which the state reports carry as the identity.

| Function | Does |
| --- | --- |
| `opensc2_fusion_init(&f)` | start at rest: the identity quaternion (W = 1), proportional gain `f.kp` 1.0, integral gain `f.ki` 0.0 (change the gains after this call) |
| `opensc2_fusion_update(&f, accel_g, gyro_dps, dt_s)` | one step: the acceleration in g (any length: it is normalised), the angular rate in °/s, and the time since the last step in seconds. With an all-zero acceleration it integrates the gyroscope alone |
| `opensc2_fusion_quat(&f, out)` | the orientation scaled for report `0x42`: W, X, Y, Z with 32767 = 1.0 |
| `opensc2_accel_from_g(g)` | g in the report's units: 16384 per g, saturating at ±32767 |
| `opensc2_gyro_from_dps(dps)` | °/s in the report's units: 32767 = 2000 °/s, saturating |

```c
#include <opensc2/opensc2.h>
#include <opensc2/fusion.h>

static struct opensc2_fusion fusion;    /* opensc2_fusion_init(&fusion) once, at start-up */

/* For each sample, with the axes already in the controller's frame (X right, Y away from the
 * player, Z up when it lies flat). */
static void imu_sample(const float accel_g[3], const float gyro_dps[3], float dt_s, uint32_t t_us)
{
    struct opensc2_imu imu = { .timestamp_us = t_us };

    opensc2_fusion_update(&fusion, accel_g, gyro_dps, dt_s);
    for (int i = 0; i < 3; i++) {
        imu.accel[i] = opensc2_accel_from_g(accel_g[i]);
        imu.gyro[i] = opensc2_gyro_from_dps(gyro_dps[i]);
    }
    opensc2_fusion_quat(&fusion, imu.quat);
    opensc2_set_imu(&imu);
}
```

[samples/imu](../samples/imu) does this with an MPU-6050 through Zephyr's sensor API. Like the real
controller, which streams IMU data only while Steam has it on, the sample and `app/` send readings
only while controller setting 48 is not 0 (`opensc2_setting_get(OPENSC2_SETTING_IMU_MODE)`); the
library itself sends whatever the application set.

### Calibration helpers

`<opensc2/cal.h>`: the arithmetic behind [Calibration and raw inputs](#calibration-and-raw-inputs) as
pure functions, for an application that processes its inputs itself (the library uses the same
ones). Each follows the protocol documentation's rules, with its factors as exact fractions; the
host tests check them against values worked out from those rules.

| Function | Does |
| --- | --- |
| `opensc2_cal_counts_to_mv(counts, full_mv)`, `opensc2_cal_mv_to_counts(mv, full_mv)` | the firmware's millivolts (`OPENSC2_CAL_STICK_FULL_MV` 3300, `OPENSC2_CAL_TRIGGER_FULL_MV` 1650) |
| `opensc2_cal_raw_mode(mode, counts, full_mv, &out)` | settings 46 and 62: 1 millivolts, 2 counts |
| `opensc2_cal_stick_pack()` / `_unpack()`, `_trigger_`, `_pressure_` | the keys' byte formats |
| `opensc2_stick_setup(&p, &cal)`, `opensc2_stick_value(&p, x, y, &out)`, `opensc2_stick_axis_value()` | a stick: validity, centre, two halves, the 10 % band; options `p.dead_zone` and `p.grow_range` |
| `opensc2_trigger_setup(&p, &cal)`, `opensc2_trigger_value(&p, counts)` | a trigger: direction, the 5 % bands, full 2 % early |
| `opensc2_pressure_setup(&p, &cal)`, `opensc2_pressure_sample(&p, raw, setting)`, `opensc2_pressure_value(&p)`, `p.click` | a pad's pressure: the filter, the range, the gain, the click's hysteresis |
| `opensc2_pressure_raw_value(raw)` | setting 60 = 1: the raw word itself, below 0 counted as 0 |
| `opensc2_cal_rec_init()`, `_rec_step()`, `_rec_frame()`, `_rec_stick()`, `_rec_trigger()`, `_rec_pressure()` | the factory recordings of `0xD8`, `0xC0` and `0xC3` (`OPENSC2_CAL_REC_STICKS`, `_TRIGGERS`, `_PRESSURE`); `_rec_step()` and `_rec_frame()` return the bits of the records finished (`OPENSC2_CAL_SAVE_STICK_L`, `_STICK_R`, `_TRIGGER_L`, `_TRIGGER_R`, `_PRESSURE_L`, `_PRESSURE_R`) |

### Sensor helpers

`<opensc2/sensor_helpers.h>`: what the real controller does between its sensors and the report, for an
application's own parts. The levels in the controller's units belong to Valve's sensor chip: scale
your part's readings to them. Untested on real parts.

| Function | Does |
| --- | --- |
| `opensc2_approx_distance(dx, dy)` | the real controller's fast length approximation (within about 3 %) |
| `opensc2_cap_reset(&t)`, `opensc2_cap_sample(&t, word, on, off)`, `opensc2_stick_touch_on_level(s30)`, `_off_level(s31)`, `opensc2_grip_on_level(s34)`, `opensc2_grip_off_level(s34, s35)` | stick touch and grip: the two-word mean (the first word after a reset fills both), the hysteresis, the levels from the settings |
| `opensc2_pad_touch_reset(&t)`, `opensc2_pad_touch_step(&t, level, s72, s73, &x, &y)` | a pad's touch from a signal level (settings 72, 73) with the damping near lift-off |
| `opensc2_pad_edge(m)`, `opensc2_pad_stretch(side, axis, m)` | the edge push and setting 66's stretch (its constants fit Valve's pads only) |
| `opensc2_pad_scale(m)` | a position (millionths of the pad, `OPENSC2_PAD_CENTRE` 500000 the centre) to the pad scale (±16383) |
| `opensc2_pad_filter_reset(&f)`, `opensc2_pad_filter_step(&f, &x, &y, rate)` | setting 65's speed-adaptive position filter (floating point; reset it while the pad is not touched) |
| `opensc2_pad_rotate(side, s2, s24, &x, &y)`, `opensc2_pad_rotation_angle(side, s2)`, `opensc2_sin8192(deg)`, `opensc2_cos8192(deg)` | settings 2 and 24's rotation, with the real controller's angle steps; the angle a pad turns by, 0..359 |
| `opensc2_stabiliser_reset(&s)`, `opensc2_stabiliser_step(&s, radius, &x, &y)` | the touch stabiliser (`OPENSC2_STABILISER_FINE` 4369, `_COARSE` 8738; reset it on a sample without a touch) |
| `opensc2_imu_accel_from_ms2(v)`, `opensc2_imu_gyro_from_rads(v)` | the real controller's IMU scaling (m/s² and rad/s, truncated) |
| `opensc2_imu_mount(m, in, out)`, `opensc2_imu_mount_quat(m, in, out)` | a mounting matrix on acceleration, angular rate and the quaternion |
| `opensc2_imu_dead_zone(v, t)` | the gyro dead zone after the bias |
| `opensc2_gyro_bias_init(&b, start)`, `opensc2_gyro_bias_update(&b, gyro, threshold, settle)` | a rest-based gyro-bias estimate of OpenSC2's own, for IMUs without one (setting 85 as its threshold, subtract while setting 84 is on) |
| `opensc2_adc_average(samples, channels, scans, out)` | an ADC burst's average per channel (the real controller converts four times) |

### Battery helpers

`<opensc2/gauge.h>`: a battery level for report `0x43` the way the real controller's fuel gauge
works, with voltage curves of OpenSC2's own (Valve's tables are its own data). Untested on a
battery.

| Function | Does |
| --- | --- |
| `opensc2_gauge_divider_mv(pin_mv, total_ohm, bottom_ohm, offset_mv)` | the battery's millivolts from the ADC pin's: pin × total / bottom + the offset (`cal/volt_offset`), rounded (VDDH / 5: total 5, bottom 1) |
| `opensc2_gauge_history_reset(&h)`, `opensc2_gauge_history_add(&h, mv)`, `opensc2_gauge_latest(&h)`, `opensc2_gauge_average(&h)`, `opensc2_gauge_max(&h)` | the last 40 readings (`OPENSC2_GAUGE_HISTORY`; the first after a reset fills all of them): the latest, their average rounded down, their maximum |
| `opensc2_gauge_curve(points, n, mv)` | a curve's charge in tenths of a percent (linear between points, 0 below the first, 1000 above the last); OpenSC2's single-cell LiPo curves are `opensc2_gauge_lipo_discharging` and `opensc2_gauge_lipo_charging` (with `_len`) |
| `opensc2_gauge_stretch(permille, target_permille)` | the stretch towards the charge target (`OPENSC2_GAUGE_TARGET_MV`, 4120, or `OPENSC2_GAUGE_TARGET_CHARGING_MV`, 4160, during a charge cycle): the charge times (200 − s) / 100, s the curve's reading at the target, in percent, 0..100 |
| `opensc2_gauge_init(&g, kept_level)`, `opensc2_gauge_step(&g, mv, charge_state)` | the level a controller reports: `OPENSC2_GAUGE_NO_LEVEL` (255) until the first reading (or a level kept over a warm reboot); charging uses the charging curve and only rises, done reads 100, anything else uses the discharging curve and only falls; a change between charging and not starts the history over |

Back to the [documentation map](README.md).
