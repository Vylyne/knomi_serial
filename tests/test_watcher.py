#!/usr/bin/env python3
"""The watcher service, and Klipper's willingness to distrust it.

Worth testing without hardware because the interesting cases are all about
things being wrong: a map written before a cable moved, a file half-written when
something read it, a port that belongs to somebody else. It is only safe
because nothing downstream believes it, and that is the property under test
here.

    python tests/test_watcher.py       # or: pytest tests/
"""

import json
import os
import sys
import tempfile

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_ROOT, "klippy_extras"))
sys.path.insert(0, os.path.join(_ROOT, "service"))

import knomi_serial as k  # noqa: E402
import knomi_serial_watch as w  # noqa: E402


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, wanted {want!r}")


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def watcher(tmp, ports, identify):
    """A Watcher with the two things it touches replaced.

    `ports` is what the machine currently shows; `identify` maps a port to the
    display on it, or None for something that is not one.
    """
    clock = Clock()
    obj = w.Watcher(tmp, now=clock)
    obj.clock = clock
    w.k.candidate_ports = lambda skip=(): list(ports)
    obj._identify = lambda port: (
        (identify[port], {"fw": "0.5.0", "var": "knomi"})
        if identify.get(port) else None)
    w._openable = lambda port: True
    return obj


def temp():
    fd, path = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    os.unlink(path)
    return path


def test_a_display_is_recorded_when_its_port_appears():
    path = temp()
    ports = ["/dev/ttyUSB0"]
    obj = watcher(path, ports, {"/dev/ttyUSB0": "19aa44"})
    check("changed", obj.tick(), True)
    check("recorded", obj.devices["19aa44"]["port"], "/dev/ttyUSB0")
    check("written", k.port_map(path), {"19aa44": "/dev/ttyUSB0"})


def test_an_unplugged_display_is_dropped():
    """A path that cannot be opened is worse than no path - it costs a connect."""
    path = temp()
    ports = ["/dev/ttyUSB0"]
    obj = watcher(path, ports, {"/dev/ttyUSB0": "19aa44"})
    obj.tick()
    ports.clear()
    check("changed", obj.tick(), True)
    check("forgotten", obj.devices, {})


def test_a_stranger_on_a_ch340_is_not_asked_again_immediately():
    """Something else on the printer must not have its port held open on a loop."""
    path = temp()
    obj = watcher(path, ["/dev/ttyUSB0"], {})
    asked = []
    obj._identify = lambda port: asked.append(port) or None
    obj.tick()
    obj.tick()
    obj.tick()
    check("asked once", asked, ["/dev/ttyUSB0"])


def test_a_port_already_identified_is_left_alone():
    path = temp()
    obj = watcher(path, ["/dev/ttyUSB0"], {"/dev/ttyUSB0": "19aa44"})
    obj.tick()
    asked = []
    obj._identify = lambda port: asked.append(port) or None
    check("no change", obj.tick(), False)
    check("not re-asked", asked, [])


def test_the_map_survives_a_restart():
    path = temp()
    obj = watcher(path, ["/dev/ttyUSB0"], {"/dev/ttyUSB0": "19aa44"})
    obj.tick()
    again = w.Watcher(path)
    check("loaded", again.devices["19aa44"]["port"], "/dev/ttyUSB0")


def test_klipper_ignores_a_file_it_does_not_understand():
    """Forward compatibility: a newer writer must not be guessed at."""
    path = temp()
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"version": 99, "devices": {"19aa44": {"port": "/dev/x"}}}, f)
    check("no hint", k.port_map(path), {})


def test_klipper_ignores_a_file_that_is_not_there():
    check("no hint", k.port_map("/nonexistent/devices.json"), {})


def test_klipper_ignores_a_half_written_file():
    """save() is atomic so this should not happen, but a reader must not care."""
    path = temp()
    with open(path, "w", encoding="utf-8") as f:
        f.write('{"version": 1, "devices": {"19aa44": {"po')
    check("no hint", k.port_map(path), {})


def test_the_written_file_is_what_klipper_reads():
    """One format, two programs - the pair that must not drift apart."""
    path = temp()
    w.save(path, {"19aa44": {"port": "/dev/ttyUSB3", "seen": 1.0,
                             "fw": "0.5.0", "var": "knomi"}})
    check("round trip", k.port_map(path), {"19aa44": "/dev/ttyUSB3"})


def cluster(path, printing=False, ident="19aa44"):
    """A cluster whose only source of ports is the watcher's map."""
    c = k.KnomiCluster.__new__(k.KnomiCluster)
    c.devices = []
    c.tools = {}
    c._ports = {}
    c._discover_after = 0
    c._rejected = set()
    state = "printing" if printing else "standby"
    c.print_stats = type("S", (), {
        "get_status": staticmethod(lambda e: {"state": state})})()
    c.reactor = type("R", (), {"monotonic": staticmethod(lambda: 100.0)})()
    d = k.Knomi_Serial.__new__(k.Knomi_Serial)
    d.screen_name = "T0_knomi"
    d.config_serial = None
    d.config_device_id = ident
    d.resolved_port = None
    c.devices.append(d)
    k._DEVICE_MAP_PATH = path
    # Any fall through to listening is a failure of the test's premise.
    c._discover_once = lambda skip=(): (_ for _ in ()).throw(
        AssertionError("fell back to listening"))
    return c


def test_a_display_is_reconnected_mid_print_from_the_map():
    """The whole reason resolve_port reads the file instead of listening."""
    path = temp()
    w.save(path, {"19aa44": {"port": "/dev/ttyUSB2", "seen": 1.0}})
    c = cluster(path, printing=True)
    check("resolved during a print", c.resolve_port("19aa44"), "/dev/ttyUSB2")


def test_listening_is_still_refused_mid_print():
    """A file read is safe on the reactor. Six seconds of listening is not."""
    path = temp()
    w.save(path, {"19aa45": {"port": "/dev/ttyUSB2", "seen": 1.0}})
    c = cluster(path, printing=True)
    check("no answer, and no listening", c.resolve_port("19aa44"), None)


def test_a_map_that_proved_wrong_is_not_read_back_and_retried():
    """Otherwise a stale file is an infinite reconnect loop.

    Connect, fail the identity check, clear the cache, read the same wrong
    answer out of the same file, connect again - every five seconds, forever.
    """
    path = temp()
    w.save(path, {"19aa44": {"port": "/dev/ttyUSB2", "seen": 1.0}})
    c = cluster(path, printing=True)
    check("tried once", c.resolve_port("19aa44"), "/dev/ttyUSB2")
    c.reject_port("19aa44", "/dev/ttyUSB2")
    check("not tried again", c.resolve_port("19aa44"), None)


def test_the_same_display_on_a_new_port_is_a_fresh_answer():
    """Rejection is of a pairing, not of a display - it must not be permanent."""
    path = temp()
    w.save(path, {"19aa44": {"port": "/dev/ttyUSB2", "seen": 1.0}})
    c = cluster(path, printing=True)
    c.resolve_port("19aa44")
    c.reject_port("19aa44", "/dev/ttyUSB2")
    # The watcher notices the move and rewrites the map.
    w.save(path, {"19aa44": {"port": "/dev/ttyUSB7", "seen": 2.0}})
    check("the new port is tried", c.resolve_port("19aa44"), "/dev/ttyUSB7")


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
