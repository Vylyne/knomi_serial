#!/usr/bin/env python3
"""The two places system dependencies are declared, and their agreeing.

Moonraker reads one or the other, never both: `_configure_sysdeps` returns True
when `system_dependencies` is configured and `install_script` is then never
looked at. So which of these two files is authoritative depends on a line in
somebody else's `moonraker.conf`, and a package listed in only one of them is
installed on some printers and not others - with the failure landing at service
start, as an ImportError, a long way from the omission.

Nothing but a test can hold them together. The upstream convention is a comment
asking politely; this checks.

    python tests/test_packaging.py       # or: pytest tests/
"""

import json
import os
import re
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

#: Exactly what Moonraker does to install.sh, from _read_system_dependencies in
#: moonraker/components/update_manager/app_deploy.py. Copied rather than
#: approximated: a stricter pattern here would pass on a line Moonraker ignores.
_PKGLIST_RE = re.compile(r'PKGLIST="(.*)"')


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, wanted {want!r}")


def from_install_sh():
    with open(os.path.join(_ROOT, "install.sh"), encoding="utf-8") as f:
        data = f.read()
    found = _PKGLIST_RE.findall(data)
    return sorted(
        pkg
        for line in found
        for pkg in line.lstrip("${PKGLIST}").strip().split()
    )


def from_json():
    path = os.path.join(_ROOT, "moonraker-system-dependencies.json")
    with open(path, encoding="utf-8") as f:
        return sorted(json.load(f)["debian"])


def test_the_two_declarations_agree():
    """Whichever one a printer's moonraker.conf points at, it gets the same set."""
    check("packages", from_install_sh(), from_json())


def test_moonraker_can_find_the_pkglist():
    """A PKGLIST line Moonraker's own regex misses is not a declaration."""
    pkgs = from_install_sh()
    if not pkgs:
        raise AssertionError(
            "no PKGLIST= line in install.sh that Moonraker's regex matches")


def test_pyserial_is_declared():
    """The watcher runs under the system python3, not Klipper's virtualenv.

    Klipper's venv has pyserial because Klipper needs it; the system interpreter
    the unit invokes may not, and `import serial` is the watcher's first act.
    """
    if "python3-serial" not in from_json():
        raise AssertionError("python3-serial missing - the service will not start")


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
