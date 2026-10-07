#!/usr/bin/env python3
"""What install.sh declares, and how anyone finds out it is out of date.

Two things that fail a long way from their cause, and only a test catches
either.

The apt packages are declared in exactly one file, which `moonraker.conf` names
by path. Moonraker's `_verify_path` only *warns* when that path is wrong, so
moving the file installs nothing on every printer and lands as an ImportError at
service start - a failure that looks nothing like a renamed file.

And a Moonraker update never runs install.sh at all: it moves files, installs
those packages, and restarts services. Anything else install.sh owns - the
systemd unit, the `[update_manager]` section - is frozen at whatever version
last ran it, which is what the version stamp exists to notice.

    python tests/test_packaging.py       # or: pytest tests/
"""

import json
import os
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_ROOT, "klippy_extras"))

import knomi_serial as k  # noqa: E402

#: Under scripts/ rather than the repo root, matching where mcu-updater keeps
#: its own and therefore what both [update_manager] sections point at. Named
#: here rather than searched for, so moving it fails this test instead of
#: failing silently in Moonraker - _verify_path only logs a warning.
_SYSDEPS = os.path.join(_ROOT, "scripts", "moonraker-system-dependencies.json")

_VERSION_FILE = os.path.join(_ROOT, "scripts", "install-version")


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, wanted {want!r}")


def from_json():
    with open(_SYSDEPS, encoding="utf-8") as f:
        return sorted(json.load(f)["debian"])


def install_sh():
    with open(os.path.join(_ROOT, "install.sh"), encoding="utf-8") as f:
        return f.read()


def test_the_declared_path_is_where_the_file_is():
    """moonraker.conf names this path; a moved file is a warning in a log.

    Moonraker's _verify_path complains and carries on, so the packages simply
    never install and the watcher fails to start much later, for reasons that
    look nothing like a missing file.
    """
    if not os.path.isfile(_SYSDEPS):
        raise AssertionError(f"{_SYSDEPS} is what the config points at")
    declared = "system_dependencies: scripts/moonraker-system-dependencies.json"
    if declared not in install_sh():
        raise AssertionError("install.sh writes a different path than this")


def test_pyserial_is_declared():
    """The watcher runs under the system python3, not Klipper's virtualenv.

    Klipper's venv has pyserial because Klipper needs it; the system interpreter
    the unit invokes may not, and `import serial` is the watcher's first act.
    """
    if "python3-serial" not in from_json():
        raise AssertionError("python3-serial missing - the service will not start")


def test_pyudev_is_declared():
    """The watcher's wait is a netlink poll, so this is not optional any more.

    It used to ask sysfs once a second, which needs nothing installed. Now it
    blocks on udev, and without pyudev the service does not start at all.
    """
    if "python3-pyudev" not in from_json():
        raise AssertionError("python3-pyudev missing - the service will not start")


def test_the_install_version_is_a_number():
    """install.sh cats this into a stamp file and both readers parse it."""
    with open(_VERSION_FILE, encoding="utf-8") as f:
        int(f.read().strip())


def test_install_sh_reads_the_version_rather_than_carrying_its_own():
    """One source, so there is nothing to keep in sync and nothing to forget."""
    if "scripts/install-version" not in install_sh():
        raise AssertionError("install.sh should stamp from scripts/install-version")


def test_a_current_install_is_not_nagged():
    """The stamp matching means everything install.sh owns is already right."""
    check("nothing to say", k.install_version_notice(3, 3), None)
    check("nor when ahead", k.install_version_notice(3, 4), None)


def test_an_older_install_is_told_what_to_run():
    """The only signal there is. A Moonraker update cannot produce this itself."""
    notice = k.install_version_notice(3, 2)
    if not notice or "install.sh" not in notice:
        raise AssertionError(f"should name install.sh, got {notice!r}")


def test_an_unreadable_stamp_reads_as_never_installed():
    """Missing, empty and half-written all mean the same thing: assume stale.

    It must never raise either - this is read on the way into klippy:ready, and
    an exception there takes the printer down over a missing hint file.
    """
    missing = os.path.join(_ROOT, "no", "such", "file")
    check("missing", k._install_version(missing), 0)
    check("not a number", k._install_version(_SYSDEPS), 0)
    check("the repo's own", k._install_version(_VERSION_FILE) > 0, True)


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
