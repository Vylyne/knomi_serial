#!/usr/bin/env python3
"""What another program is allowed to depend on, and a check that it still holds.

mcu-updater flashes these displays, and to do that safely it has to know which
display is on which port *at the moment esptool writes* - not where one was when
Klipper last looked, because Klipper has to be stopped for the flash and its
answer goes stale the moment it is. So it resolves identity itself, against free
ports, immediately before writing.

It does that by importing this repo's module directly:

    sys.path.insert(0, os.path.join(<repo>, "klippy_extras"))
    import knomi_serial as k
    found = k.discover_reports()

Which makes `discover_reports` a public interface with a consumer outside this
repository, and one that fails in a bad place: a rename or a changed return
shape surfaces during a firmware flash, with Klipper stopped and a display
half-written. Renaming a private helper is a refactor; renaming this is a
breaking change to somebody else's flashing safeguard.

Note the surface is the *module function*, not `scripts/discover.py`. The script
is a human-facing tool and free to change; these are the pieces that are not.

    python tests/test_contract.py       # or: pytest tests/
"""

import inspect
import os
import subprocess
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_ROOT, "klippy_extras"))

import knomi_serial as k  # noqa: E402


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, wanted {want!r}")


def test_the_module_imports_without_klipper():
    """The consumer imports this file from a plain python3, outside Klipper.

    Anything imported at module scope has to exist there too, so a `from . import
    ...` or a klippy helper pulled in at the top would break flashing while
    leaving Klipper itself perfectly happy - a failure this repo's own tests
    would never see, because they run the same way the consumer does.
    """
    out = subprocess.run(
        [sys.executable, "-c",
         "import sys; sys.path.insert(0, sys.argv[1]); import knomi_serial",
         os.path.join(_ROOT, "klippy_extras")],
        capture_output=True, text=True, timeout=60,
    )
    if out.returncode != 0:
        raise AssertionError(f"bare import failed:\n{out.stderr[-400:]}")


def test_discover_reports_is_callable_with_no_arguments():
    """`k.discover_reports()` and `k.discover_reports(listen=N)`, both used."""
    sig = inspect.signature(k.discover_reports)
    for name in ("ports", "listen", "skip"):
        if name not in sig.parameters:
            raise AssertionError(f"discover_reports lost its `{name}` parameter")
        if sig.parameters[name].default is inspect.Parameter.empty:
            raise AssertionError(f"`{name}` must stay optional - called with none")


def test_the_returned_shape_is_what_the_consumer_reads():
    """{id: {"port": ..., "fw": ..., "var": ...}}, id lowercase.

    Only these three fields are read. Adding more is safe; removing one or
    changing the key from the hardware id is not.
    """
    line = (k._CMD_PREFIX + k._CMD_REPORT
            + b"id=19AA44;fw=0.5.0;proto=5;var=knomi\n")

    class Port:
        def __init__(self):
            self.data = line

        @property
        def in_waiting(self):
            return len(self.data)

        def read(self, n):
            out, self.data = self.data[:n], self.data[n:]
            return out

        def close(self):
            pass

    real = k.serial.Serial
    k.serial.Serial = lambda *a, **kw: Port()
    try:
        got = k.discover_reports(["/dev/ttyUSB0"], listen=0.5)
    finally:
        k.serial.Serial = real

    check("keyed by id, lowercased", sorted(got), ["19aa44"])
    fields = got["19aa44"]
    check("port", fields.get("port"), "/dev/ttyUSB0")
    check("fw", fields.get("fw"), "0.5.0")
    check("var", fields.get("var"), "knomi")


def test_candidate_ports_still_exists():
    """Used to decide there is anything to do before paying for a listen."""
    if not callable(getattr(k, "candidate_ports", None)):
        raise AssertionError("candidate_ports is part of the same surface")


def test_the_map_file_reader_keeps_its_name():
    """port_map and the file's version are read by anything sharing the map."""
    if not callable(getattr(k, "port_map", None)):
        raise AssertionError("port_map is public")
    check("format version", k._DEVICE_MAP_VERSION, 1)


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
