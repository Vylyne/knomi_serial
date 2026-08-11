#!/usr/bin/env python3
"""Finding a display by its hardware id rather than by which socket it is in.

Worth testing without hardware because the interesting cases are the ones you
cannot conveniently arrange on a bench: a port that answers nothing, a port that
cannot be opened at all, and six displays where the answer must not depend on
which order they were enumerated in.

    python tests/test_discovery.py       # or: pytest tests/
"""

import os
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_ROOT, "klippy_extras"))

import knomi_serial as k  # noqa: E402


class FakePort:
    """Enough of a pyserial Serial to be listened to.

    `script` is the bytes it will hand over, in the order asked for. A port that
    yields nothing stands for one with something else on the end of it.
    """

    opened = []

    def __init__(self, data=b"", fail=False):
        self.data = data
        self.fail = fail
        self.closed = False

    @property
    def in_waiting(self):
        if self.fail:
            raise OSError("gone")
        return len(self.data)

    def read(self, n):
        out, self.data = self.data[:n], self.data[n:]
        return out

    def close(self):
        self.closed = True


def line(ident):
    """A report line as the firmware actually emits it."""
    return (k._CMD_PREFIX + k._CMD_REPORT
            + f"id={ident};fw=0.5.0;proto=5;busy=2".encode() + b"\n")


def patched(ports):
    """Run discover() against a fixed set of fake ports."""
    real = k.serial.Serial
    made = {}

    def fake(path, *a, **kw):
        if ports[path] is None:
            raise k.serial.SerialException("in use")
        made[path] = ports[path]
        return ports[path]

    k.serial.Serial = fake
    try:
        return k.discover(list(ports), listen=0.5), made
    finally:
        k.serial.Serial = real


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, wanted {want!r}")


def test_one_display_is_found_by_its_id():
    found, _ = patched({"/dev/ttyUSB0": FakePort(line("19AA44"))})
    check("mapping", found, {"19AA44": "/dev/ttyUSB0"})


def test_id_is_independent_of_which_socket():
    """The whole point: the same display on another port is the same display."""
    a, _ = patched({"/dev/ttyUSB0": FakePort(line("19AA44"))})
    b, _ = patched({"/dev/ttyUSB3": FakePort(line("19AA44"))})
    check("same id", set(a), set(b))
    check("different port", (a["19AA44"], b["19AA44"]),
          ("/dev/ttyUSB0", "/dev/ttyUSB3"))


def test_six_displays_all_resolve():
    ids = ["19AA44", "19AA45", "7B3B14", "CCBA97", "0000FF", "ABCDEF"]
    ports = {f"/dev/ttyUSB{n}": FakePort(line(i)) for n, i in enumerate(ids)}
    found, _ = patched(ports)
    check("all six", sorted(found), sorted(ids))
    check("no port used twice", len(set(found.values())), 6)


def test_a_silent_port_is_dropped_not_waited_on():
    """Something else on a CH340 must not stop the rest being found."""
    found, _ = patched({
        "/dev/ttyUSB0": FakePort(b""),
        "/dev/ttyUSB1": FakePort(line("19AA44")),
    })
    check("only the display", found, {"19AA44": "/dev/ttyUSB1"})


def test_a_port_that_will_not_open_is_skipped():
    """Held by another section, or unplugged since it was enumerated."""
    found, _ = patched({
        "/dev/ttyUSB0": None,
        "/dev/ttyUSB1": FakePort(line("19AA44")),
    })
    check("the openable one", found, {"19AA44": "/dev/ttyUSB1"})


def test_a_port_that_dies_mid_listen_is_skipped():
    found, _ = patched({
        "/dev/ttyUSB0": FakePort(b"", fail=True),
        "/dev/ttyUSB1": FakePort(line("19AA44")),
    })
    check("survivor", found, {"19AA44": "/dev/ttyUSB1"})


def test_every_port_is_closed_again():
    """Discovery must not leave a handle on a port Klipper is about to open."""
    ports = {
        "/dev/ttyUSB0": FakePort(b""),
        "/dev/ttyUSB1": FakePort(line("19AA44")),
    }
    _, made = patched(ports)
    for path, port in made.items():
        if not port.closed:
            raise AssertionError(f"{path} was left open")


def test_report_id_ignores_lines_that_are_not_reports():
    for text in (b"", b"KNOMI_CMD:CFG?", b"KNOMI_CMD:GCODE:HOME", b"noise"):
        if k.report_id(text) is not None:
            raise AssertionError(f"{text!r} should not look like a report")
    # And a report with no id - firmware older than this feature.
    old = k._CMD_PREFIX + k._CMD_REPORT + b"fw=0.4.0;proto=4"
    check("no id field", k.report_id(old), None)


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
