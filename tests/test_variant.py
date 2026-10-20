#!/usr/bin/env python3
"""The board's Arduino variant agrees with the firmware's own pin map.

variants/knomi_v2/pins_arduino.h is what the core falls back to when a caller
passes no pins, and src/knomi_v2.h is what the firmware passes when it does.
The board used to borrow Adafruit's Feather variant, whose defaults put I2C on
the camera bus and whose initVariant() drove the panel reset and a camera pin
high before setup() - so these check that the two headers name the same pins,
and that the variant stays header-only.

    python tests/test_variant.py       # or: pytest tests/
"""

import json
import os
import re
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_MANIFEST = os.path.join(_ROOT, "boards", "knomi.json")
_BOARD_PINS = os.path.join(_ROOT, "src", "knomi_v2.h")


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def _build():
    return json.loads(_read(_MANIFEST))["build"]


def _variant_dir():
    build = _build()
    return os.path.join(_ROOT, build["variants_dir"], build["variant"])


def variant_pins():
    """{name: gpio} from the variant's `static const uint8_t NAME = N;` lines."""
    text = _read(os.path.join(_variant_dir(), "pins_arduino.h"))
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"static const uint8_t (\w+)\s*=\s*(\d+);", text)}


def board_pins():
    """{name: value} from src/knomi_v2.h's `#define NAME N` lines."""
    text = _read(_BOARD_PINS)
    return {m.group(1): int(m.group(2), 0)
            for m in re.finditer(r"^#define (\w+) (0x[0-9A-Fa-f]+|\d+)\s*$", text, re.M)}


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, want {want!r}")


def test_the_manifest_uses_the_variant_in_this_repo():
    build = _build()
    check("variants_dir", build.get("variants_dir"), "variants")
    check("variant directory exists", os.path.isdir(_variant_dir()), True)
    flags = " ".join(build.get("extra_flags", []))
    check("an Adafruit board flag", "ADAFRUIT" in flags, False)


def test_the_core_defaults_are_the_firmwares_pins():
    """A bare Wire.begin() or SPI.begin() lands where the firmware's own does."""
    variant, board = variant_pins(), board_pins()
    for arduino, ours in (("SDA", "I2C0_SDA"), ("SCL", "I2C0_SCL"),
                          ("MOSI", "LCD_MOSI"), ("SCK", "LCD_SCLK"), ("SS", "LCD_CS")):
        check(f"{arduino} vs {ours}", variant.get(arduino), board[ours])


def test_the_pins_knomi_v2_h_does_not_name():
    """From docs/hardware.md: U0TXD/U0RXD go to the CH340K, LCD-SDO is GPIO13."""
    variant = variant_pins()
    check("TX", variant.get("TX"), 43)
    check("RX", variant.get("RX"), 44)
    check("MISO", variant.get("MISO"), 13)


def test_the_variant_runs_no_code():
    """A .c or .cpp here can define initVariant(), which runs before setup()."""
    sources = sorted(name for name in os.listdir(_variant_dir())
                     if name.endswith((".c", ".cpp", ".cc", ".S")))
    check("sources in the variant", sources, [])


def main():
    tests = [(n, f) for n, f in sorted(globals().items())
             if n.startswith("test_") and callable(f)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f"  ok    {name}")
        except Exception as e:
            failed += 1
            print(f"  FAIL  {name}\n          {type(e).__name__}: {e}")
    print(f"\n  {len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
