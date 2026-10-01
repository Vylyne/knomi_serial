# Design decisions

These are the reasons behind current contracts. Keep this page about choices
that future changes could accidentally undo; use the [README](../README.md)
for operator instructions, [architecture.md](architecture.md) for the system
map, and [protocol.md](protocol.md) for exact wire layouts.

## Identity is hardware, not location

The durable display ID is the low three bytes of the ESP32 eFuse MAC. The
CH340K has no useful USB serial number, and `/dev/ttyUSB*`, by-id suffixes, and
hub paths can change without the display changing. A `device_id:` section must
not silently fall back to an unrelated port. Klipper, the watcher, and flashing
tools may use a port as a current location, but an updater rechecks the ID
before a write. This avoids showing one tool's state on another display or
flashing the board that happened to take an old port. See
[mcu-updater.md](mcu-updater.md).

## Shared state has one Klipper owner

`KnomiCluster` computes printer-wide and per-tool job state once, then each
display section projects what it needs. It also registers every section in one
device map for consumers such as mcu-updater. Duplicating discovery or shared
state in each section would multiply work and make sections disagree about the
same printer. An invalid section fails Klipper configuration rather than
leaving a deceptively partial device map.

## One firmware, runtime page order

The `knomi` image contains the pages; `printer.cfg` chooses their order and
therefore the landing page. An earlier build flag saved little flash at the
cost of two images to build, test, and select. E-stop is appended outside the
idle row so omitting it from `pages:` cannot remove it. Settings sent by the
host override compiled defaults only when their `present` bit is set, keeping
firmware defaults meaningful. See [protocol.md](protocol.md#config--388-bytes).

## Separate changing state from configuration

The host sends the compact changing `STATE` frame at 10 Hz and sends `CONFIG`
only on request. The device compares the host's config CRC with the payload it
actually adopted and requests a replacement when they differ. This is cheaper
than resending static macros and layout on every tick, and makes a lost frame,
restart, or config edit converge by the same mechanism. Both CRCs are exposed
in section status so intended and applied configuration are distinguishable.

## A button has a source, a resolver, and optional visible slot

`touch`, `gpio`, and host `event` describe where an edge came from; `page`,
`gcode_macro`, and `observe` describe what it does. `internal` is reserved,
not silently accepted. A slotless observed external input can animate locally
without taking a page's touch position. A visible shared binding takes only
its own slot, not the page. Bare macro resolvers fire on committed release;
edge-specific macros can fire on press or release. Cancel is not release. The
legacy `hardware_keys` mask stays as touch suppression for old configs, not a
second button configuration language.

The firmware receives compact indexed records, never macro names. Klipper
validates names and targets before sending an event and checks the config CRC,
index, and configured edge before executing a returned macro. The UI drops
queued edges observed under an older CRC. These checks make config replacement
safe even when input and UI tasks interleave. Exact records and commands are
in [protocol.md](protocol.md#button_event--6-bytes).

## Keep input dispatch in the UI task

Host-forwarded and GPIO edges share one bounded input queue rather than a
second receive-only queue. This gives both producers the same lifecycle path
and keeps LVGL callbacks on the UI task, at the cost of shared backpressure.
If the queue overflows, the UI cancels active inputs and clears it; GPIO does
not consume an edge it failed to enqueue, so it can retry after the next poll.
This is safer than losing a release and leaving a button logically held.

All idle pages coexist, so page actions register with their page as owner,
not as one global callback per slot. Touch supplies that exact owner; an
external button resolves the page currently on the glass, then keeps the
selected action through its press lifecycle. This avoids routing a release
to a different page after a swipe. Live swipe behavior still needs the
checks in [verification.md](verification.md#live-acceptance-checks).

Config-change cancellation belongs to the UI task. The GPIO task releases
and reconfigures pins and produces debounced edges, while the UI cancels held
actions and rejects edges carrying an older config CRC. A held switch during
a config swap remains a live-hardware verification case, not a proven result.

E-stop is a full-page, two-tap control with its own press/release/cancel
lifecycle, not one of the five semantic button slots. A future non-touch
E-stop would need an explicit binding; it must not silently inherit a corner
slot's dispatch. The display control is not a physical power-cut E-stop.

## Home follows the physical positions; Move does not

Home has four diagonal actions matching optional physical switches and a
touchable centre home-all icon. A shared binding retains the physical slot;
its displaced Home action becomes a separate touch-only target instead of
pretending that target still aligns with the switch. The all-slots-claimed
case is rejected rather than silently hiding a homing action. Move has six
directions, so a four-button diagonal layout would misrepresent its controls.
It uses an XY pad and a separate Z pair. When a visible shared right-hand
binding needs room, both Z controls stay on the edge and move closer together.
These layouts were judged on the actual 240-pixel, 32.4 mm round panel rather
than desktop-sized mockups.

Home uses an opaque black mark with a white symbol until an axis is homed.
Homed axes and home-all use a solid machine-accent mark instead of an extra
ring; QGL/ZTA remains neutral until Klipper reports its adjustment as applied.
That state is distinct from axis homing and follows the selected QGL or Z-tilt
object. Accent symbols
use whichever of black or white has greater sRGB contrast. The UI caches that
derived ink when the saved config loads and when the accent changes, without
adding a field to the fixed wire or flash payload. The shutdown RESTART button
uses the same accent/ink pair so a changed machine colour does not leave it
with an unrelated fixed fill.

## Keep the serial tool contract narrow and explicit

The display does not run a Moonraker or Wi-Fi client. Klipper owns its serial
port while running; the watcher offers an identity map when Klipper is down.
Discovery helpers in `klippy_extras/knomi_serial.py` must import outside
Klipper because updater and bench tools call them directly. The live cluster
map, watcher fallback, and pre-flash verification have different freshness
and authority; none should be silently treated as another. Their stable fields
and function signatures are specified in [mcu-updater.md](mcu-updater.md).
