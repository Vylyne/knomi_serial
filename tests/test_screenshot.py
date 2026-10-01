#!/usr/bin/env python3
"""Behavior of the screenshot tool's generated device configuration."""

import importlib.util
import os
import sys
from types import SimpleNamespace

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
        check(name, frame[7 + 12], value)


def test_home_capture_can_show_applied_tramming():
    frame = screenshot.state_for("idle", 0, tram="qgl", tram_applied=True)
    check("tram applied", frame[7 + 11], 1)
    check("tram type", frame[7 + 12], screenshot.k.PrinterTramType.QGL.value)


def test_home_capture_can_show_all_axes_unhomed():
    frame = screenshot.state_for("idle", 0, homed=False)
    check("homed flags", frame[7 + 6:7 + 9], b"\0\0\0")


def test_capture_can_override_machine_accent_for_contrast_review():
    config = screenshot.build_config(color_machine=0xff0000)
    check("machine accent", config.color_machine, 0xff0000)


def test_machine_accent_requires_six_hex_digits():
    check("valid accent", screenshot.parse_machine_color("#FF0000"), 0xff0000)
    for value in ("F", "12345", "GG0000", "1234567"):
        try:
            screenshot.parse_machine_color(value)
        except ValueError:
            continue
        raise AssertionError(f"accepted invalid machine accent {value!r}")


def test_init_capture_clears_old_error_before_driving_disconnected_state():
    class Port:
        in_waiting = 0

        def __init__(self):
            self.writes = []

        def write(self, frame):
            self.writes.append(frame)

    port = Port()
    args = SimpleNamespace(
        quiet=True, color="9572BF", type="ABS", tram="none",
        settle=0, square=False, unhomed=False, tram_applied=False,
    )
    clock = iter(i * 0.5 for i in range(20))
    original_time = screenshot.time.time
    original_sleep = screenshot.time.sleep
    original_capture = screenshot.capture
    original_write_png = screenshot.write_png
    try:
        screenshot.time.time = lambda: next(clock)
        screenshot.time.sleep = lambda _seconds: None
        screenshot.capture = lambda *_args: ((1, 1), b"\0\0")
        screenshot.write_png = lambda *_args, **_kwargs: None
        screenshot.shoot(port, "unused.png", "init", screenshot.build_config(), 0, args)
    finally:
        screenshot.time.time = original_time
        screenshot.time.sleep = original_sleep
        screenshot.capture = original_capture
        screenshot.write_png = original_write_png

    check("clear message first", port.writes[0], screenshot.k.encode_message(""))
    check("drive disconnected", port.writes[1], screenshot.state_for("init", 0))
    check("retry message after boot", sum(
        write == screenshot.k.encode_message("") for write in port.writes) > 1,
        True)


def test_documentation_shots_include_clean_init_before_error_waiting():
    check("first documentation shot", screenshot.DOC_SHOTS[0], ("init", "init", False))
    check("error follows", screenshot.DOC_SHOTS[1], ("waiting", "waiting", False))


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
