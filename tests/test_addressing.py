#!/usr/bin/env python3
"""Which screen a KNOMI_TOOL command lands on.

Worth testing without hardware because the rules are the kind that read as
obvious and are not: a command with no target is fine on one machine and
ambiguous on the next, and `TOOL=` can legitimately match more than one screen.

    python tests/test_addressing.py      # or: pytest tests/
"""

import os
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_ROOT, "klippy_extras"))

import knomi_serial as k  # noqa: E402


class FakeGcmd:
    """Enough of Klipper's GCodeCommand for the resolver."""

    class error(Exception):
        pass

    _REQUIRED = object()

    def __init__(self, **params):
        self.params = params

    def get(self, key, default=_REQUIRED):
        if key in self.params:
            return self.params[key]
        if default is self._REQUIRED:
            raise self.error(f"missing {key}")
        return default

    def get_int(self, key, default, minval=None, maxval=None):
        return int(self.params[key]) if key in self.params else default


class FakeDevice:
    def __init__(self, screen_name, config_tool):
        self.screen_name = screen_name
        self.config_tool = config_tool


def cluster(*devices):
    """A KnomiCluster with only the parts the resolver touches."""
    c = k.KnomiCluster.__new__(k.KnomiCluster)
    c.devices = list(devices)
    c.tools = {}
    c.was_printing = False
    return c


ONE = lambda: cluster(FakeDevice("knomi_serial", None))  # noqa: E731
MANY = lambda: cluster(  # noqa: E731
    FakeDevice("T0_knomi", "0"),
    FakeDevice("T1_knomi", "1"),
    # A second display of tool 0, which is what makes TOOL= a one-to-many
    # lookup rather than a rename of SCREEN=.
    FakeDevice("spare", "0"),
)


def resolve(c, **params):
    return c.resolve(FakeGcmd(**params))


def refuses(c, **params):
    try:
        got = resolve(c, **params)
    except FakeGcmd.error as e:
        return str(e)
    raise AssertionError(f"expected a refusal, got {got!r}")


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, wanted {want!r}")


def test_single_screen_needs_no_target():
    """Nothing to disambiguate, so asking would be ceremony."""
    check("bare", resolve(ONE()), ["knomi_serial"])


def test_single_screen_still_accepts_its_name():
    check("named", resolve(ONE(), SCREEN="knomi_serial"), ["knomi_serial"])


def test_several_screens_require_a_target():
    message = refuses(MANY())
    for name in ("T0_knomi", "T1_knomi", "spare"):
        if name not in message:
            raise AssertionError(f"refusal should list {name}: {message}")


def test_screen_addresses_exactly_one():
    check("by name", resolve(MANY(), SCREEN="T1_knomi"), ["T1_knomi"])


def test_tool_addresses_every_screen_showing_it():
    """Two displays of one tool follow one spool."""
    check("shared tool", sorted(resolve(MANY(), TOOL="0")), ["T0_knomi", "spare"])


def test_tool_accepts_the_forms_a_slicer_writes():
    for form in ("1", "T1", "t1"):
        check(f"TOOL={form}", resolve(MANY(), TOOL=form), ["T1_knomi"])


def test_unknown_targets_are_refused_by_name():
    if "nope" not in refuses(MANY(), SCREEN="nope"):
        raise AssertionError("refusal should quote the name asked for")
    if "9" not in refuses(MANY(), TOOL="9"):
        raise AssertionError("refusal should quote the tool asked for")


def test_both_at_once_is_refused():
    """Not silently picking one - they could disagree."""
    refuses(MANY(), SCREEN="T0_knomi", TOOL="1")


def test_a_screen_without_a_tool_keeps_its_state():
    """The bug that moved addressing off tools in the first place.

    tool_state(None) used to hand back a fresh record that was never stored, so
    a single display declaring no `tool:` got default colour and material back
    on every tick and nothing could ever set them.
    """
    c = ONE()
    c.tool_state("knomi_serial").color = 0x9572BF
    check("colour survives", c.tool_state("knomi_serial").color, 0x9572BF)


def test_ending_a_print_clears_used_everywhere():
    c = MANY()
    for name in ("T0_knomi", "T1_knomi"):
        c.tool_state(name).used = False
    c.was_printing = True
    c._track_job("complete")
    for name in ("T0_knomi", "T1_knomi"):
        check(f"{name} used", c.tool_state(name).used, True)


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
