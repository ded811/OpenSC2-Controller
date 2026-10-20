# rumble: rumble and haptics on one motor

Steam's rumble and haptic effects on a single PWM-driven vibration motor. For anyone who wants
force feedback on their controller and needs to see what the library hands over.

> [!NOTE]
> Builds for the SuperMini (`promicro_nrf52840/nrf52840/uf2`); not run on hardware.

## What it shows

- The `rumble` callback: output report `0x80`, decoded by the library into two motor speeds
  (0..65535); the motor runs at the stronger one.
- The `haptic` callback: Steam's haptic effects (clicks, pulses, tones, sweeps and scripts,
  reports `0x81` to `0x85`) as short buzzes, and "stop all" as a stop. PCM streams are ignored: a
  motor cannot play samples. Lizard mode's own pad ticks and clicks (report ID 0) would come the
  same way, on a board with trackpads.
- The library's rumble timeout: when Steam stops repeating a rumble (500 ms,
  `CONFIG_OPENSC2_RUMBLE_TIMEOUT_MS`) or the link drops, the callback gets speed 0 with `timeout`
  set.
- Short work in callbacks: they run in the library's radio thread (wireless) or in the USB stack's
  thread (wired; lizard mode's haptic events in the USB report thread), so they only set the PWM
  and schedule a work item.
- `PWM_DT_SPEC_GET_BY_NAME(..., rumble)`: a PWM channel found by its name in `zephyr,user`.
- The pairing button with the same rules as every sample.

## Parts

- The SuperMini nRF52840, a Pro Micro-sized nRF52840 board with the nice!nano pinout and the
  Adafruit nRF52 UF2 bootloader.
- A small vibration motor (an ERM: a coin or cylinder motor) with a logic-level N-channel MOSFET
  and a flyback diode, or a ready-made MOSFET module with a logic-level input. The nRF52840's PWM
  drives it ([nordic,nrf-pwm.yaml](https://github.com/nrfconnect/sdk-zephyr/blob/ncs-v3.4.0/dts/bindings/pwm/nordic,nrf-pwm.yaml)).
- A push button for pairing.

Never drive a motor straight from a pin: the pin cannot give the current, and the motor's
switching spikes can damage it.

## Wiring

With a MOSFET:

| SuperMini pin | Connects to |
| --- | --- |
| P0.22 | the MOSFET's gate (or the module's input) |
| VCC or BAT+ | the motor's + side |
| GND | the MOSFET's source, and the button |
| P0.31 | pairing button, other side to GND |

The motor's other side goes to the MOSFET's drain, with the diode across the motor (its cathode on
the + side). A resistor of about 100 kOhm from gate to GND keeps the motor off while the board
starts. The board switches VCC with P0.13; the sample turns it on at boot. Check how much current
your board's VCC can give before you run a motor from it. The silkscreen names pins by three
digits: "006" is P0.06, "111" is P1.11. Pins are set in
[boards/promicro_nrf52840_nrf52840_uf2.overlay](boards/promicro_nrf52840_nrf52840_uf2.overlay).

## Build and flash

Windows, in PowerShell in the repository folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\rumble -OutDir C:\b\osc2-rum
```

The image is `C:\b\osc2-rum\rumble\zephyr\zephyr.uf2`. `C:\b\osc2-rum` is any short folder:
Windows' 260-character path limit breaks deep build folders.

Linux and macOS, in the nRF Connect SDK's shell with `ZEPHYR_BASE` set
([Build](../../docs/getting-started.md#build); not tried for this project yet):

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/rumble samples/rumble
```

The image is `build/rumble/rumble/zephyr/zephyr.uf2`.

To flash, short RST to GND twice quickly: a USB drive appears. Copy `zephyr.uf2` onto it; the board
restarts with the new firmware ([Flash](../../docs/getting-started.md#flash)).

## Add it in Steam

These steps are how the tested firmware in `app/` behaves; this sample follows the same rules
but has not run on hardware.

1. Plug the puck into the PC and start Steam. Switch a real Steam Controller 2 off and keep it
   unplugged while you add the board.
2. In Steam, start adding a controller. Steam asks you to connect it with a USB-C cable.
3. Plug the board in with a USB-C data cable (not a charge-only cable). It comes up wired. This
   sample does not drive the board's LED.
4. Steam pairs the board with the puck over the cable and reboots it into wireless mode. The board
   then connects through the puck.
5. From then on the board connects through the puck by itself whenever it starts wireless. Plugged
   into a PC again it comes up wired; hold the pairing button while plugging in to keep it
   wireless.

If Steam later forgets the controller, add it again the same way. The same steps for `app/`, with
what its LED shows: [Add it in Steam](../../docs/getting-started.md#add-it-in-steam).

The pairing button (P0.31):

| When | What it does |
| --- | --- |
| held while plugging in | this boot is wireless |
| pressed and released while wired | the board reboots into wireless mode |
| pressed while wireless | it answers an open pairing window (untested with Steam) |

## What to expect

The sample is built to do the following; none of it has run on hardware yet.

- When a game rumbles the controller through Steam, the motor runs at the stronger of the two
  speeds. It stops when Steam sends speed 0, about 500 ms after Steam's last rumble report, or at
  once when the link drops.
- Steam's haptic effects become short buzzes of about their length (10 ms to 1 s).
- The library's side of this, the decoding of the rumble and haptic reports, is checked on
  hardware by the test suite, on `app/` ([Testing](../../docs/testing.md#results-so-far)); this
  sample's motor code has not run.
- In wireless mode the board's USB serial port has a shell
  ([Reach the shell](../../docs/getting-started.md#reach-the-shell)): `opensc2 status` shows the
  mode and the link. It also shows the board's serials: mask them before you post the output.
- When the PC goes to sleep, the puck tells the controller to turn off, and the radio stays off:
  this sample has no code that turns it back on (`app/` turns it on at any button press). Reset the
  board (short RST to GND once), holding the pairing button if it is on USB power, so that it
  starts wireless again.

## Change it

- **Two motors**: use `left_speed` (the low-frequency motor) and `right_speed` (the high-frequency
  one) separately, on two PWM channels. Channels of one nRF PWM instance share one period: keep
  both motors at the same frequency on one instance, or use two instances.
- **The buzz**: its strength (`52000` of 65535) and its length limits are in `buzz()`;
  `app/src/rumble.c` has a fuller version (pulse trains, strength from the effect's gain).
- **Steam's settings**: controller setting 70 (haptics on) and 27 (rumble gain, dB) are Steam's;
  read them with `opensc2_setting_get(OPENSC2_SETTING_HAPTICS_ENABLED)` and
  `OPENSC2_SETTING_RUMBLE_GAIN_DB` to follow them. The sample ignores them.
- **An LRA** (a linear resonant actuator) needs a driver chip. The DRV2605L can take a PWM input,
  but it must be switched to that mode over I2C first; Zephyr also has a driver for it
  (`ti,drv2605`, Zephyr's haptics API). This sample does neither.
- **Your own project**: start from [template/](../../template)
  ([docs/integration.md](../../docs/integration.md)).

Back to the [samples](../README.md) · the [documentation map](../../docs/README.md)
