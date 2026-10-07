#!/usr/bin/env python3
"""Things that are true of the repo rather than of the code.

A file that opens with a shebang is offering to be run as `./path`, and that
only works if git records it as executable. The bench scripts and the watcher
all had the shebang and not the bit, so `./scripts/discover.py` was Permission
denied where `python3 scripts/discover.py` worked.

A Windows checkout cannot notice. `os.access(path, os.X_OK)` means nothing
there and the working tree carries no mode bits, so the check asks git, which
stores the bit either way.

    python tests/test_repo_hygiene.py       # or: pytest tests/
"""

import os
import shutil
import subprocess
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def tracked_modes():
    """{path: git mode}, e.g. "100755". Empty outside a git checkout."""
    git = shutil.which("git")
    if git is None:
        return {}
    try:
        out = subprocess.run(
            [git, "ls-files", "-s"], cwd=_ROOT, capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return {}
    if out.returncode != 0:
        return {}

    modes = {}
    for line in out.stdout.decode("utf-8", "replace").splitlines():
        # "100755 <sha> 0\tpath"
        meta, _, path = line.partition("\t")
        parts = meta.split()
        if path and parts:
            modes[path] = parts[0]
    return modes


def test_every_script_with_a_shebang_is_executable():
    """A shebang is a promise that `./the/script` works. Keep it."""
    modes = tracked_modes()
    if not modes:
        return  # not a git checkout, or git unavailable

    offenders = []
    for path, mode in sorted(modes.items()):
        full = os.path.join(_ROOT, path)
        if not os.path.isfile(full):
            continue
        try:
            with open(full, "rb") as f:
                first = f.readline()
        except OSError:
            continue
        if first.startswith(b"#!") and mode != "100755":
            offenders.append(f"{path} (mode {mode})")

    if offenders:
        raise AssertionError(
            "these declare a shebang but are not executable, so `./<path>` "
            "fails:\n            " + "\n            ".join(offenders)
            + "\n          Fix with: git update-index --chmod=+x <path>")


def test_the_check_can_actually_see_the_repo():
    """Guards the test above, which passes on an empty map.

    It returns early when git is missing, so on a normal checkout this asserts
    that it really did find files.
    """
    modes = tracked_modes()
    if not modes:
        return  # not a git checkout, or git unavailable
    if len(modes) <= 20:
        raise AssertionError(f"only {len(modes)} tracked files were seen")
    if not any(path.endswith(".py") for path in modes):
        raise AssertionError("no tracked .py file was seen")


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
