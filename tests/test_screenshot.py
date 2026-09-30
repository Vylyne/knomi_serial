#!/usr/bin/env python3
"""Behavior of the screenshot tool's generated device configuration."""

import importlib.util
import os
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_SCRIPT = os.path.join(_ROOT, "scripts", "screenshot.py")
_SPEC = importlib.util.spec_from_file_location("knomi_screenshot", _SCRIPT)
screenshot = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(screenshot)


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, wanted {want!r}")


def test_page_selection_makes_one_page_the_landing_page():
    check(
        "home",
        screenshot.build_config("home").pages,
        (screenshot.k._PAGES["home"],),
    )
    check(
        "move",
        screenshot.build_config("move").pages,
        (screenshot.k._PAGES["move"],),
    )


def test_omitting_page_keeps_the_firmware_default_order():
    config = screenshot.build_config()
    check("no explicit pages", config.pages, ())
    check("no demo buttons", config.buttons, ())
    check(
        "page override absent",
        config.present & screenshot.k._HAS_PAGE_ORDER,
        0,
    )


def test_shared_button_capture_places_observers_at_ne_and_se():
    config = screenshot.build_config("move", shared_buttons=True)
    check("button override", bool(config.present & screenshot.k._HAS_BUTTONS), True)
    check(
        "shared buttons",
        [(binding.name, binding.slot.name, binding.resolver.name,
          binding.argument) for binding in config.buttons],
        [("feed", "NE", "OBSERVE", 1),
         ("retract", "SE", "OBSERVE", 2)],
    )
    check(
        "shared button records",
        screenshot.k.config_payload(config)[36:60],
        bytes((3, 2, 3, 0, 255, 1, 0, 0, 0, 0, 0, 0,
               3, 5, 3, 0, 255, 2, 0, 0, 0, 0, 0, 0)),
    )


def test_slotless_observer_capture_keeps_buttons_without_visual_slots():
    config = screenshot.build_config("move", slotless_buttons=True)
    check("button override", bool(config.present & screenshot.k._HAS_BUTTONS), True)
    check(
        "observers without slots",
        [(binding.name, binding.slot.name, binding.argument)
         for binding in config.buttons],
        [("feed", "NONE", 1), ("retract", "NONE", 2)],
    )
    check(
        "slotless button records",
        screenshot.k.config_payload(config)[36:60],
        bytes((3, 0, 3, 0, 255, 1, 0, 0, 0, 0, 0, 0,
               3, 0, 3, 0, 255, 2, 0, 0, 0, 0, 0, 0)),
    )


def test_home_capture_can_drive_qgl_and_zta():
    for name, value in (("qgl", 2), ("zta", 1)):
        frame = screenshot.state_for("idle", 0, tram=name)
        check(name, frame[7 + 11], value)


def main():
    tests = [
        (name, fn)
        for name, fn in sorted(globals().items())
        if name.startswith("test_") and callable(fn)
    ]
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
