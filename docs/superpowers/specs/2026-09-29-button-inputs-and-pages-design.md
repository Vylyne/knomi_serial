# Button Inputs and Home/Move Pages Design

**Date:** 2026-09-29

## Purpose

Replace the inherited Home and Move screens with layouts that belong to the
current Knomi visual system, and introduce one input model that can drive page
actions from touch, display GPIO, or a host-forwarded external event.

The work is deliberately split into three independently reviewable slices:

1. make `scripts/screenshot.py` select a page without changing the protocol;
2. add the button configuration, lifecycle, and protocol contract;
3. build the new Home and Move pages on that contract.

## Product language

The round 240 x 240 display is not a tiny rectangular control panel. Existing
current pages use a black round canvas, large central content, restrained
machine/filament/heat colour roles, minimal chrome, and small diagonal action
marks with hit regions larger than the marks. Home and Move must use the same
language.

### Home

Home uses the five positions that map to the proposed hardware:

| Slot | Action | Label |
| --- | --- | --- |
| NW | Home X | `X` |
| NE | Home Y | `Y` |
| C | Home all axes | home icon |
| SW | Gantry tramming | `QGL` or `ZTA` |
| SE | Home Z | `Z` |

There is no `HOME` title. The centre icon is the page identity and its primary
action. The four diagonal marks align with the physical/soft positions used on
other pages. A homed axis gains a restrained warm ring/dot rather than turning
the control into a pastel rectangular button. Busy actions remain visibly
disabled and still refuse behind the handler, because a hardware input cannot
be visually disabled.

The present 106 x 106 corner touch regions overlap a centre target. Home uses a
five-target geometry of approximately 72 x 72 pixels per diagonal and a
72-pixel centre target. Final dimensions are judged on a real 32.4 mm display:
240 pixels is about 7.4 px/mm, so 72 pixels is about 9.7 mm.

When a shared configured binding owns a diagonal slot, it wins that slot. On a
touch display, the displaced Home action is rendered as an inward touch-only
control without pretending it is still aligned to a physical switch. A future
non-touch target must reject a Home configuration that cannot expose every
action; silently dropping a homing action is not acceptable.

### Move

Move does not force six actions into four diagonal slots. It uses an inner
orbit: an XY directional pad on the left/centre and an independent vertical Z
rocker on the right. There is no `MOVE` title; the control geometry identifies
the page and the reclaimed upper space keeps the targets usable.

Targets are approximately 56 x 56 pixels (7.6 mm) with marks around 38 pixels
(5.1 mm). Opposing directions are spatially opposed, and X/Y are visually one
group while Z is visibly separate. Existing `MOVE:X+`, `MOVE:X-`, and sibling
commands remain unchanged.

## Input vocabulary

There are three input sources:

- `touch`: a soft target on the display;
- `gpio`: a switch connected directly to display GPIO;
- `event`: a lifecycle forwarded from Klipper with `KNOMI_BUTTON`.

There are four resolvers:

- `page`: the action registered by the current page for a slot;
- `gcode_macro`: a macro owned and executed by Klipper;
- `observe`: an external action Knomi observes for local animation only;
- `internal`: a Knomi-owned lifecycle action. The wire vocabulary is reserved
  in this slice; configuration rejects it until a named internal action is
  implemented rather than accepting a resolver that does nothing.

`output` is a GPIO property, not a button resolver, and is outside this work.

## Klipper configuration

Buttons are prefixed options in a display's existing section. The suffix is the
stable name used by `KNOMI_BUTTON`:

```ini
[knomi_serial T0_knomi]
button_feed:
    source=gpio
    pin=GPIO5
    resolver=observe FEED
    slot=NW

button_check_filament:
    source=touch
    resolver=gcode_macro CHECK_TOOL_FILAMENT_SENSORS
    slot=C
    legend=CHK
```

Klipper's multiline value is parsed as one `key=value` property per non-empty
line. Property names and enum values are case-insensitive; button names retain
their config spelling for diagnostics but are addressed case-insensitively.

Allowed properties are `source`, `pin`, `slot`, `legend`, `resolver`,
`press_resolver`, and `release_resolver`. Every invalid configuration fails at
startup and names the section, button option, property, and invalid value.

Rules:

- `source` is required and is one of `touch`, `gpio`, or `event`.
- `pin` is required only for `gpio`; it accepts `GPIO5` or `5` and is
  normalized to the integer pin.
- GPIO input is active-low with the ESP32 internal pull-up in this first slice.
  Pins already used by the board, package-reserved pins, and unsafe strapping
  pins are rejected. GPIO10 is rejected because its fitted pulldown defeats the
  internal pull-up. The supported Knomi V2 set is GPIO5, GPIO6, GPIO8, GPIO9,
  GPIO11, GPIO15, GPIO38-GPIO42, GPIO47, and GPIO48; documentation calls out
  the four preferred pins 5, 6, 8, and 9.
- `slot` is `NW`, `NE`, `C`, `SW`, `SE`, or `NONE`. `NONE` means the input has
  no on-screen legend. A `page` or `observe` resolver requires a visible slot.
- A visible `gcode_macro` resolver requires `legend`, one to four printable
  ASCII characters. A slotless macro forbids it. Page and observe resolvers
  derive their icon/text from the current page or named observe profile and
  therefore forbid `legend`.
- One slot may be claimed by at most one configured button.
- Bare `resolver` is mutually exclusive with either edge-specific property.
- `press_resolver` and `release_resolver` accept only `gcode_macro NAME`.
- `page`, `observe`, and `internal` own their entire lifecycle and therefore
  appear only as bare `resolver` values.
- `observe` initially accepts only `FEED` and `RETRACT` and cannot use
  `source=touch`, because Knomi must not synthesize the external action it is
  supposed to observe.
- `gcode_macro` requires a non-empty macro name. Bare `resolver` fires on a
  committed release. Edge-specific macros fire on their named edge.
- A configured slot and the legacy `hardware_keys` option may not claim the
  same position. `hardware_keys` remains as compatibility-only touch
  suppression and is documented as deprecated after this feature lands.

The maximum is eight configured buttons. Five can occupy visible slots and the
remaining entries permit slotless event or macro inputs without making the
config payload variable-length.

## Lifecycle contract

All sources normalize to `(button index, source, phase)` where phase is
`press`, `release`, or `cancel`. The resolver owns the active press until that
lifecycle ends.

A committed release means:

- touch pressed inside its target and released inside it without LVGL cancel;
- GPIO reached the pressed debounce state and later the released debounce
  state;
- event received `PRESSED=1` and later `PRESSED=0`.

An unmatched release is ignored. Repeated press events while already active
are ignored. A config replacement, link disconnect, deleted touch object, or
LVGL press-lost event cancels the lifecycle. Cancel never fires a release macro.
`observe` receives cancel so an animation cannot remain stuck.

GPIO is polled by a small input task and debounced for 20 ms. It does not call
LVGL. GPIO and received host events enqueue normalized phases; the UI task
drains them so page and observe callbacks always run in the LVGL task. Touch
enters the same dispatcher directly from the UI task.

## Slot ownership and page actions

A configured binding owns only its declared slot. It does not replace the
whole page deck. Shared bindings are installed first and win collisions. Each
page then registers the actions it can expose in unclaimed slots.

An unclaimed page slot receives a touch target. A slot backed by `gpio` or
`event` remains a visible legend but its touch target is suppressed. A
configured `source=touch` binding gets a touch target and its configured
legend. Page code registers actions with the input system instead of attaching
direct clicked callbacks, so touch and GPIO reach exactly the same guard and
command.

Configured shared bindings are fixed to the glass, not to a horizontally
scrolling page. Idle and Printing screens create a non-scrolling overlay above
their page content. Page-owned marks remain within their pages and appear only
where a shared overlay has not claimed the slot.

`observe FEED` and `observe RETRACT` are the first shared profiles. They update
local visual state only; they never emit a command or macro.

## Wire contract: protocol 6

The protocol version becomes 6 because `CONFIG` changes and a downstream event
frame is added.

### Config records

`CONFIG` adds `button[8]`, each a twelve-byte fixed record:

| Byte | Field | Meaning |
| --- | --- | --- |
| 0 | source | none/touch/gpio/event |
| 1 | slot | none/NW/NE/C/SW/SE |
| 2 | resolver | none/page/gcode_macro/observe/internal |
| 3 | flags | bare/press macro/release macro; reserved bits zero |
| 4 | pin | GPIO number, or 0xff when not GPIO |
| 5 | argument | observe/internal enum, otherwise zero |
| 6-9 | legend | up to four ASCII bytes, NUL-padded |
| 10-11 | reserved | zero |

The array adds 96 bytes, taking the fixed config payload from 292 to 388 bytes.
`source=none` terminates unused records. The existing content CRC continues to
cover the complete payload. A new `kHasButtons` presence bit determines whether
the records override the compiled empty default. Names and macro strings do not
go to the device.

The host retains an ordered `ButtonBinding` table matching the record indexes.
Indexes are stable for the lifetime of one config payload and have no meaning
without its CRC.

### Host-forwarded event

Frame type `BUTTON_EVENT` (`0x05`) carries:

```
config_crc:uint32  button_index:uint8  pressed:uint8
```

The device accepts it only when the CRC equals the config it currently holds,
the index exists, and the binding source is `event`. This prevents an event
queued against an old configuration from activating a different button after a
reload.

The host command is:

```gcode
KNOMI_BUTTON SCREEN=T0_knomi BUTTON=feed PRESSED=1
KNOMI_BUTTON SCREEN=T0_knomi BUTTON=feed PRESSED=0
```

It uses the existing `SCREEN=`/`TOOL=` addressing rules. The command validates
all targets and values before writing any frame, so a bad multi-screen request
cannot partially apply.

### Device-to-host macro event

For a `gcode_macro` resolver, the device emits:

```
KNOMI_CMD:BUTTON:<config-crc-hex>:<button-index>:<P|R>
```

The host ignores a CRC that is not its current config, an out-of-range index,
an edge not configured for that binding, or a binding that is not a macro.
The macro name is looked up in the host table and passed to Klipper's existing
`run_script` path. Bare resolvers emit only `R`; edge-specific resolvers emit
the configured edge or edges.

## Compatibility and documentation

Protocol 5 and 6 do not attempt mixed operation; existing mismatch reporting
already tells the operator to update the firmware/module pair. NVS config is
version-keyed, so protocol 5 records are not loaded as protocol 6 records.

Update `README.md`, `docs/protocol.md`, and `docs/hardware.md` in the same
change. Mark both existing TODO items complete only after host tests, firmware
build, live screenshots, and live GPIO verification have all succeeded. If
live GPIO hardware is unavailable, leave that checkbox open and report the
implementation as build-verified but hardware-unverified.

## Verification

- Unit tests cover config parsing, every invalid combination, section/option/
  value diagnostics, command addressing, stale CRC rejection, and protocol
  layout parity.
- The firmware build covers struct layout assertions and all C++ integration.
- Screenshot tests cover page selection without serial hardware.
- COM5 captures verify Home and Move on the real 1.28-inch panel after explicit
  flash authorization.
- A live switch on a supported GPIO verifies press, debounce, release, cancel
  on config replacement, touch suppression, and one macro invocation per
  committed lifecycle. This evidence is required before claiming direct GPIO
  support complete.
