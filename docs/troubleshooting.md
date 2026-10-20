# Troubleshooting

Answers to the problems people run into first, symptom by symptom: for everyone who builds,
flashes or adds a board.

| Symptom | Section |
| --- | --- |
| Steam says the controller needs an update | [Steam asks to update the controller](#steam-asks-to-update-the-controller) |
| Steam does not see the board on the cable | [Steam does not see the board](#steam-does-not-see-the-board) |
| The LED blinks slowly and never stays on | [The LED blinks slowly](#the-led-blinks-slowly-and-never-stays-on) |
| The LED is on with a short dark blink every second | [Wireless is off, or no PC](#the-led-is-on-with-a-short-dark-blink-every-second) |
| The pointer moves or keys are typed while Steam is closed | [Lizard mode](#the-board-types-or-moves-the-pointer-when-steam-is-closed) |
| No COM port, no shell | [I cannot find the console](#i-cannot-find-the-console) |
| No USB drive after shorting RST | [The UF2 drive does not appear](#the-uf2-drive-does-not-appear) |
| The build stops with an error | [The build fails](#the-build-fails) |
| The test suite stops with exit code 2 or 3 | [The hardware test suite refuses my board](#the-hardware-test-suite-refuses-my-board) |
| A real controller as well | [Can I keep using my real controller?](#can-i-keep-using-my-real-controller) |
| Worried about the puck | [Does any of this change my puck?](#does-any-of-this-change-my-puck) |
| Posting a log | [What can I share in an issue?](#what-can-i-share-in-an-issue) |

The LED patterns below are those of the firmware in `app/` ([app/README.md](../app/README.md#led));
the samples and the template describe their own.

## Steam asks to update the controller

**Steam says the controller needs an update.** Steam offers its controller firmware update to a
controller that reports an older build time than the controller firmware Steam ships. The prompt
goes away when the board reports a build time at least as new as Steam's current controller
firmware (tested). A fresh build reports the time it was built, so it is newer than any controller
firmware Steam had shipped by then; a prebuilt release reports its own build time. Once Steam ships
a newer controller firmware, give the board Steam's value: on Windows, with the board in wireless
mode and no terminal open on its console, run `python -I tools\match_steam_version.py --dry-run`
(shows what it would do: it reads Steam's `hardwareupdater.cfg` and finds the board's console, but
opens no device and sends nothing), then `python -I tools\match_steam_version.py`, then reboot the
board. Declining the prompt is always safe. If you accept it by mistake, the board
ignores Steam's request to enter a bootloader, so the update cannot start and the firmware stays as
it is (by design; not tried with Steam).

Some numbers, for reference:

| Build time | What it is |
| --- | --- |
| `0x6AC686B3` | Steam's October 2026 controller firmware |
| `0x6A628345` | the July 2026 controller firmware: a board reporting it got the prompt against the October release |
| `0x6A18D057` or older | Steam forces an update: the build refuses such a default, and the shell stores one only with `force` |

`opensc2 id` shows the build time the board reports (`build 0x...`). More about the tool:
[app/README.md](../app/README.md#steams-firmware-update).

## Steam does not see the board

1. **The cable.** A charge-only cable powers the board but carries no data. Use a USB-C data cable.
2. **The mode.** The board is wired only at a power-up with USB power, unless something sends it
   wireless: the pairing button held while plugging in, or Steam's reboot into wireless mode right
   before. Unplug it, check that nothing holds P0.31 against GND, and plug it in again. Wired, the
   LED turns on once the PC has set the board up (until then it shows a short dark blink every
   second; the v0.2.0 image blinks fast instead). A stored policy
   can keep it wireless for good: `opensc2 mode` in the shell shows it, `opensc2 mode auto` restores
   the normal rule.
3. **Windows.** In Device Manager the board shows up under Human Interface Devices. In its
   Properties, the Details tab, the property Hardware Ids lists `VID_28DE&PID_1302`.
4. **The session log.** Each wired session is logged in RAM, request by request. Afterwards, in
   wireless mode, `opensc2 usb log` shows what the PC and Steam asked and what the board answered
   ([Reach the shell](getting-started.md#reach-the-shell)).

## The LED blinks slowly and never stays on

The board is wireless and looks for its puck, but the puck does not connect it.

- **The puck is unplugged**, or plugged into another PC. Plug it into the PC with Steam.
- **The board is not paired with this puck.** Add it in Steam again
  ([Add it in Steam](getting-started.md#add-it-in-steam)).
- **The pairing is not there.** In the shell, `opensc2 bonds` lists the two pairing slots (1 and
  2) and which one is in use; an empty slot in use means "pair first" (the LED then blinks the same
  way; the v0.2.0 image gives a short blink every 2 seconds instead).

`opensc2 bonds` shows your puck's serial: mask it before you post the output.

## The LED is on with a short dark blink every second

That is the real controller's amber mode, on the SuperMini's one-colour LED (built; not yet tested
on hardware; the v0.2.0 image does not have it):

- **Wired, but no PC has set the board up**: a USB charger or a power bank. STEAM or the pairing
  button sends it wireless.
- **Wireless switched off on USB power**: the PC went to sleep (the puck turned the board off),
  Steam turned the controller off, or STEAM was held 7 s. A STEAM press reboots the board, which
  comes back wired on a PC; `opensc2 start` in the shell turns wireless on again
  ([api.md](api.md#the-states-while-it-runs)).

## The board types or moves the pointer when Steam is closed

That is lizard mode, as on the real controller. Lizard mode: until Steam takes over, a Steam
Controller 2 acts as a plain mouse and keyboard (the right trackpad moves the pointer, A is Enter,
B is Escape, the D-pad gives the arrow keys). Steam switches it off while it runs; about 10 seconds
after Steam stops, it comes back. OpenSC2 does the same.

So a board whose inputs are not at rest (a stick that does not centre, a button wired the wrong way
round) types or moves the pointer whenever Steam is closed. Check the wiring with `opensc2 state`,
which prints the inputs as the board reads them. A build can also leave lizard mode out
(`CONFIG_OPENSC2_LIZARD_MODE=n`), unlike the real controller: [api.md](api.md#lizard-mode).

## I cannot find the console

The board has a USB serial console with a shell only in wireless mode. On a bare board, hold P0.31
against GND with a wire while plugging it in: it starts wireless and the console appears. Right
after Steam's pairing the board is wireless too.

It is the USB serial port with ID `2FE3:0004` and the product string "OpenSC2 console". On Windows
it is a new COM port under Ports (COM & LPT) in Device Manager. Open it with any serial terminal
(for example PuTTY, Tera Term or the nRF Connect Serial Terminal); the baud rate does not matter.
On Linux it is usually `/dev/ttyACM0`, on macOS `/dev/cu.usbmodem…` (Linux and macOS untested).

A build with `CONFIG_OPENSC2_USB_CONSOLE=n` has no console. Only one program can hold the port: close
the terminal before you run `tools/hwtest` or `tools/match_steam_version.py`.

## The UF2 drive does not appear

- **The timing.** Short RST to GND twice quickly, about a quarter of a second apart. One short only
  restarts the firmware.
- **From the shell.** In wireless mode, `uf2` reboots the board into its bootloader (in `app/`).
- **The bootloader.** The build is for the Adafruit nRF52 UF2 bootloader with the S140 v6 layout
  (the application at `0x26000`, the settings in the storage partition at `0xEC000`-`0xF4000`). The
  drive's `INFO_UF2.TXT` names the bootloader and its version. A board with another bootloader
  needs another board target: [porting.md](porting.md).

## The build fails

| Message | Fix |
| --- | --- |
| an error about a long path, or files not found deep in the build folder (Windows) | use a short build folder: `-OutDir C:\b\osc2` |
| `Partition Manager output found (partitions.yml)` | the project needs a `sysbuild.conf` with `SB_CONFIG_PARTITION_MANAGER=n`; build again with `-Pristine` |
| `OpenSC2 needs the nRF Connect SDK v3.4.0` | build with the nRF Connect SDK, not plain Zephyr |
| `No toolchain index`, `No toolchain for v3.4.0` or `SDK not found` | install nRF Connect SDK v3.4.0, or pass `-NcsRoot` with the folder it is in |

More, for your own projects: [integration.md](integration.md#build-problems).

## The hardware test suite refuses my board

The suite ([testing.md](testing.md)) only talks to what it is sure is your board, and stops with an
exit code when it is not:

| Why | What to do |
| --- | --- |
| The board's serial does not start with `OSC` | keep the OSC prefix: `opensc2 id set unit OSC2MYBOARD01`, or `opensc2 id clear` for the built-in one |
| The console has other USB IDs or another product string | the suite talks only to `2FE3:0004` with the product string "OpenSC2 console": build with the default `CONFIG_OPENSC2_USB_CONSOLE_*` values |
| A terminal holds the console | close it |
| Steam is open | close Steam, or let the suite do it with `--steam-control` |
| Two pucks are plugged in | unplug one: the suite cannot tell their slots apart |
| Another program reads the board's or a slot's collection | close it (a game, a controller-mapping tool, a browser tab using WebHID) |
| Not Windows (exit code 3) | hardware runs need Windows; `--selftest` and `--dry-run` run anywhere |

The exit codes in full: [testing.md](testing.md#how-to-read-the-report).

## Can I keep using my real controller?

Valve's puck has room for several controllers; using the board and a real controller at the same
time has not been tested; switch the real one off while you add the board.

## Does any of this change my puck?

OpenSC2 never changes the puck's firmware. When Steam adds the board it writes a pairing into one of
the puck's slots, as for a real controller; forgetting the controller in Steam clears it. The test
suite only reads from the puck unless you pass `--through-puck`, and even then never sends the
puck's own commands (feature report 2).

## What can I share in an issue?

`opensc2 bonds` shows your puck's serial, and `opensc2 status` and `opensc2 id` show the board's
serials: mask them before you post them. `opensc2 usb log` and the test suite's reports are masked
and safe to share.

Never post your Steam ID, your user folder or unmasked lines of Steam's own logs. The issue forms
ask for what helps: the board, the OpenSC2 version, the build command, the puck firmware and what
happened.

Back to the [documentation map](README.md).
