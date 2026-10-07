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
        if key not in self.params:
            return default
        try:
            value = int(self.params[key])
        except (TypeError, ValueError):
            raise self.error(f"{key} must be an integer") from None
        if minval is not None and value < minval:
            raise self.error(f"{key} must be at least {minval}")
        if maxval is not None and value > maxval:
            raise self.error(f"{key} must be at most {maxval}")
        return value


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


_REQUIRED_OBJECT = object()


class FakeGcodeRegistry:
    def __init__(self):
        self.commands = {}
        self.scripts = []

    def register_command(self, *args, **kwargs):
        self.commands[args[0]] = args[1]

    def run_script(self, script):
        self.scripts.append(script)


class FakeReactor:
    pass


class FakePrinter:
    config_error = FakeConfigError

    def __init__(self):
        self.objects = {"gcode": FakeGcodeRegistry()}
        self.handlers = {}

    def get_reactor(self):
        return FakeReactor()

    def lookup_object(self, name, default=_REQUIRED_OBJECT):
        if name in self.objects:
            return self.objects[name]
        if default is not _REQUIRED_OBJECT:
            return default
        raise self.config_error(f"Unknown object '{name}'")

    def add_object(self, name, value):
        self.objects[name] = value

    def register_event_handler(self, event, handler):
        self.handlers.setdefault(event, []).append(handler)


class FakeSection:
    def __init__(self, values=None, printer=None, name="knomi_serial T0_knomi"):
        self.values = dict(values or {})
        self.printer = printer or FakePrinter()
        self.name = name

    def get_name(self):
        return self.name

    def get_printer(self):
        return self.printer

    def get(self, key, default=None):
        return self.values.get(key, default)

    def get_prefix_options(self, prefix):
        return sorted(key for key in self.values if key.startswith(prefix))

    def getfloat(self, key, default=None, above=None):
        value = float(self.values[key]) if key in self.values else default
        if above is not None and not value > above:
            raise self.error(f"{key} must be above {above}")
        return value

    @staticmethod
    def error(message):
        return FakeConfigError(message)


def configured(printer=None, name="knomi_serial T0_knomi", **values):
    values.setdefault("device_id", "19aa44")
    return k.Knomi_Serial(FakeSection(values, printer, name))


def refuses_section(phrase, **values):
    try:
        configured(**values)
    except FakeConfigError as e:
        if phrase not in str(e):
            raise AssertionError(f"wrong reason: {e}") from None
        return
    raise AssertionError(f"accepted invalid config {values!r}")


def test_device_id_is_exactly_six_hex_characters():
    for value in ("19aa4", "19aa444", "19xz44", "-12345", "12_345"):
        refuses_section("device_id", device_id=value)
    check("uppercase accepted", configured(device_id="19AA44").config_device_id,
          "19aa44")


def test_colours_are_exactly_rrggbb():
    for value in ("FFF", "1234567", "notpink", "+FA7C4", "FF_A7C"):
        refuses_section("color_machine", color_machine=value)
    configured(color_machine="#FFA7C4")


def test_times_must_be_finite_and_fit_the_wire_field():
    for value in ("nan", "inf", "4294968"):
        refuses_section("dim_time", dim_time=value)
    configured(dim_time="4294967.295")


def test_move_speeds_must_be_positive():
    for key in ("speed_x", "speed_y", "speed_z"):
        refuses_section(key, **{key: 0})
        refuses_section(key, **{key: -1})


def test_numeric_tool_must_fit_the_signed_wire_field():
    refuses_section("tool", tool="T2147483648")
    configured(tool="T2147483647")


def button(option, definition, **extra):
    return configured(**{option: definition, **extra}).button_bindings[0]


def refuses_button(option, definition, *phrases, **extra):
    try:
        configured(**{option: definition, **extra})
    except FakeConfigError as e:
        message = str(e)
        for phrase in ("knomi_serial T0_knomi", option, *phrases):
            if phrase not in message:
                raise AssertionError(f"wrong reason: {e}") from None
        return
    raise AssertionError(f"accepted invalid {option}={definition!r}")


def test_button_sources_and_resolvers_are_normalized():
    feed = button(
        "button_feed",
        "source=gpio\npin=GPIO5\nresolver=observe FEED\nslot=NW",
    )
    check(
        "GPIO observe",
        (feed.name, feed.source.name, feed.pin, feed.resolver.name,
         feed.argument, feed.slot.name),
        ("feed", "GPIO", 5, "OBSERVE", 1, "NW"),
    )

    page = button(
        "button_page",
        "source=touch\nresolver=page\nslot=SE",
    )
    check(
        "touch page",
        (page.source.name, page.resolver.name, page.slot.name),
        ("TOUCH", "PAGE", "SE"),
    )


def test_slotless_observed_buttons_leave_the_display_slots_available():
    for source in ("source=event", "source=gpio\npin=GPIO5"):
        feed = button("button_feed", source + "\nresolver=observe FEED")
        check(
            "slotless observe",
            (feed.slot.name, feed.resolver.name, feed.argument),
            ("NONE", "OBSERVE", 1),
        )


def test_macro_bindings_keep_names_on_the_host():
    event = button(
        "button_check",
        "source=event\nresolver=gcode_macro CHECK_TOOL_FILAMENT_SENSORS",
    )
    check(
        "bare macro is release",
        (event.macro, event.press_macro, event.release_macro, event.legend),
        ("CHECK_TOOL_FILAMENT_SENSORS", None,
         "CHECK_TOOL_FILAMENT_SENSORS", b""),
    )

    touch = button(
        "button_lights",
        "source=touch\nslot=C\nlegend=LGT\n"
        "resolver=gcode_macro TOGGLE_LIGHTS",
    )
    check(
        "visible macro",
        (touch.macro, touch.legend, touch.slot.name),
        ("TOGGLE_LIGHTS", b"LGT", "C"),
    )

    edges = button(
        "button_hold",
        "source=gpio\npin=6\nslot=NONE\n"
        "press_resolver=gcode_macro LIGHT_ON\n"
        "release_resolver=gcode_macro LIGHT_OFF",
    )
    check(
        "edge macros",
        (edges.macro, edges.press_macro, edges.release_macro),
        (None, "LIGHT_ON", "LIGHT_OFF"),
    )


def test_button_definitions_reject_bad_properties_and_combinations():
    cases = (
        ("source", "resolver=page\nslot=NW", ("source", "missing"), {}),
        ("mystery", "source=event\nmystery=yes\nresolver=gcode_macro GO",
         ("mystery", "yes"), {}),
        ("source", "source=serial\nresolver=gcode_macro GO",
         ("source", "serial"), {}),
        ("slot", "source=touch\nslot=N\nresolver=page", ("slot", "N"), {}),
        ("pin", "source=gpio\npin=GPIO10\nresolver=gcode_macro GO",
         ("pin", "GPIO10"), {}),
        ("pin", "source=event\npin=5\nresolver=gcode_macro GO",
         ("pin", "5"), {}),
        ("resolver", "source=event\nresolver=launch GO",
         ("resolver", "launch GO"), {}),
        ("resolver", "source=event\nresolver=gcode_macro SET_PIN PIN=laser VALUE=1",
         ("resolver", "SET_PIN PIN=laser VALUE=1"), {}),
        ("press_resolver", "source=event\npress_resolver=gcode_macro GO;M112",
         ("press_resolver", "GO;M112"), {}),
        ("legend", "source=touch\nslot=C\nlegend=TOOLONG\n"
         "resolver=gcode_macro GO", ("legend", "TOOLONG"), {}),
        ("legend", "source=event\nlegend=GO\nresolver=gcode_macro GO",
         ("legend", "GO"), {}),
        ("slot", "source=touch\nresolver=gcode_macro GO", ("slot", "NONE"), {}),
        ("resolver", "source=touch\nslot=NW\nresolver=observe FEED",
         ("resolver", "observe FEED"), {}),
        ("resolver", "source=event\nslot=NW\nresolver=observe FAN",
         ("resolver", "observe FAN"), {}),
        ("resolver", "source=event\nresolver=internal SCREEN_OFF",
         ("resolver", "internal SCREEN_OFF"), {}),
        ("resolver", "source=event\nresolver=page EXTRA\nslot=NW",
         ("resolver", "page EXTRA"), {}),
        ("resolver", "source=event", ("resolver", "missing"), {}),
        ("resolver", "source=event\nresolver=gcode_macro GO\n"
         "release_resolver=gcode_macro STOP", ("resolver", "release_resolver"), {}),
        ("press_resolver", "source=event\npress_resolver=observe FEED",
         ("press_resolver", "observe FEED"), {}),
        ("hardware_keys", "source=gpio\npin=5\nslot=NW\nresolver=page",
         ("slot", "NW"), {"hardware_keys": "NW"}),
    )
    for suffix, definition, phrases, extra in cases:
        refuses_button(f"button_{suffix}", definition, *phrases, **extra)


def test_button_names_slots_pins_and_capacity_are_unique_and_bounded():
    refuses_button(
        "button_FEED",
        "source=event\nresolver=gcode_macro GO",
        "button_feed",
        "duplicate",
        button_feed="source=event\nresolver=gcode_macro STOP",
    )
    refuses_button(
        "button_second",
        "source=event\nslot=NW\nresolver=page",
        "slot",
        "NW",
        button_first="source=event\nslot=NW\nresolver=page",
    )
    refuses_button(
        "button_second",
        "source=gpio\npin=5\nresolver=gcode_macro TWO",
        "pin",
        "5",
        button_first="source=gpio\npin=5\nresolver=gcode_macro ONE",
    )
    values = {
        f"button_b{i}": f"source=event\nresolver=gcode_macro M{i}"
        for i in range(9)
    }
    refuses_button("button_b8", values.pop("button_b8"), "at most", "8", **values)


def test_home_refuses_five_shared_slots_that_hide_an_action():
    options = {
        f"button_{slot.lower()}":
        f"source=event\nslot={slot}\nresolver=gcode_macro SHARED_{slot}\nlegend={slot}"
        for slot in ("NW", "NE", "C", "SW", "SE")
    }
    refuses_section("no room for every Home action", **options)
    configured(pages="tool, move", **options)
    configured(
        hardware_keys="NW NE SW SE",
        button_c="source=event\nslot=C\nresolver=gcode_macro CENTRE\nlegend=C",
    )


def button_devices(*definitions):
    printer = FakePrinter()
    devices = []
    for index, options in enumerate(definitions):
        device = configured(
            printer, name=f"knomi_serial T{index}_knomi",
            device_id=f"19aa4{index}", **options,
        )
        device.writes = []
        device._write = device.writes.append
        device.gcode = printer.objects["gcode"]
        devices.append(device)
    return printer, devices


def test_knomi_button_registers_and_encodes_a_single_screen_event():
    printer, (device,) = button_devices({
        "button_feed": "source=event\nresolver=gcode_macro FEED",
    })
    command = printer.objects["gcode"].commands["KNOMI_BUTTON"]
    command(FakeGcmd(BUTTON="FeEd", PRESSED="1"))
    command(FakeGcmd(BUTTON="feed", PRESSED="0"))
    check("press and release", device.writes, [
        k.encode_button_event(device.config_crc, 0, True),
        k.encode_button_event(device.config_crc, 0, False),
    ])


def test_knomi_button_validates_every_tool_target_before_writing():
    printer, devices = button_devices(
        {"button_a": "source=event\nresolver=gcode_macro A",
         "button_feed": "source=event\nresolver=gcode_macro FEED", "tool": "0"},
        {"button_feed": "source=touch\nslot=C\nlegend=FEED\n"
                        "resolver=gcode_macro FEED", "tool": "0"},
    )
    command = printer.objects["gcode"].commands["KNOMI_BUTTON"]
    try:
        command(FakeGcmd(TOOL="0", BUTTON="feed", PRESSED="1"))
    except FakeGcmd.error as e:
        if "feed" not in str(e) or "event" not in str(e):
            raise AssertionError(f"wrong refusal: {e}") from None
    else:
        raise AssertionError("accepted a touch binding as an external event")
    check("no partial writes", [d.writes for d in devices], [[], []])

    printer, devices = button_devices(
        {"button_a": "source=event\nresolver=gcode_macro A",
         "button_feed": "source=event\nresolver=gcode_macro FEED", "tool": "0"},
        {"button_feed": "source=event\nresolver=gcode_macro FEED", "tool": "0"},
    )
    command = printer.objects["gcode"].commands["KNOMI_BUTTON"]
    command(FakeGcmd(TOOL="0", BUTTON="FEED", PRESSED="0"))
    check("different indexes", [d.writes for d in devices], [
        [k.encode_button_event(devices[0].config_crc, 1, False)],
        [k.encode_button_event(devices[1].config_crc, 0, False)],
    ])

    printer, devices = button_devices(
        {"button_feed": "source=event\nresolver=gcode_macro FEED", "tool": "0"},
        {"button_other": "source=event\nresolver=gcode_macro OTHER", "tool": "0"},
    )
    command = printer.objects["gcode"].commands["KNOMI_BUTTON"]
    try:
        command(FakeGcmd(TOOL="0", BUTTON="feed", PRESSED="1"))
    except FakeGcmd.error:
        pass
    else:
        raise AssertionError("accepted a missing binding on the second screen")
    check("no writes for missing target", [d.writes for d in devices], [[], []])


def test_knomi_button_refuses_missing_unknown_or_invalid_parameters():
    printer, (device,) = button_devices({
        "button_feed": "source=event\nresolver=gcode_macro FEED",
    })
    command = printer.objects["gcode"].commands["KNOMI_BUTTON"]
    for params in (
        {"PRESSED": "1"},
        {"BUTTON": "unknown", "PRESSED": "1"},
        {"BUTTON": "feed"},
        {"BUTTON": "feed", "PRESSED": "2"},
        {"BUTTON": "feed", "PRESSED": "perhaps"},
    ):
        try:
            command(FakeGcmd(**params))
        except FakeGcmd.error:
            pass
        else:
            raise AssertionError(f"accepted invalid KNOMI_BUTTON {params!r}")
    check("invalid commands send nothing", device.writes, [])


def test_button_macro_commands_resolve_only_current_configured_edges():
    printer, (device,) = button_devices({
        "button_bare": "source=event\nresolver=gcode_macro BARE",
        "button_edges": "source=event\npress_resolver=gcode_macro ON\n"
                        "release_resolver=gcode_macro OFF",
        "button_page": "source=event\nslot=NW\nresolver=page",
    })
    crc = f"{device.config_crc:08x}".encode()
    for suffix in (b"0:R", b"1:P", b"1:R"):
        device._process_cmd(b"BUTTON:" + crc + b":" + suffix)
    check("configured macros", printer.objects["gcode"].scripts,
          ["BARE", "ON", "OFF"])

    stale = b"00000000" if crc != b"00000000" else b"ffffffff"
    device._process_cmd(b"BUTTON:" + stale + b":0:R")
    for invalid in (
        b"broken:0:R", b"", b"0:P",
        b"2:R", b"8:R", b"-1:R", b"1:X", b"1:R:EXTRA",
    ):
        device._process_cmd(b"BUTTON:" + crc + b":" + invalid)
    check("invalid events ignored", printer.objects["gcode"].scripts,
          ["BARE", "ON", "OFF"])


class FakeTemperature:
    def __init__(self, temp=25, target=0):
        self.reading = temp, target

    def get_temp(self, eventtime):
        return self.reading


class FakeHeaters:
    def __init__(self, **heaters):
        self.heaters = heaters

    def lookup_heater(self, name):
        if name not in self.heaters:
            raise FakeConfigError(f"Unknown heater '{name}'")
        return self.heaters[name]


def temperature_setup(*sections, heaters=None, objects=None):
    printer = FakePrinter()
    printer.objects.update(objects or {})
    printer.objects["heaters"] = FakeHeaters(**(heaters or {}))
    devices = []
    for index, values in enumerate(sections):
        values = dict(values)
        values.setdefault("device_id", f"19aa4{index}")
        devices.append(configured(
            printer,
            name=f"knomi_serial T{index}_knomi",
            **values,
        ))
    cluster = devices[0].cluster
    cluster.heaters = printer.objects["heaters"]
    return cluster, devices


def refuses_temperature(cluster, *phrases):
    try:
        cluster._resolve_temperature_sources()
    except FakeConfigError as e:
        message = str(e)
        for phrase in phrases:
            if phrase not in message:
                raise AssertionError(f"wrong reason: {e}") from None
        return
    raise AssertionError(f"accepted invalid temperature config ({phrases!r})")


def test_temperature_references_are_validated_before_updates_start():
    cases = (
        ({"heater_hotend": "missing"}, "heater_hotend"),
        ({"heater_bed": "missing"}, "heater_bed"),
        ({"heater_chamber": "missing"}, "heater_chamber"),
        ({"sensor_chamber": "missing"}, "sensor_chamber"),
        ({"sensor_mcu": "missing"}, "sensor_mcu"),
    )
    for values, option in cases:
        cluster, _ = temperature_setup(values)
        refuses_temperature(cluster, option, "missing")


def test_sensor_mcu_refuses_an_object_that_is_not_a_temperature_source():
    cluster, _ = temperature_setup(
        {"sensor_mcu": "mcu"},
        objects={"mcu": object()},
    )
    refuses_temperature(cluster, "sensor_mcu", "mcu", "temperature")


def test_shared_temperature_sources_may_not_disagree():
    cluster, _ = temperature_setup(
        {"heater_bed": "bed"},
        {"heater_bed": "other_bed"},
        heaters={"bed": FakeTemperature(), "other_bed": FakeTemperature()},
    )
    refuses_temperature(cluster, "heater_bed", "T0_knomi", "T1_knomi")

    cluster, _ = temperature_setup(
        {"heater_chamber": "chamber"},
        {"sensor_chamber": "chamber_sensor"},
        heaters={"chamber": FakeTemperature()},
        objects={"temperature_sensor chamber_sensor": FakeTemperature()},
    )
    refuses_temperature(cluster, "chamber", "T0_knomi", "T1_knomi")


def test_valid_temperature_sources_are_resolved_once():
    extruder = FakeTemperature(210, 220)
    bed = FakeTemperature(60, 65)
    chamber = FakeTemperature(45)
    mcu_sensor = FakeTemperature(52)
    mcu_fan = FakeTemperature(48, 50)
    custom_sensor = FakeTemperature(41)
    cluster, devices = temperature_setup(
        {
            "heater_hotend": "extruder",
            "heater_bed": "bed",
            "sensor_chamber": "chamber",
            "sensor_mcu": "toolboard",
        },
        {
            "heater_bed": "bed",
            "sensor_chamber": "chamber",
            "sensor_mcu": "mcu_fan",
        },
        {
            "heater_bed": "bed",
            "sensor_chamber": "chamber",
            "sensor_mcu": "custom_sensor",
        },
        heaters={"extruder": extruder, "bed": bed},
        objects={
            "temperature_sensor chamber": chamber,
            "temperature_sensor toolboard": mcu_sensor,
            "temperature_fan mcu_fan": mcu_fan,
            "custom_sensor": custom_sensor,
        },
    )
    cluster._resolve_temperature_sources()
    check("hotend cached", devices[0].hotend, extruder)
    check("bed cached", cluster.bed_sensor, bed)
    check("chamber cached", cluster.chamber_sensor, chamber)
    check("temperature sensor cached", devices[0].mcu_sensor, mcu_sensor)
    check("temperature fan cached", devices[1].mcu_sensor, mcu_fan)
    check("custom temperature source cached", devices[2].mcu_sensor, custom_sensor)


def startup(devices, ports, printing=False):
    """A cluster that has just run its klippy:connect discovery pass."""
    c = k.KnomiCluster.__new__(k.KnomiCluster)
    c.devices = list(devices)
    c.tools = {}
    c._ports = dict(ports)
    c._discover_after = 0
    c._rejected = set()
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
    # And the pairing is remembered, so the watcher's map cannot hand back the
    # same wrong answer on the next pass.
    check("pairing rejected", d.cluster._rejected,
          {("19aa44", "/dev/ttyUSB0")})


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
