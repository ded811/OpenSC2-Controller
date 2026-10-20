# The test protocol

The bytes between OpenSC2's hardware test hooks and the PC test suite, for people who change the
hooks or the runner: the test channel, the ring and the console. You don't need this page to use
the firmware or the library.

What the hooks do and the safety rules the firmware enforces:
[api.md](api.md#hardware-test-hooks). How the suite uses them: [testing.md](testing.md). The host
tests and the runner's self-tests check both sides against the same vectors, in
`tests/vectors/hwtest/`.

- [The test channel: vendor command `0x7E`](#the-test-channel-vendor-command-0x7e)
- [The ring](#the-ring)
- [The console: `opensc2 test`](#the-console-opensc2-test)

## The test channel: vendor command `0x7E`

One binary protocol, the same bytes on every transport:

| Transport | Request | Answer |
| --- | --- | --- |
| USB (wired) | `SET_REPORT(Feature 1)`: `01 7E <n> <request>`, zero padded to 64 bytes | the next `GET_REPORT(Feature 1)`: `01 7E <m> <answer>` |
| the radio, through the puck | the puck's feature report 1, the same bytes (a SET record) | the puck's GET (retried until the answer is there) |
| Bluetooth (the opt-in Bluetooth build; the suite's path B, `--bluetooth`) | a write of feature report 1's characteristic: `7E <n> <request>`, without the report ID | the next read of it: `7E <m> <answer>`, 63 bytes |
| the console (wireless) | `opensc2 test <id> raw <request as hex>` | `HWT <id> RAW <answer as hex>` |

`0x7E` is no command of the real controller, of Steam or of SDL (theirs start at `0x80`).

Request (n = 7 + the arguments, at most 61): key u32 `1E AB 57 7E`, protocol version u8 (1), request
ID u8 (echoed), op u8, arguments. Answer (m = 4 + the result, at most 61): version u8, request ID u8,
op u8, status u8 (0 OK, 1 BAD_OP, 2 BAD_LEN, 3 NOT_ARMED, 4 BAD_ARG, 5 BUSY, 6 DENIED, 7 NO_LINK),
result (empty unless OK). The checks run in that order: op, argument length, armed (the console
only), argument values, state, policy; the first that fails gives the status.

**Claiming (USB, the radio and Bluetooth).** A `0x7E` SET is the test channel only when n ≥ 7, the
key and the version match, and the board is armed or the op is `ARM`. Such a request is claimed: the
SET succeeds, the op runs at once, and its answer is held for that link; a failing op still succeeds
as a SET and reports its status in the answer. Anything else with opcode `0x7E` takes the usual path
(the application's `vendor_command`; without one a STALL on USB, an error result on the radio and an
ATT error over Bluetooth). A GET of
`0x7E` returns the link's held answer and never runs the op again, so repeated GETs are harmless. The
held answer lasts until the next successful SET on that link (claimed or not) replaces the stored
request; a failed SET keeps it. The end of arming drops every link's held answer, except that a
`DISARM` then holds its own. With no held answer a GET takes the usual path. Each claimed request
refreshes the lease. Claimed requests are counted in the USB session log but not logged line by line.
The dock never carries the test channel: a `0x7E` SET in a dock frame (the puck passes a PC's
feature report on to a docked controller) is never claimed and never reaches the application; the
dock answers it -22, as an opcode nobody knows. The dock's own ops (`DOCK_RUN` and the others) are
the way to test the dock.

**The console** has nothing to fall through to: a `raw` request shorter than 7 bytes or with the wrong
key or version prints `HWT <id> ERR not a test request` and `HWT <id> END -22`; every other request
runs, and while unarmed every op but `ARM`, `STATUS` and `SYS_STATUS` answers NOT_ARMED.

Ops (arguments and results little endian; state52 is report `0x42` without its ID and sequence byte):

| Op | Name | Arguments | Result | Notes |
| --- | --- | --- | --- | --- |
| `0x01` | ARM | nonce u32, lease s u16 (0: 15; at most 600), flags u8 (bit 0: restore the settings and the LED colour at the end) | caps, 36 bytes | a fresh start: the ring, the counters, injection, the battery and the sequence reset; BAD_ARG for a lease above 600 or another flag bit |
| `0x02` | DISARM | | | the end of arming |
| `0x03` | STATUS | | status, 32 bytes | also the lease's keep-alive |
| `0x10` | INJECT | flags u8 (bit 0: stop a running sequence first; bit 1: the lizard test), state52 | frames u32 | the static injected state; DENIED while lizard mode is on without bit 1 |
| `0x11` | INJECT_OFF | | frames u32 | also stops a sequence |
| `0x12` | BATTERY | charge state u8, level u8, battery, system, input mV u16, current s16, input current u16, temperature u16, gauge flags u8 (kept in the battery the library holds; no report carries it), flags u8 (bit 0: send a `0x43` now; the 3.5 s period restarts from it) | | |
| `0x13` | BATTERY_OFF | | | |
| `0x14` | SEQ_LOAD | step u8 (below the depth), hold u8 (1..255 frames), state52 | | BUSY while a sequence runs |
| `0x15` | SEQ_RUN | count u8 (1..depth; steps 0..count-1 loaded since ARM), flags u8 (bit 0 loop, bit 1 keep the last step as the static state, bit 2 the lizard test) | frames u32 | starts with the next frame built; BAD_ARG for a step not loaded, BUSY while one runs, DENIED while lizard mode is on without bit 2 |
| `0x16` | SEQ_STOP | | | `SEQ_END` how 2 |
| `0x17` | RULES | | the rule word in effect u32, the build's u32, the bits this firmware knows u32 | the rule word: bit 0 the trigger bits (setting 68), 1 no trigger bit with raw triggers (62), 2 the stick-touch fallback, 3 setting 82 in `0x42`, 4 the pads' clicks by pressure (52, 53); 8 lizard mode's stabilisers, 9 its 8-frame average, 10 its glide, 11 its ticks and clicks ([api.md](api.md#the-rules-on-every-report)) |
| `0x18` | RULES_SET | the rule word u32 | the word in effect u32 | until the end of arming or the next `ARM`, which put the build's back; lizard mode resyncs (a `LIZARD` entry, what 4); BAD_ARG for a bit RULES does not list as known |
| `0x20` | RING_READ | from u32, max entries u8 (0: as many as fit) | first u32, next u32, count u8, then that many whole entries from max(from, first), at most 48 bytes | BAD_ARG when from > next |
| `0x21` | RING_CLEAR | | next u32 | sequence numbers go on |
| `0x22` | CNT_READ | first u8, count u8 (1..13) | first u8, count u8, u32 × count | clipped at the end of the table |
| `0x23` | CNT_RESET | | | |
| `0x24` | MARK | marker u32 | the entry's sequence number u32 | |
| `0x30` | SETTINGS_READ | first u8, count u8 (1..27) | first u8, count u8, s16 × count | clipped at 86 |
| `0x40` | HOST_OUT | an output report, ID first, 1..54 bytes | the decode result s8 | as from a host, on the test link |
| `0x41` | HOST_SET | opcode u8, n u8, n payload bytes (n = the arguments − 2) | result s32 | a vendor SET on the test link; only `0x81`, `0x83`, `0x85`, `0x87`, `0x89`, `0x8E`, `0x9F`, `0xA1`, `0xAE`, `0xBE`, `0xC5`, `0xDB`, `0xE9`, `0xED`, `0xF2` with any selector but 1 (its GET marks a new crash record seen, a flash write), and `0x95` with the turn-off key `0xABDF9710` (read as the command reads it: the first four payload bytes, zero padded) run; everything else answers DENIED |
| `0x42` | HOST_GET | | opcode u8, n u8, flags u8 (bit 0 masked, bit 1 cut), then the first min(n, 54) answer bytes, or for a masked answer the CRC-32 of its n bytes | a vendor GET on the test link; masked: `0xED` of `esb/bond` or `esb/bond_2`, `0xAE` tags 0, 1 and 4, `0xF2` selector 0, `0xA1` selector 1 |
| `0x43` | APP_SEND | kind u8; kind 0: an input report, ID first, 1..53 bytes; kind 1: channel u8, flags u32 | result s32 | as the application would (`opensc2_send_report()`, `opensc2_send_haptic_status()`), through the guard like every report; DENIED for a `0x42` |
| `0x44` | RADIO_DEAF | ms u16 (100..5000) | | the radio hears nothing that long, so the link times out the normal way; BUSY while one is pending, NO_LINK unless wireless and connected |
| `0x60` | BLE_STATUS | | the Bluetooth link, 28 bytes (below) | BAD_OP in a build without Bluetooth |
| `0x61` | BLE_DROP | HCI reason u8 (not 0) | | the board ends the Bluetooth link 100 ms after the answer and then acts as if the host had ended it with this reason: `0x13` or `0x15` turn wireless off, any other advertises again 500 ms later; NO_LINK unless connected over Bluetooth; BAD_OP without Bluetooth |
| `0x62` | BLE_WINDOW | ms u32 (100..600000, or 0) | | the next advertising window lasts this long instead of the configured one (`CONFIG_OPENSC2_BLE_ADV_WINDOW_S`), once; 0 drops a pending one; BAD_OP without Bluetooth |
| `0x70` | SYS_INJECT | flags u8 (bit 0 buttons, 1 level, 2 power source), buttons u32, level u8, source u8 (0 battery, 1 USB, 2 the dock), zero u8 | | [system injection](#system-injection): what the flags name reaches the system state machine's rules from now on (the others stay as they are); BAD_ARG for another flag bit, a source above 2 or a last byte that is not 0 |
| `0x71` | SYS_INJECT_OFF | | | ends every system injection, the boot-time one too |
| `0x72` | SYS_STATUS | | the machine's status, 30 bytes | [below](#system-injection); also unarmed on the console |
| `0x73` | SYS_BOOT | flags u8, buttons u32, level u8, source u8, hold u8 (× 100 ms; 0: until the next `ARM` or `SYS_INJECT_OFF`) | | keeps a boot-time injection for the next boot, in the retained RAM area (flags 0 clears it); DENIED without that area; BAD_ARG for another flag bit or a source above 2 |
| `0x74` | SYS_EVENT | kind u8: 1 a link that gave up, 2 the puck's "PC asleep" | | as the radio would report it: 1 posts "wireless off" (with the listen timeout as its reason), 2 runs the `0xE6` path (the `host_asleep` callback, a `HOST_ASLEEP` entry, the once-per-boot cue); BAD_ARG for another kind |
| `0x80` | HAP_RESET | seed u32 | | the haptics synthesiser (below): everything stops at once, the queues and stream rings empty, the formats forgotten, the scripts and the drop sounds idle, captures off, the rumble's random burst gains restarted from the seed (5323 = `0x14CB` at boot) |
| `0x81` | HAP_CAPTURE | path u8 (0 pads, 1 grips, 2 both), blocks u8 (0 stops) | | a `HAP_BLOCK` entry for each of the next blocks the path renders; BAD_ARG for a path above 2 |
| `0x82` | HAP_STATUS | | status, 38 bytes (below) | |
| `0x83` | HAP_PLAY | what u8; 0 cue: cue u8, level u8; 1 click: channels u8, click u8 (1 A, 2 B), gain s8; 2 script: channels u8, id u8, gain s8; 3 free fall: the random byte its drop sound will use u8; 4 stop: channels u8 | result s32 | the synthesiser's own sounds, as the application or the library would play them: how many grips a cue started on (-1: it plays nothing at that level), how many channels were queued or started, 1 when a free fall was taken (0: not armed); BAD_ARG for another `what` |
| `0x90` | DOCK_LOAD | offset u8, then 1..53 bytes | | part of a 73-byte [dock frame](api.md#the-dock-link) to hand to the dock link; BAD_ARG past byte 73. The suite loads 53 bytes at 0 and 20 at 53 |
| `0x91` | DOCK_RUN | length u8 (0..73), flags u8 (bit 0: start a session when none runs) | outcome u8 (0 answered, 1 too short: the session ends, 2 bad CRC: the session ends, 3 nothing arrived: the session goes on), session u8 (1 still running), then the answer's start byte u8, type u8, result s32, flags u8 (bit 0 its CRC checks, bit 1 bytes masked), CRC-32 of the whole answer u32, CRC-32 of the masked bytes in order u32 (0 if none), the handling time µs u32 (21 bytes) | the first `length` loaded bytes as if they had arrived from the puck. NO_LINK when no session runs and bit 0 is clear; DENIED for a SET (type 2) of 73 bytes whose opcode `HOST_SET` would refuse, before anything runs. A session the hooks start ends with their arming |
| `0x92` | DOCK_READ | offset u8, count u8 (1..55) | offset u8, count u8, the answer's bytes | the last answer; masked bytes read as zeros: a probe's two serials (bytes 24 to 51), the data of a GET answer `HOST_GET` would mask. BUSY before any answer |
| `0x93` | DOCK_CTRL | what u8, then by `what`: 0 STATUS; 1 END; 2 PILOT: three readings mV u16 (`0xFFFF` a failed conversion); 3 SUPPLY: usb, plus, pilot u8 (0 or 1), entry u8 (how the controller comes onto the dock while outside the dock states: 0 at power-on, 1 in the battery wait, 2 on battery), warm u8; 4 EVENT: event u8, chord u8; 5 RESET | STATUS: flags u8 (bit 0 a session runs, 1 the dock is the only link, 2 the hooks' session, 3 built with the UART), the last session end u8, sessions u32, frames u32, errors u32, the slowest handling µs u32, the docked state u8, the supply u8, the dock's stored request's opcode u8, a report copied u8 (22 bytes); PILOT: present u8, inside u8 (a bit per reading), session u8; SUPPLY and EVENT: the event posted u8 (`0xFF` none), from u8, to u8, handled u8, actions u32 | END ends a session; PILOT runs the check between sessions (one that passes starts a session); SUPPLY and EVENT step the supply rule and the docked states on these values only, and record each step; nothing they decide is done. Numbers: [proto/dock.py](../tools/hwtest/proto/dock.py) |
| `0xA0` | RAW_INJECT | flags u8 (0), nonce u32, present u8 (bit 0 left stick, 1 right stick, 2 left trigger, 3 right trigger, 4 left pressure, 5 right pressure, 6 left and 7 right stick-touch and grip words), sticks LX, LY, RX, RY u16, triggers L, R u16, pressure L, R s16, stick touch L, R s16, grip L, R s16 (30 bytes) | frames u32 | builds with `CONFIG_OPENSC2_CAL` only (caps bit 21; BAD_OP without). The inputs in `present` replace the application's raw inputs before the library processes them ([api.md, Calibration and raw inputs](api.md#calibration-and-raw-inputs)), as `opensc2_set_raw()` would give them; a nonce other than 0 goes into the IMU timestamp of every frame built with them, so the reports that carry them are known. DENIED while lizard mode is on; BAD_ARG for a flag bit; frames as for `INJECT` |
| `0xA1` | RAW_OFF | | frames u32 | the application's raw inputs again |
| `0xA2` | CAL_STATUS | | 16 bytes: generation u32 (`opensc2_cal_generation()`); usable u8 (bit 0 left stick, 1 right stick, 2 left trigger, 3 right trigger, 4 left pressure, 5 right pressure: a stored calibration that the processing can use); raw u8 (the raw inputs the last frame processed, RAW_INJECT's bits); injected u8 (RAW_INJECT's `present`, 0 when off); flags u8 (bit 0 `CONFIG_OPENSC2_PAD_RULES`, bit 1 `CONFIG_OPENSC2_CAL_PRESSURE_CLICK`); the recordings' steps u8 × 3 (sticks `0xD8`, triggers `0xC0`, pressure `0xC3`; bit 7 while one runs); the pads' pressure filters s16 × 2 (left, right); clicks u8 (bit 0 left, bit 1 right: the pressure's click state) | reads only; also through the puck |

A raw injection is an injection: everything that stops `INJECT` stops it too (`DISARM`, the end of
the lease, lizard mode coming on: `INJECT_STOP` with `what` bit 2), and `INJECT`, applied after the
library's processing, replaces whatever the raw inputs became. Frames built with it are kept away
from lizard mode, as injected ones are.

Ops `0x70` to `0x74` answer DENIED in a build without the system state machine
(`CONFIG_OPENSC2_SYSTEM=n`, caps build flag 18 clear); `0x75` to `0x77` are kept for it and answer
BAD_OP. The dock ops `0x90` to `0x93` answer BAD_OP in a build without the dock link (caps build
flag 20 clear).

The **test link** is a third command link of the library with a stored request of its own, used only
by `HOST_SET` and `HOST_GET`, so emulating a host on the board never disturbs the stored request of
USB or the radio. It counts as active exactly when the mode's real link is (USB configured while
wired; while wireless the running transport's link: the radio connected, or Bluetooth past its
gate).

**Frames.** A frame is one state report built. Its index counts from 0, the first frame after `ARM`
(`CNT_RESET` does not restart it). `INJECT`, `INJECT_OFF` and `SEQ_RUN` answer the index of the next
frame at the moment of the request, f: the change shows from frame f or f + 1 (frame f may be half
built). The injected state replaces the application's after the `sample` callback, never as the
application's own state: `opensc2_get_state()`, the shell's `opensc2 state` and the rules that read
STEAM never see it. The library then applies its rules (the rule word's: the stick-touch fallback,
the click by pressure, setting 82, the trigger bits) and encodes the report as always.

Caps (the ARM result, 36 bytes): 0 version (1); 1 mode (0 wireless, 1 wired); 2 build flags u32
(bit 0 `TRIGGER_CLICK_AUTO`, 1 `LIZARD_MODE`, 2 `STEAM_WATCHDOG`, 3 `USB`, 4 `USB_PROFILE_OCTOBER`,
5 `USB_CONSOLE`, 6 `USB_WIRED_CONSOLE`, 7 `IDENTITY_VENDOR_WRITE`, 8 `STORAGE`, 9 `USB_LOG`, 10
`USB_REMOTE_WAKEUP`, 11 `USB_PLUG_IN_REBOOT`, 12 `SHELL`, 13 `FUSION`, each `CONFIG_OPENSC2_*`; 14
`NO_HOST_WIRELESS_MS` not 0, the charger test build; 15 the rule word, ops `RULES` and `RULES_SET`:
without it the firmware has OpenSC2 0.2.0's rules, the trigger bits only and none of lizard mode's
processing; 16 `HAPTIC_STOP_STATUS`, the `0x44` replies to stops; 17 `BLE`, the Bluetooth build; 18
`SYSTEM`, the system state machine and its ops; 19 `HAPTICS_SYNTH`, the haptics synthesiser; 20
`DOCK_LINK`, the dock link and its ops; 21 `CAL`, the library's calibration and raw inputs with
`RAW_INJECT`; bits 22 to 31 are left for other options); 6 battery report interval ms u16; 8
rumble timeout ms u16; 10 ring size u16; 12 sequence depth; 13 counters (27); 14 radio report
delay µs u16; 16 the
lease in effect s u16; 18 the stored mode policy (0 auto, 1 wireless); 19 zero; 20 the git
revision, 12 bytes NUL padded; 32 the CRC-32 of the identity's product name u32 (the name itself
does not fit; the suite checks the USB product string against it). Firmware before the product name
sent the first 32 bytes only.

Status (the STATUS result): 0 flags (bit 0 armed, 1 guard, 2 static injection, 3 battery injected, 4
sequence running, 5 lizard mode on, 6 the end of a disarm in progress, 7 a settings snapshot held); 1
mode; 2 mode reason; 3 link state (of the running wireless transport); 4 USB state; 5 bond slot (0
or 1); 6 the last radio disconnect
reason; 7 the sequence byte of the last state report; 8 nonce u32; 12 uptime ms u32; 16 the ring's
first u32; 20 next u32; 24 frames since ARM u32; 28 the step in use (`0xFF` idle); 29 steps loaded;
30 frames left in the step u16.

BLE_STATUS (the `0x60` result): 0 flags (bit 0 the Bluetooth stack on, 1 advertising, 2 connected, 3
active: past the gate, the reports flow, 4 the reports paced by the connection-event trigger (else
a timer), 5 Bluetooth the running wireless transport, 6 a pairing request pending, 7 wireless off);
1 the advertising form (0 pairing mode, 1 allow-listed for the bonded host, 2 directed to it,
`0xFF` none); 2 the slot (0 or 1); 3 the identity in use (1 and 2 slot 1, 3 and 4 slot 2); 4 the
identities with a bond (bit n for identity n); 5 identities that exist (5 once Bluetooth has run:
0, never used, and 1 to 4); 6 the link's security level; 7 the HCI reason of the last disconnect (0
none yet); 8 the link state (`enum opensc2_link_state`); 9 its reason (`enum opensc2_reason`); 10
the ATT MTU u16; 12 the connection interval µs u32; 16 ms left of the advertising window u32 (0 when
none runs); 20 state reports sent u32 and 24 dropped u32 (since the connection). No name and no
address: those are on the console (`opensc2 ble status`), which the suite masks.

### System injection

The ops `0x70` to `0x74` reach the [system state machine](api.md#power-and-the-system-state-machine).
`INJECT` replaces only what the state reports carry, and the rules that read STEAM never see it;
`SYS_INJECT` is the other way round: its buttons, battery level and power source reach the machine's
rules (the chords, STEAM pressed and held, the idle timeout, the battery level, the power source,
the wired start's STEAM rule and the plug-in rule) and never a report, so lizard mode never sees
them either. A level injected this way stands for the application's (the `0x80` rule still
applies); a battery injected with `BATTERY` only changes the reports. The end of arming (and `ARM`)
ends `SYS_INJECT`'s inputs, except in a boot that a boot-time injection started, where the injected
level and source stay (below).

**The boot-time injection.** `SYS_BOOT` keeps its inputs in the retained RAM area for the next boot
(three words: `0x5B007A5E`, the buttons, then flags | level << 8 | source << 16 | hold << 24). The
next boot reads and clears them first thing and injects them from the start, so the power-on
decision itself sees them: a source of 0 (battery) or 2 (the dock) makes a boot with USB power a
battery or dock power-on, not a wired one. They outlive the end of arming: the source and the
level stay for that boot, the buttons until their hold time ends (then every injected button is
up; with a hold of 0, until the next `ARM`); `SYS_INJECT_OFF` ends them all. While the hooks are armed, and in a boot a boot-time
injection started, a shutdown never turns the board off: it reboots (how 4 below).

**The last end.** When the machine ends (a power-off or a reboot) it keeps how in the retained RAM
area, three words (`0x5E0DE4D5`, state | cause << 8 | end << 16 | how << 24, the uptime in ms), so
the next boot's `SYS_STATUS` can show what the last boot did. How: 1 `OPENSC2_POWER_OFF` asked for,
2 a reboot state or setting 77, 3 a power-off that rebooted because the build has no battery, 4 one
that rebooted because the test hooks were in use.

SYS_STATUS (30 bytes): 0 state, 1 the chosen transport (`0xFF` none), 2 the power source, 3 the LED
mode, 4 the LED flags (bit 0 external power, 1 steady, 2 shutting down), 5 the chord matching now
(0 none, 1 STEAM alone, 2 B + R, 3 B + L, 4 A + R, 5 A + L, 6 the keylock), 6 the level the machine
was told (255: none), 7 flags (bit 0 the keylock stored, 1 buttons injected, 2 a level injected, 3
a source injected, 4 this boot came from a boot-time injection, 5 a Bluetooth transport exists, 6 a
battery build, 7 the machine has ended), 8 the idle history u16, 10 the idle period ms u32, 14 the
last cue (`0xFF` none), 15 the state the power-on decision chose, 16 its cue (`0xFF` none), 17 the
chord held at power-on, 18 1 when a power-on chord state ended with the pairing flag, 19 1 when the
last end is valid, 20 its state, 21 its cause, 22 its how, 23 its uptime ms u32, 27 the
fully-charged LED level, 28 setting 50 as the idle monitor uses it, s16.

States (byte 0, the `SYS_STATE` and `SYS_END` entries): 0 `start`, 1 `battery-wait`, 2 `wired-wait`,
3 `wired`, 4 `usb-link-on`, 5 `usb-link-off`, 6 `dock-off`, 7 `lifted`, 8 `dock-chord`, 9 `dock-on`,
10 `chord`, 11 `battery`, 12 `shutdown`, 13 `shutdown-silent`, 14 `shutdown-low`, 15
`shutdown-locked`, 16 `reboot`. Events (the causes): 0 `power-none`, 1 `power-usb`, 2 `power-dock`,
3 `configured`, 4 `steam-held`, 5 `steam-pressed`, 6 `idle`, 7 `wireless-off`, 8 `level`, 9
`keylock-held`, 10 `chord-down`, 11 `chord-up`, 12 `timer`, 13 `bootloader`, 14 `reboot`, 15
`warm-reboot`, 16 `turn-off`, 17 `steam-up`, 18 `link-started`, 19 `unplugged`.

**The haptics synthesiser** (`CONFIG_OPENSC2_HAPTICS_SYNTH`, caps bit 19; without it the ops
`0x80`..`0x83` answer BAD_OP once their length is right). Its ring entries (`0x60`..`0x63`, link 3)
record what its threads, its script players and the stream writes did, while armed. Its blocks are
rendered in integers, so a block is bit for bit the same on the board and on a PC: the CRCs of
`HAP_BLOCK` are compared with the shared vectors in `tests/vectors/hwtest/haptics.txt`. HAP_STATUS
(38 bytes): 0 features (bit 0 the drop sounds built); 1 bit per path where something plays (bit 0
pads, 1 grips); 2 bit per path with an output of the application's; 3 the amplifier mode (setting
75); 4 the master gain s8 (setting 76); 5 bit per channel whose script player runs; 6 the drop
sounds' state (0 armed, 1 a fall seen, 2 waiting for the re-arm); 7 bit per path whose thread plays
or drains; 8 blocks rendered u32 (pads), 12 (grips); 16 underruns u16 (pads), 18 (grips); 20 commands
lost to a full queue u16; 22 the generators running on each channel, a bit per type (u8 × 4); 26
the bytes in each channel's stream ring (u16 × 4); 34 each channel's stream format (u8 × 4, `0xFF`
none).

## The ring

`CONFIG_OPENSC2_TEST_RING_SIZE` bytes of whole entries; when one does not fit the oldest are dropped
(`ring_evicted` counts them). Entries have sequence numbers from 0 (`ARMED`; entry 1 is the lizard
resync of the ARM); `RING_READ`'s `first` is the oldest still there and `next` the one the next entry
gets, so a reader that asked from `from` and gets `first` > `from` lost entries. An entry: time u32 (µs
since ARM), kind u8, link u8 (0 radio, 1 USB, 2 test: the host emulation and everything the console
causes, 3 none: work queue events such as the rumble timeout or the Steam watchdog, 4 Bluetooth, 5
the dock: the dock link and the commands it ran; for `ARMED` and `MARK` the transport of the
request), data length u8 (at most 41), data:

| Kind | Name | Data |
| --- | --- | --- |
| `0x01` | ARMED | nonce u32 |
| `0x02` | MARK | marker u32 |
| `0x03` | SEQ_STEP | step u8, frame u32, the report's sequence byte u8 (the first frame of each step) |
| `0x04` | SEQ_END | frame u32 (the first frame not built from the sequence), how u8 (0 back to the static or the application's state, 1 kept the last step, 2 stopped, 3 lizard mode came on) |
| `0x05` | INJECT_STOP | reason u8 (1 lizard mode came on), frame u32 (built from the application's state again), what u8 (bit 0 the static state, bit 1 a sequence, bit 2 the raw injection) |
| `0x10` | OUTPUT | report ID u8, length u8 (ID included), decode result s8, up to 16 bytes after the ID |
| `0x11` | RUMBLE | left u16, right u16, intensity u16, gains s8 s8, type u8, timeout u8 |
| `0x12` | HAPTIC | kind u8 (`enum opensc2_haptic_kind`), report ID u8 (0: an event of the library's own, lizard mode's ticks and pad clicks, with the link lizard mode runs on), target u8, channels u8, then by kind: pulse on, off, count u16; command u8, gain s8; tone gain s8, Hz u16, ms u16, LFO Hz u16, depth u8; sweep gain s8, ms u16, start u16, end u16, antiphase u8; script id u8, gain s8; PCM control operation, format, encoding u8, rate u16; PCM data length u8, CRC-32 u32 of the samples |
| `0x13` | LED | red, green, blue, white, override, brightness (u8 each) |
| `0x14` | SETTING | number u8 (`0xFF`: all back to defaults), value s16 (only when a value changes) |
| `0x15` | LIZARD | 0 leave, 1 enter, 2 switches down, 3 switches up, 4 resync |
| `0x16` | LIZARD_RPT | flags u8 (bit 0 held back), frame u32 (the last built, `0xFFFFFFFF` none since ARM), the report as it would have gone out |
| `0x17` | POWER | request u8 (`enum opensc2_power_request`), its reason text (at most 32 characters) |
| `0x18` | VENDOR | direction u8 (0 SET, 1 GET), opcode u8, n u8 (SET: its length byte; GET: the answer's length), result s32, the request's safe bytes: `0x87` whole triplets (at most four); `0x89`, `0xAE`, `0xF2`, `0xA1` the first byte; `0xC5` 4 bytes; `0xDC`, `0xE2` 2; `0x86`, `0x95` the 4-byte key; `0xED`..`0xF0` a key ID (1 `esb/bond`, 2 `esb/bond_2`, 3 `user/wireless_transport`, 4 `user/haptic_boot_level`, 5..8 the `opensc2/` identity keys, 9 `user/keylock`, 10 `user/fully_charged_led_intensity`, 11 `mte/charge_level`, 12 `debug/resetreason`, 13..15 `settings/haptics/haptic_master_gain_db`, `amplifier_mode`, `enabled`, 16..29 `cal/joy_l`, `joy_r`, `trg_l`, `trg_r`, `prs_l`, `prs_r`, `touch_l`, `touch_r`, `rgbw_r`, `rgbw_g`, `rgbw_b`, `rgbw_w`, `volt_offset`, `sensors/gyroscope/bias`, 0 other) and the value's length; nothing for the others. None for `0x7E` |
| `0x19` | HOST_ASLEEP | |
| `0x20` | LINK | state, reason, slot, channel, protocol (u8 each) |
| `0x21` | USB | state, active, suspended |
| `0x22` | VERSION | the protocol version answered |
| `0x23` | IDENTITY | product ID u16, build time u32, bootloader build time u32, revision u32, git revision 12 bytes (never the serials) |
| `0x24` | CHANNELS | the four channel bytes of `0xE4` |
| `0x30` | STOP_STATUS | channel u8 (`0x44`'s numbering: left grip 0, right grip 1, left pad 3, right pad 4), flags u32 (`0x02`): a `0x44` the library sent as the reply to a stop, with the link the output came on |
| `0x40` | BLE | a Bluetooth link event (link 4): event u8, a u8, b u16, c u32. 1 state: a the link state, b the reason, c the slot (bits 0 to 7), the identity (8 to 15) and the advertising form (16 to 23: 0 pairing mode, 1 allow-listed, 2 directed, `0xFF` none); 2 security: a the level, b the error; 3 the ATT MTU in b; 4 parameters: b the latency, c the interval in µs; 5 the gate: a 1 up, 0 down, b the ATT MTU, c the interval; 6 the connection-event trigger: a 1 set, 0 failed (a timer paces), b the error; 7 disconnect: a the HCI reason acted on (`BLE_DROP`'s, or the stack's), b what followed (0 advertising again after 500 ms, 1 wireless off, 2 nothing: the board ended the link itself, a `0x16`), c the reason the stack reported; 8 advertising started: a the form, b the error (0 running), c the slot (bits 0 to 7), the identity (8 to 15), why (16 to 23: 0 a start, 1 again after a lost link, 2 after a failed connection) and bit 24 set when the identity was reset first; 9 advertising ended: a why (0 a host connected, 1 a connection failed: b the HCI error, `0x3C` when directed advertising went unanswered, 2 the window ran out, 3 stopped), c the identity; 10 identity: a 0 created, 1 reset (a new address, no keys), 2 a reset failed (b the error), c the identity; 11 bond: a 0 a new host bonded (c the identity, bits 8 to 15 the slot's other one, whose bond goes), 1 a bond was deleted and its identity is reset, 2 a bond was deleted but its identity is in use and kept, c the identity; 12 wireless off: a the cause (0 the window ran out, 1 the host ended the link), b the HCI reason |
| `0x50` | SYS_STATE | from u8, to u8 (the first state left and the last one entered), cause u8 (the event), transport u8, source u8 ([the numbers](#system-injection)) |
| `0x51` | LED_MODE | mode u8, flags u8 (bit 0 external power, 1 steady, 2 shutting down), level u8, fully-charged level u8 |
| `0x52` | CUE | cue u8, script u8, gain s8, played u8 (0: silent at the chime level) |
| `0x53` | SYS_END | state u8, cause u8, end u8 (1 power off, 2 reboot), how u8 (as in the last end) |
| `0x54` | CHORD | what u8 (1 down, 2 up, 3 STEAM pressed, 4 STEAM held, 5 the keylock's tap, 6 the keylock's hold), chord u8 |
| `0x55` | SOURCE | from u8, to u8, the event it posted u8 (`0xFF`: none, power present did not change) |
| `0x56` | IDLE | the idle history u16, when the idle event is posted; only while a system injection is in effect |
| `0x60` | HAP_GEN | channel u8 (0 left pad, 1 right pad, 2 left grip, 3 right grip), generator type u8 (0 click A, 1 click B, 2 pulse train, 3 tone, 4 rumble, 5 sweep, 6 stream, 7 clip), what u8 (1 started, 2 stopped by a command, 3 ended, 4 refused by the sequencer, 5 lost to a full queue, 6 ignored: nothing to play), gain s8, then four u16 by type: pulse on µs, off µs, count, 0; tone Hz, ms, LFO in 0.01 Hz (0xFFFF from 655.35 Hz up), depth (bit 15: started inverted); rumble Hz, ms, burst rate in 0.01 Hz, bit 15 inverted; sweep start Hz, end Hz, ms, bit 15 inverted; clicks and clips samples, format |
| `0x61` | HAP_BLOCK | path u8, block index u32 (0: the first after the path was idle), CRC-32 u32 of the block's bytes (pads 64, grips 256, little endian), peak u16 (the largest sample, 32767 full scale), flags u8 (bit 0 the first block, with its 4 ms lead-in; bit 1 nothing plays after it) |
| `0x62` | HAP_STREAM | channel u8, what u8 (1 a start queued, 2 started, 3 ran dry, 4 emptied by a stop, 5 a format command answered, 6 a write, 7 a `0x44` sent), fill u16 (bytes in the ring), flags u32 (the `0x44` flags of what) |
| `0x63` | HAP_SCRIPT | what u8 (1 started, 2 refused, 3 a step ran, 4 done, 5 a cue, 6 a free fall, 7 the drop sound due, 8 re-armed), channel u8 (a cue: the level), id u8 (a cue: the cue), gain s8, extra u16 (a step: its index; a cue: its script, 0 none; a free fall: 1 taken, 0 not; the drop sound: the random byte) |
| `0x70` | DOCK_SESSION | what u8 (0 start, 1 end), reason u8 (a start: 0 the pilot, 1 the test hooks; an end: 1 too short, 2 bad CRC, 3 a receive error that cut the frame short (the UART build; its DOCK_FRAME, outcome 1, follows), 4 the receiver not armed, 5 stopped), frames u32 (an end: answered in that session) |
| `0x71` | DOCK_FRAME | the request's type u8 (`0xFF` below 2 bytes), outcome u8 (0 answered, 1 size, 2 CRC), length u8, the answer's result s32 (0 without one), the handling time µs u32 |
| `0x72` | DOCK_PILOT | three readings mV s16 (−1 a failed conversion; a reading the hooks fed above 32767 shows as 32767), inside u8 (a bit per reading), present u8 (a passing check, or one the hooks fed) |
| `0x73` | DOCK_STATE | what u8 (0 a supply event, 1 an entry into the dock states, 2 another event), event u8 (the event, or the entry), chord u8, from u8, to u8 (0 away, 1 radio off, 2 lifted off, 3 a chord, 4 link up), handled u8, actions u32 (bits in `dock_rules.h`'s order: stop link, start link, pairing, select, cue, power-off chime, 4 s timer, 5 s timer, timer stop, pause, reboot, shutdown, update window, to battery, to a battery chord, LED charging, LED transport, LED colour, LED off, keylock cleared) |

The system entries (`0x50` to `0x56`; `0x57` is kept) have link 3, none; the dock's (`0x70` to
`0x73`) have link 5, the dock.

The CRC-32 is the common one (`osc2_crc32(0, ...)`, as Python's `binascii.crc32`).

Counters (u32, reset by ARM and CNT_RESET, counting only while armed), by index: 0 frames, 1
usb_sent, 2 usb_dropped, 3 radio_sent, 4 radio_dropped, 5 radio_packets, 6 radio_keepalives, 7
battery_sent, 8 battery_dropped, 9 lizard_made, 10 lizard_diverted, 11 app_reports_sent, 12
out_reports, 13 out_short, 14 vendor_set_ok, 15 vendor_set_err, 16 vendor_get_ok, 17 vendor_get_err,
18 usb_frame_err, 19 ring_entries, 20 ring_evicted, 21 radio_rx_overflow, 22 radio_answers_dropped, 23
radio_extra_dropped, 24 test_requests, 25 seq_steps, 26 usb_skipped.

## The console: `opensc2 test`

```text
opensc2 test [<id>] <verb> [<arguments>]
```

`<id>` is a number 1..65535 that every reply line carries (`-` without one). Every reply line is
`HWT <id> <TAG> [<rest>]`, and every command ends with exactly one `HWT <id> END <rc>`: 0, or −22
(bad arguments), −13 (not armed), −2 (unknown verb), with an `HWT <id> ERR <text>` line before it.
A command that ends with 0 while armed refreshes the lease.

| Verb | Arguments | Lines before `END` | Needs arming |
| --- | --- | --- | --- |
| `sync` | | none | no |
| `raw` | the request in hex (14..122 digits) | `RAW <answer in lower-case hex>` | no (the op decides) |
| `status` | | `STATUS armed= guard= inject= battery= seq=idle\|run mode=wired\|wireless reason= link=off\|no-bond\|pairing\|listening\|connected usb=off\|attached\|no-host\|configured slot=1\|2 lizard= frames= ring=<first>..<next> up= nonce=<8 hex digits> policy=auto\|wireless snapshot=` (new keys may follow) | no |
| `arm` | `<key hex8> [<lease s>]` (the key is `7e57ab1e`; nonce 0, no restore) | `ARMED nonce=00000000 lease=<s>` | no |
| `disarm` | | `DISARMED` | yes |
| `ring` | `[<from>]` | `EV <seq> <t ms>.<µs> <radio\|usb\|test\|none\|ble\|dock> <KIND> <field=value ...>` per entry, then `RING first=<n> next=<n>` | yes |
| `cnt` | | `CNT <name>=<value>` per counter | yes |
| `settings` | | `SETTINGS <first> <v> ...`, 16 values a line, 86 in all | no |
| `inject off`, `battery off`, `seq stop` | | `OK` | yes |

The shell's command buffer must hold a `raw` request of 61 bytes (`CONFIG_SHELL_CMD_BUFF_SIZE` of 160 or
more, checked at build time; the default is 256). The shell's serial backend copies what arrives into a
receive ring and drops the bytes that do not fit while the shell thread is busy (64 bytes by default,
less than such a line), so with the hooks the library raises its default to 256
(`CONFIG_SHELL_BACKEND_SERIAL_RX_RING_BUFFER_SIZE`); the test suite also writes long lines in short
chunks, and counts the shell's "RX ring buffer full" warnings. How the suite uses all of this:
[docs/testing.md](testing.md).

Back to the [documentation map](README.md).
