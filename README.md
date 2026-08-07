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
