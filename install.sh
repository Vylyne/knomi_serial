#!/bin/bash
set -e

# Resolved rather than assembled from $(pwd), so this works when run by path
# from somewhere else - `bash ~/knomi_serial/install.sh` used to produce a
# nonsense link target built from the current directory and an absolute one.
REPO="$(cd "$(dirname "$0")" && pwd)"

EXTRA_PATH="$HOME/klipper/klippy/extras/knomi_serial.py"

echo "Creating symbolic link to klippy_extras/knomi_serial.py at $EXTRA_PATH"
ln -sf "$REPO/klippy_extras/knomi_serial.py" "$EXTRA_PATH"

# if ! grep -q "klippy/extras/knomi_serial.py" "$HOME/klipper/.git/info/exclude"; then
#   echo "klippy/extras/knomi_serial.py" >> "$HOME/klipper/.git/info/exclude"
# fi

# ---------------------------------------------------------------------------
# The watcher service, generated but not installed.
#
# Generated because the paths in it are this machine's - where the repo is, who
# Klipper runs as, which python. Hardcoding those was wrong and the failure was
# quiet: systemd's ProtectHome plus a ReadWritePaths that does not exist starts
# and then cannot write, which reads as "the watcher does nothing" rather than as
# a path problem.
#
# Not installed because putting a unit in /etc/systemd/system needs root, and a
# root-owned file that runs a script out of a git repo is a decision to make
# deliberately rather than one an install script should make for you. Same
# reason the udev rules in the README are not copied into place either.
# ---------------------------------------------------------------------------

DATA="$HOME/printer_data/knomi"
PYTHON="$(command -v python3)"
UNIT="$REPO/service/knomi_serial.service"

mkdir -p "$DATA"

sed -e "s|@USER@|$USER|g" \
    -e "s|@PYTHON@|$PYTHON|g" \
    -e "s|@REPO@|$REPO|g" \
    -e "s|@DATA@|$DATA|g" \
    "$REPO/service/knomi_serial.service.in" > "$UNIT"

echo
echo "Wrote $UNIT for this machine."
echo "The watcher is optional - see service/README.md for what it buys you."
echo "Try it without installing anything:"
echo
echo "  python3 $REPO/service/knomi_serial_watch.py --once"
echo
echo "To run it as a service:"
echo
echo "  sudo cp $UNIT /etc/systemd/system/"
echo "  sudo systemctl daemon-reload"
echo "  sudo systemctl enable --now knomi_serial"
echo
