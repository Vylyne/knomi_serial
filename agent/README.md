# The watcher agent

**Optional.** Nothing needs it. One display and a `device_id:` line works
without it, and Klipper falls back to discovering the row itself whenever the
map is missing, stale in a way it notices, or written by a version it does not
recognise.

It exists for the things Klipper structurally cannot do, all of which come from
one fact: **a port can only be read by one program at a time, and Klipper holds
the ones it uses.** So Klipper can never tell you about a port it is not on, and
it can tell you nothing at all when it is not running — which is exactly when a
display is being flashed, because flashing needs the port free.

| | Klipper running | Klipper stopped |
| --- | --- | --- |
| ports Klipper holds | `printer.knomi_cluster.devices` | — |
| every other port | the agent | the agent |

So the two are complements rather than alternatives, and the split is forced by
the exclusivity rather than chosen.

## What it gives you

- **`mcu-updater` can find and flash displays with Klipper stopped**, by
  identity rather than by guessing at `/dev/ttyUSB*`.
- **Klipper starts without a discovery pass.** It reads the map, opens the
  ports named in it, and confirms each display from its first report. A couple
  of seconds off startup, and no ports opened on spec.
- **A display plugged in mid-print is noticed.** Klipper will not discover
  during a job — discovery blocks the thread that feeds the steppers — so
  without the agent a display lost mid-print stays dark until the job ends.

## Install

`install.sh` writes `agent/knomi_serial.service` with this machine's paths
already filled in — where the repo is, who Klipper runs as, which python. It
does not install it, because a root-owned unit running a script out of a git
checkout is a decision to make deliberately rather than one an install script
should make for you.

Try it first without installing anything:

```sh
python3 agent/knomi_watch.py --once
```

which does one pass, prints the map and exits. Then, if you want it running:

```sh
sudo cp agent/knomi_serial.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now knomi_serial
```

## The file

`~/printer_data/knomi/devices.json`, written atomically through a temporary file
so a reader can never catch it half-written:

```json
{
  "version": 1,
  "updated": 1723480000.0,
  "devices": {
    "19aa44": {
      "port": "/dev/ttyUSB0",
      "seen": 1723479990.0,
      "fw": "0.5.0+54.g5509d4f",
      "var": "knomi"
    }
  }
}
```

`version` is checked, not assumed: a reader that does not recognise it ignores
the file rather than guessing at its shape.

## Why it is safe to keep a map at all

Caching which display was on which port is caching something that can change
while nothing is watching — a cable moved with the machine off, a hub swapped.
Acting on a stale map is precisely the failure that addressing by hardware id
exists to prevent: one tool's readings on another tool's screen, with everything
looking healthy.

It is safe here because **nothing acts on it blindly**. Every display reports
its id in every status line, so Klipper opens the port the map names and then
checks who actually answered. A disagreement drops the connection and throws the
cache away, and the next pass discovers properly.

A wrong hint therefore costs one reconnect. It cannot produce a wrong screen.

That check lives in `_verify_identity` in `klippy_extras/knomi_serial.py`, and
it is the reason this component is allowed to exist.

## What it does

Once a second it asks which serial ports are present — a sysfs read costing
well under a millisecond, which opens nothing. Almost always the answer is the
same as last time and it goes back to sleep.

When a port **appears**, and it is not already accounted for, the agent listens
to it for a few seconds and records whatever announces itself. When a port
**disappears**, its entry is dropped, because a path that cannot be opened is
worse than no path at all.

It never holds a port. It takes the same `flock` that Klipper's sections and
`esptool` take, so a port being driven or flashed is skipped rather than
disturbed, and it listens only long enough to read one report.

Ports that answer nothing are left alone for two minutes — most likely something
on the printer that is not a display, and holding a stranger's serial port open
on a one-second loop would be rude and pointless. Ports that were busy are
retried sooner, at thirty seconds, since they will free up eventually and their
identity is worth having when they do.
