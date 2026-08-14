#!/bin/sh
# What the systemd unit runs. Everything about *how* the watcher is launched
# lives here rather than in the unit, and that is the whole point of the file:
# a Moonraker update moves this with `git pull`, while the unit sits in
# /etc/systemd/system where only root can rewrite it and only install.sh does.
# So anything that might need to change belongs on this side of the line.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"

# Checked before starting python so that a machine missing a dependency says so
# in one line, rather than printing an import traceback every five seconds for
# as long as Restart=always keeps trying. Still fails - a persistent failure is
# the honest signal - but the first line of it is actionable.
for want in serial:python3-serial pyudev:python3-pyudev; do
    mod="${want%%:*}"
    pkg="${want##*:}"
    if ! python3 -c "import $mod" >/dev/null 2>&1; then
        echo "knomi_serial: python3 cannot import $mod." >&2
        echo "  sudo apt install $pkg" >&2
        exit 1
    fi
done

# The same comparison Klipper makes and posts to the console, repeated here for
# the printer where Klipper is not running - which is exactly the case this
# service exists to cover. Said first, so it is at the top of `journalctl -u`
# rather than somewhere in the middle of it.
#
# Only when the checkout is *ahead*: an install stamped higher than the repo
# means somebody rolled the checkout back, and telling them to re-run install.sh
# is not the useful thing to say about that.
want="$(cat "$REPO/scripts/install-version" 2>/dev/null || echo 0)"
have="$(cat "$HOME/printer_data/knomi/.install-version" 2>/dev/null || echo 0)"
case "$want$have" in
    *[!0-9]*) ;;                        # unreadable either side, say nothing
    *) if [ "$have" -lt "$want" ]; then
           echo "knomi_serial: installed by an older version of install.sh." >&2
           echo "  cd $REPO && ./install.sh" >&2
       fi ;;
esac

# exec, so systemd's MainPID is python rather than this shell. Without it a
# stop would signal the shell and wait for the timeout to kill what it started.
# No arguments, deliberately: an ExecStart that passes flags is an ExecStart
# that can be left holding a flag a newer script no longer accepts, and this
# file is the one part of the launch that install.sh does not have to refresh.
exec python3 "$REPO/service/knomi_serial_watch.py"
