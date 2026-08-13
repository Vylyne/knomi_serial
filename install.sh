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

# ---------------------------------------------------------------------------
# Moonraker.
#
# Two separate things, and naming one of them wrong fails as a warning in
# moonraker.log rather than as anything visible:
#
#  * moonraker.asvc is an allowlist. Moonraker refuses to start or stop a
#    service that is not in it, and says so only in its log.
#  * managed_services may name the [update_manager <name>] section itself,
#    `klipper`, or `moonraker` - and nothing else. See _configure_managed_services
#    in moonraker/components/update_manager/app_deploy.py, where the comparison
#    is a case-sensitive string match against the section name. So the section
#    has to be called exactly what the systemd unit is called, or Moonraker
#    cannot be asked to restart it.
#
# The asvc line is appended here because it is one word on its own line and
# trivially undone. moonraker.conf is only ever appended to when there is no
# section at all - an existing one is reported, never rewritten. Renaming a
# section changes what the update panel shows, and that is not a decision to
# make silently in somebody's live printer config.
# ---------------------------------------------------------------------------

SERVICE_NAME="knomi_serial"
PRINTER_DATA="${PRINTER_DATA:-$HOME/printer_data}"
ASVC="$PRINTER_DATA/moonraker.asvc"
MOONRAKER_CONF="$PRINTER_DATA/config/moonraker.conf"

echo "Moonraker:"

if [ ! -f "$ASVC" ]; then
    echo "  no $ASVC - skipping (Moonraker not installed here?)"
elif grep -qx "$SERVICE_NAME" "$ASVC"; then
    echo "  moonraker.asvc already allows $SERVICE_NAME"
else
    printf '%s\n' "$SERVICE_NAME" >> "$ASVC"
    echo "  added $SERVICE_NAME to moonraker.asvc"
fi

if [ ! -f "$MOONRAKER_CONF" ]; then
    echo "  no $MOONRAKER_CONF - skipping"
else
    SECTION_LINE="$(grep -in '^\[update_manager .*knomi' "$MOONRAKER_CONF" | head -1 || true)"
    if [ -z "$SECTION_LINE" ]; then
        cat >> "$MOONRAKER_CONF" <<CONF

[update_manager $SERVICE_NAME]
type: git_repo
origin: https://github.com/Vylyne/knomi_serial.git
path: $REPO
primary_branch: main
managed_services: klipper $SERVICE_NAME
CONF
        echo "  added [update_manager $SERVICE_NAME] to moonraker.conf"
        echo "  restart Moonraker to pick it up"
    else
        FOUND="$(printf '%s' "$SECTION_LINE" | sed 's/^[0-9]*:\[update_manager *//; s/\].*$//')"
        echo "  [update_manager $FOUND] already present, left alone"
        if [ "$FOUND" != "$SERVICE_NAME" ]; then
            echo
            echo "  NOTE: that section is named '$FOUND', but the service is"
            echo "  '$SERVICE_NAME'. Moonraker only accepts a managed_services"
            echo "  value equal to the section name, 'klipper' or 'moonraker',"
            echo "  so it cannot be asked to restart the watcher as things are."
            echo "  To let it, rename the section and list the service:"
            echo
            echo "    [update_manager $SERVICE_NAME]"
            echo "    managed_services: klipper $SERVICE_NAME"
            echo
            echo "  Harmless to leave as is if you do not run the watcher."
        fi
    fi
fi
echo
