# Repair the functional keys of one [update_manager] section, and touch nothing
# else in the file.
#
#   awk -f scripts/moonraker_section.awk \
#       -v section=knomi_serial -v path=/home/pi/knomi_serial \
#       -v sysdeps=scripts/moonraker-system-dependencies.json \
#       -v services="klipper knomi_serial" -v branch=main \
#       < moonraker.conf > moonraker.conf.new
#
# Standalone rather than inlined in install.sh because it edits somebody's live
# printer config, and a bash function buried in an installer cannot be tested.
# tests/test_moonraker_conf.py runs this directly.
#
# POSIX awk only - Debian ships mawk, which has no gensub().
#
# What it will not do is as much the point as what it will. `origin` is left
# alone, because running a fork is a legitimate thing and nothing about the
# local checkout contradicts it. Unknown keys, comments, blank lines, ordering
# and indentation all survive byte for byte. Only the keys that decide whether
# Moonraker can manage this repo at all are rewritten, and each one that changes
# is reported on stderr for install.sh to print.
#
# An empty -v means "no opinion, leave whatever is there": install.sh passes
# branch="" on a detached HEAD, where the honest answer is that there is no
# branch to name. Writing primary_branch: HEAD would be worse than writing
# nothing, because Moonraker's recover() checks out primary_branch and hard
# resets onto it.

function want(key) {
    if (key == "path")                 return path
    if (key == "system_dependencies")  return sysdeps
    if (key == "managed_services")     return services
    if (key == "primary_branch")       return branch
    if (key == "type")                 return "git_repo"
    return ""
}

# The key on a `key: value` or `key = value` line, or "" for anything else -
# comments, blank lines, and the indented continuation lines configparser folds
# into the value above them.
function keyof(line,   k) {
    if (line ~ /^[ \t]/) return ""
    if (line !~ /^[A-Za-z_][A-Za-z0-9_]*[ \t]*[:=]/) return ""
    k = line
    sub(/[ \t]*[:=].*$/, "", k)
    return k
}

function valueof(line,   v) {
    v = line
    sub(/^[^:=]*[:=][ \t]*/, "", v)
    return trim(v)
}

# Trailing whitespace, and the carriage return a config edited on Windows
# carries. Only ever used for comparing - what gets printed is the original.
function trim(s) {
    sub(/[ \t\r]+$/, "", s)
    return s
}

function emit(key, value) {
    print key ": " value
}

# `~/knomi_serial` and `/home/pi/knomi_serial` are the same directory, and
# Moonraker expands the first. Compared expanded so that a config written the
# short way is not rewritten the long way on every run, which would mean a
# backup file and a Moonraker restart for no change at all.
function same(key, value) {
    if (key == "path" && home != "" && substr(value, 1, 2) == "~/")
        value = home substr(value, 2)
    return value == want(key)
}

# Everything owned that never appeared, appended at the end of the section
# rather than at the end of the file - which is where a naive `>>` would put it,
# inside whichever section happens to come last.
function flush(   i, n, k) {
    n = split(keys, k, " ")
    for (i = 1; i <= n; i++) {
        if (!(k[i] in seen) && want(k[i]) != "") {
            emit(k[i], want(k[i]))
            print "added " k[i] ": " want(k[i]) > "/dev/stderr"
        }
    }
}

BEGIN {
    keys = "type path primary_branch managed_services system_dependencies"
    header = "[update_manager " section "]"
    inside = 0
    done = 0
}

# A section header. Leaving ours means flushing what was missing from it first.
/^\[/ {
    if (inside) { flush(); inside = 0 }
    if (!done && trim($0) == header) { inside = 1; done = 1 }
    skipping = 0
    print
    next
}

# Outside our section nothing is examined, let alone changed. This is the
# property that matters most: `sed -i s/^path:/.../` would rewrite the path of
# every other [update_manager] section in the file.
!inside { print; next }

{
    key = keyof($0)
    if (key == "" || want(key) == "") {
        # A continuation line belonging to a key we replaced goes with it.
        # configparser reads an indented line as more of the value above, so
        # leaving one orphaned would corrupt the value we just wrote. Anything
        # not indented ends the value above it, and with it the skipping.
        if ($0 ~ /^[ \t]/) {
            if (!skipping) print
        } else {
            skipping = 0
            print
        }
        next
    }
    skipping = 0
    seen[key] = 1

    if (key == "type" && valueof($0) != "git_repo") {
        # Not a typo - somebody meant something by it. Say so and move on.
        print "kept type: " valueof($0) " (expected git_repo)" > "/dev/stderr"
        print
        next
    }
    if (same(key, valueof($0))) { print; next }

    print "changed " key ": " valueof($0) " -> " want(key) > "/dev/stderr"
    emit(key, want(key))
    skipping = 1
}

END {
    if (inside) flush()
}
