#!/usr/bin/env python3
"""Keep a map of which display is on which port, including while Klipper is down.

Klipper can only see the ports it holds, and it holds none of them when it is
not running - which is exactly when the question matters most, because flashing
a display requires the port to be free. So the answer cannot live only in
Klipper.

This watches for serial ports appearing and disappearing, identifies anything
new that is not already spoken for, and writes what it learns to a file that
Klipper and a firmware updater can both read:

    {"version": 1,
     "devices": {"19aa44": {"port": "/dev/ttyUSB0",
                            "fw": "0.5.0", "var": "knomi"}}}

No timestamps in it. The file's own mtime says when it last changed, and an
entry existing already means the display is there - identified during this run
and not vanished since. Whether this service is alive at all is a question for
`systemctl is-active knomi_serial`, not for a field that only moves when
something is unplugged.

It is never authoritative and nothing has to trust it. A display named in this
file may have been moved since; Klipper checks the id in the first report it
receives and throws the entry away if it disagrees. That is what makes a cached
map safe to keep at all - the file is a hint that saves a discovery pass, not a
claim anybody acts on blindly.

    python3 service/knomi_serial_watch.py --once   # one pass, print, exit
    python3 service/knomi_serial_watch.py          # run until stopped
"""

import argparse
import json
import logging
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(_HERE), "klippy_extras"))

try:
    import serial  # noqa: F401
except ImportError:
    sys.exit("pyserial is not installed. pip install pyserial")

import knomi_serial as k  # noqa: E402

#: Schema version of the file this writes. A reader that does not recognise it
#: should ignore the file rather than guess, and fall back to discovering.
FORMAT_VERSION = 1

DEFAULT_PATH = os.path.expanduser("~/printer_data/knomi/devices.json")

#: How often to ask which ports exist. This is a sysfs read costing well under a
#: millisecond and opens nothing, so the loop is idle almost all of the time -
#: the expensive operation is identifying a port, and that happens only when one
#: appears.
POLL_PERIOD = 1.0

#: How long to listen to a port that has just appeared. Longer than the
#: two-second report period because a freshly enumerated board has to boot
#: first, and a port existing does not mean the ESP32 behind it is awake yet.
LISTEN = 5.0

#: A port that did not identify itself is not asked again for this long. It is
#: most likely a CH340 that is not a display at all - something else on the
#: printer - and retrying it every second would mean holding a stranger's serial
#: port open forever.
RETRY_UNKNOWN = 120.0

#: A port that could not be opened is somebody else's right now: a Klipper
#: section, or a flashing tool. Worth trying again sooner than an unknown one,
#: because it will be free eventually and its identity is worth having then.
RETRY_BUSY = 30.0


def load(path):
    """The map as last written, or an empty one."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    if data.get("version") != FORMAT_VERSION:
        logging.warning("knomi_serial_watch: ignoring %s, unknown version %r",
                        path, data.get("version"))
        return {}
    devices = data.get("devices")
    return devices if isinstance(devices, dict) else {}


def save(path, devices):
    """Write the map, atomically.

    Through a temporary file and os.replace because Klipper reads this at
    startup and a firmware updater reads it whenever it likes. A reader that
    caught a half-written file would parse it as no displays at all, which is
    indistinguishable from a genuine answer.
    """
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    payload = {"version": FORMAT_VERSION, "devices": devices}
    tmp = f"{path}.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


class Watcher:
    """Identifies displays as their ports appear, and forgets them as they go."""

    def __init__(self, path=DEFAULT_PATH, listen=LISTEN, now=time.time):
        self.path = path
        self.listen = listen
        self.now = now
        self.devices = load(path)
        #: Ports identified since this process started. Deliberately not "ports
        #: named in the map": what was loaded from disk describes the machine as
        #: it was when something last looked, and the case this exists to serve
        #: is a cable moved while nothing was. Shut down, swap two leads, boot -
        #: both ports are present and both are in the file, so treating the file
        #: as settled means never looking again and being wrong forever. Every
        #: port gets identified once per run; the loaded entries are hints in
        #: the meantime.
        self.confirmed = set()
        #: Ports not worth asking again yet, and when that changes.
        self.quiet_until = {}

    def _identify(self, port):
        """Ask one port who is on it. Returns an (id, fields) pair or None."""
        try:
            found = k.discover_reports([port], listen=self.listen)
        except Exception as e:
            logging.info("knomi_serial_watch: %s could not be read: %s", port, e)
            return None
        for ident, fields in found.items():
            return ident, fields
        return None

    def tick(self):
        """One pass. Returns True if the map changed."""
        now = self.now()
        present = set(k.candidate_ports())

        # Anything whose port has physically gone is gone. Keeping the entry
        # would mean handing Klipper a path that cannot be opened, which costs
        # it a failed connect and a rediscovery to work out what this already
        # knows.
        before = dict(self.devices)
        self.devices = {
            ident: fields for ident, fields in self.devices.items()
            if fields.get("port") in present
        }
        for port in list(self.quiet_until):
            if port not in present:
                del self.quiet_until[port]
        self.confirmed &= present

        for port in sorted(present - self.confirmed):
            if now < self.quiet_until.get(port, 0):
                continue
            got = self._identify(port)
            if got is None:
                # Either nothing answered or the port belongs to someone else.
                # Told apart only to pick how long to wait before asking again.
                busy = not _openable(port)
                self.quiet_until[port] = now + (
                    RETRY_BUSY if busy else RETRY_UNKNOWN)
                continue
            ident, fields = got
            self.quiet_until.pop(port, None)
            self.confirmed.add(port)
            # Whatever used to be recorded here is not here now. Without this a
            # swap leaves both displays claiming the port, and which one a
            # reader gets depends on dict order.
            for other in [i for i, f in self.devices.items()
                          if f.get("port") == port and i != ident]:
                del self.devices[other]
            self.devices[ident] = {
                "port": port,
                "fw": fields.get("fw"),
                "var": fields.get("var"),
            }

        changed = self.devices != before
        if changed:
            save(self.path, self.devices)
        return changed


def _openable(port):
    """Whether the port is free, to tell 'busy' from 'not a display'."""
    try:
        with serial.Serial(port, k._BAUD_RATE, timeout=0, exclusive=True):
            return True
    except (serial.SerialException, OSError):
        return False


def main():
    p = argparse.ArgumentParser(
        description="Track which Knomi_Serial display is on which port.")
    p.add_argument("--out", default=DEFAULT_PATH,
                   help=f"where to write the map (default {DEFAULT_PATH})")
    p.add_argument("--listen", type=float, default=LISTEN,
                   help="seconds to wait for a new port to announce itself")
    p.add_argument("--period", type=float, default=POLL_PERIOD,
                   help="seconds between checks for ports appearing")
    p.add_argument("--once", action="store_true",
                   help="one pass, print the result, exit")
    args = p.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s knomi_serial_watch: %(message)s")

    w = Watcher(args.out, listen=args.listen)
    if args.once:
        w.tick()
        json.dump(w.devices, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0

    logging.info("watching for displays, writing %s", args.out)
    while True:
        try:
            if w.tick():
                logging.info(
                    "map now %s",
                    ", ".join(f"{i} on {f['port']}"
                              for i, f in sorted(w.devices.items())) or "empty")
        except Exception as e:
            logging.warning("pass failed: %s", e)
        time.sleep(args.period)


if __name__ == "__main__":
    sys.exit(main())
