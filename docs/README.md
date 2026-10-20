# Documentation map

Every page of OpenSC2's documentation, who it is for, and a glossary of the words the pages use:
start here if you are looking for something.

## Pages

| Page | For | What it covers |
| --- | --- | --- |
| [Getting started](getting-started.md) | first-time users | what you need, getting the code, installing the SDK, building on each OS, flashing, adding the board in Steam, the shell |
| [Troubleshooting](troubleshooting.md) | everyone | the update prompt, a board Steam does not see, a board that does not connect, lizard mode, the console, the UF2 drive, build errors, the test suite's refusals, what to share |
| [Use OpenSC2 in your own project](integration.md) | developers | the three ways to bring in the library, what every project needs, Kconfig, devicetree, the code, build problems |
| [API reference](api.md) | developers | every function, callback, field, mode rule, vendor command, setting, Kconfig option and shell command; power, chords, the status LED and cues |
| [Porting to other boards](porting.md) | people porting to other boards | what a board needs, the steps, wired mode, where the retained RAM may go, boards with more analog pins |
| [The protocol, in brief](protocol.md) | the curious | the radio link, the puck's end, the cable, the vendor commands, lizard mode, the reports, power and the status LED in short, links to the full research pages (not public yet), and every gap the library had to fill |
| [Testing on hardware](testing.md) | people changing the library, porting it, or checking a board | the automated hardware test suite: what it covers, its safety rules, how to run it, the results so far; [the Bluetooth build's procedure](testing.md#bluetooth) |
| [The test protocol](test-protocol.md) | people changing the test hooks or the runner | the test channel (vendor command `0x7E`), the system injection, the ring, the counters and the console's `opensc2 test` |
| [Releasing](releasing.md) | the maintainer | the checklist for the push, a release, making the repository public, and the day the research repository goes public |

Elsewhere in the repository:

| Page | What it covers |
| --- | --- |
| [README](../README.md) | what OpenSC2 is, what works, the quick start |
| [app/README.md](../app/README.md) | the SuperMini firmware: pins, modes, LED, shell, identity, options |
| [template/README.md](../template/README.md) | the project to copy as the start of your own |
| [samples/README.md](../samples/README.md) | the seven samples, their parts and pins |
| [tools/hwtest/README.md](../tools/hwtest/README.md) | the test suite's code, for people changing the runner |
| [CHANGELOG.md](../CHANGELOG.md), [CONTRIBUTING.md](../CONTRIBUTING.md), [SECURITY.md](../SECURITY.md) | changes, how to contribute, how to report a security problem |
| [NOTICE.md](../NOTICE.md), [THIRD_PARTY.md](../THIRD_PARTY.md), [LICENSE](../LICENSE) | trademarks and USB IDs, what a built image contains, the MIT licence |

## Glossary

**Puck.** Valve's puck: the small USB receiver and charging dock that comes with the Steam
Controller 2. Steam talks to wireless controllers through it. On USB it is `28DE:1304`. It has
slots of its own, one for each controller it is paired with.

**Wired mode.** The board is a USB Steam Controller 2 (`28DE:1302`) on its cable, and its radio is
off. Steam reads it like the real controller, and pairs it with the puck in this mode.

**Wireless mode.** The board talks to the puck over its 2.4 GHz radio (Nordic's Enhanced
ShockBurst). On USB it shows at most a debug serial port with other IDs (`2FE3:0004`), which has
the shell.

**Wireless transport.** What a wireless boot runs: the puck's radio with bond 1 or 2, or (in the
opt-in Bluetooth build, not yet tested on hardware) Bluetooth LE in slot 1 or 2, one at a time.
The choice is stored on the board, as the real controller's `user/wireless_transport`;
`opensc2 transport` shows and changes it ([api.md](api.md#wireless-transports-and-bluetooth-le)).

**Lizard mode.** Until Steam takes over, a Steam Controller 2 acts as a plain mouse and keyboard
(the right trackpad moves the pointer, A is Enter, B is Escape, the D-pad gives the arrow keys).
Steam switches it off while it runs; about 10 seconds after Steam stops, it comes back. OpenSC2
does the same.

**Bond and slot.** A bond is one pairing with one puck: 24 bytes with the puck's ID, the
controller's ID and the puck's serial. Like the real controller, the board keeps two, in two slots,
so it can be paired with two pucks. The shell numbers the two pairing slots 1 and 2; the API numbers
them 0 and 1.

**UF2.** The USB Flashing Format. The SuperMini's bootloader shows a USB drive (short RST to GND
twice quickly), and copying a `.uf2` file onto that drive flashes the firmware.

**Warm-restart mark.** A mark in RAM that start-up does not clear, set right before Steam's
"reboot into wireless mode". It makes the next boot wireless although USB power is there, and is
cleared first thing at that boot, so it never outlives one reboot.

**Build time.** When the firmware was built, in Unix seconds. The controller reports it to Steam
(attribute 4 of vendor command `0x83`), and Steam offers its update to a controller that reports an
older build time than the controller firmware Steam ships
([troubleshooting](troubleshooting.md#steam-asks-to-update-the-controller)).

**Vendor commands.** Valve's command channel in feature report 1: Steam reads the identity, sets
the LED and the settings, turns lizard mode off and pairs the controller with them. The same
commands travel on the cable and through the puck.

**Controller settings.** 86 numbered values Steam writes with vendor command `0x87`, in RAM only:
lizard mode is setting 9, the IMU stream setting 48.

**Test hooks.** An option of the library (`CONFIG_OPENSC2_TEST_HOOKS`, on in `app/`) with which the
PC test suite injects inputs and records outputs on a board with nothing wired. They do nothing
until the suite arms them, and arming ends at every reboot, but for a boot-time injection stored
for the next boot ([testing.md](testing.md), [SECURITY.md](../SECURITY.md)).

**USB session log.** What happened in a wired session, request by request, kept in RAM across a
reboot (not a power loss). In wireless mode, `opensc2 usb log` prints it.

**Mode policy.** The stored rule for boots with USB power: `auto` (the real controller's rule: USB
power at power-up means wired) or `wireless` (never wired). `opensc2 mode` shows and sets it.

**System state machine.** The real controller's rules for power and the status LED, which the
library runs (on by default): what a power-on does, STEAM held 7 s, the button chords that choose a
link, the keylock, the idle timeout, low battery and shutdown, the LED's mode and the cues
([api.md](api.md#power-and-the-system-state-machine)).

**Transport.** Where the controller's reports go when it is not wired: the puck with its first or
second pairing, or (on the real controller) a Bluetooth host. Chords such as A + right bumper
choose one.

**Keylock.** A travel lock: STEAM + L4 + L5 + R4 + R5 held 3 s on battery makes the next power-on on
battery turn straight off, and every one after it, until the board powers up on USB or the dock,
or the chord is tapped.

**Cue.** A short chime the real controller plays on its haptics (power on, power off, a transport,
pairing). The library raises cue events; the haptics synthesiser, when it is built in, plays
OpenSC2's own sound for each, otherwise the application does.

**The dock.** The puck's other job: a controller resting on it charges through three pogo pads,
`+`, `−` and `P`, and the puck talks to it over a slow serial link on the same contacts, in 73-byte
frames. `P` carries the **pilot**, a level that tells the puck a controller is there. OpenSC2 has
the controller's end of the protocol; its UART build has never run on hardware
([api.md](api.md#the-dock)).

**The blank canvas.** The firmware in `app/`: every input of a Steam Controller 2 set up in one
devicetree overlay, so you wire only what you have. The SuperMini's pins carry the buttons, the
digital triggers, one stick and the pairing button; the other inputs are listed in the overlay,
ready for a board with more pins.

**Board target.** Zephyr's name for a board and its variant. The SuperMini with its UF2 bootloader
is `promicro_nrf52840/nrf52840/uf2`.

**Zephyr module.** How a Zephyr project brings in code from outside its own folder. The
OpenSC2-Controller repository is one ([integration.md](integration.md)).

Back to the [README](../README.md).
