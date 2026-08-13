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


def device_map(**kw):
    """A device with just the fields the cluster's `devices` map reads."""
    d = k.Knomi_Serial.__new__(k.Knomi_Serial)
    d.screen_name = kw.get("screen_name", "T0_knomi")
    d.name = kw.get("name", f"knomi_serial {d.screen_name}")
    d.config_serial = kw.get("config_serial")
    d.config_device_id = kw.get("config_device_id")
    d.config_tool = kw.get("config_tool", "0")
    d.resolved_port = kw.get("resolved_port")
    d.device_report = kw.get("report", {})
    d.device_report_time = kw.get("seen")
    d.reactor = type("R", (), {"monotonic": staticmethod(lambda: 100.0)})()
    return d


REPORT = {"id": "19aa44", "fw": "0.5.0", "proto": "5", "var": "knomi"}


def test_device_map_names_the_hardware_not_the_socket():
    """What mcu-updater reads instead of parsing `serial:` out of printer.cfg."""
    c = cluster(device_map(config_device_id="19aa44", resolved_port="/dev/ttyUSB3",
                           report=REPORT, seen=99.0))
    got = c.get_status(0.0)["devices"]["T0_knomi"]
    check("id", got["device_id"], "19aa44")
    check("resolved port", got["port"], "/dev/ttyUSB3")
    check("env", got["build_variant"], "knomi")
    check("firmware", got["firmware_version"], "0.5.0")
    check("proto is a number", got["protocol_version"], 5)
    check("online", got["online"], True)
    check("how", got["addressed_by"], "device_id")


def test_the_map_carries_the_full_section_name():
    """So nothing has to rebuild it from the key, which is not uniform.

    A named section keys on its suffix and a bare one keys on the whole name, so
    prefixing the key is right for `[knomi_serial T0_knomi]` and wrong for
    `[knomi_serial]` - the single-display case, which is most people.
    """
    c = cluster(device_map(screen_name="T0_knomi"),
                device_map(screen_name="knomi_serial",
                           name="knomi_serial"))
    got = c.get_status(0.0)["devices"]
    check("named", got["T0_knomi"]["section"], "knomi_serial T0_knomi")
    check("bare", got["knomi_serial"]["section"], "knomi_serial")


def test_a_path_addressed_section_still_reports_its_id():
    """`serial:` sections must appear too, or an updater would skip them."""
    c = cluster(device_map(config_serial="/dev/ttyUSB0", report=REPORT, seen=99.0))
    got = c.get_status(0.0)["devices"]["T0_knomi"]
    check("port", got["port"], "/dev/ttyUSB0")
    check("id came from the device", got["device_id"], "19aa44")
    check("how", got["addressed_by"], "serial")


def test_a_device_that_never_answered_is_listed_as_offline():
    """An updater must see the screen that needs flashing, not an absent key."""
    c = cluster(device_map(config_device_id="19aa44"))
    got = c.get_status(0.0)["devices"]["T0_knomi"]
    check("still listed", got["device_id"], "19aa44")
    check("no port yet", got["port"], None)
    check("offline", got["online"], False)
    check("no version", got["firmware_version"], None)


def test_every_screen_appears_once():
    c = cluster(device_map(screen_name="T0_knomi", config_device_id="19aa44"),
                device_map(screen_name="T1_knomi", config_device_id="19aa45"))
    check("both", sorted(c.get_status(0.0)["devices"]), ["T0_knomi", "T1_knomi"])


class FakeConfigError(Exception):
    pass


def startup(devices, ports, printing=False):
    """A cluster that has just run its klippy:connect discovery pass."""
    c = k.KnomiCluster.__new__(k.KnomiCluster)
    c.devices = list(devices)
    c.tools = {}
    c._ports = dict(ports)
    c._discover_after = 0
    c.printer = type("P", (), {
        "config_error": staticmethod(lambda m: FakeConfigError(m))})()
    state = "printing" if printing else "standby"
    c.print_stats = type("S", (), {
        "get_status": staticmethod(lambda e: {"state": state})})()
    c.reactor = type("R", (), {"monotonic": staticmethod(lambda: 100.0)})()
    return c


def refuses_config(c, phrase):
    try:
        c._check_collisions()
    except FakeConfigError as e:
        if phrase not in str(e):
            raise AssertionError(f"wrong reason: {e}") from None
        return
    raise AssertionError(f"accepted a config it should have refused ({phrase})")


def test_two_sections_may_not_share_one_device_id():
    c = startup([device_map(screen_name="T0_knomi", config_device_id="19aa44"),
                 device_map(screen_name="T1_knomi", config_device_id="19aa44")],
                {"19aa44": "/dev/ttyUSB0"})
    refuses_config(c, "cannot be two screens")


def test_a_serial_path_and_a_device_id_may_not_be_one_display():
    """The reason discovery probes serial: ports too - this is unfindable later.

    Once the serial: section has the port open, nothing can ask what is on the
    end of it, so this collision has to be caught before anything connects.
    """
    c = startup([device_map(screen_name="T0_knomi", config_serial="/dev/ttyUSB0"),
                 device_map(screen_name="T1_knomi", config_device_id="19aa44")],
                {"19aa44": "/dev/ttyUSB0"})
    refuses_config(c, "also claims with device_id")


def test_two_serial_sections_may_not_share_a_path():
    c = startup([device_map(screen_name="T0_knomi", config_serial="/dev/ttyUSB0"),
                 device_map(screen_name="T1_knomi", config_serial="/dev/ttyUSB0")],
                {})
    refuses_config(c, "both have serial")


def test_a_sound_config_is_accepted():
    c = startup([device_map(screen_name="T0_knomi", config_serial="/dev/ttyUSB0"),
                 device_map(screen_name="T1_knomi", config_device_id="19aa45")],
                {"19aa44": "/dev/ttyUSB0", "19aa45": "/dev/ttyUSB1"})
    c._check_collisions()   # must not raise


def test_discovery_never_runs_mid_print():
    """It blocks the reactor thread, which feeds the steppers."""
    c = startup([device_map(screen_name="T0_knomi", config_device_id="ffffff")],
                {}, printing=True)
    probed = []
    c._discover_once = lambda skip=(): probed.append(skip)
    check("no port opened", c.resolve_port("ffffff"), None)
    check("discovery not attempted", probed, [])


class FakeSerial:
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


def connected(ident, port="/dev/ttyUSB0"):
    """A section that believes it has found its display and opened it."""
    d = device_map(config_device_id=ident, resolved_port=port)
    d.name = "knomi_serial T0_knomi"
    d.serial = FakeSerial()
    d.pending_cmd = b""
    d.warned_proto = None
    d.module_version = "0.5.0"
    d.cluster = startup([d], {ident: port})
    return d


def test_the_right_display_is_kept():
    d = connected("19aa44")
    d._verify_identity("19aa44")
    check("still connected", d.serial is None, False)
    check("port kept", d.resolved_port, "/dev/ttyUSB0")


def test_the_wrong_display_is_dropped_not_driven():
    """A cable moved between discovery and connect. Without this the section
    drives the wrong screen and everything looks healthy."""
    d = connected("19aa44")
    d._verify_identity("19aa45")
    check("dropped", d.serial, None)
    check("port forgotten", d.resolved_port, None)
    check("cache invalidated", d.cluster._ports, {})


def test_a_serial_section_has_nothing_to_verify():
    """The path is the address; whatever answers on it is what was asked for."""
    d = connected("19aa44")
    d.config_device_id = None
    d._verify_identity("19aa45")
    check("left alone", d.serial is None, False)


def test_firmware_too_old_to_report_an_id_is_not_dropped():
    d = connected("19aa44")
    d._verify_identity(None)
    check("left alone", d.serial is None, False)


def test_the_report_parser_lowercases_the_id_everywhere():
    """_process_report used to have its own parser that skipped this."""
    got = k.parse_report(b"id=19AA44;fw=0.5.0;proto=5")
    check("id", got["id"], "19aa44")
    check("others untouched", got["fw"], "0.5.0")


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
