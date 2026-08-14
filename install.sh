#!/bin/bash
set -e

# Resolved rather than assembled from $(pwd), so this works when run by path
# from somewhere else - `bash ~/knomi_serial/install.sh` used to produce a
# nonsense link target built from the current directory and an absolute one.
REPO="$(cd "$(dirname "$0")" && pwd)"

# The systemd unit name. Also the [update_manager] section name it must match -
# see the Moonraker block below for why those two are not independent.
SERVICE_NAME="knomi_serial"

DATA="$HOME/printer_data/knomi"
PRINTER_DATA="${PRINTER_DATA:-$HOME/printer_data}"
INSTALLED_UNIT="/etc/systemd/system/${SERVICE_NAME}.service"
UNIT="$REPO/service/${SERVICE_NAME}.service"
NO_WATCH_MARKER="$DATA/.no-watch"
STAMP="$DATA/.install-version"

# ---------------------------------------------------------------------------
# Flags.
# ---------------------------------------------------------------------------

WATCH=no
NO_WATCH=no
NO_ROOT=no

while [ $# -gt 0 ]; do
    case "$1" in
        --watch)    WATCH=yes ;;
        --no-watch) NO_WATCH=yes ;;
        --no-root)  NO_ROOT=yes ;;
        -h|--help)
            echo "usage: ./install.sh [--watch|--no-watch] [--no-root]"
            echo
            echo "  --watch     install the watcher service, undoing a previous"
            echo "              --no-watch. It is installed by default."
            echo "  --no-watch  do not install it, and remember that."
            echo "  --no-root   skip anything needing root rather than asking"
            echo "              for it. Whatever was skipped is reported, and"
            echo "              the install is not recorded as complete."
            exit 0 ;;
        *)
            echo "unknown option: $1" >&2
            echo "try: ./install.sh --help" >&2
            exit 2 ;;
    esac
    shift
done

if [ "$WATCH" = yes ] && [ "$NO_WATCH" = yes ]; then
    echo "--watch and --no-watch mean opposite things; pick one." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Privilege, decided once.
#
# Resolved into a variable here so that every privileged line below can just say
# `$SUDO systemctl ...` with no conditional wrapped around it. --no-root is
# simply the case where that variable is empty and NEED_ROOT is set, which is
# why it is worth a flag rather than a check per command.
#
# `sudo -n` when there is no terminal: a password prompt nobody can answer is
# a hang, and this script is run from places with no tty.
# ---------------------------------------------------------------------------

SUDO=""
CAN_ROOT=yes
if [ "$NO_ROOT" = yes ]; then
    CAN_ROOT=no
elif [ "$(id -u)" = 0 ]; then
    SUDO=""
elif command -v sudo >/dev/null 2>&1; then
    if [ -t 0 ]; then
        SUDO="sudo"
    else
        # Probed, because this is the case that would otherwise fail in the
        # middle of the script: sudo without a password and without a terminal
        # to ask at. Interactive sudo is not probed - it would prompt for a
        # password on a run that may not need root at all.
        SUDO="sudo -n"
        $SUDO true >/dev/null 2>&1 || CAN_ROOT=no
    fi
else
    CAN_ROOT=no
fi

# Everything install.sh owns is current. Flipped by anything that needed doing
# and could not be done, which is what stops the version stamp being written -
# see the end of this script. A step that was *declined* rather than blocked
# does not flip it: that is a choice being honoured, not a change being lost.
REQUIRED_CHANGES_MADE=yes
SKIPPED=""

skipped() {
    REQUIRED_CHANGES_MADE=no
    SKIPPED="$SKIPPED
  - $1"
}

# ---------------------------------------------------------------------------
# The Klipper module.
# ---------------------------------------------------------------------------

EXTRA_PATH="$HOME/klipper/klippy/extras/knomi_serial.py"

echo "Creating symbolic link to klippy_extras/knomi_serial.py at $EXTRA_PATH"
ln -sf "$REPO/klippy_extras/knomi_serial.py" "$EXTRA_PATH"

mkdir -p "$DATA"

# ---------------------------------------------------------------------------
# Dependencies.
#
# Moonraker installs these itself on every update, from
# scripts/moonraker-system-dependencies.json - it reads that file, it does not
# run this script. This block is for the person running install.sh by hand,
# which is the only path that would otherwise miss them.
# ---------------------------------------------------------------------------

PYTHON="$(command -v python3)"

for want in serial:python3-serial pyudev:python3-pyudev; do
    MOD="${want%%:*}"
    PKG="${want##*:}"
    "$PYTHON" -c "import $MOD" >/dev/null 2>&1 && continue
    echo "$PKG is missing from $PYTHON, which the watcher needs."
    if [ "$CAN_ROOT" = no ]; then
        echo "  not installing it - no root available"
        skipped "install $PKG"
        continue
    fi
    # Said before apt runs, not after. It is the one slow thing in here - a
    # minute is normal, longer if the package lists are stale - and announcing
    # it only on success means a silent terminal for all of that, which reads
    # as a hang rather than as work.
    echo "  installing $PKG, which can take a minute..."
    APT_LOG="$(mktemp)"
    if $SUDO apt-get install -y "$PKG" > "$APT_LOG" 2>&1; then
        echo "  installed $PKG"
    else
        # Shown rather than swallowed. "could not install it" on its own sends
        # people to run the identical command by hand to find out why.
        echo "  could not install it:"
        tail -5 "$APT_LOG" | sed 's/^/    /'
        echo "  the watcher will not start until this works:"
        echo "    sudo apt install $PKG"
        skipped "install $PKG"
    fi
    rm -f "$APT_LOG"
done

# ---------------------------------------------------------------------------
# The watcher service.
#
# The unit is generated rather than shipped because the paths in it are this
# machine's - where the repo is, who Klipper runs as, where printer_data is.
# Hardcoding those was wrong and the failure was quiet: systemd's ProtectHome
# alongside a ReadWritePaths that does not exist starts cleanly and then cannot
# write, which reads as "the watcher does nothing" rather than as a path problem.
#
# What it does *not* contain any more is how the watcher is launched. That lives
# in service/run.sh, because a Moonraker update can update a file in the repo
# and cannot touch a file in /etc.
# ---------------------------------------------------------------------------

sed -e "s|@USER@|$USER|g" \
    -e "s|@REPO@|$REPO|g" \
    -e "s|@DATA@|$DATA|g" \
    "$REPO/service/${SERVICE_NAME}.service.in" > "$UNIT"

echo
echo "Wrote $UNIT for this machine."

# Installed by default. A display row is the normal case now: the map is what
# lets a display be reconnected mid-print and what a firmware updater reads with
# Klipper stopped, and an idle process blocked on a netlink socket is not
# something a printer notices.
#
# --no-watch is how you decline, and it is *remembered*, because the notice
# about a stale install tells people to run ./install.sh for reasons that have
# nothing to do with the watcher. Without the marker, following our own
# instructions would quietly install a daemon somebody had turned down.
#
# A unit that is already there is refreshed either way. --no-watch governs
# whether you are given a service, not whether an existing one is kept current,
# and removing one stays the documented manual thing it was.
#
# No prompt anywhere, so that running this from something without a terminal
# cannot hang waiting for an answer.
WATCHER_INSTALLED=no

if [ "$WATCH" = yes ]; then
    rm -f "$NO_WATCH_MARKER"
fi

if [ "$NO_WATCH" = yes ] && [ ! -f "$INSTALLED_UNIT" ]; then
    printf '%s\n' \
        "# ./install.sh --no-watch, $(date -u '+%Y-%m-%d')." \
        "# Delete this, or run ./install.sh --watch, to install the service." \
        > "$NO_WATCH_MARKER"
    echo "Not installing the $SERVICE_NAME service, and remembering that."
    echo "  ./install.sh --watch installs it later."
elif [ ! -f "$INSTALLED_UNIT" ] && [ -f "$NO_WATCH_MARKER" ]; then
    echo "The $SERVICE_NAME service was declined by a previous --no-watch."
    echo "  ./install.sh --watch installs it."
elif [ "$CAN_ROOT" = no ] && ! cmp -s "$UNIT" "$INSTALLED_UNIT"; then
    # Tested before acting, and the test needs no privileges - the installed
    # unit is world readable. So a run with no root on a machine that is already
    # correct is a complete run, not a partial one.
    echo "The $SERVICE_NAME unit needs (re)installing and this run has no root."
    skipped "install $INSTALLED_UNIT"
elif [ "$NO_WATCH" = yes ]; then
    echo "Refreshing the $SERVICE_NAME service; --no-watch only declines a"
    echo "first install, and this machine already has one."
    WATCHER_INSTALLED=yes
else
    WATCHER_INSTALLED=yes
fi

if [ "$WATCHER_INSTALLED" = yes ]; then
    # Whether it is running now decides whether it is running afterwards.
    # `systemctl restart` would start a service somebody had deliberately
    # stopped, and `enable` would re-enable one they had deliberately disabled -
    # an update is not the place to overrule either of those.
    FIRST_INSTALL=no
    [ -f "$INSTALLED_UNIT" ] || FIRST_INSTALL=yes

    if cmp -s "$UNIT" "$INSTALLED_UNIT"; then
        echo "The $SERVICE_NAME unit is already current."
    else
        WAS_ACTIVE=no
        if systemctl is-active --quiet "${SERVICE_NAME}.service" 2>/dev/null; then
            WAS_ACTIVE=yes
            $SUDO systemctl stop "${SERVICE_NAME}.service"
        fi

        if [ "$FIRST_INSTALL" = yes ]; then
            echo "Installing the $SERVICE_NAME service."
        else
            echo "Updating the $SERVICE_NAME service."
        fi

        $SUDO install -m 0644 -o root -g root "$UNIT" "$INSTALLED_UNIT"
        $SUDO systemctl daemon-reload

        if [ "$FIRST_INSTALL" = yes ]; then
            # Enabled only on the run that first installs it. A later update
            # must not re-enable one that was switched off in between.
            $SUDO systemctl enable "${SERVICE_NAME}.service" >/dev/null 2>&1 || true
            WAS_ACTIVE=yes
        fi

        if [ "$WAS_ACTIVE" = yes ]; then
            $SUDO systemctl start "${SERVICE_NAME}.service"
            echo "  $(systemctl is-active "${SERVICE_NAME}.service" || true) - systemctl status $SERVICE_NAME"
        else
            echo "  unit refreshed; service was not running, so it was left stopped."
            echo "  start it with: sudo systemctl start $SERVICE_NAME"
        fi
    fi
else
    echo "Try the watcher without installing anything:"
    echo
    echo "  python3 $REPO/service/knomi_serial_watch.py --once"
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
# Moonraker does not run this script. `install_script:` is read as text and
# scraped for dependency lines - see _read_system_dependencies in app_deploy.py
# - so an update never reaches here. It does not need to for the code: ExecStart
# points into the repo, so a git pull updates the watcher where it stands and
# `managed_services: knomi_serial` restarts it. It does need to for anything in
# this file, which is why there is a version stamp at the bottom.
#
# The asvc line is appended here because it is one word on its own line and
# trivially undone. An existing [update_manager] section is *repaired* rather
# than rewritten: the keys that decide whether Moonraker can manage this repo at
# all are corrected, and the ones that are somebody's choice - `origin` above
# all, because running a fork is a legitimate thing - are left alone.
# ---------------------------------------------------------------------------

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

# The branch the checkout is actually on, because that is what primary_branch
# has to name: Moonraker builds its upstream ref from it, and recover() does a
# checkout followed by a hard reset. So `git checkout <branch> && ./install.sh`
# is meant to be the whole of switching branches.
#
# Empty on a detached HEAD, where rev-parse answers the literal "HEAD", and on
# anything that is not a checkout at all. The repair reads empty as "no opinion,
# leave what is there" - writing `primary_branch: HEAD` would turn the update
# panel's recover button into somebody's lost working tree.
BRANCH="$(git -C "$REPO" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
if [ "$BRANCH" = HEAD ]; then
    echo "  detached HEAD - leaving primary_branch alone"
    BRANCH=""
elif [ -n "$BRANCH" ] \
     && ! git -C "$REPO" rev-parse --verify -q "refs/remotes/origin/$BRANCH" \
          >/dev/null 2>&1; then
    # Local ref only, so no network call. Moonraker compares against
    # origin/<branch> and calls a repo it cannot find invalid.
    echo "  NOTE: origin/$BRANCH does not exist here. Moonraker will call the"
    echo "  repo invalid until that branch is pushed."
fi

if [ "$WATCHER_INSTALLED" = yes ] || [ -f "$INSTALLED_UNIT" ]; then
    MANAGED="klipper $SERVICE_NAME"
else
    MANAGED="klipper"
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
primary_branch: ${BRANCH:-main}
managed_services: $MANAGED
system_dependencies: scripts/moonraker-system-dependencies.json
CONF
        echo "  added [update_manager $SERVICE_NAME] to moonraker.conf"
        MOONRAKER_CHANGED=yes
    else
        FOUND="$(printf '%s' "$SECTION_LINE" | sed 's/^[0-9]*:\[update_manager *//; s/\].*$//')"

        # managed_services must equal the section's own name, so the value
        # depends on what the section is actually called rather than on what we
        # would have called it.
        if [ "$FOUND" = "$SERVICE_NAME" ]; then
            SECTION_MANAGED="$MANAGED"
        else
            SECTION_MANAGED="klipper"
        fi

        NEW="$(mktemp)"
        REPORT="$(mktemp)"
        if awk -f "$REPO/scripts/moonraker_section.awk" \
               -v section="$FOUND" \
               -v path="$REPO" \
               -v sysdeps="scripts/moonraker-system-dependencies.json" \
               -v services="$SECTION_MANAGED" \
               -v branch="$BRANCH" \
               -v home="$HOME" \
               "$MOONRAKER_CONF" > "$NEW" 2> "$REPORT" \
           && [ -s "$NEW" ]; then
            if cmp -s "$NEW" "$MOONRAKER_CONF"; then
                echo "  [update_manager $FOUND] is already correct"
            else
                cp "$MOONRAKER_CONF" "$MOONRAKER_CONF.knomi.bak"
                # Written through the existing file rather than moved over it,
                # so its owner and mode survive. A `mv` from /tmp would hand
                # Moonraker's config whatever mktemp chose.
                cat "$NEW" > "$MOONRAKER_CONF"
                echo "  repaired [update_manager $FOUND]:"
                sed 's/^/    /' "$REPORT"
                echo "  the previous file is at $MOONRAKER_CONF.knomi.bak"
                MOONRAKER_CHANGED=yes
            fi
        else
            echo "  could not read $MOONRAKER_CONF - left it alone"
            skipped "repair [update_manager $FOUND]"
        fi
        rm -f "$NEW" "$REPORT"

        if grep -q '^install_script:' "$MOONRAKER_CONF"; then
            echo
            echo "  NOTE: this config uses install_script:, which Moonraker"
            echo "  reads for dependencies and this repo no longer declares"
            echo "  there. Replace that line with:"
            echo
            echo "    system_dependencies: scripts/moonraker-system-dependencies.json"
        fi

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
        # No tty almost certainly means this is not a person at a keyboard.
        # Restarting the process that is running this script would kill it
        # halfway through, so say what is needed and let it finish.
        echo
        echo "  Moonraker has to restart to read that. Once this finishes:"
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

# ---------------------------------------------------------------------------
# The stamp, last.
#
# It means "everything install.sh owns is current", not "install.sh ran". A run
# that could not do something it needed to leaves the old number in place, so
# the notice Klipper posts and the watcher logs keeps firing until somebody runs
# it properly. Recording a partial run as complete would silence the one thing
# that would have told them.
# ---------------------------------------------------------------------------

echo
if [ "$REQUIRED_CHANGES_MADE" = yes ]; then
    cat "$REPO/scripts/install-version" > "$STAMP"
    echo "Done."
else
    echo "Not recording this as a complete install. Still to do:$SKIPPED"
    echo
    echo "  sudo ./install.sh"
fi
