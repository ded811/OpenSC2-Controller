# minimal: the smallest Steam Controller 2

The least an OpenSC2 project needs, in one short `main.c`: one button, a pairing button, wired and
wireless mode. For anyone who wants to see the library's bare bones before adding parts.

> [!NOTE]
> Builds for the SuperMini (`promicro_nrf52840/nrf52840/uf2`); not run on hardware.

## What it shows

- `opensc2_init()` with its error logged, and `config.boot_mode = OPENSC2_BOOT_WIRELESS` when the
  pairing button is held at power-up.
- `opensc2_set_buttons()`: one button, A, read in a loop every 5 ms with a two-read debounce.
- The pairing button's three rules with `opensc2_mode()`, `opensc2_reboot_later()` and
  `opensc2_pair_start()`.
- Everything wired mode needs: the HID node, the retained RAM area and `sram0` in the overlay, the
  USB lines in `prj.conf` ([API reference: wired mode](../../docs/api.md#wired-mode)).
- No callbacks at all. Without a `power_request` callback the library does the reboots Steam asks
  for itself, and it ignores Steam's request to enter a bootloader for a firmware update.

## Parts

- The SuperMini nRF52840, a Pro Micro-sized nRF52840 board with the nice!nano pinout and the
  Adafruit nRF52 UF2 bootloader.
- Two push buttons (any momentary switch), or a wire to touch a pin to GND.

## Wiring

| SuperMini pin | Connects to |
| --- | --- |
| P1.15 | A button, other side to GND |
| P0.31 | pairing button, other side to GND |
| USB-C | the PC (power, and wired mode) |

The internal pull-ups hold both pins high; a pressed button pulls its pin to GND. The silkscreen
names pins by three digits: "006" is P0.06, "111" is P1.11. Pins are set in
[boards/promicro_nrf52840_nrf52840_uf2.overlay](boards/promicro_nrf52840_nrf52840_uf2.overlay).

## Build and flash

Windows, in PowerShell in the repository folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project samples\minimal -OutDir C:\b\osc2-minimal
```

The image is `C:\b\osc2-minimal\minimal\zephyr\zephyr.uf2`. `C:\b\osc2-minimal` is any short
folder: Windows' 260-character path limit breaks deep build folders.

Linux and macOS, in the nRF Connect SDK's shell with `ZEPHYR_BASE` set
([Build](../../docs/getting-started.md#build); not tried for this project yet):

```sh
west build -b promicro_nrf52840/nrf52840/uf2 -d build/minimal samples/minimal
```

The image is `build/minimal/minimal/zephyr/zephyr.uf2`.

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

- In Steam, the button on P1.15 is the Steam Controller 2's A button, wired and through the puck.
- With Steam closed, A presses Enter on the PC. That is lizard mode: until Steam takes over, a
  Steam Controller 2 acts as a plain mouse and keyboard (the right trackpad moves the pointer, A is
  Enter, B is Escape, the D-pad gives the arrow keys). Steam switches it off while it runs; about
  10 seconds after Steam stops, it comes back. OpenSC2 does the same.
- In wireless mode the board's USB serial port has a shell
  ([Reach the shell](../../docs/getting-started.md#reach-the-shell)). `opensc2 state` shows
  `buttons   0x00000001` while A is held, `opensc2 status` the mode and the link, and
  `opensc2 usb log` what happened while the board was wired. `opensc2 status` shows the board's
  serials: mask them before you post the output.
- When the PC goes to sleep, the puck tells the controller to turn off: the library's system state
  machine switches wireless off, as the real controller does on USB power, and nothing in this
  sample turns it on again. Type `opensc2 start` on its console, or reset the board (short RST to
  GND once), holding the pairing button if it is on USB power, so that it starts wireless again.

## Change it

- **Other pins**: change `a-gpios` and `pair-gpios` in the overlay.
- **Another button bit**: `OPENSC2_BTN_B`, `OPENSC2_BTN_STEAM` and the others are in
  `<opensc2/types.h>`; `opensc2_set_buttons(mask, value)` changes only the bits in `mask`.
- **More buttons**: a `gpio-keys` node is easier than one loop per pin; see
  [buttons_sticks](../buttons_sticks).
- **Your own project**: start from [template/](../../template), which has the same rules and a status
  LED ([docs/integration.md](../../docs/integration.md)).

Back to the [samples](../README.md) · the [documentation map](../../docs/README.md)
