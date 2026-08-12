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

Stop Klipper first. A port it holds cannot be opened here at all, and is
reported as busy rather than guessed at.
"""

import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(_HERE), "klippy_extras"))

try:
    import serial
except ImportError:
    sys.exit("pyserial is not installed. pip install pyserial")

import knomi_serial as k  # noqa: E402


def busy(port):
    """Whether something else holds this port.

    Only asked about ports that told us nothing, to say which of the two
    reasons it was: nothing on the end of it, or Klipper already talking to it.
    """
    try:
        with serial.Serial(port, k._BAUD_RATE, timeout=0, exclusive=True):
            return False
    except (serial.SerialException, OSError):
        return True


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
    # The full report, not just the id. Discovery has already parsed a whole
    # line to find the id, so asking the port again for its firmware version
    # would be a second open of a port something else may be reading - which is
    # exactly how this used to print `?` at random when Klipper was up.
    found = k.discover_reports(ports, listen=args.listen)

    silent = [port for port in ports
              if port not in {f["port"] for f in found.values()}]

    if not found:
        print("  Nothing answered.")
        if any(busy(port) for port in silent):
            print("  Klipper is holding these ports. Stop it and try again.")
        else:
            print("  Firmware older than the one that reports `id` will not "
                  "answer; reflash it.")
        return 1

    if args.config:
        for n, (ident, fields) in enumerate(sorted(found.items())):
            print(f"\n[knomi_serial T{n}_knomi]")
            print(f"device_id: {ident}")
            print(f"# was on {fields['port']} when discovered, "
                  "which no longer matters")
        return 0

    print()
    print(f"  {'id':<10} {'port':<14} {'firmware':<24} up")
    print("  " + "-" * 62)
    for ident, fields in sorted(found.items()):
        up = fields.get("up")
        age = f"{up}s" if up and up.isdigit() else "?"
        fw = fields.get("fw", "?")
        print(f"  {ident:<10} {fields['port']:<14} {fw:<24} {age}")
    for port in silent:
        why = "in use - stop Klipper" if busy(port) else "no display answered"
        print(f"  {'-':<10} {port:<14} {why}")
    print()
    print("  Put the id in printer.cfg as `device_id:` instead of `serial:`,")
    print("  and the display keeps its identity whichever socket it is in.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
