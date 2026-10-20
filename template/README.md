# Template: your own controller project

A small, complete project to copy out of this repository and grow into your own controller
firmware. For developers who want their own firmware rather than the ready-made one in
[app/](https://github.com/ded811/OpenSC2-Controller/tree/main/app).

It is the OpenSC2 library, five buttons, a pairing button and a status LED on the SuperMini
nRF52840, a Pro Micro-sized nRF52840 board with the nice!nano pinout and the Adafruit nRF52 UF2
bootloader. It follows the same rules as the tested firmware in `app/`: wired, Steam pairs it with
Valve's puck over the USB-C cable; wireless, it connects through the puck.

> [!NOTE]
> Unofficial: not affiliated with or endorsed by Valve. Steam and Steam Controller are trademarks of
> Valve Corporation. So that Steam recognises it, the firmware uses Valve's USB IDs (`28DE:1302`) and
> the real controller's USB strings, for personal interoperability and research use. It contains no
> Valve code or binaries. See
> [NOTICE.md](https://github.com/ded811/OpenSC2-Controller/blob/main/NOTICE.md).

**Status: builds; not run on hardware.** The template builds for the SuperMini without warnings,
but it has not run on a board yet. The hardware results come from the v0.2.0 release image of
the firmware in `app/` ([What works](https://github.com/ded811/OpenSC2-Controller/blob/main/README.md#what-works)).

Links to the rest of the repository are absolute, so they keep working in your copy.

## Copy it

You need git and nRF Connect SDK v3.4.0
([install it](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/getting-started.md#install-the-nrf-connect-sdk)).
The commands below keep the library in a clone of its own and point the build at it with
`ZEPHYR_EXTRA_MODULES`.

Windows (PowerShell):

```powershell
git clone https://github.com/ded811/OpenSC2-Controller C:\src\OpenSC2-Controller
Copy-Item -Recurse C:\src\OpenSC2-Controller\template C:\src\my-controller
$env:ZEPHYR_EXTRA_MODULES = 'C:/src/OpenSC2-Controller'
powershell -NoProfile -ExecutionPolicy Bypass -File C:\src\OpenSC2-Controller\build.ps1 -Project C:\src\my-controller -OutDir C:\b\mine
# result: C:\b\mine\my-controller\zephyr\zephyr.uf2
```

- `$env:ZEPHYR_EXTRA_MODULES` lasts for this PowerShell window: set it again in a new one. Write it
  with forward slashes.
- `C:\b\mine` is any short folder: Windows' 260-character path limit breaks deep build folders.
- `build.ps1` looks for the SDK in `C:\ncs`; add `-NcsRoot <folder>` for another one.

Linux and macOS:

```sh
git clone https://github.com/ded811/OpenSC2-Controller ~/src/OpenSC2-Controller
cp -r ~/src/OpenSC2-Controller/template ~/src/my-controller
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --shell   # opens the SDK's shell; go on in it
export ZEPHYR_BASE=~/ncs/v3.4.0/zephyr            # macOS: /opt/nordic/ncs/v3.4.0/zephyr
export ZEPHYR_EXTRA_MODULES=~/src/OpenSC2-Controller
west build -b promicro_nrf52840/nrf52840/uf2 -d ~/src/my-controller/build ~/src/my-controller
# result: ~/src/my-controller/build/my-controller/zephyr/zephyr.uf2
```

Building on Linux and macOS follows the nRF Connect SDK's own instructions; it has not been tried
for this project yet.

Then make it yours: rename the folder and `project(my_controller ...)` in `CMakeLists.txt` if you
like (the `.uf2` lands in a folder named after the project folder), and put it under git (this
folder's `.gitignore` keeps `build/` out).

To try the template before you copy it, build it where it is, from the repository folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Project template -OutDir C:\b\tpl
# result: C:\b\tpl\template\zephyr\zephyr.uf2
```

### The three ways to bring in the library

The template finds the library in all three ways that
[docs/integration.md](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/integration.md)
explains:

| Way | Where the library is | What the template does |
| --- | --- | --- |
| [a plain folder](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/integration.md#way-1-a-plain-folder) | a clone anywhere | uses `ZEPHYR_EXTRA_MODULES` (above) |
| [a git submodule](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/integration.md#way-2-a-git-submodule) | `modules/opensc2` inside your project | finds it there by itself |
| [a west workspace](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/integration.md#way-3-a-west-workspace) | fetched by `west update`, listed in your `west.yml` | nothing: west hands it to the build (this folder has a `west.yml` to start from; not built this way yet) |

## Flash it and add it in Steam

Flash: short RST to GND twice quickly: a USB drive appears. Copy `zephyr.uf2` onto it; the board
restarts with the new firmware
([more on flashing](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/getting-started.md#flash)).

Then add it in Steam, the same way as the firmware in `app/` (these steps are how that tested
firmware behaves; the template follows the same rules but has not run on hardware):

1. Plug the puck into the PC and start Steam. Switch a real Steam Controller 2 off and keep it
   unplugged while you add the board.
2. In Steam, start adding a controller. Steam asks you to connect it with a USB-C cable.
3. Plug the board in with a USB-C data cable (not a charge-only cable). It comes up wired: the LED
   is on.
4. Steam pairs the board with the puck over the cable and reboots it into wireless mode: the LED
   blinks slowly while the board looks for the puck, then stays on. The board is now connected
   through the puck.
5. From then on the board connects through the puck by itself whenever it starts wireless.
   Plugged into a PC again it comes up wired; hold the pairing button (on a bare board: P0.31
   against GND) while plugging in to keep it wireless.

If Steam later forgets the controller, add it again the same way. More detail, also for when
something does not work:
[getting started](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/getting-started.md#add-it-in-steam)
and [troubleshooting](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/troubleshooting.md).
A fresh build reports the time it was built, newer than any controller firmware Steam had shipped
by then; if Steam offers a controller update anyway, see
[Steam asks to update the controller](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/troubleshooting.md#steam-asks-to-update-the-controller).

## What is in it

| File | What it does | Change it when |
| --- | --- | --- |
| [`CMakeLists.txt`](CMakeLists.txt) | finds the library (west, `ZEPHYR_EXTRA_MODULES`, `modules/opensc2`, or the repository around it), names the project, lists the sources | you add source files |
| [`prj.conf`](prj.conf) | Kconfig for any board: the library, wired mode, storage for the pairing, the log and the shell | you add parts (ADC, PWM, I2C) or change options |
| [`boards/promicro_nrf52840_nrf52840_uf2.overlay`](boards/promicro_nrf52840_nrf52840_uf2.overlay) | the SuperMini's devicetree: the buttons, the pairing button, wired mode, the retained RAM, the peripherals that are switched off | you move or add buttons, or add parts |
| [`boards/promicro_nrf52840_nrf52840_uf2.conf`](boards/promicro_nrf52840_nrf52840_uf2.conf) | the SuperMini's own Kconfig: its clock (no 32.768 kHz crystal, so the RC oscillator) | you use another board |
| [`sysbuild.conf`](sysbuild.conf) | turns the Partition Manager off, so the build keeps the board's UF2 flash layout | never, on a UF2 board |
| [`src/main.c`](src/main.c) | joins the buttons and the LED to the library | your controller's logic |
| [`west.yml`](west.yml) | a west manifest, for a west workspace (Way 3) | you use west: set `self.path` to your folder's name |
| `.gitignore` | keeps `build/` out of git | |

## Pins and the LED

Wire each button between its pin and GND: A P1.15, B P1.13, X P1.11, Y P0.10, STEAM P1.04, and the
pairing button P0.31. These are the same pins as in
[app/](https://github.com/ded811/OpenSC2-Controller/blob/main/app/README.md#pins), so wiring carries
over. On the SuperMini's silkscreen "006" is P0.06, "111" is P1.11, and so on. Nothing needs to be
wired to try it.

The on-board LED (P0.15) shows the state. It is simpler than `app/`'s:

| LED | Means |
| --- | --- |
| on | wired, or wireless and connected through the puck |
| a slow blink | wireless, looking for the puck |
| a short blink every 2 s | wireless, no pairing yet: add it in Steam |
| off | pairing over the air, the radio off (the puck turns it off when the PC sleeps: press any button to come back), or the library did not start (see the console) |

## The pairing button

- Held while plugging in: this boot is wireless.
- Pressed and released while wired: the board reboots into wireless mode.
- Pressed while wireless: it answers an open pairing window (untested with Steam).

Pairing over the air (the puck's pairing window) is implemented but untested with Steam. Steam
pairs controllers over a wired connection, and pairing over the USB-C cable is the tested way.

## The serial console

In wireless mode the board has a USB serial console with the log and a shell: the USB serial port
with ID `2FE3:0004` and the product string "OpenSC2 console" (on Windows a new COM port; any
terminal, any baud rate). On a bare board, hold P0.31 against GND with a wire while plugging it in:
it starts wireless and the console appears. Right after Steam's pairing the board is wireless too.
[Getting started](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/getting-started.md#reach-the-shell)
shows how to reach it. `opensc2 status` shows the mode and the link; `opensc2 bonds` the pairings.

`opensc2 bonds` shows your puck's serial, and `opensc2 status` and `opensc2 id` show the board's
serials: mask them before you post them.

## What to change first

1. **Buttons.** Add a child to `opensc2_buttons` in the overlay, with a pin and a `zephyr,code`
   from `OPENSC2_KEY_*`
   ([inputs.h](https://github.com/ded811/OpenSC2-Controller/blob/main/include/dt-bindings/opensc2/inputs.h)):
   `button_r { gpios = <&gpio0 9 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>; zephyr,code = <OPENSC2_KEY_R>; };`.
   `src/main.c` needs no change for a button. The overlay also shows a digital trigger.
2. **More parts.** Sticks, analog triggers, trackpads, an IMU, rumble and an RGB LED: each sample
   shows one ([samples](https://github.com/ded811/OpenSC2-Controller/blob/main/samples/README.md)),
   and `app/` has them all. Copy the overlay nodes, the Kconfig lines and the code you need.
3. **Outputs.** `src/main.c` shows where a rumble callback goes; the
   [API reference](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/api.md) has every
   callback.
4. **The revision string Steam reads.** Uncomment `CONFIG_OPENSC2_GIT_REVISION` in `prj.conf` (1 to
   12 characters, no spaces).
5. **Another board.** Rename the two `boards/` files after your board target (`/` turned into `_`),
   change the pins, and check the retained RAM region against your bootloader
   ([porting](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/porting.md)).

The serials Steam sees are development serials made from the chip's ID ("OSC..."). To set your own
on the board: `opensc2 id set unit OSC2MYBOARD01`. Keep the OSC prefix: the hardware test suite
only talks to boards whose serial starts with OSC.

## The hardware test hooks

The template builds without them. The hardware test suite
([testing](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/testing.md)) was written for
and run against `app/`; with `CONFIG_OPENSC2_TEST_HOOKS=y` it may work with your firmware too, but
that has not been tried. Leave them off in a board you give to someone else.

---

Back to the [documentation map](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/README.md).
