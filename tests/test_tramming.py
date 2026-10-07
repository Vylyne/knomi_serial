#!/usr/bin/env python3
"""Selected Klipper tramming status is shared by every display."""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "klippy_extras"))

import knomi_serial as k  # noqa: E402


class FakeTram:
    def __init__(self, applied):
        self.applied = applied
        self.seen_time = None

    def get_status(self, eventtime):
        self.seen_time = eventtime
        return {"applied": self.applied}


class FakePrinter:
    def __init__(self, **objects):
        self.objects = objects

    def lookup_object(self, name, default=None):
        return self.objects.get(name, default)


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, wanted {want!r}")


def test_qgl_and_zta_select_their_actual_status_provider():
    qgl = FakeTram(True)
    zta = FakeTram(False)
    for objects, want_type, want_object in (
        ({"quad_gantry_level": qgl}, k.PrinterTramType.QGL, qgl),
        ({"z_tilt": zta}, k.PrinterTramType.ZTA, zta),
        ({"quad_gantry_level": qgl, "z_tilt": zta}, k.PrinterTramType.ZTA, zta),
        ({}, k.PrinterTramType.NONE, None),
    ):
        got_type, got_object = k.select_tram(FakePrinter(**objects))
        check("tram type", got_type, want_type)
        check("tram provider", got_object, want_object)


def test_applied_state_tracks_the_selected_provider():
    qgl = FakeTram(True)
    cluster = k.KnomiCluster.__new__(k.KnomiCluster)
    cluster.tram_object = qgl
    check("applied", cluster._tram_applied(12.5), True)
    check("event time", qgl.seen_time, 12.5)
    qgl.applied = False
    check("cleared", cluster._tram_applied(13.5), False)
    cluster.tram_object = None
    check("no tramming", cluster._tram_applied(14.5), False)


def main():
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
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
