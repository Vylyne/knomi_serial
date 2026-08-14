#!/usr/bin/env python3
"""Keep a map of which display is on which port, including while Klipper is down.

Klipper can only see the ports it holds, and it holds none of them when it is
not running - which is exactly when the question matters most, because flashing
a display requires the port to be free. So the answer cannot live only in
Klipper.

This listens to udev for serial ports appearing and disappearing, identifies
anything new that is not already spoken for, and writes what it learns to a file
that Klipper and a firmware updater can both read:

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
#: section, or a flashing tool. First retry after this long, then doubling.
RETRY_BUSY = 30.0

#: How long a wait can grow to. Reached after a few doublings, and then held.
#:
#: A busy port is usually a display Klipper is driving, and that is the one case
#: where asking is pointless by construction: Klipper holds the ports it knows
#: about, so anything it holds is already answerable from
#: printer.knomi_cluster.devices. That is the whole split this service exists on
#: one side of. Retrying it every thirty seconds for the life of the printer is
#: this process doing Klipper's half of the table, failing, and saying so.
#:
#: What makes a long wait safe is that nothing needs the answer promptly.
#: Anything that physically changes arrives as an event and clears the wait (see
#: forget_backoff). The only thing left is a port freed without re-enumerating -
#: Klipper being stopped - and the consumer that cares about that case, a
#: firmware updater, runs its own discovery pass before flashing anything.
RETRY_CAP = 900.0


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
        #: How many times each has refused, which is what the wait doubles on.
        self.refusals = {}
        #: What each was last deferred for, so a port that has been busy since
        #: this started is not announced as busy again every time it is checked.
        self.reason = {}

    def _defer(self, port, now, base):
        """Not this one, not yet - and each time, for longer.

        Said once. A port whose situation has not changed has nothing new to
        report, and the alternative is two lines a minute for every display the
        printer is driving, forever, which is most of them.
        """
        n = self.refusals.get(port, 0)
        self.refusals[port] = n + 1
        wait = min(base * (2 ** n), RETRY_CAP)
        self.quiet_until[port] = now + wait
        why = "busy" if base == RETRY_BUSY else "silent"
        if self.reason.get(port) != why:
            self.reason[port] = why
            logging.info(
                "%s is %s - asking again in %gs, then less often", port, why,
                wait)

    def forget_backoff(self):
        """Something changed on the bus, so every refusal is worth re-testing.

        This is what lets the wait above grow to a quarter of an hour without
        the service becoming slow to react. A port unplugged and plugged back
        in, a hub repowered, anything at all arriving over netlink means the
        machine is not what it was when a port last refused.
        """
        self.quiet_until.clear()
        self.refusals.clear()

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
        for gone in [p for p in self.quiet_until if p not in present]:
            del self.quiet_until[gone]
            self.refusals.pop(gone, None)
            self.reason.pop(gone, None)
        self.confirmed &= present

        for port in sorted(present - self.confirmed):
            if now < self.quiet_until.get(port, 0):
                continue
            # Asked before listened to, in that order. A port somebody else
            # holds cannot be listened to at all, so going the other way round
            # means opening it twice to find that out - and discover_reports
            # logs the refusal on its way past, which on a running printer is a
            # line per display per attempt for as long as the printer is up.
            if not _openable(port):
                self._defer(port, now, RETRY_BUSY)
                continue
            got = self._identify(port)
            if got is None:
                self._defer(port, now, RETRY_UNKNOWN)
                continue
            ident, fields = got
            self.quiet_until.pop(port, None)
            self.refusals.pop(port, None)
            self.reason.pop(port, None)
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

    def next_deadline(self):
        """Seconds until the earliest port worth retrying, or None for never.

        Ports appearing and going away arrive as events, so the only thing left
        needing a clock is a port that answered nothing or was busy - those are
        deliberately not asked again for a while, and nothing will wake us when
        that while is up.
        """
        if not self.quiet_until:
            return None
        return max(0.0, min(self.quiet_until.values()) - self.now())


class PortEvents:
    """Blocks until the kernel says a tty appeared or went away.

    The alternative is asking once a second, which is a sysfs walk and a wakeup
    per second forever to answer "no" almost every time. This is the same
    question asked the other way round.
    """

    def __init__(self):
        # Imported here rather than at module scope because --once never waits
        # for an event, and service/README.md tells people to run that before
        # installing anything at all.
        import pyudev

        # from_netlink defaults to the "udev" source rather than "kernel",
        # which matters: the udev event arrives after udevd has created
        # /dev/ttyUSB*, and a tick opens the port immediately. The kernel event
        # would race the device node into existence.
        self._monitor = pyudev.Monitor.from_netlink(pyudev.Context())
        # Filtering before start() installs it in the kernel, so a boot's worth
        # of unrelated uevents never reaches this process at all.
        #
        # The whole tty subsystem rather than ttyUSB, on two counts. The filter
        # matches SUBSYSTEM and DEVTYPE, and tty devices carry no DEVTYPE, so
        # there is nothing there to narrow with - it would have to be a check on
        # the device node after the wakeup, which saves the tick and not the
        # wake. And it would be wrong: a 303A board is the ESP32 as the USB
        # device itself, which enumerates as ttyACM rather than ttyUSB, so
        # matching on ttyUSB would quietly stop noticing half of _USB_VENDORS.
        #
        # The cost of a tty event that is not ours is one tick - a sysfs walk
        # that finds nothing new. That is what the old loop paid every second.
        self._monitor.filter_by(subsystem="tty")
        self._monitor.start()

    def wait(self, timeout):
        """True if something changed, False if the timeout ran out first."""
        if self._monitor.poll(timeout=timeout) is None:
            return False
        # A hub powering up delivers an event per port, and each port delivers
        # more than one. A tick is a snapshot of everything, so draining the
        # burst here means one pass answers all of it.
        while self._monitor.poll(timeout=0) is not None:
            pass
        return True

    # No filter on the action. add, remove, change and bind all mean "look
    # again", and telling them apart would only create a way to miss one.


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

    # Subscribed before the first pass, deliberately. Identifying a port takes
    # seconds, and a display plugged in during that first pass would otherwise
    # happen entirely in the gap between looking and listening. Subscribing
    # first means the event waits on the socket instead.
    try:
        events = PortEvents()
    except ImportError:
        return "pyudev is not installed. sudo apt install python3-pyudev"

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
        # An event and a deadline both mean the same thing - take a snapshot.
        # They differ in one respect only: an event says the machine changed,
        # and a port that refused the last time it was asked deserves asking
        # again on that basis rather than waiting out a backoff earned under
        # conditions that no longer hold.
        if events.wait(w.next_deadline()):
            w.forget_backoff()


if __name__ == "__main__":
    sys.exit(main())
