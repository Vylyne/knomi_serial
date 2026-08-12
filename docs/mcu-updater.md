# Integrating with a firmware updater

What an updater needs to know to find these displays, tell them apart, decide
whether they are out of date, and flash them.

## What a display is

Every display has a permanent hardware id: **six lowercase hex characters**, the
low three bytes of the ESP32's eFuse MAC. `19aa44` for a board whose MAC is
`cc:ba:97:19:aa:44`, so it can be checked against `esptool chip_id` directly.

It is burned into the chip. It survives a firmware upload, an `erase_flash`, and
a change to the partition table, and it is the same whichever USB socket the
display is in. It is the only stable name a display has — the CH340K on these
boards reports **no USB serial number**, so every path-based identifier names a
socket rather than a device.

Compare ids case-insensitively. They are emitted lowercase and normalised
lowercase at both ends, but do not depend on it.

## Finding the displays

Three sources. Take the first that is available — the order matters, and getting
it backwards fails quietly.

### 1. Klipper is up → ask Klipper

```sh
curl -s 'localhost:7125/printer/objects/query?knomi_cluster'
```

```json
{"T0_knomi": {"device_id": "19aa44",
              "port": "/dev/ttyUSB0",
              "addressed_by": "device_id",
              "build_variant": "knomi",
              "firmware_version": "0.5.0+54.g5509d4f",
              "protocol_version": 5,
              "online": true,
              "tool": "0"}}
```

Under `knomi_cluster.devices`, keyed by printer.cfg section name.

Prefer this whenever Klipper is running, for three reasons. It is **live** —
`online` is derived from how recently the display actually reported, not from
when something last looked. It is the **only** source that can see the ports
Klipper holds, because a serial port can only be read by one program at a time.
And it is the only place **section names** exist; nothing else knows what
`T0_knomi` means.

A section that has never answered still appears, with `online: false` and null
versions. Do not treat a missing `firmware_version` as "nothing to do" — a
display that cannot be reached is exactly the one that may need reflashing.

### 2. Klipper is down → read the watcher's map

Check the service first, because nothing in the file will tell you it is stale:

```sh
systemctl is-active knomi_serial
```

Then `~/printer_data/knomi/devices.json`:

```json
{"version": 1,
 "devices": {"19aa44": {"port": "/dev/ttyUSB0",
                        "fw": "0.5.0+54.g5509d4f",
                        "var": "knomi"}}}
```

Ignore the file if `version` is not one you know. There are deliberately no
timestamps: the file's mtime says when it last changed, and an entry existing
means the display was identified during the current run of the service and its
port has not disappeared since.

**This is the case flashing actually needs.** esptool requires the port to
itself, so Klipper has to be stopped — which is precisely when source 1 stops
existing. That is the whole reason the service exists.

### 3. Neither → scan

Nothing is holding a port, so open them all:

```sh
python3 scripts/discover.py
```

or from Python, which is what that script does:

```python
import knomi_serial
knomi_serial.discover_reports()      # {id: {port, fw, var, ...}}
```

Takes about a second per pass — displays broadcast a status line every two
seconds unprompted, so this listens rather than asking.

## Deciding whether to flash

`firmware_version` / `fw` is the `VERSION` file plus git build metadata:

| | |
| --- | --- |
| `0.5.0` | clean tree, sitting on tag `v0.5.0` |
| `0.5.0+54.g5509d4f` | 54 commits past the tag, at `5509d4f` |
| `0.5.0+54.g5509d4f.dirty` | ...with uncommitted changes |

So a display is current when its `fw` equals what the checkout would build now.
Anything with `.dirty` was built from an uncommitted tree and cannot be compared
meaningfully — treat it as unknown rather than as out of date.

`build_variant` / `var` is the PlatformIO environment it was built from. For
these displays that is **`knomi`** — there is one firmware for every machine.
(`knomi_i2cscan` is a bench build and should never be flashed to a printer.)

## Flashing

```sh
sudo systemctl stop klipper
sudo systemctl stop knomi_serial
pio run -e knomi -t upload --upload-port /dev/ttyUSB0
sudo systemctl start knomi_serial
sudo systemctl start klipper
```

Stopping Klipper is required: it holds its ports and takes an advisory `flock`,
which esptool also takes, so the upload would fail outright.

Stopping the watcher is not strictly required but makes it deterministic. The
watcher only opens a port that is new and not yet identified this run, so it is
usually holding nothing — but if a port appears at the moment esptool wants it,
one of them loses the lock and the upload fails. The failure is clean and
retrying works; stopping it removes the race.

Flashing by identity means resolving id → port from one of the three sources
above **at flash time**, not from a remembered path. A remembered path is the
thing this whole scheme exists to avoid.

## Afterwards

The display re-enumerates, which the watcher notices as a new port and
re-identifies, picking up the new version. Confirm by re-reading whichever
source applies — the `fw` in the next status line is the flashed firmware
reporting its own version, so it is proof rather than inference.

## Gotchas

**Never open a port without `exclusive=True`.** pyserial implements it as
`flock(LOCK_EX|LOCK_NB)`, which is advisory — it excludes only the processes
that also ask. Klipper's sections, esptool, `discover.py` and the watcher all
take it. A reader that skips it does not fail against a busy port, it *shares*
the byte stream with whoever has it, and both get a random subset. The symptom
is intermittent, slow, and looks like flaky hardware.

**Two sections cannot address one display.** Klipper refuses to start if the
config names one twice, so an updater rewriting printer.cfg must not add a
`device_id:` for a display already covered by a `serial:` path.

**A port is not an identity.** `/dev/ttyUSB0` moves between reboots,
`/dev/serial/by-id/` disambiguates identical CH340s with a suffix the *kernel*
assigns by enumeration order, and a udev rule keyed to a USB path breaks when a
hub moves. Always resolve to `device_id` and address by that.
