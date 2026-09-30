#!/usr/bin/env python3
"""The firmware and the Klipper module must agree about the wire.

`src/printer/printer.h` carries static_asserts that pin the packet layout, but
they only check C++ against itself. Nothing checked C++ against Python, and the
two describe the same bytes in different languages - a field added to `struct
State` without a matching change to `_STATE_FMT` compiles clean, installs clean,
and produces a display rendering garbage from the wrong offsets.

This reads the constants straight out of the header and compares them with the
module's. Regexes over C++ are usually a bad idea; here they are reading a
handful of declarations that exist specifically to be the contract, and a
declaration reformatted enough to break the parse fails the test loudly rather
than passing it quietly.

    python tests/test_protocol.py        # or: pytest tests/
"""

import os
import re
import struct
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_ROOT, "klippy_extras"))

import knomi_serial as k  # noqa: E402

_HEADER = os.path.join(_ROOT, "src", "printer", "printer.h")

with open(_HEADER, encoding="utf-8") as f:
    SOURCE = f.read()


def const(name):
    """A `static const <type> name = <value>;` from the header."""
    m = re.search(
        r"static\s+const\s+\w+(?:\s+\w+)*\s+" + name + r"\s*=\s*([^;]+);", SOURCE)
    if not m:
        raise AssertionError(f"{name} not found in printer.h")
    return int(m.group(1).strip().rstrip("uU"), 0)


def enum_values(name):
    """`enum [class] name [: type] { ... }` as {member: value}.

    Handles both plain integers and the `1u << n` form the bitmasks use.
    """
    m = re.search(
        r"enum\s+(?:class\s+)?" + name + r"\s*(?::\s*\w+\s*)?\{(.*?)\}",
        SOURCE, re.S)
    if not m:
        raise AssertionError(f"enum {name} not found in printer.h")

    out = {}
    for member, value in re.findall(r"(\w+)\s*=\s*([^,\}]+)", m.group(1)):
        text = value.strip().rstrip("uU")
        shift = re.match(r"^(\w+)\s*<<\s*(\w+)$", text)
        if shift:
            out[member] = int(shift.group(1).rstrip("uU"), 0) << int(shift.group(2), 0)
        else:
            out[member] = int(text, 0)
    return out


def check(label, firmware, module):
    if firmware != module:
        raise AssertionError(
            f"{label}: printer.h says {firmware!r}, knomi_serial.py says {module!r}")


# --------------------------------------------------------------------------
# the checks
# --------------------------------------------------------------------------

def test_protocol_version():
    check("protocol version", const("kProtoVersion"), k._PROTO_VERSION)


def test_payload_sizes():
    # The two that matter most: a disagreement here is a display reading every
    # field from the wrong offset.
    check("state payload size", const("kStateWireSize"), k._STATE_SIZE)
    check("config payload size", const("kConfigWireSize"), k._CONFIG_SIZE)


def test_string_field_lengths():
    check("filament type length",
          const("kFilamentTypeMaxLen"), k._FILAMENT_TYPE_MAX_LEN)
    check("gcodes length", const("kGcodesMaxLen"), k._GCODES_MAX_LEN)
    check("message length", const("kMessageMaxLen"), k._MESSAGE_MAX_LEN)


def test_frame_types():
    frames = enum_values("Frame")
    check("kState", frames["kState"], k._FRAME_STATE)
    check("kConfig", frames["kConfig"], k._FRAME_CONFIG)
    check("kMessage", frames["kMessage"], k._FRAME_MESSAGE)
    # 0x04 is the snapshot request, which the module does not send - only
    # scripts/screenshot.py does. Named here so that renumbering the enum has
    # to come past this test.
    check("kSnapshot", frames["kSnapshot"], 0x04)
    check("kButtonEvent", frames["kButtonEvent"], k._FRAME_BUTTON_EVENT)


def test_status_values():
    status = enum_values("Status")
    for member, value in status.items():
        name = member[1:].upper()  # kIdle -> IDLE
        check(f"Status::{member}", value, k.PrinterStatus[name].value)


def test_tram_values():
    for member, value in enum_values("TramType").items():
        name = member[1:].upper()
        check(f"TramType::{member}", value, k.PrinterTramType[name].value)


def test_config_presence_bits():
    bits = enum_values("ConfigHas")
    expected = {
        "kHasColorMachine": k._HAS_COLOR_MACHINE,
        "kHasColorUnknown": k._HAS_COLOR_UNKNOWN,
        "kHasDimMs": k._HAS_DIM_MS,
        "kHasSleepMs": k._HAS_SLEEP_MS,
        "kHasBrightness": k._HAS_BRIGHTNESS,
        "kHasDimBrightness": k._HAS_DIM_BRIGHTNESS,
        "kHasGcodes": k._HAS_GCODES,
        "kHasKeyMask": k._HAS_KEY_MASK,
        "kHasPageOrder": k._HAS_PAGE_ORDER,
        "kHasEstopAt": k._HAS_ESTOP_AT,
        "kHasReadouts": k._HAS_READOUTS,
        "kHasButtons": k._HAS_BUTTONS,
    }
    check("presence bit count", sorted(bits), sorted(expected))
    for member, value in expected.items():
        check(member, bits[member], value)


def test_page_ids():
    pages = enum_values("Page")
    check("page count", const("kMaxPages"), k._MAX_PAGES)
    # kNone terminates the list and kEstop is appended by the device rather than
    # listed, so neither is a name the host can be asked for.
    named = {name: value for name, value in pages.items()
             if name not in ("kNone", "kEstop")}
    check("page names", sorted(n[1:].lower() for n in named), sorted(k._PAGES))
    for member, value in named.items():
        check(member, value, k._PAGES[member[1:].lower()])
    check("kNone is the terminator", pages["kNone"], 0)


def test_readout_ids():
    ids = enum_values("Readout")
    check("readout count", const("kMaxReadouts"), k._MAX_READOUTS)
    named = {n: v for n, v in ids.items() if n != "kNone"}
    check("readout names", sorted(n[1:].lower() for n in named),
          sorted(k._READOUTS))
    for member, value in named.items():
        check(member, value, k._READOUTS[member[1:].lower()])
    check("kNone is the terminator", ids["kNone"], 0)


def test_estop_positions():
    for member, value in enum_values("EstopAt").items():
        check(member, value, k._ESTOP_AT[member[1:].lower()])


def test_key_slot_bits():
    bits = enum_values("KeySlot")
    for member, value in bits.items():
        check(member, value, k._KEY_SLOTS[member[4:]])  # kKeyNW -> NW


def test_button_protocol_values():
    check("button count", const("kMaxButtons"), k._MAX_BUTTONS)
    check("button record size", const("kButtonWireSize"), k._BUTTON_SIZE)

    sources = enum_values("ButtonSource")
    for member, name in {
        "kNone": "NONE", "kTouch": "TOUCH", "kGpio": "GPIO",
        "kEvent": "EVENT",
    }.items():
        check(f"ButtonSource::{member}", sources[member], k.ButtonSource[name].value)

    slots = enum_values("ButtonSlot")
    for member, name in {
        "kNone": "NONE", "kNW": "NW", "kNE": "NE", "kC": "C",
        "kSW": "SW", "kSE": "SE",
    }.items():
        check(f"ButtonSlot::{member}", slots[member], k.ButtonSlot[name].value)

    resolvers = enum_values("ButtonResolver")
    for member, name in {
        "kNone": "NONE", "kPage": "PAGE", "kGcodeMacro": "GCODE_MACRO",
        "kObserve": "OBSERVE", "kInternal": "INTERNAL",
    }.items():
        check(
            f"ButtonResolver::{member}",
            resolvers[member],
            k.ButtonResolver[name].value,
        )

    profiles = enum_values("ButtonProfile")
    for member, name in {
        "kNone": "NONE", "kFeed": "FEED", "kRetract": "RETRACT",
    }.items():
        check(f"ButtonProfile::{member}", profiles[member], k.ButtonProfile[name].value)

    flags = enum_values("ButtonFlags")
    check("bare flag", flags["kButtonBare"], k._BUTTON_FLAG_BARE)
    check("press flag", flags["kButtonPress"], k._BUTTON_FLAG_PRESS)
    check("release flag", flags["kButtonRelease"], k._BUTTON_FLAG_RELEASE)


def test_frames_are_well_formed():
    """Framing is HEADER(4) TYPE(1) LEN(2) PAYLOAD FOOTER(4)."""
    cases = [
        ("state", k.encode_state(k.PrinterState()), k._FRAME_STATE, k._STATE_SIZE),
        ("config", k.encode_config(k.DeviceConfig()), k._FRAME_CONFIG, k._CONFIG_SIZE),
        ("message", k.encode_message("hi"), k._FRAME_MESSAGE, 2),
        ("button event", k.encode_button_event(0x12345678, 3, True),
         k._FRAME_BUTTON_EVENT, 6),
    ]
    for label, frame, want_type, want_len in cases:
        check(f"{label} header", frame[:4], k._HEADER)
        check(f"{label} footer", frame[-4:], k._FOOTER)
        frame_type, length = struct.unpack("!BH", frame[4:7])
        check(f"{label} type", frame_type, want_type)
        check(f"{label} declared length", length, want_len)
        check(f"{label} actual length", len(frame), 11 + want_len)


def test_message_is_truncated_not_overrun():
    """A long message must be cut to fit the device's buffer, not sent whole."""
    frame = k.encode_message("x" * 400)
    _, length = struct.unpack("!BH", frame[4:7])
    if length > k._MESSAGE_MAX_LEN:
        raise AssertionError(
            f"message of {length} bytes exceeds kMessageMaxLen "
            f"{k._MESSAGE_MAX_LEN}; the device would truncate it mid-frame")


def test_config_crc_is_over_the_payload_alone():
    """The device hashes the bytes it receives, which are the payload only."""
    config = k.DeviceConfig(present=k._HAS_GCODES, gcodes=b"HOME")
    payload = k.config_payload(config)
    frame = k.encode_config(config)
    check("payload is the frame body", payload, frame[7:-4])


def test_button_records_are_fixed_width_and_zero_padded():
    binding = k.ButtonBinding(
        name="feed",
        source=k.ButtonSource.GPIO,
        slot=k.ButtonSlot.NW,
        resolver=k.ButtonResolver.OBSERVE,
        pin=5,
        argument=k.ButtonProfile.FEED.value,
    )
    config = k.DeviceConfig(present=k._HAS_BUTTONS, buttons=(binding,))
    payload = k.config_payload(config)
    check(
        "first record",
        payload[36:48],
        bytes((2, 1, 3, 0, 5, 1, 0, 0, 0, 0, 0, 0)),
    )
    check("unused records", payload[48:132], bytes(84))


def test_visible_macro_record_carries_only_its_short_legend():
    binding = k.ButtonBinding(
        name="lights",
        source=k.ButtonSource.TOUCH,
        slot=k.ButtonSlot.C,
        resolver=k.ButtonResolver.GCODE_MACRO,
        legend=b"LGT",
        macro="TOGGLE_LIGHTS",
        release_macro="TOGGLE_LIGHTS",
    )
    payload = k.config_payload(k.DeviceConfig(buttons=(binding,)))
    check(
        "macro record",
        payload[36:48],
        bytes((1, 3, 2, 1, 0xFF, 0)) + b"LGT\0\0\0",
    )
    if b"TOGGLE_LIGHTS" in payload:
        raise AssertionError("macro text leaked into the device config")


def test_button_event_payload_is_crc_scoped_and_big_endian():
    frame = k.encode_button_event(0x12345678, 3, False)
    check("event payload", frame[7:-4], b"\x12\x34\x56\x78\x03\x00")


def main():
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f"  ok    {name}")
        except Exception as e:
            # Not just AssertionError: a header reformatted past what the
            # regexes read raises KeyError or ValueError, and that is a failure
            # of this contract too - it must not look like a pass.
            failed += 1
            print(f"  FAIL  {name}\n          {type(e).__name__}: {e}")
    print(f"\n  {len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
