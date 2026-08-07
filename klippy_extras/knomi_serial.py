"""Klipper extra module for Knomi_Serial.

Each `[knomi_serial <name>]` section is one screen on one serial port. They are
coordinated by a single cluster object rather than acting independently: the
printer state every screen shows - bed, chamber, progress, homing, motion - is
identical, so computing it once per tick instead of once per screen per tick is
the difference between one lookup and N at 10Hz. The cluster also owns the
things that are true of the machine rather than of a screen: which tools a job
uses, and which tool is mounted right now.
"""

import logging
import dataclasses
import enum
import os
import serial
import struct

_BAUD_RATE = 115200
_HEADER = b"\x83\xad\x83\xad"
_FOOTER = b"\xf0\x07\xf0\x07"

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

#: Wire format version for the state packet built by _send_state. Must be kept
#: in sync with printer::kProtoVersion in src/printer/printer.h.
_PROTO_VERSION = 2

#: The state packet, field for field against `struct State` in
#: src/printer/printer.h. `!` means network order and no padding of its own, so
#: the one `x` is the struct's explicit padding byte - the thing keeping the
#: int32 block 4-byte aligned on the device.
#:
#: Changing this means changing that struct, bumping _PROTO_VERSION, and
#: updating the static_assert that pins its size.
_STATE_FMT = "!I7?x10iII16s256s"
_STATE_SIZE = struct.calcsize(_STATE_FMT)

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

    tram_type: PrinterTramType = PrinterTramType.NONE

    filament_type: bytes = b""
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

    def tool_state(self, tool):
        """The record for `tool`, created on first use."""
        if tool is None:
            return ToolState()
        if tool not in self.tools:
            self.tools[tool] = ToolState()
        return self.tools[tool]

    def _handle_ready(self):
        self.heaters = self.printer.lookup_object("heaters")
        self.toolhead = self.printer.lookup_object("toolhead")
        self.virtual_sdcard = self.printer.lookup_object("virtual_sdcard")
        self.print_stats = self.printer.lookup_object("print_stats")

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
            progress=provider.progress() * 100,
            tram_type=self.tram_type,
            active_extruder=self.toolhead.get_extruder().get_name(),
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
        "Set what a tool is loaded with and whether the running job uses it. "
        "KNOMI_TOOL TOOL=0 [USED=1] [COLOR=FF8800] [TYPE=PLA]"
    )

    def cmd_KNOMI_TOOL(self, gcmd):
        tool = _normalize_tool(gcmd.get("TOOL"))
        if tool is None:
            raise gcmd.error("KNOMI_TOOL requires TOOL=")
        state = self.tool_state(tool)

        # Every parameter is optional so a caller can change one fact without
        # restating the others - updating colour mid-job must not silently
        # resurrect a tool the job stopped using.
        used = gcmd.get_int("USED", None, minval=0, maxval=1)
        if used is not None:
            state.used = bool(used)

        color = gcmd.get("COLOR", None)
        if color is not None:
            text = color.strip().lstrip("#")
            if not text:
                state.color = 0
            else:
                try:
                    state.color = int(text, 16) & 0xFFFFFF
                except ValueError:
                    raise gcmd.error(f"KNOMI_TOOL: COLOR='{color}' is not a hex colour")

        filament_type = gcmd.get("TYPE", None)
        if filament_type is not None:
            state.type = filament_type.strip()[:_FILAMENT_TYPE_MAX_LEN]

    def get_status(self, eventtime):
        return {
            "tools": {
                tool: {
                    "used": state.used,
                    "filament_color": f"{state.color:06X}" if state.color else None,
                    "filament_type": state.type or None,
                }
                for tool, state in self.tools.items()
            },
        }


class Knomi_Serial:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.reactor = self.printer.get_reactor()
        self.name = config.get_name()

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

        self.config_gcodes = config.get("gcodes", "")
        gcodes = [gcode.strip() for gcode in self.config_gcodes.split(",")]
        gcodes = [gcode for gcode in gcodes if gcode]
        gcodes = "\n".join(gcodes)
        self.gcodes = gcodes.encode("utf-8")
        if len(self.gcodes) > _GCODES_MAX_LEN:
            raise config.error(
                f"Too many G-codes specified ({len(self.gcodes)} > {_GCODES_MAX_LEN})",
            )

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

        tool = self.cluster.tool_state(self.config_tool)

        self._send_state(
            PrinterState(
                status=shared.status,
                paused=shared.paused,
                working=shared.working,
                homed_x=shared.homed_x,
                homed_y=shared.homed_y,
                homed_z=shared.homed_z,
                used=tool.used,
                # A screen with no hotend configured can never be the active
                # tool, which is correct: it is not a tool.
                active=bool(self.config_hotend)
                and self.config_hotend == shared.active_extruder,
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
                tram_type=shared.tram_type,
                filament_type=tool.type.encode("utf-8")[:_FILAMENT_TYPE_MAX_LEN],
                gcodes=self.gcodes,
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
        message = message[:_GCODES_MAX_LEN]
        message = message.split("\n")[0]
        message = message.upper()
        message = message.encode("utf-8")
        self._send_state(
            PrinterState(
                status=PrinterStatus.SHUTDOWN,
                gcodes=message,
            )
        )

    def _send_state(self, state):
        if not self.serial or not self.serial.is_open:
            return

        # The two trailing `s` fields pad with NULs and truncate on their own,
        # which is exactly the fixed-width behaviour the device reads back.
        msg = (
            _HEADER
            + struct.pack(
                _STATE_FMT,
                state.status.value,
                state.working,
                state.paused,
                state.homed_x,
                state.homed_y,
                state.homed_z,
                state.used,
                state.active,
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
                state.tram_type.value,
                state.filament_type,
                state.gcodes,
            )
            + _FOOTER
        )

        try:
            self.serial.write(msg)
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

        proto = _int("proto")
        tool = self.cluster.tool_state(self.config_tool)
        return {
            # Host side.
            "connected": self.serial is not None and self.serial.is_open,
            "port": self.config_serial,
            "module_version": self.module_version,
            "protocol_version": _PROTO_VERSION,
            # What the host believes about this tool.
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
