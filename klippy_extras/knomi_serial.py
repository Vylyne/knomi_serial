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
import json
import logging
import os
import struct
import time
import zlib

import serial
import serial.tools.list_ports

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
#: 4: config carries the page list, so there is one firmware rather than a
#:    toolchanger build and a non-toolchanger one.
#: 5: dropped eta, elapsed, layer and layer_total - nothing read them - and
#:    made the secondary readouts a configured list. The MCU pair stayed and is
#:    now shown: on a toolchanger that MCU sits in the heated chamber.
_PROTO_VERSION = 5

#: The state frame's payload, field for field against `struct State` in
#: src/printer/printer.h, down to and including filament_type. `!` means network
#: order and no padding of its own, so the layout here is the struct's layout.
#:
#: 80 bytes, against proto 2's 332. The macro list was 256 of those and had not
#: changed since Klipper started; at 10Hz it alone was 2.5 kB/s of an 11.5 kB/s
#: link, spent restating a constant. It now goes in _CONFIG_FMT, sent when the
#: device asks for it.
#:
#: Changing this means changing that struct, bumping _PROTO_VERSION, and
#: updating the static_asserts that pin its size.
_STATE_FMT = "!I7?B10iIiI16s"
_STATE_SIZE = struct.calcsize(_STATE_FMT)

#: The config frame's payload, against `struct Config`. The fourth byte was
#: padding until estop_at claimed it, which is why the size did not change.
_CONFIG_FMT = "!5I4B4B8B256s"
_CONFIG_SIZE = struct.calcsize(_CONFIG_FMT)

#: Page ids, against `enum class Page`. Order in the list is the order on the
#: device, and the screen lands on the first of them. `estop` is deliberately
#: absent: the device appends it whatever the list says, off the side named by
#: estop_at, so a list written for the idle pages cannot drop it by omission.
_PAGES = {"tool": 1, "gcode": 2, "home": 3, "move": 4}
_MAX_PAGES = 8

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
_HAS_PAGE_ORDER = 1 << 8
_HAS_ESTOP_AT = 1 << 9
_HAS_READOUTS = 1 << 10

#: Secondary readout ids, against `enum class Readout`. Which of these a screen
#: shows is a fact about the machine: a single-toolhead printer wants its bed,
#: while four tool screens each restating the one bed temperature is four copies
#: of something none of them owns.
_READOUTS = {"bed": 1, "chamber": 2, "mcu": 3}
_MAX_READOUTS = 4

#: Which side of the page row the e-stop hangs off, against `enum class
#: EstopAt`. Bottom means drag up to reach it; top is the notification-shade
#: gesture. This claimed the struct's one spare padding byte, so it changed no
#: offsets and needed no version bump.
_ESTOP_AT = {"bottom": 0, "top": 1}

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


#: Where service/knomi_serial_watch.py writes what it has seen, if it is running. Read
#: as a hint and never as truth - see port_map.
_DEVICE_MAP_PATH = os.path.expanduser("~/printer_data/knomi/devices.json")
_DEVICE_MAP_VERSION = 1


def port_map(path=None):
    """{id: port} as last observed by the watcher, if there is one.

    A hint, and treated as nothing more. The watcher can see ports Klipper cannot
    - it is running when Klipper is not, which is when a display gets flashed or
    a cable gets moved - so its map is often right and worth trying before
    spending a discovery pass. But it describes the past, and a display named
    here may have moved since.

    That is safe only because it is checked: the id is in every report, and
    _verify_identity drops any section whose display disagrees with the one it
    went looking for, clearing this cache on the way out. A wrong hint costs a
    reconnect. It cannot put one tool's readings on another tool's screen, which
    is the whole reason for addressing by identity in the first place.

    Missing file, unreadable file, unknown version: no hint, discover instead.
    The watcher is an optimisation, never a dependency - most machines have one
    display and will never run it.
    """
    # Resolved at call time rather than bound as a default, so the location can
    # be overridden and so a test can point this somewhere real.
    if path is None:
        path = _DEVICE_MAP_PATH
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    if data.get("version") != _DEVICE_MAP_VERSION:
        return {}
    devices = data.get("devices")
    if not isinstance(devices, dict):
        return {}
    return {
        ident: fields["port"]
        for ident, fields in devices.items()
        if isinstance(fields, dict) and fields.get("port")
    }


#: What install.sh stamps when it finishes, and what the checkout expects it to
#: have stamped. A Moonraker update moves files and restarts services; it never
#: runs install.sh, and cannot - the systemd unit and the [update_manager]
#: section both need doing from outside a `git pull`. So the checkout can end up
#: newer than the install, with nothing to say so.
_INSTALL_VERSION_PATH = os.path.expanduser(
    "~/printer_data/knomi/.install-version")
_INSTALL_VERSION_FILE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
    "scripts", "install-version")


def _install_version(path):
    """The number in a stamp file, or 0 for anything else.

    Missing, empty, half-written, or full of something that is not a number all
    mean the same thing here: nothing has told us this install is current, so
    assume it is not. Never raises - it is read on the way into a print.
    """
    try:
        with open(path, encoding="utf-8") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return 0


def install_version_notice(wanted, stamped):
    """What to tell the user when their install is behind the checkout.

    Only when the checkout is ahead. A stamp higher than the repo means the
    checkout was rolled back, and "re-run install.sh" is not the useful thing to
    say about that.
    """
    if stamped >= wanted:
        return None
    where = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
    if not os.path.isdir(os.path.join(where, ".git")):
        # Copied into klippy/extras instead of symlinked, so this path is not a
        # checkout and printing it would send somebody to the wrong directory.
        where = "your knomi_serial checkout"
    return (f"knomi_serial: this printer was set up by an older install.sh. "
            f"Run ./install.sh in {where} to bring it up to date.")


#: USB vendor ids worth listening to. 1A86 is the CH340 on the Knomi V2; 303A is
#: Espressif's own, for boards whose ESP32 is the USB device directly.
_USB_VENDORS = (0x1A86, 0x303A)

#: How long to listen on a candidate port. The device announces itself every two
#: seconds unprompted, so this is not a timeout on a question - it is a wait for
#: the next broadcast. Six seconds covers the worst case where opening the port
#: resets the board: about two to boot, then up to one full report period.
_DISCOVER_LISTEN = 6.0


def candidate_ports(skip=()):
    """Serial ports that could be a display."""
    return sorted(
        port.device
        for port in serial.tools.list_ports.comports()
        if port.vid in _USB_VENDORS and port.device not in skip
    )


def discover(ports=None, listen=_DISCOVER_LISTEN, skip=()):
    """{id: port}, the mapping Klipper needs. See discover_reports."""
    return {
        ident: fields["port"]
        for ident, fields in discover_reports(ports, listen, skip).items()
    }


def discover_reports(ports=None, listen=_DISCOVER_LISTEN, skip=()):
    """Map hardware id to port, by listening rather than asking.

    Every display broadcasts a report line every two seconds without being
    prompted, so discovery needs no request, no protocol of its own, and no
    cooperation from a device that might be busy. Open, listen, read `id`,
    close.

    Every candidate is opened at once and polled together. Six displays take six
    seconds that way and thirty-six one at a time, which is the difference
    between a slow Klipper start and an unacceptable one.

    Shared with scripts/discover.py rather than reimplemented there, so what the
    tool tells you to put in printer.cfg is what Klipper will look for.
    """
    if ports is None:
        ports = candidate_ports(skip)

    live = {}
    for path in ports:
        try:
            # exclusive, and this is the half that was missing. pyserial's
            # exclusive= is flock(LOCK_EX|LOCK_NB), which is advisory: it keeps
            # out other processes that also ask for it and is invisible to one
            # that does not. Sections take the lock, esptool takes the lock,
            # and discovery used to open straight past both - so probing a
            # display Klipper was already talking to did not fail, it split the
            # byte stream with it. The symptom was discovery taking visibly
            # longer with Klipper up, because a line assembled from half the
            # bytes needs several report periods to arrive intact.
            live[path] = serial.Serial(
                path, _BAUD_RATE, timeout=0, exclusive=True)
        except (serial.SerialException, OSError) as e:
            # In use by a section or by a flashing tool, or gone since it was
            # enumerated. None of those is worth stopping for - the rest of the
            # row is still findable.
            logging.info(f"knomi_serial: discovery skipped {path}: {e}")

    found = {}
    buffers = {path: b"" for path in live}
    deadline = time.time() + listen
    try:
        while live and time.time() < deadline:
            for path, port in list(live.items()):
                try:
                    waiting = port.in_waiting
                except (serial.SerialException, OSError):
                    del live[path]
                    continue
                if not waiting:
                    continue
                buffers[path] += port.read(waiting)
                while b"\n" in buffers[path]:
                    line, _, buffers[path] = buffers[path].partition(b"\n")
                    fields = report_fields(line.strip())
                    ident = fields.get("id") if fields else None
                    if ident:
                        found[ident] = dict(fields, port=path)
                        # Closed here, not left to the loop below - this one is
                        # about to be handed to a section that will open it, and
                        # discovery must not still be holding it when that
                        # happens.
                        del live[path]
                        try:
                            port.close()
                        except Exception:
                            pass
                        break
            time.sleep(0.02)
    finally:
        for port in live.values():
            try:
                port.close()
            except Exception:
                pass
    return found


def _int_or_none(text):
    try:
        return int(text)
    except (TypeError, ValueError):
        return None


def report_fields(line):
    """Every `key=value` in a report line, or None if this is not one.

    One parser for the whole line rather than one that hunts for `id` and
    another that builds a dict, because the difference between them was a
    second open of the port to re-read what had already been read.
    """
    if not line.startswith(_CMD_PREFIX + _CMD_REPORT):
        return None
    return parse_report(line[len(_CMD_PREFIX) + len(_CMD_REPORT):])


def parse_report(body):
    """The `key=value;key=value` body of a report, as a dict.

    Permissive on purpose: unknown keys from a newer firmware are ignored, and
    keys missing from an older one simply stay absent.
    """
    out = {}
    for item in body.decode("utf-8", "replace").split(";"):
        key, sep, value = item.partition("=")
        if not sep:
            continue
        key = key.strip()
        value = value.strip()
        if key == "id":
            # Lowered here as well as on the config value, so both sides of
            # the comparison are normalised in one language. The firmware
            # formats with %02x today, which makes this a no-op - but that is
            # an invariant held in C++ about a lookup performed in Python, and
            # if it ever slipped every device_id: section would stop resolving
            # against a config that still looked right.
            value = value.lower()
            if not value:
                continue
        out[key] = value
    return out


def report_id(line):
    """The `id` field of a report line, or None if this is not one."""
    fields = report_fields(line)
    return fields.get("id") if fields else None


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
        config.estop_at,
        *_fixed(config.readouts, _MAX_READOUTS),
        *_page_bytes(config.pages),
        config.gcodes,
    )


def _page_bytes(pages):
    """A page list as the fixed-width, zero-terminated array the device reads."""
    return _fixed(pages, _MAX_PAGES)


def _fixed(ids, width):
    """An id list padded and zero-terminated to a fixed width."""
    out = list(ids)[:width]
    return out + [0] * (width - len(out))


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
    estop_at: int = 0

    #: Page ids in order, from _PAGES. Padded and terminated on the way out.
    pages: tuple = ()

    #: Readout ids in order, from _READOUTS.
    readouts: tuple = ()

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
        #: Hardware id to port, from the last discovery pass, and when the next
        #: one is allowed. Filled by the pass at klippy:connect.
        self._ports = {}
        self._discover_after = 0
        #: (id, port) pairs that were named and turned out to hold a different
        #: display. Without this a stale map is a loop: connect, fail the
        #: identity check, clear the cache, read the same wrong answer back out
        #: of the same file, connect again - every five seconds, forever. Keyed
        #: on the pair, so the same display named on a different port later is
        #: a fresh answer and gets tried.
        self._rejected = set()

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

        # Before any section's own connect handler, which is what makes the
        # collision check possible at all - see _handle_connect. The cluster is
        # built by the first section's __init__ before that section registers
        # anything, so registration order gives the sequencing for free.
        printer.register_event_handler("klippy:connect", self._handle_connect)
        printer.register_event_handler("klippy:ready", self._handle_ready)

    def register(self, device):
        self.devices.append(device)

    def _handle_connect(self):
        """One discovery pass for the whole row, before anything opens a port.

        Every candidate port, including the ones named by `serial:`. This is the
        only moment the two addressing schemes can be checked against each
        other: once a `serial:` section has its port open, nothing else can ask
        what is on the end of it, and the answer is exactly what is needed to
        notice that two sections are pointing at one display.

        Blocking is correct here in a way it is not anywhere else. Startup is
        allowed to take a couple of seconds, and discovery returns as soon as
        every port has answered rather than waiting out the full window.
        """
        self._discover_once()
        self._check_collisions()

    def _discover_once(self, skip=()):
        # The watcher's map first, when it covers everything asked for. Opening
        # ports to learn what something else already wrote down is work worth
        # skipping, and skipping it is what takes a six-display startup from a
        # couple of seconds to none.
        wanted = {d.config_device_id for d in self.devices if d.config_device_id}
        if wanted:
            hinted = {i: self._map_hint(i) for i in wanted}
            if all(hinted.values()):
                self._ports = hinted
                logging.info(
                    "knomi_serial: using the watcher's map for %s",
                    ", ".join(f"{i} on {p}" for i, p in sorted(self._ports.items())),
                )
                return

        try:
            self._ports = discover(skip=skip)
        except Exception as e:
            logging.warning(f"knomi_serial: discovery failed: {e}")
            return
        logging.info(
            "knomi_serial: discovery found %s",
            ", ".join(f"{i} on {p}" for i, p in sorted(self._ports.items()))
            or "nothing",
        )

    def _check_collisions(self):
        """Refuse a config where two sections describe one display.

        Nothing downstream can recover from this. Both sections would open the
        same port, and two readers on one tty split the byte stream between
        them, so the symptom is two screens intermittently blank rather than
        anything that names the cause.
        """
        by_id = {}
        for device in self.devices:
            if not device.config_device_id:
                continue
            first = by_id.setdefault(device.config_device_id, device.screen_name)
            if first != device.screen_name:
                raise self.printer.config_error(
                    f"knomi_serial: {first} and {device.screen_name} both have "
                    f"device_id: {device.config_device_id}. One display cannot "
                    "be two screens."
                )

        # Paths are compared resolved, because a serial: section is very likely
        # pointing at a udev symlink while discovery reports the real device.
        def real(path):
            # Only for something that is actually a filesystem entry. A serial:
            # is very likely a udev symlink and has to be followed to match what
            # discovery reports, but a Windows port is a name rather than a
            # path, and realpath would happily resolve "COM5" against the
            # working directory and put that in the error message.
            try:
                if os.path.lexists(path):
                    return os.path.realpath(path)
            except OSError:
                pass
            return path

        wanted = {d.config_device_id: d.screen_name for d in self.devices
                  if d.config_device_id}
        on_port = {real(port): ident for ident, port in self._ports.items()}

        by_path = {}
        for device in self.devices:
            if not device.config_serial:
                continue
            path = real(device.config_serial)
            first = by_path.setdefault(path, device.screen_name)
            if first != device.screen_name:
                raise self.printer.config_error(
                    f"knomi_serial: {first} and {device.screen_name} both have "
                    f"serial: {device.config_serial}."
                )
            ident = on_port.get(path)
            if ident in wanted:
                raise self.printer.config_error(
                    f"knomi_serial: {device.screen_name} names "
                    f"{device.config_serial} by path, "
                    f"but that display reports id {ident}, which "
                    f"{wanted[ident]} also claims with device_id:. One display "
                    "cannot be two screens - drop one of the two sections."
                )

    def resolve_port(self, device_id):
        """Which port a hardware id is on, discovering if we do not know.

        Here rather than on the device because discovery must happen once for
        the whole row: six sections each opening every port would fight over
        them, and a port being probed by one section looks broken to another.

        Rate limited to the reconnect cadence. A display plugged in after
        Klipper started, or moved to another socket, is found on the next pass
        rather than needing a restart - which is the point of naming the device
        instead of the socket.
        """
        if device_id in self._ports:
            return self._ports[device_id]

        now = self.reactor.monotonic()
        if now < self._discover_after:
            return None
        self._discover_after = now + _RECONNECT_PERIOD

        # The watcher's map first, and this part is allowed to run mid-print:
        # it is a small file read rather than six seconds of listening, and the
        # connect that follows is the same one a serial: section already does
        # on this cadence.
        hinted = self._map_hint(device_id)
        if hinted:
            self._ports[device_id] = hinted
            return hinted

        # Listening is a different matter, and not while a job is running. This
        # is reached from the 10Hz update timer on the reactor thread, and
        # discovery blocks until the slowest port answers - up to the full
        # window when one never does. Survivable at startup and ruinous
        # mid-print, where it would stall the thread feeding the steppers for
        # seconds at a time, every few seconds, for as long as a display stayed
        # unplugged. Without the watcher running, a screen that reappears
        # during a print comes back when the job ends.
        if self._printing():
            return None

        # Never touch a port that is already somebody's. Two readers on one tty
        # do not queue - POSIX hands each of them a random subset of the bytes,
        # so probing a working display corrupts its stream rather than merely
        # being rude to it. Ports named by `serial:` were always excluded; ports
        # a device_id: section has already resolved to were not, which meant one
        # unplugged display had every working one probed every few seconds for
        # as long as it stayed unplugged.
        claimed = tuple(
            path
            for d in self.devices
            for path in (d.config_serial, d.resolved_port)
            if path
        )
        self._discover_once(skip=claimed)

        wanted = {d.config_device_id for d in self.devices if d.config_device_id}
        missing = sorted(wanted - set(self._ports))
        if missing:
            logging.info(
                "knomi_serial: no display answered for %s (found: %s)",
                ", ".join(missing),
                ", ".join(f"{i} on {p}" for i, p in sorted(self._ports.items()))
                or "none",
            )
        return self._ports.get(device_id)

    def reject_port(self, device_id, port):
        """This port was said to hold that display, and it does not.

        Throws away the cache so the next pass looks properly, and remembers
        the pairing so the answer that just failed is not simply read back out
        of the watcher's map and tried again.
        """
        if device_id and port:
            self._rejected.add((device_id, port))
        self._ports = {}
        self._discover_after = 0

    def _map_hint(self, device_id):
        """The watcher's answer for one id, unless it has already proved wrong.

        A file read - tens of microseconds, opening nothing - which is what
        makes it safe on the reactor thread mid-print where listening to ports
        is not.
        """
        port = port_map().get(device_id)
        if port and (device_id, port) not in self._rejected:
            return port
        return None

    def _printing(self):
        try:
            state = self.print_stats.get_status(self.reactor.monotonic())["state"]
        except Exception:
            # Before klippy:ready there is no print_stats and no print either.
            return False
        return state in _ACTIVE_PRINT_STATES

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

        # On the cluster rather than on a section, so a row of six displays says
        # this once. Said to the console rather than only to klippy.log, because
        # the whole failure being described is one that otherwise announces
        # itself as nothing at all - and said as information rather than raised
        # as a config error, because a stale install is "please run this", not a
        # reason to refuse to start a printer.
        notice = install_version_notice(
            _install_version(_INSTALL_VERSION_FILE),
            _install_version(_INSTALL_VERSION_PATH),
        )
        if notice:
            self.gcode.respond_info(notice)

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
        # `devices` is the whole row in one place, so a firmware updater can ask
        # one object what is out there instead of parsing printer.cfg for
        # `serial:` lines - which would no longer answer the question anyway,
        # since a display addressed by identity has no path in its section and
        # the path it happens to be on today is discovered, not configured.
        #
        # Keyed by section name because that is what a person recognises and
        # what KNOMI_TOOL already addresses. The hardware id is in the value: it
        # is the thing that stays true when the cable moves.
        return {
            "screens": {
                screen: {
                    "used": state.used,
                    "filament_color": f"{state.color:06X}" if state.color else None,
                    "filament_type": state.type or None,
                }
                for screen, state in self.tools.items()
            },
            "devices": {
                device.screen_name: device.identity()
                for device in self.devices
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

        # A path names a socket, not a display. The CH340 carries no USB serial
        # number, so /dev/ttyUSB0 moves on reboot, a by-id path for a second
        # identical unit is disambiguated by the kernel rather than the device,
        # and a udev rule on KERNELS== is stable only until a cable moves. On a
        # toolchanger that last one is the quiet failure: swap two leads and two
        # screens describe the wrong tool with nothing on the glass to say so.
        #
        # `device_id:` names the display itself, by the id burned into its chip.
        # Either is accepted; both together is refused, because two ways to
        # address one thing - one of which could be wrong - is the trap that
        # `hardware_keys` and the SCREEN=/TOOL= pair both avoid.
        self.config_serial = config.get("serial", None)
        self.config_device_id = config.get("device_id", None)
        if self.config_device_id is not None:
            self.config_device_id = self.config_device_id.strip().lower()
        if bool(self.config_serial) == bool(self.config_device_id):
            raise config.error(
                f"{self.name}: give exactly one of serial: or device_id:. "
                "Run scripts/discover.py to see which id is on which port, or "
                "read it off the waiting screen."
            )
        #: Where device_id: resolved to, once discovery has found it.
        self.resolved_port = None

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

        def _pages(raw):
            # Order is the order on screen, and the display lands on the first,
            # so this is one setting rather than a list plus a start index that
            # could disagree with it.
            order = []
            for name in str(raw).replace(",", " ").split():
                page = name.strip().lower()
                if page not in _PAGES:
                    raise config.error(
                        f"{self.name}: pages '{name}' is not one of "
                        f"{', '.join(_PAGES)}",
                    ) from None
                if _PAGES[page] in order:
                    raise config.error(f"{self.name}: pages lists '{page}' twice")
                order.append(_PAGES[page])
            if len(order) > _MAX_PAGES:
                raise config.error(
                    f"{self.name}: at most {_MAX_PAGES} pages, got {len(order)}")
            return tuple(order)

        pages = _take(_HAS_PAGE_ORDER, "pages", _pages)

        def _estop_at(raw):
            side = str(raw).strip().lower()
            if side not in _ESTOP_AT:
                raise config.error(
                    f"{self.name}: estop_at '{raw}' is not one of "
                    f"{', '.join(_ESTOP_AT)}",
                ) from None
            return _ESTOP_AT[side]

        values["estop_at"] = _take(_HAS_ESTOP_AT, "estop_at", _estop_at)

        def _readouts(raw):
            order = []
            for name in str(raw).replace(",", " ").split():
                which = name.strip().lower()
                if which not in _READOUTS:
                    raise config.error(
                        f"{self.name}: readouts '{name}' is not one of "
                        f"{', '.join(_READOUTS)}",
                    ) from None
                if _READOUTS[which] in order:
                    raise config.error(
                        f"{self.name}: readouts lists '{which}' twice")
                order.append(_READOUTS[which])
            if len(order) > _MAX_READOUTS:
                raise config.error(
                    f"{self.name}: at most {_MAX_READOUTS} readouts, "
                    f"got {len(order)}")
            return tuple(order)

        readouts = _take(_HAS_READOUTS, "readouts", _readouts)

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
            # Kept out of `values` because its empty value is a tuple, not the 0
            # every other unset field collapses to.
            pages=pages or (),
            readouts=readouts or (),
            **{key: (0 if value is None else value) for key, value in values.items()},
        )

    def _send_config(self):
        self._write(encode_config(self.device_config))

    # ------------------------------------------------------------------
    # connection
    # ------------------------------------------------------------------

    def _handle_connect(self):
        # Both kinds of section connect here now. The cluster's own connect
        # handler ran first and has already resolved every id it could find, so
        # a device_id: section has a port to open rather than a wait ahead of
        # it - which it used to, coming up several seconds after its neighbours
        # for no reason the operator could see.
        self._connect()

    def _port(self):
        """The path to open, or None if identity has not resolved to one yet."""
        if self.config_serial:
            return self.config_serial
        if self.resolved_port is None:
            self.resolved_port = self.cluster.resolve_port(self.config_device_id)
        return self.resolved_port

    def _connect(self):
        path = self._port()
        if not path:
            return False
        try:
            self.serial = serial.Serial(
                path,
                _BAUD_RATE,
                write_timeout=_WRITE_TIMEOUT,
                # TIOCEXCL on POSIX; a no-op on Windows, where ports are
                # exclusive already. A skip list is a promise the code makes to
                # itself and can forget to keep - this makes the mistake
                # impossible rather than merely absent, so a stray
                # scripts/discover.py run cannot disturb a live print either.
                exclusive=True,
            )
            return True
        except serial.SerialException as e:
            logging.warning(f"{self.name}: Failed to connect on {path}: {e}")
            self.serial = None
            # The display may have been unplugged and put back on another
            # socket. Forget where it was so the next attempt looks again -
            # which is the whole point of addressing it by identity.
            self.resolved_port = None
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

    def _verify_identity(self, reported):
        """Check that the display on this port is the one we went looking for.

        Discovery says which port an id was on; this says which id the port
        actually has, and they are not the same claim. Anything that resolves a
        port ahead of time - a discovery pass, and later a cached map written by
        something outside Klipper - is describing the past, and a cable moved in
        between would otherwise put one tool's readings on another tool's screen
        with everything looking healthy. That is the exact failure addressing by
        identity exists to prevent, so it is worth one comparison per report
        rather than trusting the lookup that got us here.

        Nothing to check for a `serial:` section: the path is the address, and
        whatever is on the end of it is what was asked for.
        """
        if not self.config_device_id or not reported:
            return
        if reported == self.config_device_id:
            return
        self._drop(
            f"expected display {self.config_device_id} on {self.resolved_port}, "
            f"but it reports {reported} - dropping it and looking again"
        )
        # Both the section's answer and the row's cache were wrong, so neither
        # is worth keeping. The next pass re-reads the ports rather than
        # handing back the same mistake.
        self.resolved_port = None
        self.cluster.reject_port(self.config_device_id, self.resolved_port)

    def _process_report(self, payload):
        fields = parse_report(payload)

        self.device_report = fields
        self.device_report_time = self.reactor.monotonic()

        self._verify_identity(fields.get("id"))

        proto = fields.get("proto")
        if proto != self.warned_proto and proto != str(_PROTO_VERSION):
            logging.warning(
                f"{self.name}: Protocol mismatch: device firmware "
                f"{fields.get('fw', 'unknown')} speaks protocol {proto}, "
                f"module {self.module_version} expects {_PROTO_VERSION}. "
                f"Reflash the device firmware from this repository.",
            )
            self.warned_proto = proto

    def identity(self):
        """What this section is, for the cluster's device map.

        Everything a firmware updater needs and nothing that changes minute to
        minute: which display, where it is, what is on it. Deliberately not the
        heap and uptime figures - those belong in this section's own
        get_status, and an updater polling the whole row does not want them.
        """
        report = self.device_report
        age = None
        if self.device_report_time is not None:
            age = self.reactor.monotonic() - self.device_report_time
        return {
            # Burned into the chip, so it is the same after a reflash, an
            # erase_flash, and a cable moved to another socket.
            # The full config section, so nothing has to rebuild it from the
            # key. `[knomi_serial T0_knomi]` keys this map as `T0_knomi` while
            # a bare `[knomi_serial]` keys it as `knomi_serial`, so prefixing
            # the key is right for one and wrong for the other - and wrong in
            # the single-display case, which is the one most people have.
            # Also the exact spelling of the printer object; note that
            # `configfile.settings` lowercases its own keys and this does not.
            "section": self.name,
            "device_id": report.get("id") or self.config_device_id,
            "port": self.config_serial or self.resolved_port,
            "addressed_by": "serial" if self.config_serial else "device_id",
            "build_variant": report.get("var"),
            "firmware_version": report.get("fw"),
            "protocol_version": _int_or_none(report.get("proto")),
            "online": age is not None and age < _DEVICE_TIMEOUT,
            "tool": self.config_tool,
        }

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
            "port": self.config_serial or self.resolved_port,
            "device_id": self.config_device_id,
            "reported_id": report.get("id"),
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
            # How many pages the screen actually built, which is the configured
            # list minus any page that would have been empty. Without it a
            # `pages:` edit can only be checked by picking the display up.
            "page_count": _int("pages"),
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
