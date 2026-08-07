"""PlatformIO pre-build script: inject version info as compile-time macros.

The canonical version lives in the `VERSION` file at the repo root. Release
builds (clean tree sitting exactly on tag `v<VERSION>`) report that number
verbatim; anything else gets semver build metadata appended so a dev build is
never mistaken for a release:

    0.4.0                     clean, tagged v0.4.0
    0.4.0+3.gd34db33          3 commits past the tag
    0.4.0+3.gd34db33.dirty    ...with uncommitted changes
    0.4.0+g d34db33           tag missing entirely
    0.4.0                     no git at all (zip install)
"""

import os
import subprocess

Import("env")

_PROJECT_DIR = env["PROJECT_DIR"]


def _git(*args):
    try:
        out = subprocess.check_output(
            ("git",) + args,
            cwd=_PROJECT_DIR,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, OSError):
        return ""
    return out.decode("utf-8", "replace").strip()


def _base_version():
    try:
        with open(os.path.join(_PROJECT_DIR, "VERSION")) as f:
            return f.read().strip()
    except OSError:
        return "0.0.0"


def _version():
    base = _base_version()

    commit = _git("rev-parse", "--short", "HEAD")
    if not commit:
        # No git available (zip install, or git not on PATH). The VERSION file
        # is all we have, and it is the right answer for a release tarball.
        return base

    dirty = bool(_git("status", "--porcelain"))
    tag = "v" + base
    behind = _git("rev-list", "--count", "%s..HEAD" % tag)

    if not behind:
        # Tag does not exist yet.
        meta = "g" + commit
    elif behind == "0" and not dirty:
        return base
    else:
        meta = "%s.g%s" % (behind, commit)

    if dirty:
        meta += ".dirty"
    return "%s+%s" % (base, meta)


version = _version()
variant = env["PIOENV"]

print("knomi-serial: firmware version %s (%s)" % (version, variant))

env.Append(
    CPPDEFINES=[
        ("KNOMI_FW_VERSION", env.StringifyMacro(version)),
        ("KNOMI_BUILD_VARIANT", env.StringifyMacro(variant)),
    ]
)
