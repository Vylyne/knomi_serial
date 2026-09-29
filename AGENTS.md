# AGENTS.md

## Scope

Knomi_Serial has three cooperating parts:

- ESP32-S3 firmware in `src/`, built with PlatformIO.
- The Klipper extra in `klippy_extras/knomi_serial.py`.
- The udev-driven host watcher in `service/`.

Keep changes within the requested part unless a shared contract requires both
sides to move together.

## Read before editing

- Start with `README.md` for supported behavior and operator-facing contracts.
- Read `docs/protocol.md` before changing packets, fields, framing, or versions.
- Read `docs/hardware.md` before changing pins or hardware assumptions.
- Read `docs/mcu-updater.md` before changing identity, discovery, status, or
  flashing integration.
- Read `service/README.md` before changing the watcher or its installation.

## Project invariants

- The display uses a direct 115200-baud USB serial link. Do not add a WiFi or
  Moonraker dependency to the firmware.
- A six-hex-character ESP32 hardware ID is the durable display identity. The
  CH340K has no usable USB serial number; `/dev/ttyUSB*`, by-id suffixes, and
  physical USB paths identify ports, not displays.
- Keep `serial:` support, but do not silently substitute a path for a configured
  `device_id:` or guess when identity evidence conflicts.
- There is one normal firmware environment, `knomi`. Page selection belongs in
  runtime configuration; do not restore per-layout firmware variants.
- `_PROTO_VERSION` and `printer::kProtoVersion` describe one wire contract.
  Update both sides, their layout assertions, `docs/protocol.md`, and protocol
  tests whenever that contract changes.
- Shared printer state is computed once by `KnomiCluster`. Avoid adding
  per-display copies of work that is identical across the row.
- Invalid `printer.cfg` values should fail at configuration/startup with a
  message naming the section, option, and value. Do not defer deterministic
  configuration failures into the periodic update loop.
- Klipper, discovery tools, simulators, screenshot tools, and esptool must not
  compete for a display port. Stop the owning services before direct hardware
  access.

## Conventions

- Keep the Python code compatible with the version in `ruff.toml` and avoid new
  runtime dependencies unless the feature truly requires one.
- Reuse the module's encoders in bench tools instead of creating a second wire
  implementation.
- Preserve the installer's symlink-based layout and Moonraker update-manager
  behavior.
- Keep README `## Features` checkboxes and `## TODO` synchronized with actual
  behavior. Do not mark hardware work complete without live-device evidence.
- Preserve unrelated working-tree changes. Do not commit, push, tag, publish,
  deploy, or flash unless the user asks.

## Verification

Install host-side test tools with:

```sh
python -m pip install -r requirements-dev.txt
```

For a focused change, run its nearest test first. Before completion, run the
full gate used by CI:

```sh
for test in tests/test_*.py; do python "$test" || exit 1; done
ruff check .
pio run -e knomi
```

On PowerShell, run the test scripts with:

```powershell
$failed = $false
Get-ChildItem tests/test_*.py | Sort-Object Name | ForEach-Object {
    python $_.FullName
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) { exit 1 }
ruff check .
pio run -e knomi
```

`tests/test_moonraker_conf.py` requires `awk`. On Windows, Git for Windows
normally provides it under `C:\Program Files\Git\usr\bin`; add that directory
to the test process's `PATH` when necessary.

Report firmware compilation and host tests separately from any live Klipper or
display verification.
