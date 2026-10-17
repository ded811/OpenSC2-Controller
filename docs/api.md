# API reference

Everything a project needs from `<opensc2/opensc2.h>` (which also includes `<opensc2/types.h>`,
`<opensc2/codec.h>` and `<opensc2/settings.h>`), plus the optional `<opensc2/fusion.h>`. The library
is hardware-agnostic: it has no pins. Your application reads its buttons, sensors and touch parts
and hands the values over; it receives the outputs through callbacks and drives whatever it has.

- [Life cycle](#life-cycle)
- [Inputs](#inputs): every field, its range, and where Steam sees it
- [Outputs](#outputs): callbacks and what is in them
- [Identity](#identity)
- [Bonds and pairing](#bonds-and-pairing)
- [Vendor commands](#vendor-commands): what the library answers itself
- [Controller settings](#controller-settings)
- [Kconfig options](#kconfig-options)
- [Threads and timing](#threads-and-timing)
- [Shell commands](#shell-commands)
- [Pure helpers](#pure-helpers)

"In Steam" below means what Steam and SDL's open-source Steam Controller 2 driver
(`SDL_hidapi_steam_triton.c`) read from the field. How Steam's own configurator presents each one
is not documented here and has not been checked against this library.

## Life cycle

```c
#include <opensc2/opensc2.h>

static void on_link(const struct opensc2_link_info *info, void *user)
{
	/* show info->state on an LED */
}

static const struct opensc2_callbacks callbacks = { .link_changed = on_link };
static const struct opensc2_config config = {
	.callbacks = &callbacks,
	.bond_slot = OPENSC2_SLOT_STORED,
	.start = true,
};

int main(void)
{
	opensc2_init(&config);
	/* ... opensc2_set_buttons(), opensc2_set_stick(), ... */
}
```

| Function | Does |
| --- | --- |
| `opensc2_init(&config)` | Loads the bonds, the selected slot and any stored identity from Zephyr settings, checks the identity, starts the radio thread. Once. `-EINVAL` for an invalid identity. |
| `opensc2_start()` | Radio on: listen for the bonded puck, or wait in `OPENSC2_LINK_NO_BOND` when the slot is empty. |
| `opensc2_stop()` | Radio off. Like the real controller, nothing is sent; the puck notices the silence. |
| `opensc2_pair_start(seconds)` | Pair with a puck whose pairing window is open. `0` = `CONFIG_OPENSC2_PAIRING_TIMEOUT_S` (60). Starts the radio if needed. |
| `opensc2_pair_cancel()` | Stop pairing. |
| `opensc2_link_state()` | `OFF`, `NO_BOND`, `PAIRING`, `LISTENING` or `CONNECTED`. |
| `opensc2_link_info(&info)` | State, last reason, slot, channel list, protocol version, RSSI, packet and report counters, connection time. |

`struct opensc2_config`:

| Field | Meaning |
| --- | --- |
| `identity` | `NULL`, or an identity; empty or zero fields fall back to Kconfig ([Identity](#identity)). |
| `callbacks`, `user_data` | The callbacks below; `user_data` is passed to each. |
| `bond_slot` | 0 or 1, or `OPENSC2_SLOT_STORED` for the slot chosen last (stored with `opensc2_select_slot()`). |
| `start` | Start the radio at once. |

Link states and reasons (`link_changed` gets both):

| State | Means |
| --- | --- |
| `OPENSC2_LINK_OFF` | Radio off: before `opensc2_start()`, after `opensc2_stop()`, after the puck turned the controller off, or after `CONFIG_OPENSC2_LISTEN_TIMEOUT_S`. |
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

## Inputs

Set fields with the setters, or the whole `struct opensc2_state` with `opensc2_set_state()`. All
setters are thread safe, cheap, and usable from interrupts. Whatever you never set stays zero, which
reads as neutral. The library builds a state report (input report `0x42`, 53 bytes) for every packet
the puck sends, about every 3 ms; the optional `sample` callback runs just before each one, so a
project can also sample there.

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

With `CONFIG_OPENSC2_TRIGGER_CLICK_AUTO` (default on) the trigger bits follow the trigger values:
on at controller setting 68 percent (90 % by default), off 5 % below. A digital trigger button can
therefore just call `opensc2_set_trigger(side, pressed ? 32767 : 0)`.

### Battery

`struct opensc2_battery`, sent as input report `0x43` every `CONFIG_OPENSC2_BATTERY_REPORT_INTERVAL_MS`
and answered to vendor command `0xBE`. Until set: 100 %, "charging done".

| Field | Unit | Note |
| --- | --- | --- |
| `charge_state` | `OPENSC2_CHARGE_DISCHARGING` (1), `_CHARGING` (2), `_DONE` (4), `_RESET` (0), `_SRC_VALIDATE` (3) | SDL's `EChargeState`; SDL shows 1 as on battery, 2 charging, 4 charged |
| `level` | percent | the battery level Steam shows |
| `battery_mv`, `system_mv`, `input_mv` | mV | |
| `current_ma`, `input_current_ma` | mA | battery current signed |
| `temperature` | raw | the charger's raw value on the real controller |
| `gauge_flags` | | only in `0xBE` |

### Other input reports

`opensc2_send_report(report, len)` queues any input report (ID first, at most 64 bytes) to ride with
the next state reports. The puck only passes IDs `0x40` to `0x45` with exactly their descriptor
length plus one: `0x40` mouse (5), `0x41` keyboard (8; only Enter, Escape, Tab and the arrows, no
modifiers), `0x42` state (53), `0x43` battery (14), `0x44` haptic stream status (5), `0x45` (45).
`opensc2_send_haptic_status(channel, flags)` builds `0x44`: channel left grip 0, right grip 1, left
pad 3, right pad 4; flags `0x01` bytes dropped, `0x02` ran dry or stopped, `0x04` below the water
mark (send more), `0x08` at the water mark (pause), `0x10` the format needs an 8 kHz channel, `0x20`
format set, `0x40` format cannot change while playing.

### Lizard mode

Until Steam turns it off (controller setting 9), the real controller is a plain mouse and keyboard.
With `CONFIG_OPENSC2_LIZARD_MODE` (default on) the library does a simplified version from the input
state: the right trackpad moves the pointer, the left one scrolls, the right trigger or a right-pad
click is the left mouse button, the left trigger the right one; A is Enter, B and VIEW are Escape,
the D-pad the arrow keys, MENU is Tab. No pointer momentum. With `CONFIG_OPENSC2_STEAM_WATCHDOG`, if
Steam stops repeating "lizard off" for 10 s, lizard mode comes back and the settings return to their
defaults, as on the real controller.

## Outputs

All callbacks are in `struct opensc2_callbacks`; any may be `NULL`. They run in the library's radio
thread, except `power_request`, which runs in the system work queue. Return quickly: the link only
lives while acknowledgements keep up with the puck.

| Callback | Called for | Gets |
| --- | --- | --- |
| `link_changed(info)` | every state change | `struct opensc2_link_info` |
| `sample(state)` | before each state report | the state, to update in place (it is kept) |
| `rumble(r)` | output report `0x80` | `left_speed`, `right_speed` 0..65535 (0 stops), `intensity`, `left/right_gain_db`, `type`; `timeout` set when the library stops a rumble the host did not repeat (`CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS`, 500) or the link dropped |
| `haptic(h)` | output reports `0x81`..`0x89` | `kind`, `channels`, raw `target`, and the decoded fields below |
| `output_report(report, len)` | any output report, raw | ID first |
| `led(led)` | vendor command `0xC5`, settings 37 and 45 | `red`, `green`, `blue`, `white` percent; `override` (setting 37: show this instead of the status pattern); `brightness` (setting 45, 0..100, default 50) |
| `setting_changed(number, value)` | vendor command `0x87` (each setting), `0x81`/`0x85` (setting 9), `0x8E` and the Steam watchdog (`OPENSC2_SETTINGS_ALL`) | |
| `host_asleep()` | the puck says the PC is asleep (the link stays up) | the real controller plays a short cue |
| `power_request(req)` | see below | |
| `vendor_command(cmd, answer, answer_len)` | vendor commands the library does not answer itself | see [Vendor commands](#vendor-commands) |

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

Power requests (`power_request`, in the system work queue; vendor commands about 20 ms after they
arrive, so their answer still goes out):

| Request | From | Suggested action |
| --- | --- | --- |
| `OPENSC2_POWER_TURN_OFF_PUCK` | the puck's "turn off" (the PC sleeps) | power down if on battery; the link goes `OFF` by itself once the puck stops. A button press can then `opensc2_start()` again, which also lets the puck wake the PC. |
| `OPENSC2_POWER_TURN_OFF` | `0x9F`, or `0x95` with `0xABDF9710` | `opensc2_stop()` or power down |
| `OPENSC2_POWER_REBOOT` | `0x95` with another value | reboot |
| `OPENSC2_POWER_REBOOT_KEEP_LINK` | `0x95` with `0xA427AF52` | reboot |
| `OPENSC2_POWER_BOOTLOADER` | `0x90`, or `0x95` with `0xECAABAC0`: Steam's firmware update | your bootloader cannot take Steam's update; ignoring it is usually right |
| `OPENSC2_POWER_FACTORY_RESET` | `0x86` with `0xEFC1A917` | the bonds, slot and chime level are already erased; reboot |

## Identity

`struct opensc2_identity`, all of it sent once per connection in the identity packet `0xF5` and used
for vendor commands `0x83`, `0xAE` and `0xF2`. The October 2026 puck firmware answers Steam's
attribute queries from the identity packet; older puck firmware passes the queries to the controller,
which the library answers too.

| Field | Rule | Default (Kconfig) | Where it shows |
| --- | --- | --- | --- |
| `unit_serial` | exactly 13 characters, `0-9` and `A-Z` | `CONFIG_OPENSC2_UNIT_SERIAL` "OSC2DEV000001", or with `CONFIG_OPENSC2_SERIAL_FROM_CHIP` (default) "OSC" + 10 hex digits of the chip ID | the serial Steam reads (`0xAE` tag 1); pairing packets |
| `board_serial` | same | "OSC2PCB000001", or "OSB" + chip ID | `0xAE` tags 0 and 4 |
| `git_revision` | 1 to 12 printable characters | `CONFIG_OPENSC2_GIT_REVISION` "opensc2-0.1" | `0xAE` tag 3, `0xF2` |
| `product_id` | non-zero; Steam knows the Steam Controller 2 as `0x1302` | `CONFIG_OPENSC2_PRODUCT_ID` | attribute 1 |
| `board_revision` | any; real controllers use `0x45`, `0x48`..`0x4B` | `CONFIG_OPENSC2_BOARD_REVISION` `0x4B` | attribute 9 |
| `build_time` | Unix seconds | `CONFIG_OPENSC2_BUILD_TIME`, 0 = when the build was configured | attribute 4 |
| `bootloader_build_time` | Unix seconds, 0 = none | `CONFIG_OPENSC2_BOOTLOADER_BUILD_TIME` | attribute 10 |

The "OSC"/"OSB" development serials are this project's, not a real controller's. The real controller
also has a Bluetooth name ("Steam Ctrl (BT) " + serial), but nothing is named over the puck's link
and this library has no Bluetooth.

Three ways to set it, later ones winning field by field:

1. **Kconfig** (`CONFIG_OPENSC2_*` in `prj.conf`), checked at build time.
2. **At run time**: `config.identity` in `opensc2_init()`. Empty strings and zero numbers keep the
   Kconfig value. An invalid identity makes `opensc2_init()` return `-EINVAL`.
3. **On a built board, without rebuilding**: `opensc2_identity_store(&id)` (fields left empty keep
   what they are), the shell (`opensc2 id set unit ABC0123456789`, `opensc2 id clear`), or, if
   `CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE` is on, vendor command `0xEE` with the setting names
   `opensc2/unit_serial`, `opensc2/board_serial`, `opensc2/git_revision`, `opensc2/board_revision`.
   Stored under the settings key `opensc2/id`; validated before it is stored; kept by a factory reset.

`opensc2_identity_get()` returns the identity in use; `opensc2_identity_check()` (pure) validates one
and returns a mask of `OPENSC2_ID_BAD_*` problems.

## Bonds and pairing

A bond is one pairing with one puck, 24 bytes: `puck_id` (chosen by the puck), `controller_id` (chosen
by this controller when pairing) and the puck's serial. Like the real controller there are two slots,
so one board can be paired with two pucks; `opensc2_select_slot(0|1)` chooses (and stores the choice).

| Function | Does |
| --- | --- |
| `opensc2_bond_get(slot, &bond)` | read one |
| `opensc2_bond_set(slot, &bond)` | store one (both IDs zero: delete). Listening restarts with it. |
| `opensc2_select_slot(slot)`, `opensc2_selected_slot()` | the slot in use |

Pairing: Steam opens the puck's pairing window (60 s); `opensc2_pair_start()` then answers the puck's
beacon, exchanges IDs, stores the bond in the selected slot (replacing what was there) and starts
listening. The puck connects within a second. Bonds can also arrive through vendor commands, as Steam
does for real controllers over a cable: `0xA2` (slot 1) and `0xEE`/`0xEF`/`0xF0` on `esb/bond` and
`esb/bond_2`. Treat the stored IDs as private: they identify the puck.

Storage keys (Zephyr settings): `opensc2/bond0`, `opensc2/bond1`, `opensc2/slot`, `opensc2/chime`,
`opensc2/id`.

## Vendor commands

The puck passes Steam's feature report 1 to the controller as records in its packets: a SET runs a
command, a GET answers the command the last SET named (with no SET before it, `0x83`). The library
answers these itself:

| Opcode | Kind | Answer / action |
| --- | --- | --- |
| `0x83` | get | six `{tag, u32}`: 1 product ID, 2 capabilities (0), 10 bootloader build time, 4 build time, 9 board revision, 11 report interval (4000 µs) |
| `0xAE` | get | the tag, then a string zero padded to 20 bytes: 1 unit serial, 0 and 4 board serial, 3 git revision, others "NA" |
| `0xF2` | get | selector 0: build time, board revision, git revision (16), unit serial (16); 1: an empty crash record (33 zero bytes); 2: uptime in ms (u32) and the last disconnect reason (u8) |
| `0xA1` | get | selector 1: reset cause (u32, Zephyr's `RESET_*` flags), the chip's 8-byte device ID, 8 zero bytes |
| `0xBE` | get | battery data, 15 bytes (from `opensc2_set_battery()`) |
| `0xE9` / `0xC5` | get / set | LED colour, four percentages (calls `led`) |
| `0xDB` / `0xDC` | get / set | user store type 1: the power-on chime level (stored; `opensc2_chime_level()`) |
| `0x89` / `0x87` / `0x8E` | get / set / set | controller settings (calls `setting_changed`) |
| `0x81` / `0x85` | set | lizard mode off / on |
| `0xED` / `0xEE` / `0xEF` / `0xF0` | get / set / save / delete by name | `esb/bond`, `esb/bond_2`, `user/wireless_transport` (2 = slot 1, 3 = slot 2), `user/haptic_boot_level`; other names go to `vendor_command` (a `0xED` nobody answers gets an empty answer) |
| `0xA2` | set | write the bond of slot 1, exactly 24 bytes (an ID of zero deletes it) |
| `0x86`, `0x90`, `0x95`, `0x9F` | set | factory reset, bootloader, reboot, turn off: `power_request` |

Everything else (the calibration commands `0xD8`, `0xC0`, `0xC3`, `0xE2`, `0xFE`, and anything new)
goes to `vendor_command`:

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
payload of the SET before it.

## Controller settings

86 numbered 16-bit values that Steam writes with `0x87` and reads with `0x89`, in RAM only (defaults
at every boot), as on the real controller. `opensc2_setting_get(n)`; constants in
`<opensc2/settings.h>` (`OPENSC2_SETTING_*`, with SDL's names in the comments). The ones a project is
most likely to act on:

| # | Constant | Default | Meaning |
| --- | --- | --- | --- |
| 9 | `OPENSC2_SETTING_LIZARD_MODE` | 1 | lizard mode; Steam sets 0 |
| 27 | `OPENSC2_SETTING_RUMBLE_GAIN_DB` | -10 | rumble gain offset, dB |
| 37 | `OPENSC2_SETTING_LED_OVERRIDE` | 0 | show `0xC5`'s colour |
| 45 | `OPENSC2_SETTING_LED_BRIGHTNESS` | 50 | status LED brightness, percent |
| 48 | `OPENSC2_SETTING_IMU_MODE` | 0 | IMU stream: `OPENSC2_IMU_MODE_SEND_RAW_ACCEL` \| `_SEND_RAW_GYRO` from SDL when sensors are on |
| 50 | `OPENSC2_SETTING_SLEEP_TIMEOUT_S` | 900 | idle power-off on battery |
| 52, 53 | `OPENSC2_SETTING_LEFT/RIGHT_CLICK_PRESSURE` | 40 | pad click threshold, percent |
| 68 | `OPENSC2_SETTING_TRIGGER_THRESHOLD` | 90 | trigger digital threshold, percent (used by the library) |
| 70, 76 | `OPENSC2_SETTING_HAPTICS_ENABLED`, `_HAPTIC_GAIN_DB` | 1, -3 | haptics on, master gain |
| 71 | `OPENSC2_SETTING_STEAM_WATCHDOG` | 1 | the 10 s lizard watchdog (used by the library) |

The documentation gives no minimum or maximum for the settings, so values are stored as sent.

## Kconfig options

| Option | Default | |
| --- | --- | --- |
| `CONFIG_OPENSC2` | n | the library (selects ESB) |
| `CONFIG_ESB_MAX_PAYLOAD_LENGTH` | must be 128 | checked at build time |
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
| `CONFIG_OPENSC2_STORAGE` | y with `CONFIG_SETTINGS` | bonds and identity in Zephyr settings |
| `CONFIG_OPENSC2_SHELL` | y with `CONFIG_SHELL` | |
| `CONFIG_OPENSC2_FUSION` | n | the IMU fusion helper |
| `CONFIG_OPENSC2_IDENTITY_VENDOR_WRITE` | n | identity through `0xEE` |
| `CONFIG_OPENSC2_RX_QUEUE_DEPTH`, `_REPORT_QUEUE_DEPTH` | 8, 6 | |
| identity: `_UNIT_SERIAL`, `_BOARD_SERIAL`, `_SERIAL_FROM_CHIP`, `_GIT_REVISION`, `_PRODUCT_ID`, `_BOARD_REVISION`, `_BUILD_TIME`, `_BOOTLOADER_BUILD_TIME` | see [Identity](#identity) | |

## Threads and timing

- One radio thread (`opensc2_radio`, priority `CONFIG_OPENSC2_THREAD_PRIORITY`). Keep long-running
  application threads below it (a higher number).
- The puck sends a keep-alive about every 3 ms; each reply goes out with the acknowledgement of the
  puck's next packet. State reports are sampled `CONFIG_OPENSC2_REPORT_DELAY_US` after a keep-alive.
- Link loss: 10 ms on the current channel, then 13 ms on each alternate, 500 ms in all.
- A flash write (storing a bond) stalls the CPU for a few milliseconds; the link tolerates 500 ms
  without a packet, so it should ride that out (not measured).
- The library needs the 32 MHz crystal while the radio is on and requests it itself.

## Shell commands

With `CONFIG_SHELL` (on the SuperMini: the USB serial port):

| Command | Does |
| --- | --- |
| `opensc2 status` | link, channels, counters, lizard and IMU mode, identity |
| `opensc2 pair [seconds]`, `opensc2 cancel` | pairing |
| `opensc2 start`, `opensc2 stop` | radio on / off |
| `opensc2 slot [1|2]`, `opensc2 bonds`, `opensc2 forget <1|2>` | bonds |
| `opensc2 id`, `opensc2 id set <unit|board|git|pid|rev|build|blbuild> <value>`, `opensc2 id clear` | identity |
| `opensc2 state` | the current input state |

## Pure helpers

`<opensc2/codec.h>` has every wire format as a pure function (encode the state, battery and identity
records; decode output reports; parse connection requests, channel changes, protocol versions,
pairing packets and records; build acknowledgement payloads). They need no kernel and are what the
host tests exercise. `<opensc2/fusion.h>` (`CONFIG_OPENSC2_FUSION`) turns accelerometer and gyroscope
readings into the orientation quaternion with a small Mahony filter, and converts g and °/s into the
report's units (`opensc2_accel_from_g()`, `opensc2_gyro_from_dps()`). Without it, or another source
of orientation, the quaternion stays zero.
