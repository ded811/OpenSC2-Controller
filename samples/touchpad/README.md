# touchpad: a touch module as the right trackpad

A Cirque Pinnacle trackpad module, read by Zephyr's own driver, as the Steam Controller 2's right
trackpad. For anyone who wants to add a trackpad, or any other touch device that Zephyr's input
subsystem knows.

> [!NOTE]
> Builds for the SuperMini (`promicro_nrf52840/nrf52840/uf2`); not run on hardware.

## What it shows

- An `INPUT_CALLBACK_DEFINE` on the touch device that collects `INPUT_ABS_X`, `INPUT_ABS_Y` and
  either `INPUT_ABS_Z` (touch strength) or `INPUT_BTN_TOUCH` until the report's `sync` flag, then
  calls `opensc2_set_trackpad(OPENSC2_RIGHT, &pad)`.
- The position scaled to -32767..32767 with Y flipped (touch devices count Y from the top; the
  controller counts up as positive), and the touch strength as the pad's pressure.
- The pad's click as a button bit (`OPENSC2_KEY_RIGHT_PAD_CLICK`, bit 22) from `gpio-keys`.
- A part powered from the board's switched VCC before its driver starts: VCC goes on in a
  `SYS_INIT` hook, since Zephyr starts the touch driver before `main()`.
- The pairing button with the same rules as every sample.

## Parts

- The SuperMini nRF52840, a Pro Micro-sized nRF52840 board with the nice!nano pinout and the
  Adafruit nRF52 UF2 bootloader.
- A Cirque Pinnacle trackpad module with its I2C interface (for example the TM040040 or TM035035),
  run by Zephyr's `cirque,pinnacle` driver (its devicetree binding in nRF Connect SDK v3.4.0:
  [cirque,pinnacle-common.yaml](https://github.com/nrfconnect/sdk-zephyr/blob/ncs-v3.4.0/dts/bindings/input/cirque,pinnacle-common.yaml)).
  The module must be set for I2C: some are delivered set for SPI, and their datasheet says how to
  switch.
- Two push buttons: the pad's click and the pairing button.

## Wiring

| SuperMini pin | Connects to |
| --- | --- |
| P0.17 | the module's SDA |
| P0.20 | the module's SCL |
| P0.22 | the module's data-ready output (HW_DR) |
| VCC | the module's 3.3 V supply |
| GND | the module's GND, and the buttons |
| P1.15 | pad click button, other side to GND |
| P0.31 | pairing button, other side to GND |

SDA and SCL need pull-up resistors to VCC (about 4.7 kOhm each) unless the module has them. The
board switches VCC with P0.13; the sample turns it on before the touch driver starts. The silkscreen
names pins by three digits: "006" is P0.06, "111" is P1.11. Pins are set in
[boards/promicro_nrf52840_nrf52840_uf2.overlay](boards/promicro_nrf52840_nrf52840_uf2.overlay).

## Build and flash

Windows, in PowerShell in the repository folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\touchpad -OutDir C:\b\osc2-tp
```

The image is `C:\b\osc2-tp\touchpad\zephyr\zephyr.uf2`. `C:\b\osc2-tp` is any short folder:
Windows' 260-character path limit breaks deep build folders.

Linux and macOS, in the nRF Connect SDK's shell with `ZEPHYR_BASE` set
([Build](../../docs/getting-started.md#build); not tried for this project yet):

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/touchpad samples/touchpad
```

The image is `build/touchpad/touchpad/zephyr/zephyr.uf2`.

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

- In Steam, the module is the right trackpad (touch, position and pressure) and the button on
  P1.15 its click, wired and through the puck. What the pad does is up to its configuration in
  Steam: mouse, camera or anything else.
- With Steam closed, the pad moves the mouse pointer and its click is the left mouse button. That
  is lizard mode: until Steam takes over, a Steam Controller 2 acts as a plain mouse and keyboard
  (the right trackpad moves the pointer, A is Enter, B is Escape, the D-pad gives the arrow keys).
  Steam switches it off while it runs; about 10 seconds after Steam stops, it comes back. OpenSC2
  does the same.
- In wireless mode the board's USB serial port has a shell
  ([Reach the shell](../../docs/getting-started.md#reach-the-shell)). `opensc2 state` shows
  `pad R     touch <x> <y> pressure <p>` while a finger is on the pad, and `buttons   0x00400000`
  while the click is held. If the module did not start, the log says "The touch module did not
  start". `opensc2 status` shows the board's serials: mask them before you post the output.
- When the PC goes to sleep, the puck tells the controller to turn off: the library's system state
  machine switches wireless off, as the real controller does on USB power, and nothing in this
  sample turns it on again. Type `opensc2 start` on its console, or reset the board (short RST to
  GND once), holding the pairing button if it is on USB power, so that it starts wireless again.

## Change it

- **Another touch device**: Zephyr's touch-panel drivers report the same events, for example the
  FT5336 (`focaltech,ft5336`), the GT911 (`goodix,gt911`) and the CST816S (`hynitron,cst8xx`).
  Replace the `touch` node in the overlay, keep its label, and set `trackpad-size` in
  `zephyr,user` to the coordinate range the device reports. A device that reports a touch key
  instead of a strength gets half pressure while touched.
- **The pad's direction**: the Y flip is the minus sign in `on_touch()`; turn a mounted pad by
  swapping or negating `pad.x` and `pad.y` there.
- **The left trackpad**: a second touch device with its own callback and `OPENSC2_LEFT`; its click
  is `OPENSC2_KEY_LEFT_PAD_CLICK`. `app/src/trackpad.c` does both pads from the devicetree
  (`opensc2,right-trackpad`, `opensc2,left-trackpad`).
- **No touch at all**: if the log says the module did not start, check SDA, SCL, power and the
  I2C setting first. The data-ready line is set active high (HW_DR goes high when a sample is
  ready, and the driver reads on that edge); the binding's text mentions active low for a direct
  connection, so if nothing arrives, try `GPIO_ACTIVE_LOW` on `data-ready-gpios`.
- **Your own project**: start from [template/](../../template)
  ([docs/integration.md](../../docs/integration.md)).

Back to the [samples](../README.md) · the [documentation map](../../docs/README.md)
