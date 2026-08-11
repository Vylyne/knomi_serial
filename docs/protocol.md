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

Proto 5's state frame is 80 bytes of payload, 91 on the wire: **7.9%**. What
left the tick did not disappear — it moved to a channel that only carries it
when it changes, or it left because nothing was reading it.

## `STATE` — 80 bytes

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
| 60     | `uint32`   | `config_crc` |
| 64     | `char[16]` | `filament_type`, NUL-padded |

Proto 5 removed `eta`, `elapsed`, `layer` and `layer_total`. They were added
speculatively and no screen ever read them — sixteen bytes sampled, packed, sent
ten times a second, byte-swapped on arrival and discarded. The contract test
makes adding a field back cheap, which is a better trade than carrying one
against a design that does not exist yet.

The MCU pair stayed and is now shown, because on a toolchanger that MCU rides
inside the heated chamber and its temperature is a reading nothing else reports.
It keeps a target because `sensor_mcu:` may name a `temperature_fan`, which has
one; a plain `temperature_sensor` reports zero and the target is simply not
drawn.

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

## `CONFIG` — 292 bytes

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
| `uint8`    | `estop_at` — 0 below the page row, 1 above it |
| `uint8[4]` | `readouts` — secondary readout ids in order, terminated by 0 |
| `uint8[8]` | `page_order` — page ids in order, terminated by 0 |
| `char[256]`| `gcodes`, newline-separated |

`page_order` is why there is one firmware rather than two. A build flag used to
compile the home and move pages out, which saved 1572 bytes of a 4.7 MB flash
and cost a second image to build, test and choose between. Off-screen pages are
neither updated nor drawn, so the real cost of a page you do not want is one
more swipe past it — a preference, and preferences belong in `printer.cfg`.

    1 tool    2 gcode    3 home    4 move    5 estop

The device appends `estop` whatever the list says, so it is never sent. Keeping
it out of the list means a `pages:` written while thinking about idle pages
cannot drop it by omission. The printing screen appends it the same way.

It hangs off the row on the second axis rather than sitting in it, so it is one
swipe from every page instead of several along - `estop_at` picks the side.
That byte was the struct's padding until it was given a meaning, so claiming it
moved no offsets and needed no version bump: `present` says what was actually
set, and a host predating it sends zero, which is the behaviour that byte
already had.

A page that would be empty is skipped — `gcode` with no macros configured gets
no G-code page — and an id this firmware has no page for is skipped rather than
refused, the same way an unknown frame type is. The screen lands on the first
page built, so ordering picks the landing place too rather than needing a second
setting that could contradict it.

`readouts` is which secondary temperatures a screen shows above its hotend, in
the order given, each in its own scrim - two short numbers under one wide pill
is mostly dead dark space and reads as a bar rather than as two readings.

    1 bed    2 chamber    3 mcu

A fixed pair would have been wrong for both machines this has to serve. A
single-toolhead printer wants its bed; four tool screens each restating the one
bed temperature is four copies of something none of them owns, while the MCU in
the chamber is a reading only that tool can give. A readout the machine does not
have is skipped, so listing one costs nothing on a printer without it.

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
    page_order              bit 8
    estop_at                bit 9
    readouts                bit 10

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

Report keys: `id` `fw` `proto` `var` `sleep` `scr` `page` `cfg` `heap` `minheap`
`up` `pages` `busy` `peak` `psram` `lvfree` `lvfrag` `flush` `fpx` `fus`.

### `id` is hardware, not issued

`id` is the low three bytes of the eFuse MAC, six hex characters. It is the one
report key that never changes, which is what makes discovery possible: the host
opens a candidate port, waits for a line that is already coming, and reads who
is on the end of it. No request, no response, no handshake to version.

It is hardware-derived rather than a code the host issues and the device stores,
because the requirement is that it survive a firmware update — and a stored code
only clears that bar for the easiest case:

| | `pio run -t upload` | `esptool erase_flash` | partition table changes |
| --- | --- | --- | --- |
| code in NVS | survives | **lost** | **lost** |
| eFuse MAC | survives | survives | survives |

Nothing generates it, nothing stores it, and it cannot be duplicated or reset.
The top three bytes are dropped because they are Espressif's OUI and identical
on every unit — six characters of noise in something a person types into
printer.cfg. It is not hashed, so it can still be checked against
`esptool chip_id`; hashing would trade that away to save two characters.

`esp_read_mac(ESP_MAC_WIFI_STA)`, not `ESP.getEfuseMac()` — the latter hands
back the six bytes reversed against the printed order, which would break exactly
the property above.

### The device map

`printer.knomi_cluster.devices` is every section in one place, keyed by section
name:

```json
{"T0_knomi": {"device_id": "19AA44", "port": "/dev/ttyUSB3",
              "addressed_by": "device_id", "build_variant": "knomi",
              "firmware_version": "0.5.0", "protocol_version": 5,
              "online": true, "tool": "0"}}
```

For firmware updaters, which otherwise have to read `serial:` out of printer.cfg
to find the displays. That no longer answers the question: a section addressed
by `device_id:` has no path in it at all, and the path it is on today was
discovered rather than configured. A section still appears here when it has
never answered — `online: false`, versions `null` — because a display that needs
flashing is precisely the one an updater must not be blind to.

Deliberately excluded: heap, uptime, and the rest of the per-tick figures. Those
are in each section's own `get_status`, and something polling the whole row for
versions does not want them.

## Reserved for a proto 5: screen templates

Not built. Written down because the shape was worked out and is worth not
re-deriving, and because the decisions it implies are easier to make before
something forces them than after.

Every screen today is the same kind of thing: one tool, its heat, its filament.
A display that is not about a tool — a machine-wide status panel, chamber and
exhaust and filter and build plate — wants none of `State`'s per-tool fields and
several that do not exist anywhere in the protocol.

Enumerating those into `State` is the wrong answer. Every screen would carry
fans it never shows, and each new subsystem would be another version bump.
Instead a section would declare what kind of screen it is:

```ini
[knomi_serial chamber_panel]
template: sensors
sensors:
    Chamber = heater_chamber
    Exhaust = fan_generic exhaust
    Filter  = fan_generic nevermore
```

### The layout is the config

Rows are lines; columns are entries separated by commas on one line. The shape
you type is the shape on the glass:

```ini
sensors:                          sensors:                    sensors:
    a, b, c                           a                           a
                                      b, c                        b
                                                                  c

    one row of three              one, then two               a column
```

This is the part worth keeping. The expensive half of a grid was never drawing
it — it was *choosing* it: picking between 1×2 and 2×2 and 2×3, measuring on two
axes, and getting steadily worse if it started adapting to the length of what is
in each tile. Letting the config say removes all of that, and nested flex does
the remaining arithmetic on its own: a column of rows, `space-evenly` on both
axes, no measuring.

It also stops the layout caring which way the panel is mounted. A tall display
is lines of one; a wide one is one line of several; the parser does not need to
know the difference.

Klipper's config parser keeps newlines in an indented multi-line value — the
same mechanism `[gcode_macro]` bodies rely on — so this needs no special
handling on the host.

### A layout per machine state

What is worth showing while a print runs is not what is worth showing while the
machine sits idle. So: one layout per state, and the default covers anything not
given one.

```ini
[knomi_serial chamber_panel]
template: sensors

# What exists. Declared once, whatever is on screen.
sensors:
    chamber = heater_chamber
    exhaust = fan_generic exhaust
    filter  = fan_generic nevermore

# How it is arranged. `layout:` is the default and covers every state without
# one of its own.
layout:
    chamber
    exhaust, filter

layout_printing:
    chamber, exhaust
```

Declaring *what exists* separately from *how it is arranged* is the part that
matters. It keeps the values array one fixed thing in one fixed order, so the
state changing does not change what the numbers on the wire mean — and it lets
two states show the same sensor without declaring it twice. A layout is then
only a set of references, and the whole per-state feature costs no extra bytes
in the tick at all.

It also needs nothing new at runtime: screens are already torn down and rebuilt
whenever the printer's status changes, so picking a different arrangement is
that same rebuild reading a different list.

### What it costs on the wire

Names ship as one string, newline between rows and comma within a row, parsed
once when config is adopted rather than per tick — the same shape `gcodes`
already uses. The values frame is a flat array in declaration order.

The one structural consequence: several layouts will not fit a fixed-size
`CONFIG`. The framing already carries a length, so a variable-length payload is
possible today — it is `config::apply` insisting on an exact size that would
have to give, becoming a fixed header plus a variable tail. Cheap to design in
now, tedious to retrofit later, which is the reason this paragraph exists.

| template | needs | shows |
| --- | --- | --- |
| `tool` (default) | `heater_hotend`, `tool`, `pages` | today's UI, unchanged |
| `sensors` | a named list | name, icon, value — and a target where one exists |

The value/target rule already exists: the tool page hides the hotend's target
line when it is zero, because no target and a target of zero are different
facts. A sensor list is that rule applied N times.

It divides cleanly across the two channels already here. Names and icons are
static, so they belong in `CONFIG`. Only the values change, so they go in a new
frame type carrying an array in the declared order — which is the type byte
introduced in proto 3 doing the job it was added for, rather than another
redesign.

Two things to know before starting:

**Icons need a font pipeline.** LVGL's built-in symbols have no thermometer and
no fan — `TINT`, `CHARGE`, `WARNING`, `USB`, `SD_CARD` and so on. Real sensor
icons mean generating a custom icon font with LVGL's converter. Solved, but a
build step rather than a line of code. Names alone work for a first pass.

**Do not write a tiling algorithm.** See above — the config says the shape, and
that is the whole reason a grid is affordable here. An automatic one would have
to choose between 1×2 and 2×2 and 2×3, measure on two axes, and would only get
worse the moment it started adapting to how long each label is.

## Changing any of this

1. Edit `struct State` or `struct Config` in `src/printer/printer.h`.
2. Edit `_STATE_FMT` or `_CONFIG_FMT` in `klippy_extras/knomi_serial.py`.
3. Update `kStateWireSize` / `kConfigWireSize` and the `static_assert`s.
4. Bump `kProtoVersion` and `_PROTO_VERSION`.
5. Update `scripts/simulate.py` if the new field is worth simulating.

The asserts turn a layout disagreement into a build failure. Skipping the bump
turns it into a display that renders garbage and does not say why.
