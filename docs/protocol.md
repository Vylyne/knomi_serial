# The wire

Klipper and the display talk over one serial port at 115200 baud. Downstream
(host to device) is binary frames; upstream (device to host) is lines of text.
The two halves are deliberately different: the host sends a fixed set of facts
ten times a second and wants that cheap, while the device sends a handful of
events and reports and wants those readable in a terminal.

Both ends are pinned to `kProtoVersion` / `_PROTO_VERSION`. They must match.
The device reports the version it speaks in every status line, and the module
logs a warning naming both versions if they disagree.

## Framing

    HEADER(4)   0x83AD83AD
    TYPE(1)     see below
    LEN(2)      payload length, big-endian
    PAYLOAD     LEN bytes
    FOOTER(4)   0xF007F007

Every multi-byte field is network order. A receiver scans for the header a byte
at a time, so any amount of noise between frames is recovered from.

| Type | Name      | When                                        |
|------|-----------|---------------------------------------------|
| 0x01 | `STATE`   | every 100 ms                                |
| 0x02 | `CONFIG`  | when the device asks                        |
| 0x03 | `MESSAGE` | when there is something to tell the operator |
| 0x04 | `SNAPSHOT` | when a human asks for a screenshot |

A device that does not recognise a type skips it using `LEN` and stays in sync.
That is the reason the length is on the wire at all: it lets the two ends
disagree about the protocol's edges without disagreeing about its middle.

## Why there is more than one frame

Proto 2 had a single 332-byte packet sent at 10 Hz — 3400 B/s of an 11520 B/s
link, or **29.5%** of the port, permanently.

256 of those bytes were the G-code macro list, which is read from `printer.cfg`
at startup and cannot change while Klipper is running. The link spent three
quarters of its budget restating a constant.

Proto 3's state frame is 96 bytes of payload, 107 on the wire: **9.3%**. What
left the tick did not disappear — it moved to a channel that only carries it
when it changes.

## `STATE` — 96 bytes

Matches `struct State` in `src/printer/printer.h` down to `filament_type`, and
`_STATE_FMT` in `klippy_extras/knomi_serial.py`. Both files carry a
`static_assert` / `struct.calcsize` that fails the build if they drift.

| Offset | Type       | Field                                      |
|--------|------------|--------------------------------------------|
| 0      | `uint32`   | `status` — disconnected/idle/printing/shutdown |
| 4      | `bool`×7   | `working` `paused` `homed_x/y/z` `used` `active` |
| 11     | `uint8`    | `tram_type` |
| 12     | `int32`×8  | hotend, bed, chamber, mcu — each temp then target |
| 44     | `int32`    | `progress`, 0–100 |
| 48     | `int32`    | `tool_number`, −1 for none |
| 52     | `uint32`   | `filament_color`, `0x00RRGGBB`; 0 means unknown, not black |
| 56     | `int32`    | `flow` — extrusion rate, µm of filament per second, signed |
| 60     | `int32`×2  | `eta`, `elapsed` — seconds, −1 unknown |
| 68     | `int32`×2  | `layer`, `layer_total` — −1 unknown |
| 76     | `uint32`   | `config_crc` |
| 80     | `char[16]` | `filament_type`, NUL-padded |

The seven flags and the one-byte tram type exactly fill the gap after `status`,
which is what keeps the `int32` block 4-byte aligned. Adding a flag consumes
that padding rather than shifting everything below it.

### `flow` is a rate, and comes from `motion_report`

Not the extruder position, and not the change in it since the last packet.

Position aliases at 10 Hz: a fast extrude and a slow one can land on the same
pair of samples, so anything derived from it lies about speed. It is also the
*commanded* position, which runs a whole lookahead queue ahead of the nozzle and
arrives in bursts as the queue fills — a wave driven by it would surge and stall
while the print ran smoothly. And it is reset by `G92 E0`, which some slicers
emit every layer, so every reset would read as an enormous retraction.

`motion_report.live_extruder_velocity` samples the trapezoid queue at the current
print time, which is what the stepper is actually being told. The host multiplies
by 1000 and sends µm/s. Only the mounted tool gets a non-zero value; a docked
tool shares the toolhead's motion report and none of its filament.

## `CONFIG` — 280 bytes

Everything that is true for hours at a time: the macro list, colours, the sleep
timings, and which corners are soft keys.

| Type       | Field |
|------------|-------|
| `uint32`   | `present` — bitmask, see below |
| `uint32`   | `color_machine` |
| `uint32`   | `color_filament_unknown` |
| `uint32`   | `dim_ms`, `sleep_ms` |
| `uint8`×2  | `brightness`, `dim_brightness` (0–16) |
| `uint8`    | `key_mask` — corners that are legends: NW 1, NE 2, SW 4, SE 8 |
| `char[256]`| `gcodes`, newline-separated |

`key_mask` is how a corner stops being a touch target without losing its symbol.
A corner with a switch behind it — wired to the device, or bound to a
`[gcode_button]` on the host — already reports its own press, and two ways to
fire one action, one of them invisible, is a way to fire it by accident. The
mark stays exactly where it was and becomes the key's legend.

Adopting a config reloads whatever screen is up, because which corners are soft
is decided when a page is built. Colours would follow on their own, being read
at draw time, so without the reload a `printer.cfg` edit would apply to half the
screen and wait for a status change for the rest.

### `present` means override, not replace

Each field has a bit in `present`. A field whose bit is clear keeps whatever the
firmware was compiled with, from `user_conf.h`.

Without this the host would quietly become the authority on every setting, and
editing `user_conf.h` and reflashing would appear to work right up until the
first config frame arrived and put it all back. The module only sets a bit for
an option actually written in `printer.cfg`.

    color_machine           bit 0
    color_filament_unknown  bit 1
    dim_ms                  bit 2
    sleep_ms                bit 3
    brightness              bit 4
    dim_brightness          bit 5
    gcodes                  bit 6
    key_mask                bit 7

### How it stays in sync

The host hashes the config payload once, with `zlib.crc32`, and stamps that
`uint32` on every state frame. The per-tick cost is copying a precomputed
integer: nothing is diffed and nothing is decided.

The device compares it against the CRC of the config it actually holds —
computed locally, from the bytes it received, not taken on trust from the frame.
If they differ it sends `CFG?`, at most once a second. The host answers with a
`CONFIG` frame. The device adopts it, recomputes, and stops asking.

That one comparison covers every way the two can fall out of step:

- **Device reset.** Boots holding nothing, CRC is 0, asks immediately.
- **Klipper restart.** New host, possibly new config; whatever the device holds
  either matches or it does not.
- **`printer.cfg` edited.** New CRC on the next tick, device notices, asks.
- **Frame lost or corrupt.** The device's own hash will not match, so it asks
  again. The resync mechanism *is* the integrity check.
- **Host cannot answer.** The device keeps the config it has — stale config
  beats no config — and asks once a second until it can.

There is no acknowledgement, no retry timer, and nothing on the host that has to
remember which devices are current. A content hash cannot get out of step with
its content the way a sequence number can.

Both sides expose it: the device reports `cfg=` in its status line, and the
module surfaces `device_config_crc` and `config_applied` in `get_status`, so
"I pushed it" and "it took" are separable facts.

## `MESSAGE` — up to 127 bytes

UTF-8 text for the operator: currently the first line of Klipper's shutdown
reason, uppercased. Shown by the shutdown screen, and by the init screen if it
arrives before the first state.

Sent on change rather than every tick. Under proto 2 this rode in the `gcodes`
field, which meant the shutdown screen and the G-code page read the same bytes
for opposite purposes — and it is why removing the macro list from the tick had
to move this somewhere rather than simply drop it.

The device also writes its own faults here: `SHORT FRAME`, `BAD FRAME`,
`MALFORMED PACKET`, `PROTO MISMATCH`.

## `SNAPSHOT` — no payload

Send back a picture of the glass. The display has no network, no filesystem to
dump to and no second port, so the frame returns the way everything else does:
as lines of base64 on the link that is already open.

    KNOMI_CMD:SNAP:BEGIN:240,240,RGB565LE
    KNOMI_CMD:SNAP:<512 chars of base64>     x300
    KNOMI_CMD:SNAP:END

384 raw bytes per line. About fourteen seconds for a 240×240 frame, during which
the UI is doing nothing else — a developer and documentation tool, not something
to poll.

The frame is collected in the flush callback rather than with `lv_snapshot`.
Every pixel that reaches the panel already passes through there, so this needs
no second render pass, no `LV_USE_SNAPSHOT`, and no draw buffer carved out of
LVGL's 64k arena — and it captures what was actually displayed rather than a
re-rendering of what should have been. It lands in PSRAM, 8MB of which is
otherwise unused on this part. `capture_begin` invalidates the screen first,
because LVGL only renders what is dirty and a still screen would otherwise
produce nothing at all.

Raw RGB565 rather than anything compressed: PNG on the device would want a
deflate implementation and the RAM to run it, to halve a transfer that happens
when somebody types a command. `scripts/screenshot.py` does the conversion,
where zlib is already in the standard library, and masks the corners the round
bezel hides.

## Upstream: `KNOMI_CMD:`

One line per message, newline-terminated:

    KNOMI_CMD:STOP                  emergency stop; invokes Klipper's shutdown
    KNOMI_CMD:RESTART               firmware_restart
    KNOMI_CMD:GCODE:<text>          run a G-code
    KNOMI_CMD:MOVE:<axis><sign>     jog, e.g. MOVE:X+
    KNOMI_CMD:CFG?                  send config
    KNOMI_CMD:RPT:<k=v;k=v;...>     status report, every 2 s
    KNOMI_CMD:SNAP:...              screenshot, see above

Reports are parsed permissively in both directions: unknown keys from newer
firmware are ignored, and keys missing from older firmware simply stay absent
from `get_status`. That is what lets the report grow without a protocol bump.

Report keys: `fw` `proto` `var` `sleep` `scr` `page` `cfg` `heap` `minheap` `up`
`busy` `peak` `psram` `lvfree` `lvfrag` `flush` `fpx` `fus`.

## Changing any of this

1. Edit `struct State` or `struct Config` in `src/printer/printer.h`.
2. Edit `_STATE_FMT` or `_CONFIG_FMT` in `klippy_extras/knomi_serial.py`.
3. Update `kStateWireSize` / `kConfigWireSize` and the `static_assert`s.
4. Bump `kProtoVersion` and `_PROTO_VERSION`.
5. Update `scripts/simulate.py` if the new field is worth simulating.

The asserts turn a layout disagreement into a build failure. Skipping the bump
turns it into a display that renders garbage and does not say why.
