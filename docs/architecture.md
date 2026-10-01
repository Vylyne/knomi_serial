# Architecture and scope

Knomi_Serial is a toolchanger-first display system for Knomi V2 hardware. One
firmware image serves each display; Klipper decides at runtime which tool,
readouts, pages, and controls it shows. A single-display printer uses the same
path. This document describes the current system and its boundaries, not a
proposed implementation plan. Operator configuration lives in the
[README](../README.md); byte layouts live in the [wire contract](protocol.md).

## Goals and boundaries

- Give each tool a small, legible local display without making the display a
  second source of printer truth. Klipper supplies shared job and motion state.
- Keep display identity stable when USB ports move. Identify a board by its
  six-hex-character ESP32 hardware ID, then resolve its current port.
- Let touch, a directly wired switch, or a host-forwarded event use one button
  lifecycle while keeping macros and their names on the Klipper host.
- Fail invalid configuration at Klipper startup, and make firmware/module
  protocol and configuration mismatches visible.

The display has no Wi-Fi or Moonraker client. It uses the board's CH340K serial
link at 115200 baud. Only one process may own that port at a time. The supported
firmware build is the `knomi` PlatformIO environment; page selection is runtime
configuration, not a separate image. The hardware and pin limits are in
[hardware.md](hardware.md).

## Responsibilities and data flow

| Part | Owns | Does not own |
| --- | --- | --- |
| `KnomiCluster` in the Klipper extra | Shared printer/tool state, coordinated discovery, display registration, `KNOMI_TOOL` and `KNOMI_BUTTON` addressing | A separate copy of shared state per display |
| Each `knomi_serial` section | Its address, validated configuration, serial connection, button-name table, and status | Other sections' port or tool identity |
| ESP32-S3 firmware | Rendering, touch/GPIO sampling, button lifecycle, and the configuration it actually adopted | Klipper macro names or printer-wide truth |
| `knomi_serial` watcher | An identity-to-port map while Klipper cannot answer | Authority to flash a remembered port without rechecking identity |

The host sends frequent printer `STATE` frames and a `CONFIG` frame when the
device requests it. The device reports status and commands over the same serial
link. The config payload CRC lets both sides distinguish the host's intended
configuration from the one the device adopted. Protocol 6 carries compact
button records and CRC-scoped event indexes; names and macro text stay on the
host. See [protocol.md](protocol.md) for framing and [decisions.md](decisions.md)
for why these boundaries exist.

`printer.knomi_cluster.devices` is the live, complete list of configured
display sections when Klipper is ready. Each entry identifies the full config
section, durable hardware ID, current port, firmware/protocol version, online
state, and tool. A section's own status adds detailed runtime diagnostics and
the host/device config CRCs. When Klipper is down, the watcher's `devices.json`
is a useful hint, but a flashing tool must verify identity again at the port
before writing. The exact fields, fallback order, and importable discovery API
are commitments in [mcu-updater.md](mcu-updater.md), not in this overview.

## Pages and input ownership

The idle pages are an ordered horizontal row selected by `pages:`. Emergency
stop sits on the vertical axis and cannot be omitted by that row. The printing
screen is separate. Home uses NW/NE/SW/SE for X/Y/tramming/Z, with home-all at
the centre. Move keeps four-way XY and a distinct Z pair; six jog actions are
not forced into four diagonal buttons. Both use the round visual language and
large touch targets of the other pages. A visible shared NE or SE binding moves
the *pair* of Move Z controls closer together along the right edge, without
creating a permanent shared touch action.

The semantic button slots are NW, NE, SW, SE, and C. A named binding may claim
one slot or be slotless. A shared binding owns its claimed position; the page
keeps actions in unclaimed positions. On Home, a displaced action gets a
separate touch-only control, and configurations that would hide all five Home
actions are rejected. On Move, shared marks have smaller touch regions so they
do not cover jog targets. The legacy `hardware_keys` mask only suppresses touch
at a position; it does not define an input or synthesize an action. The E-stop
page hides shared legends, while wired and host-forwarded inputs still run.

The UI theme keeps a RAM-only machine-accent palette: the accent and its
contrasting black/white ink are derived on startup from the loaded config and
updated on accent changes. The wire config remains the source of truth; it does
not carry a derived ink field. Home receives the selected QGL/Z-tilt object's
`applied` status in the shared state tick, independently of axis homing, and
uses the accent palette for that control only while adjustment is applied.

Touch, GPIO, and host events normalize to press, committed release, or cancel.
The UI task owns lifecycle and page/observation callbacks; a GPIO or serial
task never calls LVGL. Duplicate presses and unmatched releases are inert.
Config replacement, link loss, and touch press-loss cancel a held action;
cancel does not execute a release macro. Queued input edges carry the config
CRC observed by their producer and are discarded if the UI's current config
differs. This prevents an old edge from being reinterpreted as a new binding.
GPIO uses active-low pull-ups and a 20 ms stable-state debounce; its live switch
behavior remains to be verified. The README defines the user-facing button
syntax, and [verification.md](verification.md) tracks the remaining checks.

## Current limits

- The wire currently describes tool-oriented state, not arbitrary machine-wide
  panels. Sensor-only templates are an idea, not a supported config option;
  the deferred design is separated from the current contract in
  [protocol.md](protocol.md#future-screen-templates).
- `internal` is reserved as a button resolver but rejected by configuration
  until an action exists. `observe` currently supports FEED and RETRACT and
  never initiates either action.
- Protocol versions are not mixed; firmware and Klipper extra must agree.
- The Home/Move layouts were captured on a real panel and their touch controls
  checked. Swipe/shared-button routing, command-level action counts, and live
  GPIO behavior remain open; see [verification.md](verification.md).
