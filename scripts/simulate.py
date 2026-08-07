#!/usr/bin/env python3
"""Stand in for Klipper so a display can be exercised on a bench.

Speaks both directions of the real link. Downstream it sends state packets built
by knomi_serial.encode_state - the same encoder Klipper uses, so the screen sees
byte for byte what it sees in service. Upstream it answers the commands the
display sends, so pressing PAUSE actually pauses, G28 actually homes, and the
stop button actually shuts the machine down.

That second half is the point. Without it the buttons do nothing and half the UI
cannot be tested at all.

    python scripts/simulate.py --list
    python scripts/simulate.py COM5
    python scripts/simulate.py COM5 --progress 54 --color FFA7C4 --type ABS
    python scripts/simulate.py COM5 --duration 30 --cycle-colours

With no pinned values it loops a whole print: cold, heating, printing, finished.
Pin anything and that value stops moving, so `--progress 54` parks the fill at
the ink crossover.

If the display is wired to a running Klipper, stop Klipper first. Both would be
writing to the same port and the screen would see interleaved packets.
"""

import argparse
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(_HERE), "klippy_extras"))

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is not installed. pip install pyserial")

import knomi_serial as k

_PERIOD = 0.1

#: Deliberately includes the awkward ones. White and yellow are where the ink
#: picker has to flip to black, true black is where it must not, and the pink is
#: near enough to the machine's own accent to test that identity survives it.
PRESETS = [
    ("FFA7C4", "ABS"),
    ("FF6B1A", "PLA"),
    ("1FA6E0", "PETG"),
    ("F5F5F0", "PLA"),
    ("F2D024", "PLA"),
    ("111111", "ABS"),
    ("3FBF6A", "PETG"),
    ("8A6BD6", "TPU"),
]

#: Roughly how long each command keeps the machine busy, in seconds. The screen
#: watches `working` to know something is moving, so a command that completed
#: instantly would never show it.
DURATIONS = {
    "G28": 6.0,
    "G28 X": 2.5,
    "G28 Y": 2.5,
    "G28 Z": 3.0,
    "Z_TILT_ADJUST": 8.0,
    "QUAD_GANTRY_LEVEL": 8.0,
    "LOAD_FILAMENT": 6.0,
    "UNLOAD_FILAMENT": 6.0,
}

MOVE_STEP = {"X": 10.0, "Y": 10.0, "Z": 10.0}


class Sim:
    """The printer the display thinks it is talking to."""

    def __init__(self, args):
        self.args = args
        self.clock = 0.0
        self.paused = False
        self.shutdown = None
        self.homed = {"X": False, "Y": False, "Z": False}
        self.pos = {"X": 0.0, "Y": 0.0, "Z": 0.0}
        self.busy_until = 0.0
        self.events = []

    # -- time ----------------------------------------------------------

    def tick(self, dt):
        # A paused print stops advancing, which is the whole observable
        # difference between paused and printing.
        if not self.paused and self.shutdown is None:
            self.clock += dt

    @property
    def working(self):
        return time.time() < self.busy_until

    def _busy(self, seconds):
        self.busy_until = max(self.busy_until, time.time() + seconds)

    def note(self, text):
        self.events.append(text)

    # -- upstream commands ---------------------------------------------

    def handle(self, body):
        """A KNOMI_CMD the display sent, minus its prefix."""
        if body == b"STOP":
            # Klipper uppercases the first line of the shutdown message and
            # sends it in the gcodes field; the shutdown screen renders that.
            self.shutdown = "STOP REQUESTED BY KNOMI_SERIAL"
            self.note("STOP -> shutdown")
            return

        if body == b"RESTART":
            self.shutdown = None
            self.paused = False
            self.homed = {axis: False for axis in self.homed}
            self.busy_until = 0.0
            self.note("RESTART -> back to idle, unhomed")
            return

        if body.startswith(b"GCODE:"):
            self._gcode(body[len(b"GCODE:"):].decode("utf-8", "replace").strip())
            return

        if body.startswith(b"MOVE:"):
            self._move(body[len(b"MOVE:"):].decode("utf-8", "replace").strip())
            return

        self.note(f"unrecognised: {body.decode('utf-8', 'replace')}")

    def _gcode(self, gcode):
        upper = gcode.upper()

        if upper == "PAUSE":
            self.paused = True
            self.note("PAUSE")
            return
        if upper == "RESUME":
            self.paused = False
            self.note("RESUME")
            return
        if upper == "CANCEL_PRINT":
            # Jump the clock past the printing phase so the loop lands in
            # "finished" rather than snapping back to a fresh print.
            warm = 8.0
            total = warm + self.args.duration + 6.0
            self.clock = (self.clock // total) * total + warm + self.args.duration
            self.paused = False
            self.note("CANCEL_PRINT")
            return

        if upper == "G28":
            self.homed = {axis: True for axis in self.homed}
        elif upper.startswith("G28 "):
            for axis in upper[4:].split():
                if axis in self.homed:
                    self.homed[axis] = True

        self._busy(DURATIONS.get(upper, 2.0))
        self.note(gcode)

    def _move(self, direction):
        # The device sends e.g. "X+" - axis then sign, which is how
        # knomi_serial._process_cmd reads it too.
        if len(direction) < 2:
            self.note(f"bad move: {direction}")
            return
        axis, sign = direction[-2].upper(), direction[-1]
        if axis not in self.pos or sign not in "+-":
            self.note(f"bad move: {direction}")
            return
        step = MOVE_STEP[axis] * (1 if sign == "+" else -1)
        self.pos[axis] += step
        self._busy(0.8)
        self.note(f"move {axis}{sign} -> {self.pos[axis]:.0f}")

    # -- downstream state ----------------------------------------------

    def phase(self):
        """Where the fake print is. (label, progress, hotend, target, bed, bedt)"""
        warm, done = 8.0, 6.0
        duration = self.args.duration
        total = warm + duration + done
        t = self.clock % total

        if t < warm:
            f = t / warm
            return ("heating", 0.0, 25 + 220 * f, 245, 25 + 75 * f, 100)
        t -= warm
        if t < duration:
            return ("printing", 100.0 * t / duration, 243, 245, 100, 100)
        t -= duration
        f = t / done
        return ("finished", 100.0, 245 - 200 * f, 0, 100 - 70 * f, 0)

    def state(self):
        args = self.args
        label, progress, hot, tgt, bed, bedt = self.phase()

        if self.shutdown is not None:
            return "shutdown", k.PrinterState(
                status=k.PrinterStatus.SHUTDOWN,
                gcodes=self.shutdown.encode("utf-8")[:k._GCODES_MAX_LEN],
            )

        status = k.PrinterStatus.PRINTING if label == "printing" else k.PrinterStatus.IDLE
        if args.status is not None:
            status = {
                "idle": k.PrinterStatus.IDLE,
                "printing": k.PrinterStatus.PRINTING,
                "shutdown": k.PrinterStatus.SHUTDOWN,
            }[args.status]
        if args.progress is not None:
            progress = args.progress
        if args.hotend is not None:
            hot = args.hotend
        if args.target is not None:
            tgt = args.target

        if args.cycle_colours:
            step = max(1.0, args.duration / 4)
            colour, ftype = PRESETS[int(self.clock // step) % len(PRESETS)]
        else:
            colour, ftype = PRESETS[0]
        if args.color is not None:
            colour = args.color
        if args.type is not None:
            ftype = args.type

        return label, k.PrinterState(
            status=status,
            working=self.working,
            paused=self.paused,
            homed_x=self.homed["X"],
            homed_y=self.homed["Y"],
            homed_z=self.homed["Z"],
            used=args.used,
            active=args.active,
            hotend_temp=hot,
            hotend_target=tgt,
            bed_temp=bed,
            bed_target=bedt,
            chamber_temp=args.chamber,
            chamber_target=0,
            mcu_temp=0,
            mcu_target=0,
            progress=progress,
            tool_number=args.tool,
            filament_color=int(colour, 16),
            tram_type=k.PrinterTramType.QGL,
            filament_type=ftype.encode("utf-8")[:15],
            gcodes=b"HOME\nQGL\nPURGE",
        )


def _bytes(value):
    try:
        n = int(value)
    except (TypeError, ValueError):
        return "?"
    if n >= 1 << 20:
        return f"{n / (1 << 20):.1f}M"
    if n >= 1 << 10:
        return f"{n // (1 << 10)}k"
    return str(n)


def drain(port, sim, seen):
    """Read the display's uplink: reports to the status line, commands to the sim."""
    try:
        waiting = port.in_waiting
    except (OSError, serial.SerialException):
        return
    if not waiting:
        return

    seen["buf"] += port.read(waiting)
    while b"\n" in seen["buf"]:
        line, _, seen["buf"] = seen["buf"].partition(b"\n")
        line = line.strip()
        if not line.startswith(b"KNOMI_CMD:"):
            continue
        body = line[len(b"KNOMI_CMD:"):]

        if body.startswith(b"RPT:"):
            text = body[4:].decode("utf-8", "replace")
            fields = dict(p.split("=", 1) for p in text.split(";") if "=" in p)
            seen["rpt"] = fields

            # Identity is announced once; the live numbers ride the status line
            # instead, or every heap reading would scroll the screen away.
            identity = {key: fields.get(key) for key in ("fw", "proto", "var")}
            if identity != seen.get("identity"):
                seen["identity"] = identity
                sim.note(
                    "fw {fw} proto {proto} {var}  psram {ps}  lvgl free {lv}".format(
                        fw=fields.get("fw", "?"), proto=fields.get("proto", "?"),
                        var=fields.get("var", "?"),
                        ps=_bytes(fields.get("psram")),
                        lv=_bytes(fields.get("lvfree")),
                    )
                )
                if fields.get("proto") != str(k._PROTO_VERSION):
                    sim.note(
                        f"WARNING: display speaks protocol {fields.get('proto')}, "
                        f"this encoder writes {k._PROTO_VERSION}. Reflash it."
                    )
        elif body.startswith(b"I2C:"):
            sim.note("i2c: " + body[4:].decode("utf-8", "replace"))
        else:
            sim.handle(body)


def device_load(seen):
    """What the screen says about its own load."""
    rpt = seen.get("rpt")
    if not rpt:
        return "device silent"
    if "busy" not in rpt:
        return "heap {}".format(_bytes(rpt.get("heap")))
    return "ui {:.1f}% peak {:.1f}ms heap {} frag {}%".format(
        int(rpt["busy"]) / 10.0,
        int(rpt.get("peak", 0)) / 1000.0,
        _bytes(rpt.get("heap")),
        rpt.get("lvfrag", "?"),
    )


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for p in ports:
        print(f"  {p.device:20} {p.description}")


def main():
    p = argparse.ArgumentParser(
        description="Stand in for Klipper and drive a Knomi_Serial display.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("port", nargs="?", help="serial port, e.g. COM5 or /dev/ttyUSB0")
    p.add_argument("--list", action="store_true", help="list serial ports and exit")
    p.add_argument("--baud", type=int, default=k._BAUD_RATE)
    p.add_argument("--duration", type=float, default=45.0,
                   help="seconds for the fake print to run (default 45)")
    p.add_argument("--once", action="store_true", help="send one packet and exit")

    p.add_argument("--progress", type=float, help="pin progress, 0-100")
    p.add_argument("--hotend", type=float, help="pin hotend temperature")
    p.add_argument("--target", type=float, help="pin hotend target")
    p.add_argument("--chamber", type=float, default=0, help="chamber temperature")
    p.add_argument("--status", choices=("idle", "printing", "shutdown"))
    p.add_argument("--color", "--colour", dest="color", help="filament colour, RRGGBB")
    p.add_argument("--type", help="filament type, e.g. PLA")
    p.add_argument("--tool", type=int, default=0, help="tool number, -1 for none")
    p.add_argument("--cycle-colours", "--cycle-colors", dest="cycle_colours",
                   action="store_true", help="step through the preset filaments")
    p.add_argument("--no-used", dest="used", action="store_false",
                   help="this tool is not in the job - the screen should sleep")
    p.add_argument("--no-active", dest="active", action="store_false",
                   help="this tool is not the mounted one")
    p.set_defaults(used=True, active=True)

    args = p.parse_args()

    if args.list:
        list_ports()
        return 0
    if not args.port:
        p.print_help()
        print("\nNo port given. Ports on this machine:")
        list_ports()
        return 2

    if args.color:
        args.color = args.color.strip().lstrip("#")
        try:
            int(args.color, 16)
        except ValueError:
            return p.error(f"--color '{args.color}' is not hex")

    try:
        port = serial.Serial(args.port, args.baud, write_timeout=1.0)
    except serial.SerialException as e:
        sys.exit(f"Could not open {args.port}: {e}")

    print(f"Driving {args.port} at {args.baud}. Buttons on the screen work. Ctrl-C to stop.")
    sim = Sim(args)
    seen = {"buf": b""}
    last = time.time()

    try:
        while True:
            now = time.time()
            sim.tick(now - last)
            last = now

            label, state = sim.state()
            port.write(k.encode_state(state))
            drain(port, sim, seen)

            # Anything the display did scrolls above the status line, so the
            # cause of a state change is visible rather than inferred.
            while sim.events:
                print("\r  \033[K" + sim.events.pop(0))

            homed = "".join(a if sim.homed[a] else "-" for a in ("X", "Y", "Z"))
            print(
                "\r  {:<9} {:>3.0f}%  {:>3.0f}/{:<3.0f}  {:<5} homed {}{}{}  |  {}\033[K".format(
                    "paused" if sim.paused else label,
                    state.progress, state.hotend_temp, state.hotend_target,
                    state.filament_type.decode(), homed,
                    "  MOVING" if sim.working else "",
                    "" if state.used else "  [not in job]",
                    device_load(seen),
                ),
                end="", flush=True,
            )

            if args.once:
                break
            time.sleep(_PERIOD)
    except KeyboardInterrupt:
        print()
    finally:
        try:
            # Leave the screen in a defined state rather than frozen on the last
            # frame, which would otherwise look like a live print forever.
            port.write(k.encode_state(
                k.PrinterState(status=k.PrinterStatus.DISCONNECTED)))
            port.flush()
            port.close()
        except Exception:
            pass
    print("Stopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
