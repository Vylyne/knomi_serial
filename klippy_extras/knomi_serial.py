"""Klipper extra module for Knomi_Serial.

Each `[knomi_serial <name>]` section is one screen on one serial port. They are
coordinated by a single cluster object rather than acting independently: the
printer state every screen shows - bed, chamber, progress, homing, motion - is
identical, so computing it once per tick instead of once per screen per tick is
the difference between one lookup and N at 10Hz. The cluster also owns the
things that are true of the machine rather than of a screen: which tools a job
uses, and which tool is mounted right now.
"""

import dataclasses
import enum
import logging
import os
import struct
import zlib

import serial

_BAUD_RATE = 115200
_HEADER = b"\x83\xad\x83\xad"
_FOOTER = b"\xf0\x07\xf0\x07"

#: Frame types, against `enum class Frame` in src/printer/printer.h. Every frame
#: is HEADER(4) TYPE(1) LEN(2) PAYLOAD(LEN) FOOTER(4), network order throughout.
_FRAME_STATE = 0x01
_FRAME_CONFIG = 0x02
_FRAME_MESSAGE = 0x03

_RECV_PERIOD = 0.1
_SEND_PERIOD = 0.1

#: How long a write may block before the device is treated as wedged. pyserial
#: defaults to blocking forever, and this runs on Klipper's reactor thread -
#: which is also the thread talking to the MCUs. A screen that stops draining
#: its buffer would stall the whole reactor and surface as a timing error, with
#: nothing pointing at the display.
_WRITE_TIMEOUT = 0.2

#: A port that failed is retried on this cadence rather than being abandoned
#: until the next Klipper restart.
_RECONNECT_PERIOD = 5.0

_GCODES_MAX_LEN = 255
_FILAMENT_TYPE_MAX_LEN = 15
_MESSAGE_MAX_LEN = 127

#: Sent where the host has no answer - an unsliced print with no layer count, a
#: job too young to estimate. Distinct from zero, which is a real answer.
_UNKNOWN = -1

#: Wire format version. Must be kept in sync with printer::kProtoVersion in
#: src/printer/printer.h.
#:
#: 3: typed frames. Everything that does not change from tick to tick left the
#:    state packet - which was most of it. See the note above _STATE_FMT.
_PROTO_VERSION = 3

#: The state frame's payload, field for field against `struct State` in
#: src/printer/printer.h, down to and including filament_type. `!` means network
#: order and no padding of its own, so the layout here is the struct's layout.
#:
#: 96 bytes, against proto 2's 332. The macro list was 256 of those and had not
#: changed since Klipper started; at 10Hz it alone was 2.5 kB/s of an 11.5 kB/s
#: link, spent restating a constant. It now goes in _CONFIG_FMT, sent when the
#: device asks for it.
#:
#: Changing this means changing that struct, bumping _PROTO_VERSION, and
#: updating the static_asserts that pin its size.
_STATE_FMT = "!I7?B10iI5iI16s"
_STATE_SIZE = struct.calcsize(_STATE_FMT)

#: The config frame's payload, against `struct Config`. The `x` is its explicit
#: padding, keeping gcodes on a 4-byte boundary.
_CONFIG_FMT = "!5I3Bx256s"
_CONFIG_SIZE = struct.calcsize(_CONFIG_FMT)

#: Bits of Config.present, against `enum ConfigHas`. A field whose bit is clear
#: keeps whatever the firmware was built with, so printer.cfg overrides the
#: defaults in user_conf.h rather than replacing them.
_HAS_COLOR_MACHINE = 1 << 0
_HAS_COLOR_UNKNOWN = 1 << 1
_HAS_DIM_MS = 1 << 2
_HAS_SLEEP_MS = 1 << 3
_HAS_BRIGHTNESS = 1 << 4
_HAS_DIM_BRIGHTNESS = 1 << 5
_HAS_GCODES = 1 << 6
_HAS_KEY_MASK = 1 << 7

#: Bits of Config.key_mask, against `enum KeySlot`. A corner named here keeps
#: its symbol as a legend and loses its touch target, because something else
#: already reports that press.
_KEY_SLOTS = {"NW": 1 << 0, "NE": 1 << 1, "SW": 1 << 2, "SE": 1 << 3}

#: Used only if the VERSION file cannot be found next to this module, which
#: happens if knomi_serial.py was copied into klippy/extras rather than
#: symlinked there by install.sh.
_FALLBACK_VERSION = "0.5.0"

#: A device reporting less often than this is treated as offline. The firmware
#: reports every REPORT_PERIOD_MS (2s), so this allows a couple of missed ones.
_DEVICE_TIMEOUT = 10.0

_CMD_PREFIX = b"KNOMI_CMD:"
_CMD_MAX_LEN = 512
_CMD_STOP = b"STOP"
_CMD_RESTART = b"RESTART"
_CMD_GCODE = b"GCODE:"
_CMD_MOVE = b"MOVE:"
_CMD_REPORT = b"RPT:"
_CMD_CONFIG_REQUEST = b"CFG?"

#: Distinct from None, which is a legitimate "device sent no proto key" value.
_UNSET = object()

#: Print states in which a job is underway. Leaving this set is what clears the
#: per-tool `used` flags.
_ACTIVE_PRINT_STATES = ("printing", "paused")


def _module_version():
    path = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
    try:
        with open(os.path.join(path, "VERSION")) as f:
            return f.read().strip() or _FALLBACK_VERSION
    except OSError:
        return _FALLBACK_VERSION


def encode_frame(frame_type, payload):
    """Wrap a payload in the framing every message shares.

    The type and length are what make more than one kind of message possible: a
    device can skip a frame it has no name for instead of losing sync, which is
    what lets a firmware and a host disagree about the protocol's edges without
    disagreeing about the middle.
    """
    return (
        _HEADER
        + struct.pack("!BH", frame_type, len(payload))
        + payload
        + _FOOTER
    )


def encode_state(state):
    """A PrinterState as the bytes that go on the wire, framing included.

    Module level and public so scripts/simulate.py can drive a display with the
    exact encoder Klipper uses. A second implementation would drift, and then a
    bench test would be exercising the test rig rather than the firmware.
    """
    # The trailing `s` field pads with NULs and truncates on its own, which is
    # exactly the fixed-width behaviour the device reads back.
    payload = struct.pack(
        _STATE_FMT,
        state.status.value,
        state.working,
        state.paused,
        state.homed_x,
        state.homed_y,
        state.homed_z,
        state.used,
        state.active,
        state.tram_type.value,
        int(state.hotend_temp),
        int(state.hotend_target),
        int(state.bed_temp),
        int(state.bed_target),
        int(state.chamber_temp),
        int(state.chamber_target),
        int(state.mcu_temp),
        int(state.mcu_target),
        int(state.progress),
        int(state.tool_number),
        int(state.filament_color),
        int(state.flow),
        int(state.eta),
        int(state.elapsed),
        int(state.layer),
        int(state.layer_total),
        int(state.config_crc),
        state.filament_type,
    )
    return encode_frame(_FRAME_STATE, payload)


def encode_config(config):
    """A DeviceConfig as the bytes that go on the wire, framing included."""
    return encode_frame(_FRAME_CONFIG, config_payload(config))


def config_payload(config):
    """The config frame's payload alone, which is what gets hashed.

    Separate from encode_config because the CRC has to be over exactly the bytes
    the device will hash back, and the device only ever sees the payload.
    """
    return struct.pack(
        _CONFIG_FMT,
        config.present,
        config.color_machine,
        config.color_filament_unknown,
        config.dim_ms,
        config.sleep_ms,
        config.brightness,
        config.dim_brightness,
        config.key_mask,
        config.gcodes,
    )


def encode_message(text):
    """Something to put in front of the operator, framing included."""
    if isinstance(text, str):
        text = text.encode("utf-8")
    return encode_frame(_FRAME_MESSAGE, text[:_MESSAGE_MAX_LEN])


def _normalize_tool(value):
    """`T0`, `t0` and `0` are the same tool.

    The config says `tool: T0` because that is what the printer calls it, while
    a slicer emitting `KNOMI_TOOL TOOL={i}` has only the index. Normalizing both
    ends to the bare number means neither has to know what the other wrote.
    """
    if value is None:
        return None
    text = str(value).strip().upper()
    if text.startswith("T"):
        text = text[1:]
    return text or None


class PrinterStatus(enum.Enum):
    DISCONNECTED = 0x00
    IDLE = 0x01
    PRINTING = 0x02
    SHUTDOWN = 0x03


class PrinterTramType(enum.Enum):
    NONE = 0x00
    ZTA = 0x01
    QGL = 0x02


@dataclasses.dataclass(frozen=True)
class PrinterState:
    status: PrinterStatus = PrinterStatus.DISCONNECTED

    working: bool = False
    paused: bool = False
    homed_x: bool = False
    homed_y: bool = False
    homed_z: bool = False
    used: bool = True
    active: bool = False

    hotend_temp: float = 0
    hotend_target: float = 0

    bed_temp: float = 0
    bed_target: float = 0

    chamber_temp: float = 0
    chamber_target: float = 0

    mcu_temp: float = 0
    mcu_target: float = 0

    progress: float = 0

    tool_number: int = -1
    filament_color: int = 0

    #: Extrusion rate in micrometres of filament per second, signed.
    flow: int = 0

    #: Seconds left and seconds so far, or _UNKNOWN.
    eta: int = _UNKNOWN
    elapsed: int = _UNKNOWN

    layer: int = _UNKNOWN
    layer_total: int = _UNKNOWN

    #: CRC32 of the config payload this host is holding. The device compares it
    #: against the config it has and asks again if they differ.
    config_crc: int = 0

    tram_type: PrinterTramType = PrinterTramType.NONE

    filament_type: bytes = b""


@dataclasses.dataclass(frozen=True)
class DeviceConfig:
    """Settings pushed once, rather than restated ten times a second.

    Only fields whose bit is set in `present` are applied - the rest keep the
    firmware's compiled-in defaults, so an option absent from printer.cfg leaves
    user_conf.h in charge of it.
    """

    present: int = 0

    color_machine: int = 0
    color_filament_unknown: int = 0

    dim_ms: int = 0
    sleep_ms: int = 0

    brightness: int = 0
    dim_brightness: int = 0
    key_mask: int = 0

    gcodes: bytes = b""


@dataclasses.dataclass(frozen=True)
class SharedState:
    """The part of the packet that is identical on every screen."""

    status: PrinterStatus = PrinterStatus.DISCONNECTED
    working: bool = False
    paused: bool = False
    homed_x: bool = False
    homed_y: bool = False
    homed_z: bool = False
    bed_temp: float = 0
    bed_target: float = 0
    chamber_temp: float = 0
    chamber_target: float = 0
    progress: float = 0
    tram_type: PrinterTramType = PrinterTramType.NONE
    #: Name of the extruder the toolhead currently has mounted, so each screen
    #: can decide whether it is the active one. Derived from the toolhead rather
    #: than from a toolchanger module, so this works on any printer.
    active_extruder: str = ""
    #: Extrusion rate of whichever extruder is mounted, micrometres per second.
    #: Only the active tool's screen gets it - a docked tool is not extruding,
    #: whatever the toolhead is doing.
    flow: int = 0
    eta: int = _UNKNOWN
    elapsed: int = _UNKNOWN
    layer: int = _UNKNOWN
    layer_total: int = _UNKNOWN


@dataclasses.dataclass
class ToolState:
    """What the host knows about one tool's filament and its role in the job."""

    #: True until something says otherwise. A module restart mid-print must not
    #: black out every screen, and a wrongly-awake screen is a far cheaper
    #: mistake than a wrongly-dark one.
    used: bool = True
    color: int = 0
    type: str = ""


class KnomiCluster:
    """Shared timers, shared printer state, and the per-tool job picture."""

    def __init__(self, printer):
        self.printer = printer
        self.reactor = printer.get_reactor()
        self.devices = []
        self.tools = {}

        self.gcode = printer.lookup_object("gcode")
        self.heaters = None
        self.toolhead = None
        self.virtual_sdcard = None
        self.print_stats = None
        self.motion_report = None

        self.last_busy = 0
        self.was_printing = False
        self.tram_type = PrinterTramType.NONE

        self.gcode.register_command(
            "KNOMI_TOOL",
            self.cmd_KNOMI_TOOL,
            desc=self.cmd_KNOMI_TOOL_help,
        )

        printer.register_event_handler("klippy:ready", self._handle_ready)

    def register(self, device):
        self.devices.append(device)

    def tool_state(self, screen):
        """The filament record for one screen, created on first use.

        Keyed by screen rather than by tool. It was keyed by the normalised
        `tool:` value, which meant a section that declared no tool had no key -
        `tool_state(None)` handed back a fresh ToolState that was never stored,
        so a single-display machine got default colour and material back on
        every tick and nothing could ever set them.
        """
        if screen not in self.tools:
            self.tools[screen] = ToolState()
        return self.tools[screen]

    def _names(self):
        return ", ".join(sorted(d.screen_name for d in self.devices))

    def resolve(self, gcmd):
        """Which screens a command is addressed to, as a list of keys.

        Two ways in, because two different callers need different things.

        `SCREEN=` names the object, which is what Klipper does everywhere else -
        `HEATER=`, `FAN=`, `PIN=` - and is the only form that can reach a
        section declaring no `tool:` at all.

        `TOOL=` addresses by the `tool:` value instead, and exists because it is
        the only form a slicer can emit generically: `TOOL={i}` sits in the same
        loop as `filament_colour[i]`, so the macro writes itself. Naming screens
        means writing that mapping out by hand in PRINT_START.

        It returns a list because `TOOL=` may match more than one screen, and
        two displays showing one tool should follow one spool.

        Neither is needed when there is only one screen - there is nothing to
        disambiguate, and asking would be ceremony.
        """
        name = gcmd.get("SCREEN", None)
        tool = _normalize_tool(gcmd.get("TOOL", None))

        if name is not None and tool is not None:
            raise gcmd.error("Give SCREEN= or TOOL=, not both")

        if name is not None:
            wanted = name.strip()
            for device in self.devices:
                if device.screen_name == wanted:
                    return [device.screen_name]
            raise gcmd.error(
                f"No screen named '{wanted}'. Configured: {self._names()}")

        if tool is not None:
            matched = [d.screen_name for d in self.devices if d.config_tool == tool]
            if not matched:
                raise gcmd.error(
                    f"No screen declares tool '{tool}'. Configured: {self._names()}")
            return matched

        if len(self.devices) == 1:
            return [self.devices[0].screen_name]
        raise gcmd.error(
            "SCREEN= or TOOL= is required when more than one screen is "
            f"configured: {self._names()}")

    def _handle_ready(self):
        self.heaters = self.printer.lookup_object("heaters")
        self.toolhead = self.printer.lookup_object("toolhead")
        self.virtual_sdcard = self.printer.lookup_object("virtual_sdcard")
        self.print_stats = self.printer.lookup_object("print_stats")

        # Core Klipper, loaded on every printer - but looked up softly, because
        # a fork that drops it should cost the screens their wave animation and
        # nothing else.
        self.motion_report = self.printer.lookup_object("motion_report", None)

        if self.printer.lookup_object("z_tilt", None):
            self.tram_type = PrinterTramType.ZTA
        elif self.printer.lookup_object("quad_gantry_level", None):
            self.tram_type = PrinterTramType.QGL
        else:
            self.tram_type = PrinterTramType.NONE

        self.reactor.register_timer(self._handle_update, self.reactor.NOW)
        self.reactor.register_timer(self._handle_read, self.reactor.NOW)

    def _handle_update(self, eventtime):
        try:
            shared = self._compute_shared(eventtime)
        except Exception as e:
            logging.warning(f"knomi_serial: Shared update error: {e}")
            return eventtime + _SEND_PERIOD

        for device in self.devices:
            try:
                device.update(eventtime, shared)
            except Exception as e:
                logging.warning(f"{device.name}: Update error: {e}")
        return eventtime + _SEND_PERIOD

    def _handle_read(self, eventtime):
        for device in self.devices:
            try:
                device.poll(eventtime)
            except Exception as e:
                logging.warning(f"{device.name}: Read error: {e}")
        return eventtime + _RECV_PERIOD

    def _compute_shared(self, eventtime):
        if self.printer.is_shutdown():
            return None

        status = PrinterStatus.IDLE
        paused = False
        if self.print_stats.state == "printing":
            status = PrinterStatus.PRINTING
        if self.print_stats.state == "paused":
            status = PrinterStatus.PRINTING
            paused = True

        self._track_job(self.print_stats.state)

        _, _, lookahead_empty = self.toolhead.check_busy(eventtime)
        if not lookahead_empty:
            self.last_busy = eventtime
        working = eventtime - self.last_busy < 1

        toolhead_status = self.toolhead.get_status(eventtime)
        homed = toolhead_status["homed_axes"]

        bed_temp, bed_target = self._bed(eventtime)
        chamber_temp, chamber_target = self._chamber(eventtime)

        # Workaround for Danger Klipper.
        if hasattr(self.virtual_sdcard, "progress"):
            provider = self.virtual_sdcard
        else:
            provider = self.virtual_sdcard.get_virtual_sdcard_gcode_provider()

        progress = provider.progress() * 100
        elapsed, eta, layer, layer_total = self._job_timing(eventtime, progress)

        return SharedState(
            status=status,
            working=working,
            paused=paused,
            homed_x="x" in homed,
            homed_y="y" in homed,
            homed_z="z" in homed,
            bed_temp=bed_temp,
            bed_target=bed_target,
            chamber_temp=chamber_temp,
            chamber_target=chamber_target,
            progress=progress,
            tram_type=self.tram_type,
            active_extruder=self.toolhead.get_extruder().get_name(),
            flow=self._flow(eventtime),
            eta=eta,
            elapsed=elapsed,
            layer=layer,
            layer_total=layer_total,
        )

    def _flow(self, eventtime):
        """Extrusion rate in micrometres per second, signed.

        Taken from motion_report rather than differencing the extruder position
        between ticks. The position is the commanded one, which runs a full
        lookahead queue ahead of what the nozzle is doing and jumps in bursts as
        the queue fills - a wave driven by it would surge and stall while the
        print ran smoothly. It is also reset by any G92 E0, which some slicers
        emit every layer, so every reset would read as an enormous retraction.
        motion_report samples the trapezoid queue at the current print time,
        which is the same thing the stepper is being told.
        """
        if self.motion_report is None:
            return 0
        try:
            status = self.motion_report.get_status(eventtime)
            return int(status["live_extruder_velocity"] * 1000)
        except (KeyError, TypeError, ValueError):
            return 0

    def _job_timing(self, eventtime, progress):
        """Seconds elapsed, seconds left, and where we are in the layer stack.

        The estimate is file progress extrapolated linearly, which is the same
        arithmetic every other display does and is wrong in the same familiar
        ways - long first layers pull it high, and it settles as the job runs.
        Klipper does not compute one, and inventing a better model here would
        mean disagreeing with the number the user already sees in Mainsail.
        """
        try:
            stats = self.print_stats.get_status(eventtime)
        except Exception:
            return _UNKNOWN, _UNKNOWN, _UNKNOWN, _UNKNOWN

        if stats.get("state") not in _ACTIVE_PRINT_STATES:
            return _UNKNOWN, _UNKNOWN, _UNKNOWN, _UNKNOWN

        elapsed = int(stats.get("print_duration") or 0)

        eta = _UNKNOWN
        # Below a percent the extrapolation divides by almost nothing and
        # produces days. Better to say nothing until the job has some history.
        if progress >= 1.0 and elapsed > 0:
            eta = int(elapsed * (100.0 - progress) / progress)

        info = stats.get("info") or {}
        layer = info.get("current_layer")
        layer_total = info.get("total_layer")
        return (
            elapsed,
            eta,
            _UNKNOWN if layer is None else int(layer),
            _UNKNOWN if layer_total is None else int(layer_total),
        )

    def _bed(self, eventtime):
        """Whichever bed any device configured. There is only one bed."""
        for device in self.devices:
            if device.config_bed:
                return self.heaters.lookup_heater(device.config_bed).get_temp(eventtime)
        return 0, 0

    def _chamber(self, eventtime):
        """Whichever chamber source any device configured. They cannot disagree."""
        for device in self.devices:
            if device.config_heater_chamber:
                heater = self.heaters.lookup_heater(device.config_heater_chamber)
                return heater.get_temp(eventtime)
            if device.config_sensor_chamber:
                sensor = self.printer.lookup_object(
                    f"temperature_sensor {device.config_sensor_chamber}",
                )
                temp, _ = sensor.get_temp(eventtime)
                return temp, 0
        return 0, 0

    def _track_job(self, state):
        """Reset `used` when a job ends, so the next one starts from a clean slate.

        Filament colour and type deliberately survive: the spool is still in the
        tool when the print finishes, so the idle screen should keep showing it.
        """
        printing = state in _ACTIVE_PRINT_STATES
        if self.was_printing and not printing:
            for tool in self.tools.values():
                tool.used = True
        self.was_printing = printing

    cmd_KNOMI_TOOL_help = (
        "Tell a screen what is loaded and whether the job uses it. KNOMI_TOOL "
        "[SCREEN=name | TOOL=0] [USED=1] [COLOR=FF8800] [TYPE=PLA]"
    )

    def cmd_KNOMI_TOOL(self, gcmd):
        screens = self.resolve(gcmd)

        # Parsed before anything is applied, so a bad colour cannot leave half
        # the change made - and, with TOOL= matching several screens, cannot
        # leave some of them updated and the rest not.
        used = gcmd.get_int("USED", None, minval=0, maxval=1)

        color = gcmd.get("COLOR", None)
        if color is not None:
            text = color.strip().lstrip("#")
            if not text:
                color = 0
            else:
                try:
                    color = int(text, 16) & 0xFFFFFF
                except ValueError:
                    raise gcmd.error(
                        f"KNOMI_TOOL: COLOR='{gcmd.get('COLOR')}' is not a hex colour",
                    ) from None

        filament_type = gcmd.get("TYPE", None)
        if filament_type is not None:
            filament_type = filament_type.strip()[:_FILAMENT_TYPE_MAX_LEN]

        # Every parameter is optional so a caller can change one fact without
        # restating the others - updating colour mid-job must not silently
        # resurrect a tool the job stopped using.
        for screen in screens:
            state = self.tool_state(screen)
            if used is not None:
                state.used = bool(used)
            if color is not None:
                state.color = color
            if filament_type is not None:
                state.type = filament_type

    def get_status(self, eventtime):
        return {
            "screens": {
                screen: {
                    "used": state.used,
                    "filament_color": f"{state.color:06X}" if state.color else None,
                    "filament_type": state.type or None,
                }
                for screen, state in self.tools.items()
            },
        }


class Knomi_Serial:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.reactor = self.printer.get_reactor()
        self.name = config.get_name()
        # `[knomi_serial T0_knomi]` is addressed as `T0_knomi`, the way Klipper
        # addresses `[fan_generic my_fan]` as `my_fan`. A bare `[knomi_serial]`
        # is addressed by that, though on a machine with one screen nothing has
        # to name it at all.
        self.screen_name = self.name.split(maxsplit=1)[-1]

        self.gcode = None
        self.heaters = None
        self.toolhead = None

        self.serial = None
        self.next_connect = 0
        self.pending_cmd = b""

        self.module_version = _module_version()
        self.device_report = {}
        self.device_report_time = None
        self.warned_proto = _UNSET
        #: Last message pushed, so a shutdown does not resend the same string
        #: ten times a second for as long as the printer stays down.
        self.last_message = None

        self.config_serial = config.get("serial")
        self.config_tool = _normalize_tool(config.get("tool", None))

        self.config_hotend = config.get("heater_hotend", None)
        self.config_bed = config.get("heater_bed", None)

        self.config_heater_chamber = config.get("heater_chamber", None)
        self.config_sensor_chamber = config.get("sensor_chamber", None)
        if self.config_heater_chamber and self.config_sensor_chamber:
            raise config.error(
                "Only one of heater_chamber and sensor_chamber can be specified",
            )

        self.config_sensor_mcu = config.get("sensor_mcu", None)
        self.mcu_sensor = None

        self.config_move = [
            config.getfloat("move_x", 10.0),
            config.getfloat("move_y", 10.0),
            config.getfloat("move_z", 10.0),
        ]
        self.config_speed = [
            config.getfloat("speed_x", 100.0),
            config.getfloat("speed_y", 100.0),
            config.getfloat("speed_z", 100.0),
        ]

        self.device_config = self._build_config(config)
        # Once, here. The device is told this on every state frame, and stamping
        # a precomputed integer is the entire per-tick cost of keeping config in
        # sync - there is nothing to diff and nothing to decide.
        self.config_crc = zlib.crc32(config_payload(self.device_config))

        # One cluster serves every section. The first one to load builds it;
        # load_object is not used because that would need a second file in
        # klippy/extras, and install.sh symlinks exactly one.
        self.cluster = self.printer.lookup_object("knomi_cluster", None)
        if self.cluster is None:
            self.cluster = KnomiCluster(self.printer)
            self.printer.add_object("knomi_cluster", self.cluster)
        self.cluster.register(self)

        self.printer.register_event_handler("klippy:connect", self._handle_connect)
        self.printer.register_event_handler("klippy:ready", self._handle_ready)
        self.printer.register_event_handler(
            "klippy:shutdown",
            self._handle_shutdown,
        )
        self.printer.register_event_handler(
            "klippy:disconnect",
            self._handle_disconnect,
        )

    # ------------------------------------------------------------------
    # config
    # ------------------------------------------------------------------

    def _build_config(self, config):
        """Everything true for hours at a time, gathered into one frame.

        Each field carries a presence bit, and a field the user did not write is
        left out rather than sent as this module's idea of a default. Otherwise
        the host would silently become the authority on every setting, and
        editing user_conf.h and reflashing would appear to work right up until
        the first config frame arrived and undid it.
        """
        present = 0
        values = {}

        def _take(bit, key, parse):
            nonlocal present
            raw = config.get(key, None)
            if raw is None:
                return None
            present |= bit
            return parse(raw)

        def _color(key):
            def parse(raw):
                text = str(raw).strip().lstrip("#")
                try:
                    return int(text, 16) & 0xFFFFFF
                except ValueError:
                    raise config.error(
                        f"{self.name}: {key}='{raw}' is not a hex colour",
                    ) from None

            return parse

        def _ms(key):
            def parse(raw):
                try:
                    seconds = float(raw)
                except ValueError:
                    raise config.error(
                        f"{self.name}: {key}='{raw}' is not a number",
                    ) from None
                if seconds < 0:
                    raise config.error(f"{self.name}: {key} cannot be negative")
                return int(seconds * 1000)

            return parse

        def _level(key):
            def parse(raw):
                try:
                    level = int(raw)
                except ValueError:
                    raise config.error(
                        f"{self.name}: {key}='{raw}' is not a number",
                    ) from None
                if not 0 <= level <= 16:
                    raise config.error(f"{self.name}: {key} must be 0-16")
                return level

            return parse

        values["color_machine"] = _take(
            _HAS_COLOR_MACHINE, "color_machine", _color("color_machine")
        )
        values["color_filament_unknown"] = _take(
            _HAS_COLOR_UNKNOWN,
            "color_filament_unknown",
            _color("color_filament_unknown"),
        )
        values["dim_ms"] = _take(_HAS_DIM_MS, "dim_time", _ms("dim_time"))
        values["sleep_ms"] = _take(_HAS_SLEEP_MS, "sleep_time", _ms("sleep_time"))
        values["brightness"] = _take(
            _HAS_BRIGHTNESS, "brightness", _level("brightness")
        )
        values["dim_brightness"] = _take(
            _HAS_DIM_BRIGHTNESS, "dim_brightness", _level("dim_brightness")
        )

        def _keys(raw):
            # A corner listed here has a switch behind it - wired to the device,
            # or bound to a [gcode_button] on the host - so the glass stops
            # being a second, invisible way to fire the same action. The symbol
            # stays exactly where it was and becomes the key's legend.
            mask = 0
            for name in str(raw).replace(",", " ").split():
                slot = name.strip().upper()
                if slot not in _KEY_SLOTS:
                    raise config.error(
                        f"{self.name}: hardware_keys '{name}' is not one of "
                        f"{', '.join(sorted(_KEY_SLOTS))}",
                    )
                mask |= _KEY_SLOTS[slot]
            return mask

        values["key_mask"] = _take(_HAS_KEY_MASK, "hardware_keys", _keys)

        gcodes = b""
        raw_gcodes = config.get("gcodes", None)
        if raw_gcodes is not None:
            present |= _HAS_GCODES
            names = [name.strip() for name in raw_gcodes.split(",")]
            gcodes = "\n".join(name for name in names if name).encode("utf-8")
            if len(gcodes) > _GCODES_MAX_LEN:
                raise config.error(
                    f"Too many G-codes specified ({len(gcodes)} > {_GCODES_MAX_LEN})",
                )

        if values["dim_ms"] is not None and values["sleep_ms"] is not None:
            if values["dim_ms"] > values["sleep_ms"]:
                raise config.error(
                    f"{self.name}: dim_time must not be later than sleep_time",
                )

        return DeviceConfig(
            present=present,
            gcodes=gcodes,
            **{key: (0 if value is None else value) for key, value in values.items()},
        )

    def _send_config(self):
        self._write(encode_config(self.device_config))

    # ------------------------------------------------------------------
    # connection
    # ------------------------------------------------------------------

    def _handle_connect(self):
        self._connect()

    def _connect(self):
        try:
            self.serial = serial.Serial(
                self.config_serial,
                _BAUD_RATE,
                write_timeout=_WRITE_TIMEOUT,
            )
            return True
        except serial.SerialException as e:
            logging.warning(f"{self.name}: Failed to connect: {e}")
            self.serial = None
            return False

    def _drop(self, reason):
        logging.warning(f"{self.name}: {reason}")
        if self.serial:
            try:
                self.serial.close()
            except Exception:
                pass
        self.serial = None
        self.pending_cmd = b""

    def _handle_ready(self):
        self.gcode = self.printer.lookup_object("gcode")
        self.heaters = self.printer.lookup_object("heaters")
        self.toolhead = self.printer.lookup_object("toolhead")

        if self.config_sensor_mcu:
            self.mcu_sensor = (
                self.printer.lookup_object(
                    f"temperature_sensor {self.config_sensor_mcu}", None
                )
                or self.printer.lookup_object(
                    f"temperature_fan {self.config_sensor_mcu}", None
                )
                or self.printer.lookup_object(self.config_sensor_mcu, None)
            )
            if not self.mcu_sensor:
                logging.warning(
                    f"{self.name}: Could not find sensor_mcu '{self.config_sensor_mcu}'"
                )

    def _handle_shutdown(self):
        self._send_shutdown_state()

    def _handle_disconnect(self):
        self._send_state(PrinterState(status=PrinterStatus.DISCONNECTED))
        self._drop("Disconnecting")

    # ------------------------------------------------------------------
    # driven by the cluster
    # ------------------------------------------------------------------

    def update(self, eventtime, shared):
        if self.serial is None:
            # Retry on a slow cadence. Without this a single transient failure
            # left the screen dark until the next Klipper restart.
            if eventtime >= self.next_connect:
                self.next_connect = eventtime + _RECONNECT_PERIOD
                self._connect()
            return

        if shared is None:
            self._send_shutdown_state()
            return

        hotend_temp, hotend_target = 0, 0
        if self.config_hotend:
            hotend = self.heaters.lookup_heater(self.config_hotend)
            hotend_temp, hotend_target = hotend.get_temp(eventtime)

        mcu_temp, mcu_target = 0, 0
        if self.mcu_sensor:
            mcu_temp, mcu_target = self.mcu_sensor.get_temp(eventtime)

        tool = self.cluster.tool_state(self.screen_name)

        # A screen with no hotend configured can never be the active tool, which
        # is correct: it is not a tool.
        active = bool(self.config_hotend) and self.config_hotend == shared.active_extruder

        self._send_state(
            PrinterState(
                status=shared.status,
                paused=shared.paused,
                working=shared.working,
                homed_x=shared.homed_x,
                homed_y=shared.homed_y,
                homed_z=shared.homed_z,
                used=tool.used,
                active=active,
                hotend_temp=hotend_temp,
                hotend_target=hotend_target,
                bed_temp=shared.bed_temp,
                bed_target=shared.bed_target,
                chamber_temp=shared.chamber_temp,
                chamber_target=shared.chamber_target,
                mcu_temp=mcu_temp,
                mcu_target=mcu_target,
                progress=shared.progress,
                tool_number=int(self.config_tool) if self._tool_is_numeric() else -1,
                filament_color=tool.color,
                # Only the mounted tool is extruding. A docked one shares the
                # toolhead's motion report and none of its filament.
                flow=shared.flow if active else 0,
                eta=shared.eta,
                elapsed=shared.elapsed,
                layer=shared.layer,
                layer_total=shared.layer_total,
                config_crc=self.config_crc,
                tram_type=shared.tram_type,
                filament_type=tool.type.encode("utf-8")[:_FILAMENT_TYPE_MAX_LEN],
            )
        )

    def _tool_is_numeric(self):
        return self.config_tool is not None and self.config_tool.isdigit()

    def poll(self, eventtime):
        if not self.serial or not self.serial.is_open:
            return

        try:
            while self.serial.in_waiting:
                char = self.serial.read()
                if char == b"\n":
                    if self.pending_cmd.startswith(_CMD_PREFIX):
                        self._process_cmd(self.pending_cmd[len(_CMD_PREFIX) :])
                    self.pending_cmd = b""
                elif len(self.pending_cmd) < _CMD_MAX_LEN:
                    self.pending_cmd += char
                else:
                    # Line noise with no newline in sight; drop it rather than
                    # growing the buffer without bound.
                    self.pending_cmd = b""
        except serial.SerialException as e:
            self._drop(f"Read error: {e}")

    # ------------------------------------------------------------------
    # the wire
    # ------------------------------------------------------------------

    def _send_shutdown_state(self):
        message, _ = self.printer.get_state_message()
        message = message.split("\n")[0].upper()[:_MESSAGE_MAX_LEN]

        # Two frames, because the reason and the state are two different facts.
        # Under proto 2 the reason went in the macro list - the only string field
        # in the packet - which worked, and meant the shutdown screen and the
        # G-code page read the same bytes for opposite purposes.
        if message != self.last_message:
            self.last_message = message
            self._write(encode_message(message))
        self._send_state(
            PrinterState(status=PrinterStatus.SHUTDOWN, config_crc=self.config_crc)
        )

    def _send_state(self, state):
        self._write(encode_state(state))

    def _write(self, data):
        if not self.serial or not self.serial.is_open:
            return

        try:
            self.serial.write(data)
        except serial.SerialException as e:
            # Covers SerialTimeoutException, which is what _WRITE_TIMEOUT
            # produces when a screen stops draining its buffer.
            self._drop(f"Lost connection: {e}")

    def _process_report(self, payload):
        # Reports are `key=value;key=value`. Parsed permissively on purpose:
        # unknown keys from a newer firmware are ignored, and keys missing from
        # an older one simply stay absent from get_status.
        fields = {}
        for item in payload.decode("utf-8", "replace").split(";"):
            key, sep, value = item.partition("=")
            if sep:
                fields[key.strip()] = value.strip()

        self.device_report = fields
        self.device_report_time = self.reactor.monotonic()

        proto = fields.get("proto")
        if proto != self.warned_proto and proto != str(_PROTO_VERSION):
            logging.warning(
                f"{self.name}: Protocol mismatch: device firmware "
                f"{fields.get('fw', 'unknown')} speaks protocol {proto}, "
                f"module {self.module_version} expects {_PROTO_VERSION}. "
                f"Reflash the device firmware from this repository.",
            )
            self.warned_proto = proto

    def get_status(self, eventtime):
        report = self.device_report
        age = None
        if self.device_report_time is not None:
            age = eventtime - self.device_report_time
        online = age is not None and age < _DEVICE_TIMEOUT

        def _int(key):
            try:
                return int(report[key])
            except (KeyError, ValueError):
                return None

        def _hex(key):
            try:
                return int(report[key], 16)
            except (KeyError, ValueError, TypeError):
                return None

        device_crc = _hex("cfg")

        proto = _int("proto")
        tool = self.cluster.tool_state(self.screen_name)
        return {
            # Host side.
            "connected": self.serial is not None and self.serial.is_open,
            "port": self.config_serial,
            "module_version": self.module_version,
            "protocol_version": _PROTO_VERSION,
            "config_crc": f"{self.config_crc:08X}",
            # What the host believes about this tool.
            # What KNOMI_TOOL addresses this section as. Not `screen`, which
            # already means which of the UI's screens is loaded.
            "screen_name": self.screen_name,
            "tool": self.config_tool,
            "used": tool.used,
            "filament_color": f"{tool.color:06X}" if tool.color else None,
            "filament_type": tool.type or None,
            # Device side. All None until the device reports in.
            "device_online": online,
            "report_age": age,
            "firmware_version": report.get("fw"),
            "device_protocol_version": proto,
            "protocol_match": proto == _PROTO_VERSION if proto else None,
            "build_variant": report.get("var"),
            # What config the device is actually running, so "I pushed it" and
            # "it took" are separable.
            "device_config_crc": None if device_crc is None else f"{device_crc:08X}",
            "config_applied": (
                None if device_crc is None else device_crc == self.config_crc
            ),
            "sleep_state": report.get("sleep"),
            "screen": report.get("scr"),
            "page": _int("page"),
            "free_heap": _int("heap"),
            "min_free_heap": _int("minheap"),
            "device_uptime": _int("up"),
        }

    def _process_cmd(self, cmd):
        try:
            if cmd.startswith(_CMD_REPORT):
                self._process_report(cmd[len(_CMD_REPORT) :])
                return
            if cmd == _CMD_CONFIG_REQUEST:
                # The device noticed the CRC we stamp on every state frame does
                # not match what it holds. It asks; we answer. That is the whole
                # protocol - no acks, no retry timer, and nothing here that has
                # to remember which devices are up to date.
                self._send_config()
                return
            if cmd == _CMD_STOP:
                self.printer.invoke_shutdown(f"Stop requested by {self.name}")
                return
            if cmd == _CMD_RESTART:
                self.gcode.request_restart("firmware_restart")
                return
            if cmd.startswith(_CMD_GCODE):
                gcode = cmd[len(_CMD_GCODE) :]
                self.gcode.run_script(gcode.decode("utf-8"))
                return
            if cmd.startswith(_CMD_MOVE):
                axis = {
                    ord("X"): 0,
                    ord("Y"): 1,
                    ord("Z"): 2,
                }[cmd[-2]]
                direction = {
                    ord("+"): 1,
                    ord("-"): -1,
                }[cmd[-1]]
                pos = self.toolhead.get_position()
                pos[axis] += self.config_move[axis] * direction
                self.toolhead.manual_move(pos, self.config_speed[axis])
        except Exception as e:
            logging.warning(f"{self.name}: Command error: {e}")


def load_config(config):
    knomi_serial = Knomi_Serial(config)
    return knomi_serial


def load_config_prefix(config):
    knomi_serial = Knomi_Serial(config)
    return knomi_serial
