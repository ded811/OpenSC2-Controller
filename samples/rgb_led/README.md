# rgb_led: an RGB status LED that follows Steam's colour

An RGB LED on three PWM outputs that shows the board's state, and the colour Steam sets once it
takes over the controller's LED. For anyone who wants a status LED like the real controller's.

> [!NOTE]
> Builds for the SuperMini (`promicro_nrf52840/nrf52840/uf2`); not run on hardware.

## What it shows

- `opensc2_led_status()`: the real controller's LED mode, which the library's system state machine
  selects (on by default), with Steam's colour (vendor command `0xC5`) while Steam's LED override
  is on (controller setting 37) and the brightness Steam sets (controller settings 45 and 38).
- The pattern player of `<opensc2/led.h>` (`opensc2_led_player_step()` every 10 ms): the real
  controller's patterns and fades as a colour, and `opensc2_led_duty()` with the unit's LED trims
  (`opensc2_cal_led_trim()`, the calibration keys `cal/rgbw_*`): the same patterns and colours as
  `app/` (`app/src/status.c`). The white channel is mixed into red, green and blue.
- Three PWM channels of one PWM instance (so one period), found by their names (`red`, `green`,
  `blue`) in `zephyr,user`, with the polarity set for a common-anode or a common-cathode LED.
- The pairing button with the same rules as every sample.

## Parts

- The SuperMini nRF52840, a Pro Micro-sized nRF52840 board with the nice!nano pinout and the
  Adafruit nRF52 UF2 bootloader.
- A common-anode RGB LED and three resistors of a few hundred ohms (or a common-cathode LED, see
  below), on three channels of the nRF52840's PWM
  ([nordic,nrf-pwm.yaml](https://github.com/nrfconnect/sdk-zephyr/blob/ncs-v3.4.0/dts/bindings/pwm/nordic,nrf-pwm.yaml)).
- A push button for pairing.

## Wiring

| SuperMini pin | Connects to |
| --- | --- |
| P0.22 | the red cathode, through a resistor |
| P0.24 | the green cathode, through a resistor |
| P1.00 | the blue cathode, through a resistor |
| VCC | the LED's common anode |
| P0.31 | pairing button, other side to GND |
| GND | the button |

For a common-cathode LED, connect the cathode to GND and each anode through its resistor to its
pin, and change the three `PWM_POLARITY_INVERTED` in the overlay to `PWM_POLARITY_NORMAL`. P1.00 is
also the board's default I2C data pin, so the overlay switches that I2C bus off. The board
switches VCC with P0.13; the sample turns it on at boot. The silkscreen names pins by three
digits: "006" is P0.06, "111" is P1.11. Pins are set in
[boards/promicro_nrf52840_nrf52840_uf2.overlay](boards/promicro_nrf52840_nrf52840_uf2.overlay).

## Build and flash

Windows, in PowerShell in the repository folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\rgb_led -OutDir C:\b\osc2-rgb
```

The image is `C:\b\osc2-rgb\rgb_led\zephyr\zephyr.uf2`. `C:\b\osc2-rgb` is any short folder:
Windows' 260-character path limit breaks deep build folders.

Linux and macOS, in the nRF Connect SDK's shell with `ZEPHYR_BASE` set
([Build](../../docs/getting-started.md#build); not tried for this project yet):

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/rgb_led samples/rgb_led
```

The image is `build/rgb_led/rgb_led/zephyr/zephyr.uf2`.

To flash, short RST to GND twice quickly: a USB drive appears. Copy `zephyr.uf2` onto it; the board
restarts with the new firmware ([Flash](../../docs/getting-started.md#flash)).

## Add it in Steam

These steps are how the tested firmware in `app/` behaves; this sample follows the same rules
but has not run on hardware.

1. Plug the puck into the PC and start Steam. Switch a real Steam Controller 2 off and keep it
   unplugged while you add the board.
2. In Steam, start adding a controller. Steam asks you to connect it with a USB-C cable.
3. Plug the board in with a USB-C data cable (not a charge-only cable). It comes up wired: the RGB
   LED is dark at first, then green once the PC has set the board up.
4. Steam pairs the board with the puck over the cable and reboots it into wireless mode: the LED
   goes dark, pulses slowly in white-blue while the board looks for the puck, then stays on. The
   board is now connected through the puck.
5. From then on the board connects through the puck by itself whenever it starts wireless. Plugged
   into a PC again it comes up wired; hold the pairing button while plugging in to keep it
   wireless.

If Steam later forgets the controller, add it again the same way. The same steps for `app/`:
[Add it in Steam](../../docs/getting-started.md#add-it-in-steam).

The pairing button (P0.31):

| When | What it does |
| --- | --- |
| held while plugging in | this boot is wireless |
| pressed and released while wired | the board reboots into wireless mode |
| pressed while wireless | it answers an open pairing window (untested with Steam) |

## What to expect

The sample is built to do the following; none of it has run on hardware yet.

| The LED | Means |
| --- | --- |
| Steam's colour | while Steam's LED override is on |
| white-blue, on | wireless, connected through the puck |
| white-blue, a slow pulse (in over 0.1 s, about 1 s on, out, dark) | wireless, looking for the puck (also when it is not paired yet) |
| white-blue, two quick flashes, repeating | pairing over the air |
| green | wired, set up by the PC |
| amber | wired, but no PC has set the board up (a charger, a power bank), or wireless switched off on USB power (the PC went to sleep): STEAM brings it back wired |
| off | the first second or so after power-up, a reboot or a shutdown |

These are the steady colours of a board that counts as fully charged, as this one does without a
battery; an application that reports a lower level gets the real controller's breathing patterns.
Each colour is scaled by the unit's LED trims (90, 35, 120 and 80 % until a host stores others) and
by Steam's brightness setting (controller setting 45, 50 % by default, through the curve of
setting 38).

- In wireless mode the board's USB serial port has a shell
  ([Reach the shell](../../docs/getting-started.md#reach-the-shell)): `opensc2 status` shows the
  mode and the link. It also shows the board's serials: mask them before you post the output.
- When the PC goes to sleep, the puck tells the controller to turn off: the system state machine
  switches wireless off and the LED turns amber. This sample has no STEAM button: reset the board
  (short RST to GND once), holding the pairing button if it is on USB power, so that it starts
  wireless again, or type `opensc2 start` on its console.

## Change it

- **Colours and patterns**: `led_step()` turns the player's colour into the three duties; the
  patterns themselves are the library's ([docs/api.md](../../docs/api.md#status-led)), as the
  real controller plays them. A board without the system state machine
  (`CONFIG_OPENSC2_SYSTEM=n`) gets a plain mode from `get_status()` instead.
- **The LED callback**: the library also calls a `led_status` callback when the mode changes; this
  sample steps the player every 10 ms instead, since the patterns move between calls anyway.
- **The on-board LED**: `app/src/status.c` shows the same patterns on the board's one-colour LED
  (and on an RGB LED with the same channel names).
- **Other pins**: any free pins work, the nRF52840 routes PWM to any GPIO; change the `psels` in the
  overlay. Keep the three channels on one PWM instance with one period.
- **Your own project**: start from [template/](../../template)
  ([docs/integration.md](../../docs/integration.md)).

Back to the [samples](../README.md) · the [documentation map](../../docs/README.md)
