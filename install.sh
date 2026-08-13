#!/bin/bash
set -e

# Resolved rather than assembled from $(pwd), so this works when run by path
# from somewhere else - `bash ~/knomi_serial/install.sh` used to produce a
# nonsense link target built from the current directory and an absolute one.
REPO="$(cd "$(dirname "$0")" && pwd)"

# The systemd unit name. Also the [update_manager] section name it must match -
# see the Moonraker block below for why those two are not independent.
SERVICE_NAME="knomi_serial"

EXTRA_PATH="$HOME/klipper/klippy/extras/knomi_serial.py"

echo "Creating symbolic link to klippy_extras/knomi_serial.py at $EXTRA_PATH"
ln -sf "$REPO/klippy_extras/knomi_serial.py" "$EXTRA_PATH"

# if ! grep -q "klippy/extras/knomi_serial.py" "$HOME/klipper/.git/info/exclude"; then
#   echo "klippy/extras/knomi_serial.py" >> "$HOME/klipper/.git/info/exclude"
# fi

# ---------------------------------------------------------------------------
# The watcher service.
#
# The unit is generated rather than shipped because the paths in it are this
# machine's - where the repo is, who Klipper runs as, which python. Hardcoding
# those was wrong and the failure was quiet: systemd's ProtectHome alongside a
# ReadWritePaths that does not exist starts cleanly and then cannot write, which
# reads as "the watcher does nothing" rather than as a path problem.
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

# Installed if you already have it, or if you ask for it - never decided here.
#
# Unlike a Klipper module, a background daemon is not implied by installing
# this repo. Most printers have one display, address it by device_id, and need
# nothing watching anything. So the rule is that install.sh keeps the service up
# to date, and does not decide to give you one: already installed means the unit
# is refreshed and the service restarted, which is what makes this safe to run
# from Moonraker's update manager after every pull.
#
# No prompt, deliberately. install_script runs non-interactively under the
# update manager, and a read here would hang an update rather than ask anybody
# anything.
INSTALLED_UNIT="/etc/systemd/system/${SERVICE_NAME}.service"
if [ "${1:-}" = "--watch" ] || [ -f "$INSTALLED_UNIT" ]; then
    # Whether it is running now decides whether it is running afterwards.
    # `systemctl restart` would start a service somebody had deliberately
    # stopped, and `enable` would re-enable one they had deliberately disabled -
    # an update is not the place to overrule either of those.
    FIRST_INSTALL=no
    [ -f "$INSTALLED_UNIT" ] || FIRST_INSTALL=yes

    WAS_ACTIVE=no
    if systemctl is-active --quiet "${SERVICE_NAME}.service" 2>/dev/null; then
        WAS_ACTIVE=yes
        sudo systemctl stop "${SERVICE_NAME}.service"
    fi

    if [ "$FIRST_INSTALL" = yes ]; then
        echo "Installing the $SERVICE_NAME service."
    else
        echo "Updating the $SERVICE_NAME service."
    fi

    sudo install -m 0644 -o root -g root "$UNIT" "$INSTALLED_UNIT"
    sudo systemctl daemon-reload

    if [ "$FIRST_INSTALL" = yes ]; then
        # Enabled only on the run that asked for the service. A later update
        # must not re-enable one that was switched off in between.
        sudo systemctl enable "${SERVICE_NAME}.service" >/dev/null 2>&1 || true
        WAS_ACTIVE=yes
    fi

    if [ "$WAS_ACTIVE" = yes ]; then
        sudo systemctl start "${SERVICE_NAME}.service"
        echo "  $(systemctl is-active "${SERVICE_NAME}.service" || true) - systemctl status $SERVICE_NAME"
    else
        echo "  unit refreshed; service was not running, so it was left stopped."
        echo "  start it with: sudo systemctl start $SERVICE_NAME"
    fi
else
    echo "The watcher is optional - see service/README.md for what it buys you."
    echo "Try it without installing anything:"
    echo
    echo "  python3 $REPO/service/knomi_serial_watch.py --once"
    echo
    echo "To install it as a service, re-run with --watch:"
    echo
    echo "  ./install.sh --watch"
fi
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

PRINTER_DATA="${PRINTER_DATA:-$HOME/printer_data}"
ASVC="$PRINTER_DATA/moonraker.asvc"
MOONRAKER_CONF="$PRINTER_DATA/config/moonraker.conf"

echo "Moonraker:"
MOONRAKER_CHANGED=no

if [ ! -f "$ASVC" ]; then
    echo "  no $ASVC - skipping (Moonraker not installed here?)"
elif grep -qx "$SERVICE_NAME" "$ASVC"; then
    echo "  moonraker.asvc already allows $SERVICE_NAME"
else
    printf '%s\n' "$SERVICE_NAME" >> "$ASVC"
    echo "  added $SERVICE_NAME to moonraker.asvc"
    MOONRAKER_CHANGED=yes
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
        MOONRAKER_CHANGED=yes
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

# Neither file is re-read while Moonraker runs, and moonraker.asvc is the one
# that bites: _init_allowed_services() loads it once in Machine.__init__ and
# caches the result, so a service appended to it stays forbidden until a
# restart - and stays forbidden silently, since the refusal is a line in
# moonraker.log.
if [ "$MOONRAKER_CHANGED" = yes ]; then
    # Read from [server] rather than assumed - the port is configurable.
    PORT="$(awk '/^\[server\]/{s=1;next} /^\[/{s=0} s && /^[[:space:]]*port:/{gsub(/[^0-9]/,"",$0); print; exit}' "$MOONRAKER_CONF" 2>/dev/null || true)"
    PORT="${PORT:-7125}"
    if [ ! -t 0 ]; then
        # No tty almost certainly means this is Moonraker's own install_script.
        # Restarting the process that is running this script would kill the
        # update halfway through, so say what is needed and let it finish.
        echo
        echo "  Moonraker has to restart to read that. Once the update finishes:"
        echo "    sudo systemctl restart moonraker"
    elif ! command -v curl >/dev/null 2>&1; then
        echo "  restart Moonraker to pick that up: sudo systemctl restart moonraker"
    elif curl -fsS -m 5 -X POST "http://localhost:$PORT/server/restart" >/dev/null 2>&1; then
        echo "  asked Moonraker to restart, so it picks that up"
    else
        echo "  could not reach Moonraker on port $PORT - restart it yourself:"
        echo "    sudo systemctl restart moonraker"
    fi
fi
echo
