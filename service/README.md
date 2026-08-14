# The watcher service

**Installed by default, and `--no-watch` declines it.** Nothing strictly needs
it — one display and a `device_id:` line works without it, and Klipper falls back
to discovering the row itself whenever the map is missing, stale in a way it
notices, or written by a version it does not recognise.

It used to be opt-in, on two arguments. One was cost: it asked sysfs which ports
existed once a second, forever, and most printers have one display and need
nothing watching anything. That argument is gone — it blocks on a udev socket
now and wakes when the kernel says something changed, which is not a thing a
printer notices. The other was principle, that a daemon should not be implied by
installing a repo, and that one survives as the flag.

What changed on the other side is that the map got more load-bearing than it was
when this was written: reconnecting a display mid-print reads it, and flashing
one with Klipper stopped has no other source.

It exists for the things Klipper structurally cannot do, all of which come from
one fact: **a port can only be read by one program at a time, and Klipper holds
the ones it uses.** So Klipper can never tell you about a port it is not on, and
it can tell you nothing at all when it is not running — which is exactly when a
display is being flashed, because flashing needs the port free.

| | Klipper running | Klipper stopped |
| --- | --- | --- |
| ports Klipper holds | `printer.knomi_cluster.devices` | — |
| every other port | the watcher | the watcher |

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
  without it a display lost mid-print stays dark until the job ends.

## Install

Try it first without installing anything:

```sh
python3 service/knomi_serial_watch.py --once
```

One pass, prints the map, exits — and it needs no pyudev, which is why that
import is deferred. Then:

```sh
./install.sh
```

`install.sh` generates the unit with this machine's paths already in it — where
the repo is, who Klipper runs as, where `printer_data` is — then installs,
enables and starts it.

**`install.sh` decides whether you are *given* one, not whether you are running
one.** A unit that is already there is refreshed either way:

| before | `./install.sh` | `./install.sh --no-watch` |
| --- | --- | --- |
| not installed | installs, enables, starts | nothing |
| installed, running | unit refreshed, restarted | same |
| installed, stopped | unit refreshed, **left stopped** | same |

That second half matters: a service you stopped on purpose stays stopped. A
plain `systemctl restart` would start it again, and `enable` would undo a
deliberate `disable`, which is not an update's business.

**`--no-watch` is remembered**, in `~/printer_data/knomi/.no-watch`:

| | no unit installed | unit installed |
| --- | --- | --- |
| `./install.sh`, no marker | installs, enables, starts | refreshed |
| `./install.sh`, marker present | declines, and says how | refreshed |
| `./install.sh --no-watch` | declines, writes the marker | refreshed |
| `./install.sh --watch` | installs, removes the marker | refreshed |

It has to be remembered, because the notice about a stale install tells you to
run `./install.sh` for reasons that have nothing to do with the watcher. Without
the marker, following that instruction would quietly hand you a daemon you had
turned down. Deleting the file is a valid way to change your mind, and
`--watch` is the tidy way.

There is no prompt anywhere, deliberately, so that running this from something
without a terminal cannot hang waiting for an answer. `--no-root` is there for
the same kind of reason: it skips anything needing root instead of asking for
it, reports what it skipped, and does not record the install as complete.

To remove it:

```sh
sudo systemctl disable --now knomi_serial
sudo rm /etc/systemd/system/knomi_serial.service
sudo systemctl daemon-reload
./install.sh --no-watch
```

That last line is the one people forget. Installing is the default now, so
without the marker the next `./install.sh` puts it straight back.

## Updates, and what an update cannot do

A Moonraker update of a `git_repo` does three things: moves files with git,
installs the packages named in `system_dependencies`, and restarts the services
named in `managed_services`. There is no post-update hook — `install_script:` is
read as text and scraped for dependency lines, never executed.

Mostly that is enough. The Klipper module is a symlink into the repo, so `git
pull` updates it where it stands. The watcher is launched by `service/run.sh`,
which is also in the repo, so how it starts updates the same way and
`managed_services: knomi_serial` restarts it into the new version. Everything
that might need to change is deliberately on that side of the line.

What an update cannot touch is `/etc/systemd/system/knomi_serial.service`. It
needs root, so git cannot write it, and only `install.sh` does. The same goes
for the `[update_manager]` section itself. So `install.sh` stamps a number into
`~/printer_data/knomi/.install-version` when it finishes, and both Klipper and
the watcher compare it against `scripts/install-version` in the checkout:

- Klipper says so in the console at startup — not a config error, because a
  stale install is "please run this", not a reason to refuse to start a printer.
- The watcher says so as its first line in `journalctl -u knomi_serial`, for the
  printer where Klipper is not running.

A run that could not finish — no root available, a package it could not install
— does not write the stamp. Which means the notice keeps appearing until someone
runs it properly, rather than a half-done install quietly claiming to be current.

## The file

`~/printer_data/knomi/devices.json`, written atomically through a temporary file
so a reader can never catch it half-written:

```json
{
  "version": 1,
  "devices": {
    "19aa44": {
      "port": "/dev/ttyUSB0",
      "fw": "0.5.0+54.g5509d4f",
      "var": "knomi"
    }
  }
}
```

**No timestamps**, deliberately. The file's mtime already says when it last
changed, and it is only written when something actually did. An entry existing
means the display is present — identified during this run, and its port has not
disappeared since — so a "last seen" would never change what a reader does with
it.

Nor can a timestamp tell you this service is running: it only moves when a
display is plugged or unplugged, so a map untouched for a fortnight is the
normal state of a printer nobody has been fiddling with. For that,
`systemctl is-active knomi_serial`. Anything else means writing to the card on a
timer to prove a liveness systemd already knows.

`version` is checked, not assumed: a reader that does not recognise it ignores
the file rather than guessing at its shape.

## For a firmware updater

Take Klipper's answer when there is one, and this file only when there is not:

1. **Klipper up** — `printer.knomi_cluster.devices`, keyed by section name, with
   `device_id`, `port`, `build_variant`, `firmware_version` and `online`. This
   is live rather than last-observed, it is the only thing that can see the
   ports Klipper holds, and it is the only place the config's section names
   exist at all. Nothing here knows what `T0_knomi` means.
2. **Klipper down** — this file, after `systemctl is-active knomi_serial`. If
   the service is not running the file is whatever was true when it stopped,
   and no field in it will tell you that.
3. **Neither** — scan, the way `scripts/discover.py` does. Nothing is running
   that could be holding a port, so it is free to open all of them.

Flashing needs case 2 specifically: esptool wants the port to itself, so Klipper
has to be stopped, which is exactly when its mapping disappears. That is the
whole reason this service exists.

Worth gating on whether the user has these displays configured at all, so a
printer with none pays nothing for the check.

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

It blocks on udev's netlink socket, and the kernel wakes it when a tty appears
or goes away. Nothing at all happens in between — no timer, no wakeup, no sysfs
walk on a loop to answer "same as last time" once a second.

The one thing left on a clock is the backoff below: nothing sends an event when
a two-minute wait is up, so the block has a timeout set to whenever that is, and
running out of it means the same thing as an event — take a fresh snapshot.

When a port **appears**, and it has not been identified *during this run*, it
listens for a few seconds and records whatever announces itself. When a port
**disappears**, its entry is dropped, because a path that cannot be opened is
worse than no path at all.

"During this run" rather than "is in the file" is deliberate, and it is the
difference between the map being useful and being confidently wrong. Loading a
map and treating it as settled means never re-examining a port — so shut the
machine down, swap two leads, boot, and both ports are present, both are named,
and nothing ever looks again. Which is exactly the case this exists to cover.
Loaded entries are hints until this run has confirmed them; identifying a port
also evicts whatever used to be recorded against it.

Nothing garbage-collects the file while the service is not running. It does not
need to: every entry is a hint, Klipper confirms each one from the display's
first report, and a wrong entry costs a reconnect.

It never holds a port. It takes the same `flock` that Klipper's sections and
`esptool` take, so a port being driven or flashed is skipped rather than
disturbed, and it listens only long enough to read one report.

Ports that answer nothing are left alone for two minutes — most likely something
on the printer that is not a display, and re-opening a stranger's serial port
every time anything on the machine is plugged in would be rude and pointless.
Ports that were busy are
retried sooner, at thirty seconds, since they will free up eventually and their
identity is worth having when they do.
