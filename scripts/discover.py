#!/usr/bin/env python3
"""Find every display and say which is which.

Each one carries a permanent id burned into its chip, and announces it in the
status line it already sends twice a second. This opens every candidate port,
listens, and prints what answered - so a row of six identical displays can be
written into printer.cfg without unplugging any of them to find out.

    python scripts/discover.py
    python scripts/discover.py --config      # ready to paste

The same id is on each display's waiting screen, under its name, so you can also
just read it off the glass.

Nothing else may be driving the ports at the time - stop Klipper first, or the
sections it has open will not answer.
"""

import argparse
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(_HERE), "klippy_extras"))

try:
    import serial
except ImportError:
    sys.exit("pyserial is not installed. pip install pyserial")

import knomi_serial as k  # noqa: E402


def details(port, seconds=3.0):
    """Firmware version and uptime, for a port we already know answers."""
    out = {}
    try:
        with serial.Serial(port, k._BAUD_RATE, timeout=0) as handle:
            buf = b""
            deadline = time.time() + seconds
            while time.time() < deadline:
                if handle.in_waiting:
                    buf += handle.read(handle.in_waiting)
                    while b"\n" in buf:
                        line, _, buf = buf.partition(b"\n")
                        line = line.strip()
                        if not line.startswith(k._CMD_PREFIX + k._CMD_REPORT):
                            continue
                        body = line[len(k._CMD_PREFIX) + len(k._CMD_REPORT):]
                        text = body.decode("utf-8", "replace")
                        out = dict(
                            item.split("=", 1)
                            for item in text.split(";")
                            if "=" in item
                        )
                        return out
                time.sleep(0.02)
    except (serial.SerialException, OSError):
        pass
    return out


def main():
    p = argparse.ArgumentParser(
        description="Find Knomi_Serial displays and report their hardware ids.")
    p.add_argument("--listen", type=float, default=k._DISCOVER_LISTEN,
                   help="seconds to wait for each display to announce itself")
    p.add_argument("--config", action="store_true",
                   help="print printer.cfg sections instead of a table")
    args = p.parse_args()

    ports = k.candidate_ports()
    if not ports:
        print("  No candidate ports. Is anything plugged in?")
        return 1

    print(f"  Listening on {', '.join(ports)} for {args.listen:.0f}s ...")
    found = k.discover(ports, listen=args.listen)
    if not found:
        print("  Nothing answered.")
        print("  If Klipper is running it already holds these ports - stop it "
              "and try again.")
        print("  Firmware older than the one that reports `id` will not answer "
              "either; reflash it.")
        return 1

    if args.config:
        for n, (ident, port) in enumerate(sorted(found.items())):
            print(f"\n[knomi_serial T{n}_knomi]")
            print(f"device_id: {ident}")
            print(f"# was on {port} when discovered, which no longer matters")
        return 0

    print()
    print(f"  {'id':<10} {'port':<14} {'firmware':<24} up")
    print("  " + "-" * 62)
    for ident, port in sorted(found.items()):
        info = details(port)
        up = info.get("up")
        age = f"{up}s" if up and up.isdigit() else "?"
        fw = info.get("fw", "?")
        print(f"  {ident:<10} {port:<14} {fw:<24} {age}")
    print()
    print("  Put the id in printer.cfg as `device_id:` instead of `serial:`,")
    print("  and the display keeps its identity whichever socket it is in.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
