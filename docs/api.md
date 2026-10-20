# API reference

The library's whole interface, for developers who use it in their own firmware: every function,
callback, field, rule and option.

Everything a project needs from `<opensc2/opensc2.h>` (which also includes `<opensc2/types.h>`,
`<opensc2/codec.h>` and `<opensc2/settings.h>`), plus the optional `<opensc2/fusion.h>`. The library
is hardware-agnostic: it has no pins. Your application reads its buttons, sensors and touch parts
and hands the values over; it receives the outputs through callbacks and drives whatever it has.
The same inputs and outputs work wireless (through the puck) and wired (on USB). How to bring the
library into a project: [integration.md](integration.md).

- [Functions at a glance](#functions-at-a-glance)
- [Life cycle](#life-cycle)
- [Modes](#modes): wired or wireless, the policy, reboots
- [Wired mode](#wired-mode): the USB device, what a project needs, the session log
- [Inputs](#inputs): every field, its range, and where Steam sees it
- [Outputs](#outputs): callbacks and what is in them
- [Identity](#identity)
- [Bonds and pairing](#bonds-and-pairing)
- [Vendor commands](#vendor-commands): what the library answers itself
- [Controller settings](#controller-settings), and the other state Steam sets
- [Kconfig options](#kconfig-options)
- [Threads and timing](#threads-and-timing)
- [Shell commands](#shell-commands)
- [Hardware test hooks](#hardware-test-hooks): input injection, output capture, the lizard guard
- [Pure helpers](#pure-helpers), and [the IMU fusion helper](#the-imu-fusion-helper)

"In Steam" below means what Steam and SDL's open-source Steam Controller 2 driver
(`SDL_hidapi_steam_triton.c`) read from the field. How Steam's own configurator presents each one
is not documented here and has not been checked against this library.

## Functions at a glance

| Function | Does | More |
| --- | --- | --- |
| `opensc2_init(&config)` | start the library once: load what is stored, decide the mode, start USB or the radio | [Life cycle](#life-cycle) |
| `opensc2_start()`, `opensc2_stop()` | radio on, radio off (wireless mode) | [Life cycle](#life-cycle) |
| `opensc2_pair_start(s)`, `opensc2_pair_cancel()` | pair over the air with a puck's open pairing window | [Bonds and pairing](#bonds-and-pairing) |
| `opensc2_link_state()`, `opensc2_link_info(&info)` | the radio link's state and counters | [Life cycle](#life-cycle) |
| `opensc2_mode()`, `opensc2_mode_reason()` | this boot's mode (wired or wireless) and why | [Modes](#modes) |
| `opensc2_policy()`, `opensc2_policy_set(p)` | the stored rule for boots with USB power | [Modes](#modes) |
| `opensc2_reboot(how, why)`, `opensc2_reboot_later(how, why, ms)` | reboot by the rule, into wireless, or into wired once | [Modes](#modes) |
| `opensc2_usb_state()`, `opensc2_usb_info(&info)` | the wired USB device's state and counters | [Wired mode](#wired-mode) |
| `opensc2_set_buttons(mask, value)` | press and release buttons and touch bits | [Inputs](#inputs) |
| `opensc2_set_stick(side, x, y)`, `opensc2_set_trigger(side, v)` | a stick, a trigger | [Inputs](#inputs) |
| `opensc2_set_trackpad(side, &pad)`, `opensc2_set_imu(&imu)` | a trackpad, the IMU | [Inputs](#inputs) |
| `opensc2_set_state(&state)`, `opensc2_get_state(&state)` | the whole input state at once | [Inputs](#inputs) |
| `opensc2_set_battery(&battery)` | the battery report | [Battery](#battery) |
| `opensc2_send_report(report, len)`, `opensc2_send_haptic_status(channel, flags)` | any other input report | [Other input reports](#other-input-reports) |
| `opensc2_bond_get(slot, &bond)`, `opensc2_bond_set(slot, &bond)` | read or store a pairing | [Bonds and pairing](#bonds-and-pairing) |
| `opensc2_select_slot(slot)`, `opensc2_selected_slot()` | the pairing slot in use (0 or 1) | [Bonds and pairing](#bonds-and-pairing) |
| `opensc2_identity_get(&id)`, `opensc2_identity_store(&id)`, `opensc2_identity_clear()` | who the controller says it is | [Identity](#identity) |
| `opensc2_setting_get(n)`, `opensc2_lizard_mode()`, `opensc2_led_get(&led)`, `opensc2_chime_level()` | the controller settings and the other state Steam sets | [Controller settings](#controller-settings) |
| `opensc2_setting_default(n)`, `opensc2_setting_clamp(n, v)` | a setting's default and range (pure) | [Controller settings](#controller-settings) |
| `opensc2_fusion_init(&f)`, `opensc2_fusion_update(&f, a, g, dt)`, `opensc2_fusion_quat(&f, q)` | the orientation quaternion from an IMU without its own fusion | [The IMU fusion helper](#the-imu-fusion-helper) |
| `opensc2_accel_from_g(g)`, `opensc2_gyro_from_dps(dps)` | g and °/s in the report's units | [The IMU fusion helper](#the-imu-fusion-helper) |
| `opensc2_encode_state()`, `opensc2_decode_output()`, ... | every wire format as a pure function (`<opensc2/codec.h>`) | [Pure helpers](#pure-helpers) |

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
| `opensc2_init(&config)` | Loads the bonds, the selected slot, the stored identity and the mode policy from Zephyr settings, checks the identity, decides this boot's mode, then starts the USB device (wired) or the radio thread (wireless). Once. `-EINVAL` for an invalid identity. A wired boot whose USB device cannot be set up goes on wireless (`OPENSC2_MODE_USB_FAILED`, the radio started if `start` is set). After an error the debug console (`CONFIG_OPENSC2_USB_CONSOLE`) still starts, so the log can be read. |
| `opensc2_start()` | Radio on: listen for the bonded puck, or wait in `OPENSC2_LINK_NO_BOND` when the slot is empty. `-EBUSY` while wired. |
| `opensc2_stop()` | Radio off. Like the real controller, nothing is sent; the puck notices the silence. |
| `opensc2_pair_start(seconds)` | Pair over the air with a puck whose pairing window is open. `0` = `CONFIG_OPENSC2_PAIRING_TIMEOUT_S` (60). Starts the radio if needed. `-EBUSY` while wired (Steam pairs a wired board itself). |
| `opensc2_pair_cancel()` | Stop pairing. |
| `opensc2_link_state()` | `OFF`, `NO_BOND`, `PAIRING`, `LISTENING` or `CONNECTED` (always `OFF` while wired). |
| `opensc2_link_info(&info)` | State, last reason, slot, channel list, protocol version, RSSI, packet and report counters, connection time. |

`struct opensc2_config`:

| Field | Meaning |
| --- | --- |
| `identity` | `NULL`, or an identity; empty or zero fields fall back to Kconfig ([Identity](#identity)). |
| `callbacks`, `user_data` | The callbacks below; `user_data` is passed to each. |
| `bond_slot` | 0 or 1, or `OPENSC2_SLOT_STORED` for the slot chosen last (stored with `opensc2_select_slot()`). |
| `start` | Start the radio at once in wireless mode. |
| `boot_mode` | `OPENSC2_BOOT_DEFAULT` (the rule below), `OPENSC2_BOOT_WIRELESS` or `OPENSC2_BOOT_WIRED` for this boot. |

Link states and reasons (`link_changed` gets both):

| State | Means |
| --- | --- |
| `OPENSC2_LINK_OFF` | Radio off: before `opensc2_start()`, after `opensc2_stop()`, after the puck turned the controller off, after `CONFIG_OPENSC2_LISTEN_TIMEOUT_S`, or wired. |
| `OPENSC2_LINK_NO_BOND` | Radio on, but the selected slot has no bond: pair first. |
| `OPENSC2_LINK_PAIRING` | Answering a puck's pairing beacon. |
| `OPENSC2_LINK_LISTENING` | Waiting on channel 2 for the bonded puck's connection request. |
| `OPENSC2_LINK_CONNECTED` | On the puck's private pipe; reports flow. |

| Reason | Means |
| --- | --- |
| `LINK_LOST` (1) | 500 ms without a packet from the puck. |
| `DISABLED` (2) | `opensc2_stop()`, a local turn-off, pairing started while connected, or another slot selected. |
| `PUCK_TURN_OFF` (3) | The puck sent "turn off" (the PC went to sleep). The link then goes `OFF`. |
| `BAD_VERSION` (4) | The puck's protocol version is not 0 or 1. |
| `PAIRED`, `PAIRING_FAILED` | End of a pairing window. |
| `LISTEN_TIMEOUT`, `BOND_CHANGED` | As named. |

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

The marks live in RAM that start-up does not clear (or, with `CONFIG_OPENSC2_WARM_MARK_GPREGRET2`, in
the nRF52's second retention register); a mark is set only right before the reboot and read and
cleared first thing at the next boot, so it never outlives one reboot. A power loss clears it.

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
once. Not when the application chose the mode at boot, nor after a failed wired start.

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
| Report descriptor | 372 bytes: a mouse collection (`0x40`), a keyboard collection (`0x41`) and the vendor collection with every other report, in that order (Windows' `Col01`, `Col02`, `Col03`) |
| Form | `CONFIG_OPENSC2_USB_PROFILE_JULY` (default): USB 2.00, class `EF/02/01`, release 3.07, 64-byte GET replies, the form Steam was seen pairing over the cable. `..._OCTOBER`: USB 2.01 with a USB 2.0 extension BOS descriptor, class `00/00/00`, release 4.04, 63-byte GET replies |
| Feature report 1 | the vendor commands: a 64-byte `SET_REPORT(Feature 1)` runs a command (`01 <opcode> <n> <payload>`) and, when it succeeds, becomes USB's stored request; `GET_REPORT(Feature 1)` answers the stored request, 64 bytes whatever wLength (Steam asks 65 first) and zeros after the answer. Anything refused is a STALL: an unknown opcode, a SET that is not 64 bytes or has n above 61, a GET of a command without an answer. Other report types and IDs (also output reports on the control endpoint, feature report 2) stall |
| Output reports | on interrupt OUT, at their declared length or longer, decoded as on the radio link |
| Input reports | the state report `0x42` every 132 ticks of the 32,768 Hz clock (a 4000 µs timer, 4.03 ms) from the moment the host configures the device; the battery report `0x43` every 3.5 s; lizard-mode reports; `opensc2_send_report()`. Eight IN buffers: a state report that finds none free is dropped, so nothing waits for the host; lizard and application reports wait for a free buffer |
| Suspend | nothing changes (as the October 2026 controller), but reports are dropped while the bus is suspended. `CONFIG_OPENSC2_USB_REMOTE_WAKEUP`: STEAM held about 20 ms wakes the PC, as the July 2026 controller did |
| Leaving | only on the way to a reboot: one all-zero `0x42` (sequence 0), 10 ms, USB off. Steam's `0x95` with `0xA427AF52` is acknowledged and acted on 20 ms later |
| Turn-off | `0x9F` and `0x95` with `0xABDF9710` are acknowledged and ignored while wired |
| While waiting for the host | `OPENSC2_USB_ATTACHED` for 0.5 s, then `OPENSC2_USB_NO_HOST`; STEAM pressed asks for `OPENSC2_POWER_REBOOT_KEEP_LINK` (back to wireless, as the real controller). In a charger test build only, no request from a host within `CONFIG_OPENSC2_NO_HOST_WIRELESS_MS` asks for it too ([above](#test-builds-only-no-host-then-wireless)) |
| Cable pulled | matters only to a board that keeps running (a battery). USB power gone for 0.2 s: the reports stop, the state goes `OFF`, and the library asks for `OPENSC2_POWER_USB_UNPLUGGED` (without a callback: a plain reboot, which comes back wireless). A shorter dip changes nothing. The real controller shuts down |

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
| Orientation | `imu.quat[4]` W, X, Y, Z | 32767 = 1.0; all zero = none | 45 (W), 47, 49, 51 | not read by SDL |
| Battery | `opensc2_set_battery(&b)` | see below | input report `0x43`, every 3.5 s | battery level and charge state (SDL power info) |

Axes, as the real controller: X right, Y away from the player, Z up when it lies flat. SDL turns them
into its own frame (it uses report X, Z, -Y).

An untouched trackpad is sent as position 0, 0. The pad's `touched` and `clicked` flags set its
touch and click bits; you can also set those bits directly in the button word.

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
call `opensc2_set_trigger(side, pressed ? 32767 : 0)`.

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
streaming, on the link the output came on, right after the `haptic` or `rumble` callback. The
order is the real controller's: the left channel first for `0x81` and `0x82`, the right first for
`0x86`, the right grip before the left for `0x80`. Steam's pair of all-zero `0x81` reports (target
0, then 1) gets `44 04 02 00 00 00`, then `44 03 02 00 00 00`. Turn the option off when your
haptics engine sends its own `0x44` reports.

### Lizard mode

Lizard mode: until Steam takes over, a Steam Controller 2 acts as a plain mouse and keyboard (the
right trackpad moves the pointer, A is Enter, B is Escape, the D-pad gives the arrow keys). Steam
switches it off while it runs; about 10 seconds after Steam stops, it comes back. OpenSC2 does the
same, on every link. With `CONFIG_OPENSC2_LIZARD_MODE` (default on) the library builds the mouse and
keyboard reports from the input state:

- **On** at every boot (controller setting 9 = 1; settings live in RAM).
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
  non-zero (a held button repeats), then one zero report.
- **Keyboard** `0x41`: A is Enter, B and VIEW are Escape, the D-pad the arrows, MENU is Tab. When one
  of those changes, every held button in bit order takes the next key slot (its key, or 0), and more
  than six held buttons give the roll-over code in every slot.
- Not reproduced yet: the touch-down stabiliser, the 8-frame motion average, the glide after a flick
  and the haptic ticks.

## Outputs

All callbacks are in `struct opensc2_callbacks`; any may be `NULL`. Wireless, they run in the
library's radio thread; wired, in its USB report thread (`sample`) or the USB stack's thread (output
reports, vendor commands); `power_request`, `usb_changed` and a rumble timeout run in the system work
queue. Return quickly: the radio link only lives while acknowledgements keep up with the puck.

| Callback | Called for | Gets |
| --- | --- | --- |
| `link_changed(info)` | every radio state change | `struct opensc2_link_info` |
| `sample(state)` | before each state report | the state, to update in place (it is kept) |
| `rumble(r)` | output report `0x80` | `left_speed`, `right_speed` 0..65535 (0 stops), `intensity`, `left/right_gain_db`, `type`; `timeout` set when the library stops a rumble the host did not repeat (`CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS`, 500) or the link dropped |
| `haptic(h)` | output reports `0x81`..`0x89` | `kind`, `channels`, raw `target`, and the decoded fields below |
| `output_report(report, len)` | any output report, raw | ID first |
| `led(led)` | vendor command `0xC5`, settings 37 and 45 | `red`, `green`, `blue`, `white` percent; `override` (setting 37: show this instead of the status pattern); `brightness` (setting 45, 0..100, default 50) |
| `setting_changed(number, value)` | a setting changed value (vendor command `0x87`, `0x81` for setting 9), or all of them went back to their defaults (`0x8E`, the Steam watchdog: `OPENSC2_SETTINGS_ALL`) | |
| `host_asleep()` | the puck says the PC is asleep (the link stays up) | the real controller plays a short cue |
| `power_request(req)` | see below | |
| `vendor_command(cmd, answer, answer_len)` | vendor commands the library does not answer itself | see [Vendor commands](#vendor-commands) |
| `usb_changed(info)` | wired: the USB device's state changed | `struct opensc2_usb_info` |

Haptic channels: `OPENSC2_HAPTIC_LEFT_PAD`, `_RIGHT_PAD`, `_LEFT_GRIP`, `_RIGHT_GRIP` (the real
controller's four actuators). `channels` is the target byte decoded the way the real controller
decodes it, including its quirks: report `0x81` swaps the two pads, and the PCM reports number their
targets differently again (in `0x87`, target 1 selects nothing and 4 the right grip).

| `kind` | Report | Fields |
| --- | --- | --- |
| `OPENSC2_HAPTIC_PULSE` | `0x81` | `pulse.on_us`, `off_us`, `count`. The real controller: on time 0 stops everything, off time 0 plays click A, a count of 3 or 4 click B. |
| `OPENSC2_HAPTIC_COMMAND` | `0x82` | `command.command`: 0 stop all, 1 click A, 2 click B; `command.gain_db` |
| `OPENSC2_HAPTIC_TONE` | `0x83` | `tone.gain_db`, `freq_hz`, `duration_ms`, `lfo_hz`, `lfo_depth` (percent) |
| `OPENSC2_HAPTIC_SWEEP` | `0x84` | `sweep.gain_db`, `duration_ms`, `start_hz`, `end_hz`, `antiphase` (targets 6 and 7) |
| `OPENSC2_HAPTIC_SCRIPT` | `0x85` | `script.id` (the real controller's built-in scripts 1..17), `script.gain_db` |
| `OPENSC2_HAPTIC_PCM_CONTROL` | `0x86` | `pcm_control.operation` (0 nothing, 1 stop, 2 set format), `format`, `encoding` (s16le, s8, mu-law), `rate_hz` (8000, 4000, 2000, 1000) |
| `OPENSC2_HAPTIC_PCM_DATA` | `0x87`, `0x88` (twice: left grip, right grip), `0x89` | `pcm.data`, `pcm.len` (valid during the callback only) |

Report `0x8A` does nothing, as on the real controller.

### Power requests

`power_request(req)`, in the system work queue; vendor commands are acted on about 20 ms after they
arrive, so their answer still goes out. Without the callback the library does the suggested action
itself where it is a reboot, and ignores the others.

| Request | From | Suggested action |
| --- | --- | --- |
| `OPENSC2_POWER_TURN_OFF_PUCK` | the puck's "turn off" (the PC sleeps) | power down if on battery; the link goes `OFF` by itself once the puck stops. A button press can then `opensc2_start()` again, which also lets the puck wake the PC. |
| `OPENSC2_POWER_TURN_OFF` | `0x9F`, or `0x95` with `0xABDF9710` (never while wired) | `opensc2_stop()` or power down |
| `OPENSC2_POWER_REBOOT` | `0x95` with another value | `opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL)` |
| `OPENSC2_POWER_REBOOT_KEEP_LINK` | `0x95` with `0xA427AF52` (Steam's "reboot into wireless mode", after it paired the board over the cable); STEAM pressed while no host configured the wired device; in a charger test build only, no host within `CONFIG_OPENSC2_NO_HOST_WIRELESS_MS` | `opensc2_reboot(OPENSC2_REBOOT_WIRELESS, NULL)` |
| `OPENSC2_POWER_BOOTLOADER` | `0x90`, or `0x95` with `0xECAABAC0`: Steam's firmware update | your bootloader cannot take Steam's update; ignoring it is usually right (Steam's update then fails, and the firmware stays as it is) |
| `OPENSC2_POWER_FACTORY_RESET` | `0x86` with `0xEFC1A917` | the bonds, slot and chime level are already erased; `opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL)` |
| `OPENSC2_POWER_USB_PLUGGED` | USB power appeared while wireless with the link down or STEAM held (policy auto) | `opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL)`: back wired |
| `OPENSC2_POWER_USB_UNPLUGGED` | USB power gone for 0.2 s while wired (only a board with a battery or another supply still runs to see it) | power off if the board can (the real controller shuts down), or `opensc2_reboot(OPENSC2_REBOOT_PLAIN, NULL)`, which comes back wireless without USB power (the default, and what the blank canvas does). Ignoring it leaves the board wired with neither link until a reset or a re-plug |

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
also has a Bluetooth name ("Steam Ctrl (BT) " + serial), but nothing is named over the puck's link
and this library has no Bluetooth. Steam's updater forces an update on controllers whose build time
is at or before `0x6A18D057`. Such a build time stops the build when it is the default
(`CONFIG_OPENSC2_BUILD_TIME`, or with 0 the build machine's clock), the shell stores one only with
`force`, and the library warns at boot when one is in use.

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
   Kconfig value. An invalid identity makes `opensc2_init()` return `-EINVAL`.
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
| `opensc2_identity_store(&id)` | store fields on the board (empty or zero fields are not stored and keep their value); validated first (`-EINVAL`); new connections use it at once, the USB strings from the next boot |
| `opensc2_identity_clear()` | forget the stored identity, the product name too (the settings keys `opensc2/id` and `opensc2/name`): back to the `opensc2_init()` and Kconfig values. Returns 0 or the settings subsystem's negative errno |
| `opensc2_identity_check(&id)` | pure: 0 for a valid identity, or a mask of `OPENSC2_ID_BAD_*` problems |

## Bonds and pairing

A bond is one pairing with one puck, 24 bytes: `puck_id`, `controller_id` and the puck's serial. Like
the real controller there are two slots, so one board can be paired with two pucks;
`opensc2_select_slot(0|1)` chooses (and stores the choice). The shell numbers the two pairing slots
1 and 2; the API numbers them 0 and 1.

| Function | Does |
| --- | --- |
| `opensc2_bond_get(slot, &bond)` | read one |
| `opensc2_bond_set(slot, &bond)` | store one (both IDs zero: delete). Listening restarts with it. |
| `opensc2_select_slot(slot)`, `opensc2_selected_slot()` | the slot in use |

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

Storage keys (Zephyr settings): `opensc2/bond0`, `opensc2/bond1`, `opensc2/slot`, `opensc2/chime`,
`opensc2/id`, `opensc2/name`, `opensc2/mode`.

## Vendor commands

Steam's feature report 1 reaches the controller through the puck, as records in its packets, and on
USB, as a 64-byte SET and a GET. Both work the same way: a SET runs a command and, if it succeeds,
stores it; a GET answers the stored one. Each link keeps its own stored request, `0x83` until a
SET. A GET record's one byte is not the opcode: Valve's puck leaves it unset, and drops an answer
whose opcode is not its SET's. A refused command is a result code through the puck and a STALL on
USB; a GET of a command that has no answer is `{opcode, 0}` through the puck and a STALL on USB.
The library answers these itself:

| Opcode | Kind | Answer / action |
| --- | --- | --- |
| `0x83` | get | six `{tag, u32}`: 1 product ID, 2 capabilities (0), 10 bootloader build time, 4 build time, 9 board revision, 11 report interval (4000 µs) |
| `0xAE` | get | the tag (only the tag byte counts, whatever the length), then a string zero padded to 20 bytes: 1 unit serial, 0 and 4 board serial, 3 git revision, others "NA" |
| `0xF2` | get | selector 0: build time, board revision, git revision (16), unit serial (16); 1: an empty crash record (33 zero bytes); 2: uptime in ms (u32) and the last disconnect reason (u32) |
| `0xA1` | get | 18 bytes. Selector 1: 1, a byte of reset-cause flags in the real controller's encoding (`0x01` power-on, `0x02` brown-out, `0x08` debug, a low-power wake, a CPU lock-up or any other cause, `0x10` the reset pin, `0x20` the watchdog, `0x40` software), the chip's 8-byte device ID, 8 zero bytes; any other selector: 0 and 17 zero bytes. The flags come from Zephyr's `hwinfo_get_reset_cause()`; nothing clears the chip's register, so the causes of several resets add up until a power-on, as on the real controller |
| `0xBE` | get | battery data, 15 bytes (from `opensc2_set_battery()`; byte 13 is 1 while charging, byte 14 the charge state without a `0x80` flag: [Battery](#battery)) |
| `0xE9` / `0xC5` | get / set | LED colour, four percentages (`0xC5` clamps each to 100 and calls `led`). `0xE9` reads it back as the real controller does: a channel reads 100 when it is exactly 100, otherwise 0 (`opensc2_led_get()` has the colour as kept) |
| `0xDB` / `0xDC` | get / set | user store type 1: the power-on chime level (stored; `opensc2_chime_level()`) |
| `0x89` / `0x87` / `0x8E` | get / set / set | controller settings (calls `setting_changed`); `0x89` answers the defaults of settings 48, 84 and 85, as the real controller does ([Controller settings](#controller-settings)) |
| `0x81` / `0x85` | set | lizard mode off / its keyboard and buttons back ([Lizard mode](#lizard-mode)); refused without an active link |
| `0xED` / `0xEE` / `0xEF` / `0xF0` | get / set / save / delete by name | every key of the real controller ([below](#settings-by-name)): `esb/bond`, `esb/bond_2`, `user/wireless_transport` (2 = the first slot, 3 = the second), `user/haptic_boot_level`, the keys the library keeps (`user/keylock`, `user/fully_charged_led_intensity`, `mte/charge_level`, `debug/resetreason`, `settings/haptics/...`), and the calibration keys (`cal/...`, the application's). `0xED` answers length 0 for an empty bond; `0xEE` refuses an empty value and a value that is not the key's size; `0xEF` needs the key's NUL as the last byte and refuses an empty bond; other names go to `vendor_command` (a `0xED` nobody answers gets an empty answer) |
| `0xE2` | set | persist calibration: acknowledged for side 0 or 1 (after `vendor_command`, which may do something with it), refused above |
| `0xA2` | set | write the bond of the first slot (API slot 0, shell slot 1), exactly 24 bytes (an ID of zero deletes it) |
| `0x86`, `0x90`, `0x95`, `0x9F` | set | factory reset, bootloader, reboot, turn off: `power_request`; turn-off is ignored while wired |

Everything else (the calibration commands `0xD8`, `0xC0`, `0xC3`, `0xFE`, and anything new) goes to
`vendor_command`; a command nobody knows is refused (on USB a STALL, as the real controller stalls
Steam's `0xC1`):

```c
static int on_vendor(const struct opensc2_vendor_cmd *cmd, uint8_t *answer, uint8_t *answer_len,
             void *user)
{
    if (cmd->opcode == 0xD8 && !cmd->is_get) {
        start_my_stick_calibration(cmd->payload, cmd->len);
        return 0;                 /* SET result 0 */
    }
    return -ENOTSUP;              /* unknown: the SET fails, a GET answers {opcode, 0} */
}
```

For a GET, write at most 61 bytes to `answer` and set `*answer_len`. `cmd->payload` of a GET is the
payload of the SET before it. Commands may take their time (a flash write): Steam waits for each.
While wired the callback runs in Zephyr's USB stack thread, inside the host's control request: only
its return value counts. (The stack would stall a GET that leaves `errno` set, which C library calls
may do; the library clears it before it answers.)

### Settings by name

`0xED` reads a key, `0xEE` sets it in RAM, `0xEF` saves the RAM value and `0xF0` deletes it (from
flash, and the RAM value goes back to its default). The payload is the key's name with its NUL,
then for `0xEE` the value; `0xEF` and `0xF0` need the NUL as the last byte. The library knows every
key of the real controller and nothing else, so a host cannot fill the storage partition with
names of its own:

| Key | Size | `0xED` | Kept as | The factory reset (`0x86`) |
| --- | --- | --- | --- | --- |
| `esb/bond`, `esb/bond_2` | 24 | the bond, length 0 when empty | the two bonds (`opensc2/bond0`, `bond1`) | deletes |
| `user/wireless_transport` | 1 | 2 + the slot | the slot (`opensc2/slot`); 0 and 1, Bluetooth, are taken and ignored | deletes |
| `user/haptic_boot_level` | 1 | the chime level | `opensc2/chime` | deletes |
| `user/keylock` | 4 | the value, zeros until set | `opensc2/k/keylock` | deletes |
| `user/fully_charged_led_intensity` | 1 | length 0 (no read function) | `opensc2/k/fcled`; 3 until set | deletes |
| `mte/charge_level` | 8 | the value, zeros until set | `opensc2/k/mte` | deletes |
| `debug/resetreason` | 33 | the crash record, zeros when there is none | `opensc2/k/crash` | deletes |
| `settings/haptics/haptic_master_gain_db`, `amplifier_mode`, `enabled` | 2 each | length 0 (no read function) | `opensc2/k/hgain`, `hamp`, `hen` | keeps |
| `cal/joy_l`, `_r` (18), `cal/trg_l`, `_r` (7), `cal/prs_l`, `_r` (8), `cal/touch_l`, `_r` (10), `cal/rgbw_r`, `_g`, `_b`, `_w` (1), `cal/volt_offset` (2), `cal/sensors/gyroscope/bias` (12) | as listed | the application's `vendor_command`; length 0 without one (the gyro bias never) | the application's | keeps |

`0xEE` takes exactly the key's size (another length is refused, as on the real controller). The
values of `user/keylock`, `user/fully_charged_led_intensity`, `mte/charge_level` and the haptics
keys are kept and read back but nothing in the library acts on them yet; `debug/resetreason` holds
the crash record of vendor command `0xF2` selector 1. With `CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE`,
`0xEE` also takes OpenSC2's identity keys ([Identity](#identity)).

## Controller settings

86 numbered 16-bit values that Steam writes with `0x87` and reads with `0x89`, in RAM only (defaults
at every boot), as on the real controller. `opensc2_setting_get(n)`; constants in
`<opensc2/settings.h>` (`OPENSC2_SETTING_*`, with SDL's names in the comments). The ones a project is
most likely to act on:

| # | Constant | Default | Meaning |
| --- | --- | --- | --- |
| 9 | `OPENSC2_SETTING_LIZARD_MODE` | 1 | lizard mode; Steam sends `0x81`, SDL writes 0 |
| 27 | `OPENSC2_SETTING_RUMBLE_GAIN_DB` | -10 | rumble gain offset, dB |
| 37 | `OPENSC2_SETTING_LED_OVERRIDE` | 0 | show `0xC5`'s colour |
| 45 | `OPENSC2_SETTING_LED_BRIGHTNESS` | 50 | status LED brightness, percent |
| 48 | `OPENSC2_SETTING_IMU_MODE` | 0 | IMU stream: `OPENSC2_IMU_MODE_SEND_RAW_ACCEL` \| `_SEND_RAW_GYRO` from SDL when sensors are on |
| 50 | `OPENSC2_SETTING_SLEEP_TIMEOUT_S` | 900 | idle power-off on battery |
| 52, 53 | `OPENSC2_SETTING_LEFT/RIGHT_CLICK_PRESSURE` | 40 | pad click threshold, percent (-1: 100 %) |
| 68 | `OPENSC2_SETTING_TRIGGER_THRESHOLD` | 90 | trigger digital threshold, percent (used by the library) |
| 70, 76 | `OPENSC2_SETTING_HAPTICS_ENABLED`, `_HAPTIC_GAIN_DB` | 1, -3 | haptics on, master gain |
| 71 | `OPENSC2_SETTING_STEAM_WATCHDOG` | 1 | the 10 s lizard watchdog (used by the library) |
| 82 | `OPENSC2_SETTING_PAD_IGNORE_ON_STICK` | 3 | ignore a pad while its stick is touched (used by lizard mode) |

`0x87` clamps a value to its range where the range is known (9 and 71: 0..1; 11: 0..1000; 12:
1..1000; 14: 0..32767; 36 and 45: 0..100; 68: 40..99; 70: 0..2; `opensc2_setting_clamp()`); other
settings keep any value. The defaults and the ranges of 36 and 68 are in the protocol
documentation the same authors wrote
([controller settings](https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/docs/controller/03-usb-and-hid.md#controller-settings),
not public yet); the ranges of 9, 11, 12, 14, 45, 70 and 71 come from the same authors' study of
the controller's firmware (not in that documentation yet). `setting_changed` is called only when a
value changes.
`0x8E` and the Steam watchdog keep settings 48, 84 and 85 (the real controller passes those to its
IMU instead of storing them), and `0x89` answers their defaults (0, 1, 100) whatever was written,
as the real controller does; `opensc2_setting_get()` and the `setting_changed` callback give the
application the values Steam wrote.

The functions for the settings and the other state Steam sets:

| Function | Returns |
| --- | --- |
| `opensc2_setting_get(n)` | setting `n` as it is now (Steam's last write, or its default); 0 for a number above 85 |
| `opensc2_lizard_mode()` | `true` while lizard mode is on (setting 9 is not 0): from every boot until Steam turns it off, and again when the Steam watchdog brings it back |
| `opensc2_led_get(&led)` | the LED colour and settings Steam set, as the `led` callback gets them: `red`, `green`, `blue`, `white` in percent (`0xC5`), `override` (setting 37) and `brightness` (setting 45) |
| `opensc2_chime_level()` | the power-on chime level Steam stores with `0xDC`: 1 quiet, 2 normal (the default), anything else silent |
| `opensc2_setting_default(n)` | pure: the default of setting `n` (0 for a number with no known default) |
| `opensc2_setting_clamp(n, v)` | pure: `v` clamped to setting `n`'s range, as `0x87` stores it (unchanged for a number with no known range) |

## Kconfig options

| Option | Default | |
| --- | --- | --- |
| `CONFIG_OPENSC2` | n | the library (selects ESB and REBOOT) |
| `CONFIG_ESB_MAX_PAYLOAD_LENGTH` | 128 (128 or more) | checked at build time |
| `CONFIG_ESB_TX_FIFO_SIZE` | 2 recommended | the real controller keeps two replies queued |
| `CONFIG_OPENSC2_THREAD_PRIORITY`, `_STACK_SIZE` | 5, 2048 | the radio thread |
| `CONFIG_OPENSC2_TX_POWER_DBM` | 6 | as the real controller |
| `CONFIG_OPENSC2_REPORT_DELAY_US` | 2000 | sample a state report this long after a keep-alive |
| `CONFIG_OPENSC2_LISTEN_TIMEOUT_S` | 0 (never) | the real controller gives up after 60 s |
| `CONFIG_OPENSC2_PAIRING_TIMEOUT_S` | 60 | |
| `CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS` | 500 | |
| `CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS` | 3500 | 0: never |
| `CONFIG_OPENSC2_TRIGGER_CLICK_AUTO` | y | |
| `CONFIG_OPENSC2_LIZARD_MODE`, `_STEAM_WATCHDOG` | y, y | |
| `CONFIG_OPENSC2_STORAGE` | y with `CONFIG_SETTINGS` | bonds, identity and policy in Zephyr settings |
| `CONFIG_OPENSC2_SHELL` | y with `CONFIG_SHELL` | |
| `CONFIG_OPENSC2_FUSION` | n | the IMU fusion helper |
| `CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE` | n | identity through `0xEE` |
| `CONFIG_OPENSC2_HAPTIC_STOP_STATUS` | y | `0x44 {channel, 0x02}` after every stop, as the real controller ([Other input reports](#other-input-reports)) |
| `CONFIG_OPENSC2_RX_QUEUE_DEPTH`, `_REPORT_QUEUE_DEPTH` | 8, 6 | |
| `CONFIG_OPENSC2_DEFAULT_POLICY_AUTO` / `_WIRELESS` | auto | the mode policy until one is stored |
| `CONFIG_OPENSC2_USB` | y with the `opensc2,usb-hid` node | wired mode |
| `CONFIG_OPENSC2_USB_PROFILE_JULY` / `_OCTOBER` | July | the USB device's form |
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
| identity: `_UNIT_SERIAL`, `_BOARD_SERIAL`, `_SERIAL_FROM_CHIP`, `_GIT_REVISION`, `_PRODUCT_ID`, `_BOARD_REVISION`, `_BUILD_TIME`, `_BOOTLOADER_BUILD_TIME` | see [Identity](#identity) | |

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
  whatever the link.
- A flash write (storing a bond) stalls the CPU for a few milliseconds; the radio link tolerates
  500 ms without a packet, so it should ride that out (not measured); on USB state reports pause.
- The library needs the 32 MHz crystal while the radio is on and requests it itself; the USB driver
  does the same while wired.

## Shell commands

With `CONFIG_SHELL` (on the SuperMini: the USB serial port, in wireless mode):

| Command | Does |
| --- | --- |
| `opensc2 status` | mode and why, link, channels, counters, lizard and IMU mode, identity, the last wired session; in a charger test build also a `test aid:` line |
| `opensc2 mode [auto\|wireless]` | show the mode, or store the policy (a changed policy reboots at once when it changes the next boot's mode) |
| `opensc2 reboot [wireless\|wired]` | reboot by the rule, into wireless, or into wired once |
| `opensc2 usb`, `opensc2 usb log [clear]` | the USB state, the USB session log |
| `opensc2 pair [seconds]`, `opensc2 cancel` | pairing over the air |
| `opensc2 start`, `opensc2 stop` | radio on / off |
| `opensc2 slot [1\|2]`, `opensc2 bonds`, `opensc2 forget <1\|2>` | bonds |
| `opensc2 id`, `opensc2 id set <unit\|board\|git\|pid\|rev\|build\|blbuild\|name> <value> [force]`, `opensc2 id clear` | identity: `set` stores that one field as given (0 too); numbers in hex with `0x`, or `0`; a name with spaces in double quotes; `force` for a build time at or before `0x6A18D057` ([Identity](#identity)) |
| `opensc2 state` | the current input state |
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
  sequence; a battery the PC sends replaces the application's in `0x43` and `0xBE`;
- **output capture**: a RAM ring records what the library decoded or called back (rumble, haptics,
  the LED, settings, lizard mode, power requests, vendor commands, link and USB events), entry by
  entry, so outputs with no hardware behind them can be checked byte for byte;
- **the lizard guard**: lizard mode's mouse and keyboard reports (`0x40`, `0x41`) never leave the
  board; they go into the ring instead;
- **counters** of everything that flows (frames, reports sent and dropped, packets, commands).

Unarmed, the board behaves exactly as without the option, byte for byte, including the STALL of every
unknown opcode (the host tests compare the two builds request by request). Arming is RAM only: it ends
at every reboot, with `DISARM`, or when its lease runs out because the PC went quiet. About 3 KB of RAM
with the defaults (the ring, the sequence steps, the settings snapshot).

Safety rules the firmware enforces itself:

- **Injection needs lizard mode off.** `INJECT` and `SEQ_RUN` answer DENIED while setting 9 is not 0
  (unless the request carries the lizard-test flag, which only the suite's opt-in lizard group sets).
  An injection stops in the very frame lizard mode comes back (the Steam watchdog, `0x8E`, setting 9 =
  1): that frame is built from the application's state again, an `INJECT_STOP` entry is written, and
  lizard mode never sees an injected state. The guard is a second, independent layer.
- **Arming releases first, the guard comes off last.** `ARM` first releases whatever lizard mode
  holds towards the host (an empty keyboard and an all-zero mouse report, only when it holds
  something), then turns the guard on. At the end of arming the guard stays on 50 ms more, lizard mode
  resyncs (it takes what it sees next as its starting point, so nothing of the armed session produces
  a report later), and only then the guard comes off.
- **The settings come back.** `ARM` with flag bit 0 snapshots the 86 settings and the LED colour; the
  end of arming (also the lease running out, so also when the PC side died) writes back whatever
  differs, settings 9 and 71 last.
- **The host emulation is an allowlist.** `HOST_SET` runs only commands that write no flash and reach
  no bootloader ([the list](test-protocol.md#the-test-channel-vendor-command-0x7e)); everything else
  answers DENIED without running.
- **Nothing identifying leaves.** The ring never keeps bond values, serials or answers; `HOST_GET`
  replaces bond, serial and chip-ID answers with their CRC-32; the console prints no serial.

The key (`0x7E57AB1E`) is published with the code. It stops accidents, not an attacker: anything that
can send feature reports to the board or to the puck can arm the hooks and inject controller state
reports. That adds no capability a local program or a radio impersonating the controller does not
already have, and the guard and the lizard rule keep injected inputs out of the PC's mouse and
keyboard.

The default `app/` build has the hardware test hooks on (`CONFIG_OPENSC2_TEST_HOOKS=y`), so the test
suite checks the very image you run. They do nothing until the suite arms them; arming lives in RAM
and ends at every reboot, or 15 s after the PC goes quiet. The key that arms them is published with
the code: it prevents accidents, it is not a lock. For a board you give to someone else, build with
`CONFIG_OPENSC2_TEST_HOOKS=n`. The samples and the template build without them.

Kconfig: `CONFIG_OPENSC2_TEST_HOOKS` (off by default), `CONFIG_OPENSC2_TEST_RING_SIZE` (2048 bytes,
512 to 16384) and `CONFIG_OPENSC2_TEST_SEQ_DEPTH` (8 steps, 2 to 32). With the hooks, Zephyr's
`CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE` defaults to 256, and `CONFIG_SHELL_CMD_BUFF_SIZE`
must be 160 or more (checked at build time). The bytes of the test channel, the ring and the
console: [test-protocol.md](test-protocol.md). How the suite uses them: [testing.md](testing.md).

## Pure helpers

`<opensc2/codec.h>` has every wire format as a pure function (encode the state, battery and identity
records; decode output reports; parse connection requests, channel changes, protocol versions,
pairing packets and records; build acknowledgement payloads), and the USB device's: the report
descriptor (`opensc2_hid_report_desc`), the two profiles, the device, configuration and HID
descriptors they give, the check of a feature SET and the build of a feature GET reply, and a text
for a setup packet. They need no kernel and are what the host tests exercise.

### The IMU fusion helper

`<opensc2/fusion.h>` (`CONFIG_OPENSC2_FUSION`, which needs the FPU: `CONFIG_FPU=y`) turns
accelerometer and gyroscope readings into the orientation quaternion of report `0x42`, for an IMU
without fusion of its own: a small Mahony filter in floating point. It has no magnetometer, so the
heading drifts slowly, like any 6-axis "game rotation vector". Without it, or another source of
orientation, the quaternion stays zero.

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

Back to the [documentation map](README.md).
