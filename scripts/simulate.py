#!/usr/bin/env python3
"""Drive a Knomi_Serial display with made-up printer state, over USB.

For working on the UI without starting a print - or without a printer. Plug the
display into any machine with Python and pyserial and run this at it.

The packets come from knomi_serial.encode_state, the same encoder Klipper uses,
so what the screen sees here is byte for byte what it sees in service. That is
the point: a separate implementation would drift, and a bench test that
exercises the test rig teaches you nothing about the firmware.

    python scripts/simulate.py --list
    python scripts/simulate.py /dev/ttyUSB0
    python scripts/simulate.py COM7 --progress 54 --color FFA7C4 --type ABS
    python scripts/simulate.py COM7 --duration 30 --cycle-colours

With no pinned values it runs a whole print on loop: cold, heating, printing
from nothing to done, then finished. Pin anything and that value stops moving,
so `--progress 54` parks the fill exactly at the ink crossover.

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


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for p in ports:
        print(f"  {p.device:20} {p.description}")


def phase(elapsed, duration):
    """Where we are in a looping fake print.

    Returns (status, progress, hotend, target, bed, bed_target, label). The
    heat-up and cool-down are real ramps rather than jumps, because watching the
    heat colour cross from steel to amber is half of what this is for.
    """
    warm, done = 8.0, 6.0
    total = warm + duration + done
    t = elapsed % total

    if t < warm:
        k_ = t / warm
        return ("idle", 0, 25 + 220 * k_, 245, 25 + 75 * k_, 100, "heating")
    t -= warm
    if t < duration:
        return ("printing", 100.0 * t / duration, 243 + 2 * (t % 2), 245, 100, 100, "printing")
    t -= duration
    k_ = t / done
    return ("idle", 100, 245 - 200 * k_, 0, 100 - 70 * k_, 0, "finished")


def build(args, elapsed):
    status, progress, hot, tgt, bed, bedt, label = phase(elapsed, args.duration)

    if args.status is not None:
        status = args.status
    if args.progress is not None:
        progress = args.progress
    if args.hotend is not None:
        hot = args.hotend
    if args.target is not None:
        tgt = args.target

    if args.cycle_colours:
        colour, ftype = PRESETS[int(elapsed // max(1.0, args.duration / 4)) % len(PRESETS)]
    else:
        colour, ftype = PRESETS[0]
    if args.color is not None:
        colour = args.color
    if args.type is not None:
        ftype = args.type

    state = k.PrinterState(
        status=k.PrinterStatus.PRINTING if status == "printing" else (
            k.PrinterStatus.SHUTDOWN if status == "shutdown" else k.PrinterStatus.IDLE
        ),
        working=status == "printing",
        paused=args.paused,
        homed_x=True,
        homed_y=True,
        homed_z=True,
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
        tram_type=k.PrinterTramType.NONE,
        filament_type=ftype.encode("utf-8")[:15],
        gcodes=b"HOME\nQGL\nPURGE",
    )
    return state, label


def drain(port, seen):
    """Print whatever the display says back, so the link is visibly two-way."""
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
            fields = dict(
                part.split("=", 1) for part in text.split(";") if "=" in part
            )
            if fields != seen.get("last_rpt"):
                seen["last_rpt"] = fields
                print(
                    "\n  device: fw={fw} proto={proto} {var} "
                    "sleep={sleep} screen={scr} heap={heap}".format(
                        fw=fields.get("fw", "?"), proto=fields.get("proto", "?"),
                        var=fields.get("var", "?"), sleep=fields.get("sleep", "?"),
                        scr=fields.get("scr", "?"), heap=fields.get("heap", "?"),
                    )
                )
                if fields.get("proto") != str(k._PROTO_VERSION):
                    print(
                        f"  WARNING: display speaks protocol {fields.get('proto')}, "
                        f"this encoder writes {k._PROTO_VERSION}. Reflash it."
                    )
        else:
            print("\n  device: " + body.decode("utf-8", "replace"))


def main():
    p = argparse.ArgumentParser(
        description="Feed a Knomi_Serial display simulated printer state.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("port", nargs="?", help="serial port, e.g. COM7 or /dev/ttyUSB0")
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
    p.add_argument("--paused", action="store_true")
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

    print(f"Driving {args.port} at {args.baud}. Ctrl-C to stop.")
    seen = {"buf": b""}
    start = time.time()
    last_label = None

    try:
        while True:
            elapsed = time.time() - start
            state, label = build(args, elapsed)
            port.write(k.encode_state(state))

            if label != last_label and not args.once:
                last_label = label
                print(f"\n[{label}]")
            print(
                "\r  {:>3.0f}%  hotend {:>3.0f}/{:<3.0f}  bed {:>3.0f}  "
                "#{}  {:<5} {}".format(
                    state.progress, state.hotend_temp, state.hotend_target,
                    state.bed_temp, f"{state.filament_color:06X}",
                    state.filament_type.decode(),
                    "" if state.used else "[not in job]",
                ),
                end="", flush=True,
            )

            drain(port, seen)
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
