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

One binary protocol, the same bytes on three transports:

| Transport | Request | Answer |
| --- | --- | --- |
| USB (wired) | `SET_REPORT(Feature 1)`: `01 7E <n> <request>`, zero padded to 64 bytes | the next `GET_REPORT(Feature 1)`: `01 7E <m> <answer>` |
| the radio, through the puck | the puck's feature report 1, the same bytes (a SET record) | the puck's GET (retried until the answer is there) |
| the console (wireless) | `opensc2 test <id> raw <request as hex>` | `HWT <id> RAW <answer as hex>` |

`0x7E` is no command of the real controller, of Steam or of SDL (theirs start at `0x80`).

Request (n = 7 + the arguments, at most 61): key u32 `1E AB 57 7E`, protocol version u8 (1), request
ID u8 (echoed), op u8, arguments. Answer (m = 4 + the result, at most 61): version u8, request ID u8,
op u8, status u8 (0 OK, 1 BAD_OP, 2 BAD_LEN, 3 NOT_ARMED, 4 BAD_ARG, 5 BUSY, 6 DENIED, 7 NO_LINK),
result (empty unless OK). The checks run in that order: op, argument length, armed (the console
only), argument values, state, policy; the first that fails gives the status.

**Claiming (USB and the radio).** A `0x7E` SET is the test channel only when n ≥ 7, the key and the
version match, and the board is armed or the op is `ARM`. Such a request is claimed: the SET
succeeds, the op runs at once, and its answer is held for that link; a failing op still succeeds as a
SET and reports its status in the answer. Anything else with opcode `0x7E` takes the usual path (the
application's `vendor_command`; without one a STALL on USB and an error result on the radio). A GET of
`0x7E` returns the link's held answer and never runs the op again, so repeated GETs are harmless. The
held answer lasts until the next successful SET on that link (claimed or not) replaces the stored
request; a failed SET keeps it. The end of arming drops every link's held answer, except that a
`DISARM` then holds its own. With no held answer a GET takes the usual path. Each claimed request
refreshes the lease. Claimed requests are counted in the USB session log but not logged line by line.

**The console** has nothing to fall through to: a `raw` request shorter than 7 bytes or with the wrong
key or version prints `HWT <id> ERR not a test request` and `HWT <id> END -22`; every other request
runs, and while unarmed every op but `ARM` and `STATUS` answers NOT_ARMED.

Ops (arguments and results little endian; state52 is report `0x42` without its ID and sequence byte):

| Op | Name | Arguments | Result | Notes |
| --- | --- | --- | --- | --- |
| `0x01` | ARM | nonce u32, lease s u16 (0: 15; at most 600), flags u8 (bit 0: restore the settings and the LED colour at the end) | caps, 36 bytes | a fresh start: the ring, the counters, injection, the battery and the sequence reset; BAD_ARG for a lease above 600 or another flag bit |
| `0x02` | DISARM | | | the end of arming |
| `0x03` | STATUS | | status, 32 bytes | also the lease's keep-alive |
| `0x10` | INJECT | flags u8 (bit 0: stop a running sequence first; bit 1: the lizard test), state52 | frames u32 | the static injected state; DENIED while lizard mode is on without bit 1 |
| `0x11` | INJECT_OFF | | frames u32 | also stops a sequence |
| `0x12` | BATTERY | charge state u8, level u8, battery, system, input mV u16, current s16, input current u16, temperature u16, gauge flags u8, flags u8 (bit 0: send a `0x43` now; the 3.5 s period restarts from it) | | |
| `0x13` | BATTERY_OFF | | | |
| `0x14` | SEQ_LOAD | step u8 (below the depth), hold u8 (1..255 frames), state52 | | BUSY while a sequence runs |
| `0x15` | SEQ_RUN | count u8 (1..depth; steps 0..count-1 loaded since ARM), flags u8 (bit 0 loop, bit 1 keep the last step as the static state, bit 2 the lizard test) | frames u32 | starts with the next frame built; BAD_ARG for a step not loaded, BUSY while one runs, DENIED while lizard mode is on without bit 2 |
| `0x16` | SEQ_STOP | | | `SEQ_END` how 2 |
| `0x20` | RING_READ | from u32, max entries u8 (0: as many as fit) | first u32, next u32, count u8, then that many whole entries from max(from, first), at most 48 bytes | BAD_ARG when from > next |
| `0x21` | RING_CLEAR | | next u32 | sequence numbers go on |
| `0x22` | CNT_READ | first u8, count u8 (1..13) | first u8, count u8, u32 × count | clipped at the end of the table |
| `0x23` | CNT_RESET | | | |
| `0x24` | MARK | marker u32 | the entry's sequence number u32 | |
| `0x30` | SETTINGS_READ | first u8, count u8 (1..27) | first u8, count u8, s16 × count | clipped at 86 |
| `0x40` | HOST_OUT | an output report, ID first, 1..54 bytes | the decode result s8 | as from a host, on the test link |
| `0x41` | HOST_SET | opcode u8, n u8, n payload bytes (n = the arguments − 2) | result s32 | a vendor SET on the test link; only `0x81`, `0x83`, `0x85`, `0x87`, `0x89`, `0x8E`, `0x9F`, `0xA1`, `0xAE`, `0xBE`, `0xC5`, `0xDB`, `0xE9`, `0xED`, `0xF2`, and `0x95` with the turn-off key `0xABDF9710` (read as the command reads it: the first four payload bytes, zero padded) run; everything else answers DENIED |
| `0x42` | HOST_GET | | opcode u8, n u8, flags u8 (bit 0 masked, bit 1 cut), then the first min(n, 54) answer bytes, or for a masked answer the CRC-32 of its n bytes | a vendor GET on the test link; masked: `0xED` of `esb/bond` or `esb/bond_2`, `0xAE` tags 0, 1 and 4, `0xF2` selector 0, `0xA1` selector 1 |
| `0x43` | APP_SEND | kind u8; kind 0: an input report, ID first, 1..53 bytes; kind 1: channel u8, flags u32 | result s32 | as the application would (`opensc2_send_report()`, `opensc2_send_haptic_status()`), through the guard like every report; DENIED for a `0x42` |
| `0x44` | RADIO_DEAF | ms u16 (100..5000) | | the radio hears nothing that long, so the link times out the normal way; BUSY while one is pending, NO_LINK unless wireless and connected |
| `0x80` | HAP_RESET | seed u32 | | the haptics synthesiser (below): everything stops at once, the queues and stream rings empty, the formats forgotten, the scripts and the drop sounds idle, captures off, the rumble's random burst gains restarted from the seed (5323 = `0x14CB` at boot) |
| `0x81` | HAP_CAPTURE | path u8 (0 pads, 1 grips, 2 both), blocks u8 (0 stops) | | a `HAP_BLOCK` entry for each of the next blocks the path renders; BAD_ARG for a path above 2 |
| `0x82` | HAP_STATUS | | status, 38 bytes (below) | |
| `0x83` | HAP_PLAY | what u8; 0 cue: cue u8, level u8; 1 click: channels u8, click u8 (1 A, 2 B), gain s8; 2 script: channels u8, id u8, gain s8; 3 free fall: the random byte its drop sound will use u8; 4 stop: channels u8 | result s32 | the synthesiser's own sounds, as the application or the library would play them: the grips a cue started on (-1: it plays nothing at that level), the channels queued or started, 1 when a free fall was taken (0: not armed); BAD_ARG for another `what` |

The **test link** is a third command link of the library with a stored request of its own, used only
by `HOST_SET` and `HOST_GET`, so emulating a host on the board never disturbs the stored request of
USB or the radio. It counts as active exactly when the mode's real link is (USB configured while
wired, the radio connected while wireless).

**Frames.** A frame is one state report built. Its index counts from 0, the first frame after `ARM`
(`CNT_RESET` does not restart it). `INJECT`, `INJECT_OFF` and `SEQ_RUN` answer the index of the next
frame at the moment of the request, f: the change shows from frame f or f + 1 (frame f may be half
built). The injected state replaces the application's after the `sample` callback, never as the
application's own state: `opensc2_get_state()`, the shell's `opensc2 state` and the rules that read
STEAM never see it. The library then applies the trigger rule and encodes the report as always.

Caps (the ARM result, 36 bytes): 0 version (1); 1 mode (0 wireless, 1 wired); 2 build flags u32
(bit 0 `TRIGGER_CLICK_AUTO`, 1 `LIZARD_MODE`, 2 `STEAM_WATCHDOG`, 3 `USB`, 4 `USB_PROFILE_OCTOBER`,
5 `USB_CONSOLE`, 6 `USB_WIRED_CONSOLE`, 7 `IDENTITY_VENDOR_WRITE`, 8 `STORAGE`, 9 `USB_LOG`, 10
`USB_REMOTE_WAKEUP`, 11 `USB_PLUG_IN_REBOOT`, 12 `SHELL`, 13 `FUSION`, each `CONFIG_OPENSC2_*`; 14
`NO_HOST_WIRELESS_MS` not 0, the charger test build; 19 `HAPTICS_SYNTH`, the haptics synthesiser);
6 battery report interval ms u16; 8 rumble
timeout ms u16; 10 ring size u16; 12 sequence depth; 13 counters (27); 14 radio report delay µs u16;
16 the lease in effect s u16; 18 the stored mode policy (0 auto, 1 wireless); 19 zero; 20 the git
revision, 12 bytes NUL padded; 32 the CRC-32 of the identity's product name u32 (the name itself
does not fit; the suite checks the USB product string against it). Firmware before the product name
sent the first 32 bytes only.

Status (the STATUS result): 0 flags (bit 0 armed, 1 guard, 2 static injection, 3 battery injected, 4
sequence running, 5 lizard mode on, 6 the end of a disarm in progress, 7 a settings snapshot held); 1
mode; 2 mode reason; 3 link state; 4 USB state; 5 bond slot (0 or 1); 6 the last radio disconnect
reason; 7 the sequence byte of the last state report; 8 nonce u32; 12 uptime ms u32; 16 the ring's
first u32; 20 next u32; 24 frames since ARM u32; 28 the step in use (`0xFF` idle); 29 steps loaded;
30 frames left in the step u16.

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
causes, 3 none: work queue events such as the rumble timeout or the Steam watchdog; for `ARMED` and
`MARK` the transport of the request), data length u8 (at most 41), data:

| Kind | Name | Data |
| --- | --- | --- |
| `0x01` | ARMED | nonce u32 |
| `0x02` | MARK | marker u32 |
| `0x03` | SEQ_STEP | step u8, frame u32, the report's sequence byte u8 (the first frame of each step) |
| `0x04` | SEQ_END | frame u32 (the first frame not built from the sequence), how u8 (0 back to the static or the application's state, 1 kept the last step, 2 stopped, 3 lizard mode came on) |
| `0x05` | INJECT_STOP | reason u8 (1 lizard mode came on), frame u32 (built from the application's state again), what u8 (bit 0 the static state, bit 1 a sequence) |
| `0x10` | OUTPUT | report ID u8, length u8 (ID included), decode result s8, up to 16 bytes after the ID |
| `0x11` | RUMBLE | left u16, right u16, intensity u16, gains s8 s8, type u8, timeout u8 |
| `0x12` | HAPTIC | kind u8 (`enum opensc2_haptic_kind`), report ID u8, target u8, channels u8, then by kind: pulse on, off, count u16; command u8, gain s8; tone gain s8, Hz u16, ms u16, LFO Hz u16, depth u8; sweep gain s8, ms u16, start u16, end u16, antiphase u8; script id u8, gain s8; PCM control operation, format, encoding u8, rate u16; PCM data length u8, CRC-32 u32 of the samples |
| `0x13` | LED | red, green, blue, white, override, brightness (u8 each) |
| `0x14` | SETTING | number u8 (`0xFF`: all back to defaults), value s16 (only when a value changes) |
| `0x15` | LIZARD | 0 leave, 1 enter, 2 switches down, 3 switches up, 4 resync |
| `0x16` | LIZARD_RPT | flags u8 (bit 0 held back), frame u32 (the last built, `0xFFFFFFFF` none since ARM), the report as it would have gone out |
| `0x17` | POWER | request u8 (`enum opensc2_power_request`), its reason text (at most 32 characters) |
| `0x18` | VENDOR | direction u8 (0 SET, 1 GET), opcode u8, n u8 (SET: its length byte; GET: the answer's length), result s32, the request's safe bytes: `0x87` whole triplets (at most four); `0x89`, `0xAE`, `0xF2`, `0xA1` the first byte; `0xC5` 4 bytes; `0xDC`, `0xE2` 2; `0x86`, `0x95` the 4-byte key; `0xED`..`0xF0` a key ID (1 `esb/bond`, 2 `esb/bond_2`, 3 `user/wireless_transport`, 4 `user/haptic_boot_level`, 5..8 the `opensc2/` identity keys, 0 other) and the value's length; nothing for the others. None for `0x7E` |
| `0x19` | HOST_ASLEEP | |
| `0x20` | LINK | state, reason, slot, channel, protocol (u8 each) |
| `0x21` | USB | state, active, suspended |
| `0x22` | VERSION | the protocol version answered |
| `0x23` | IDENTITY | product ID u16, build time u32, bootloader build time u32, revision u32, git revision 12 bytes (never the serials) |
| `0x24` | CHANNELS | the four channel bytes of `0xE4` |
| `0x60` | HAP_GEN | channel u8 (0 left pad, 1 right pad, 2 left grip, 3 right grip), generator type u8 (0 click A, 1 click B, 2 pulse train, 3 tone, 4 rumble, 5 sweep, 6 stream, 7 clip), what u8 (1 started, 2 stopped by a command, 3 ended, 4 refused by the sequencer, 5 lost to a full queue, 6 ignored: nothing to play), gain s8, then four u16 by type: pulse on µs, off µs, count, 0; tone Hz, ms, LFO in 0.01 Hz, depth (bit 15: started inverted); rumble Hz, ms, burst rate in 0.01 Hz, bit 15 inverted; sweep start Hz, end Hz, ms, bit 15 inverted; clicks and clips samples, format |
| `0x61` | HAP_BLOCK | path u8, block index u32 (0: the first after the path was idle), CRC-32 u32 of the block's bytes (pads 64, grips 256, little endian), peak u16 (the largest sample, 32767 full scale), flags u8 (bit 0 the first block, with its 4 ms lead-in; bit 1 nothing plays after it) |
| `0x62` | HAP_STREAM | channel u8, what u8 (1 a start queued, 2 started, 3 ran dry, 4 emptied by a stop, 5 a format command answered, 6 a write, 7 a `0x44` sent), fill u16 (bytes in the ring), flags u32 (the `0x44` flags of what) |
| `0x63` | HAP_SCRIPT | what u8 (1 started, 2 refused, 3 a step ran, 4 done, 5 a cue, 6 a free fall, 7 the drop sound due, 8 re-armed), channel u8 (a cue: the level), id u8 (a cue: the cue), gain s8, extra u16 (a step: its index; a cue: its script, 0 none; a free fall: 1 taken, 0 not; the drop sound: the random byte) |

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
| `ring` | `[<from>]` | `EV <seq> <t ms>.<µs> <radio\|usb\|test\|none> <KIND> <field=value ...>` per entry, then `RING first=<n> next=<n>` | yes |
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
