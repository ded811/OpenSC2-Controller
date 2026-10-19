# buttons_sticks: buttons, a stick and a trigger

Five buttons, one analog stick and one analog trigger, all described in the devicetree. For anyone
who wants to see how buttons and analog inputs reach the library, in less code than `app/` uses.

> [!NOTE]
> Builds for the SuperMini (`promicro_nrf52840/nrf52840/uf2`); not run on hardware.

## What it shows

- A `gpio-keys` node whose `zephyr,code` values are bits of the controller's button word
  (`<dt-bindings/opensc2/inputs.h>`), read with `INPUT_CALLBACK_DEFINE` and handed over with
  `opensc2_set_buttons(BIT(code), ...)`.
- ADC channels named in `zephyr,user` (`"stick-x"`, `"stick-y"`, `"trigger"`), read in `main()` with
  `ADC_DT_SPEC_GET_BY_NAME`.
- `opensc2_set_stick()` and `opensc2_set_trigger()`, with the stick's centre and the trigger's rest
  taken once at power-on. The library sets the trigger's digital bit by itself from controller
  setting 68 (90 % by default: on above 95 % of the travel, off below 85 %).
- The pairing button with the same rules as every sample, here on P0.09 (below).
- `vcc-enable-gpios`: the board's switched VCC turned on before the first reading.

## Parts

- The SuperMini nRF52840, a Pro Micro-sized nRF52840 board with the nice!nano pinout and the
  Adafruit nRF52 UF2 bootloader.
- Six push buttons: A, B, X, Y, STEAM and the pairing button, read by Zephyr's `gpio-keys` driver
  ([gpio-keys.yaml](https://github.com/nrfconnect/sdk-zephyr/blob/ncs-v3.4.0/dts/bindings/input/gpio-keys.yaml)).
- A thumbstick with two potentiometers (the common dual-axis joystick breakouts work).
- A potentiometer, or a Hall-effect sensor with an analog output between GND and VCC, as the
  trigger.

## Wiring

| SuperMini pin | Connects to |
| --- | --- |
| P1.15 | A button, other side to GND |
| P1.13 | B button, other side to GND |
| P1.11 | X button, other side to GND |
| P0.10 | Y button, other side to GND |
| P1.04 | STEAM button, other side to GND |
| P0.09 | pairing button, other side to GND |
| P0.02 (AIN0) | the stick's X wiper |
| P0.29 (AIN5) | the stick's Y wiper |
| P0.31 (AIN7) | the trigger's wiper (or the Hall sensor's output) |
| VCC | one end of each potentiometer (the Hall sensor's supply) |
| GND | the other end of each potentiometer, and the buttons |

**Why the pairing button is on P0.09.** The SuperMini has only three analog pins (P0.02, P0.29 and
P0.31), and the stick and the trigger take all three. So the pairing button moves from P0.31, its
pin in every other sample and the template, to P0.09. P0.09 and P0.10 are also the board's default
UART pins, so the overlay switches the UART off (the log and the shell are on the USB serial port).

The ADC measures against VDD and the potentiometers run from VCC, so the readings do not depend on
the exact supply voltage. The board switches VCC with P0.13; the sample turns it on at boot. The
silkscreen names pins by three digits: "006" is P0.06, "111" is P1.11. Pins are set in
[boards/promicro_nrf52840_nrf52840_uf2.overlay](boards/promicro_nrf52840_nrf52840_uf2.overlay).

## Build and flash

Windows, in PowerShell in the repository folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\buttons_sticks -OutDir C:\b\osc2-bs
```

The image is `C:\b\osc2-bs\buttons_sticks\zephyr\zephyr.uf2`. `C:\b\osc2-bs` is any short folder:
Windows' 260-character path limit breaks deep build folders.

Linux and macOS, in the nRF Connect SDK's shell with `ZEPHYR_BASE` set
([Build](../../docs/getting-started.md#build); not tried for this project yet):

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/buttons_sticks samples/buttons_sticks
```

The image is `build/buttons_sticks/buttons_sticks/zephyr/zephyr.uf2`.

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

The pairing button (P0.09 in this sample):

| When | What it does |
| --- | --- |
| held while plugging in | this boot is wireless |
| pressed and released while wired | the board reboots into wireless mode |
| pressed while wireless | it answers an open pairing window (untested with Steam) |

## What to expect

The sample is built to do the following; none of it has run on hardware yet.

- In Steam: A, B, X, Y and STEAM, the left stick, and the right trigger, which also clicks (its
  digital bit) near the end of its travel, wired and through the puck.
- Keep your thumbs off the stick and the trigger while the board starts: their resting positions
  at power-on are the centre and the released end.
- With Steam closed, A presses Enter and B presses Escape on the PC, and the trigger, pulled to the
  end, clicks the left mouse button. That is lizard mode: until Steam takes over, a Steam Controller 2 acts as a plain mouse
  and keyboard (the right trackpad moves the pointer, A is Enter, B is Escape, the D-pad gives the
  arrow keys). Steam switches it off while it runs; about 10 seconds after Steam stops, it comes
  back. OpenSC2 does the same.
- In wireless mode the board's USB serial port has a shell
  ([Reach the shell](../../docs/getting-started.md#reach-the-shell)). `opensc2 state` shows what
  the board sends: `buttons` with bit 0 for A (`0x00000001`), bit 1 for B, bit 2 for X, bit 3 for Y
  and bit 16 for STEAM (`0x00010000`); `sticks L <x> <y>` from -32767 to 32767; `triggers` with the
  right one from 0 to 32767. `opensc2 status` shows the board's serials: mask them before you post
  the output.
- When the PC goes to sleep, the puck tells the controller to turn off, and the radio stays off:
  this sample has no code that turns it back on (`app/` turns it on at any button press). Reset the
  board (short RST to GND once), holding the pairing button if it is on USB power, so that it
  starts wireless again.

## Change it

- **Other pins or buttons**: edit the `gpio-keys` children in the overlay; each `zephyr,code` is a
  bit from `<dt-bindings/opensc2/inputs.h>` (`OPENSC2_KEY_L`, `OPENSC2_KEY_DPAD_UP`, ...). Each
  button takes one of the nRF52840's eight GPIOTE channels; for more than eight, the overlay's last
  comment shows the `sense-edge-mask` lines `app/` uses.
- **The stick's feel**: `to_axis()` in `src/main.c` reaches full output 1600 counts (of 4095) from
  the centre, after a dead zone of 100 counts; the trigger starts 80 counts above its rest and is
  full 3280 counts above it. `app/src/inputs.c` has the full version: ranges that grow with use, a
  dead zone in percent, unwired pins held neutral, inverted axes.
- **A digital trigger** (a button instead of the potentiometer): give a button the code
  `OPENSC2_KEY_RIGHT_TRIGGER_CLICK` and call `opensc2_set_trigger(OPENSC2_RIGHT, pressed ?
  OPENSC2_TRIGGER_MAX : 0)` for it, as `app/src/inputs.c` does.
- **Two sticks and analog triggers**: the SuperMini has no more analog pins. Use a board with six or
  more (for example the Seeed XIAO nRF52840, see
  [docs/porting.md](../../docs/porting.md#boards-with-more-analog-pins); untested) or an external
  ADC.
- **Another board**: P0.09 and P0.10 are NFC pins on the nRF52840; the SuperMini's board files make
  them GPIOs (`nfct-pins-as-gpios`). Check yours before you use them.
- **Your own project**: start from [template/](../../template)
  ([docs/integration.md](../../docs/integration.md)).

Back to the [samples](../README.md) · the [documentation map](../../docs/README.md)
