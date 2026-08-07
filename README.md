# Knomi_Serial

### Acknowledgement

This is a fork of [ruiqimao/zerod](https://github.com/ruiqimao/zerod). I forked this update so I could compile it and I'm tweaking it for my own personal use with a toolchanger

## Round display for klipper printers

Knomi_Serial is an alternative firmware for the BTT Knomi V2 and other similar displays that replaces the
network-reliant Moonraker connection with a direct serial connection to the Klipper host.

### Installation

Knomi_Serial firmware can be built and flahsed using [PlatformIO](https://platformio.org/).

An additional Klipper module is needed to enable the Knomi_Serial configuration options, which can be
installed by running the included `install.sh` script on the Klipper host.

### Klipper configuration
```ini
[knomi_serial T0_knomi] # both a named device like this or a single like [knomi_serial] are available.
serial:  # Path to the serial port for the Knomi_Serial device.
tool:    # Which tool this screen belongs to, e.g. T0. Optional; needed only for
         # KNOMI_TOOL to address this screen. `T0`, `t0` and `0` are equivalent.

heater_hotend: extruder  # Name of the hotend heater.
heater_bed: heater_bed   # Name of the bed heater.

# Only one of heater_chamber and sensor_chamber should be configured.
heater_chamber:  # Name of the chamber heater.
sensor_chamber:  # Name of the chamber thermistor.

move_x: 10  # Number of millimeters to move the toolhead in the positive X direction.
move_y: 10  # Number of millimeters to move the toolhead in the positive Y direction.
move_z: 10  # Number of millimeters to move the toolhead in the positive Z direction.

speed_x: 100  # Speed to move the toolhead in the X direction.
speed_y: 100  # Speed to move the toolhead in the Y direction.
speed_z: 100  # Speed to move the toolhead in the Z direction.

gcodes:  # Comma separated G-Codes to display on the Knomi_Serial device.
```

### Telling the screens about the job

```
KNOMI_TOOL TOOL=0 [USED=1] [COLOR=FF8800] [TYPE=PLA]
```

`USED` is what decides whether a screen sleeps. The host is *told* which tools a
job uses rather than inferring it from nozzle temperature, because temperature
cannot separate a docked tool still in the job from one that is merely warm from
the chamber — with ooze prevention dropping a docked tool by 100°C, and a chamber
at 60°C, those two sit close enough together that no threshold splits them.

Every parameter except `TOOL` is optional, so one fact can be changed without
restating the others, and the command is safe to repeat. That makes mid-job
reassignment ordinary rather than a special case:

```gcode
# print start, one per tool - Orca knows all three from is_extruder_used[],
# filament_colour[] and filament_type[]
KNOMI_TOOL TOOL=0 USED=1 COLOR={filament_colour[0]} TYPE={filament_type[0]}

# T0 jammed, hand the rest of the job to T4
KNOMI_TOOL TOOL=0 USED=0
KNOMI_TOOL TOOL=4 USED=1 COLOR=FF0000 TYPE=ABS
```

`USED` returns to true for every tool when a print ends, so the next job starts
clean, and it defaults to true on startup — a module restart mid-print must not
black out every screen. Filament colour and type deliberately survive a print
ending, because the spool is still in the tool.

`SLEEP_HOT_THRESHOLD` remains as a safety net only: a hot nozzle keeps its screen
lit whatever the host believes. It has to sit **above chamber temperature**, or a
tool idling at chamber heat reads as busy and the screen never sleeps at all.

### Status reference

The device reports its own state back over the same serial link every two seconds
(`REPORT_PERIOD_MS` in `src/user_conf.h`), so it is available to macros and to the
Moonraker API as `printer["knomi_serial T0_knomi"]` (or `printer.knomi_serial` for an
unnamed section):

| Field | Description |
| --- | --- |
| `connected` | Host has the serial port open. |
| `port` | Configured serial path. |
| `module_version` | Version of this Klipper module. |
| `protocol_version` | Wire format version this module speaks. |
| `tool` | Normalised `tool:` value, e.g. `0`. `None` if unset. |
| `used` | Whether the running job uses this tool. |
| `filament_color` | Loaded filament colour as `RRGGBB`, or `None`. |
| `filament_type` | Loaded material name, or `None`. |
| `device_online` | Device has reported within the last 10 seconds. |
| `report_age` | Seconds since the last report, or `None`. |
| `firmware_version` | Version actually flashed on the device. |
| `device_protocol_version` | Wire format version the device speaks. |
| `protocol_match` | Whether the two protocol versions agree. |
| `build_variant` | `knomi` or `knomi_toolchanger`. |
| `sleep_state` | `awake`, `dim`, or `off`. |
| `screen` | `init`, `idle`, `printing`, or `shutdown`. |
| `page` | Index of the idle screen page in view. |
| `free_heap` / `min_free_heap` | Current and lowest-ever free heap, bytes. |
| `device_uptime` | Seconds since the device booted. |

Every device-side field is `None` until the first report arrives, so a device running
firmware older than this feature reads as `device_online: False` with a `None` version.

### Versioning

The canonical version is the `VERSION` file at the repo root. `scripts/version.py` runs
before each build and compiles it into the firmware, appending semver build metadata
when the tree is not a clean release build:

```
0.4.0                    clean tree, tagged v0.4.0
0.4.0+3.gd34db33         3 commits past the tag
0.4.0+3.gd34db33.dirty   ...with uncommitted changes
0.4.0+gd34db33           no matching tag
```

The Klipper module reads the same `VERSION` file, which works because `install.sh`
symlinks it into `klippy/extras` rather than copying it.

To cut a release: bump `VERSION`, commit, then `git tag -a v0.4.0 -m 0.4.0`. Moonraker's
update manager infers the repo version from that tag on its own — it needs at least one
tag in `vX.Y.Z` form, and no manifest file in this repo. Comparing the tag Moonraker
reports against `firmware_version` above is what tells you the device is due a reflash.

`printer::kProtoVersion` (`src/printer/printer.h`) and `_PROTO_VERSION`
(`klippy_extras/knomi_serial.py`) are separate from the release version and are bumped
only when the `State` packet layout changes. A mismatch is logged once to `klippy.log`
and surfaced as `protocol_match: False`.
