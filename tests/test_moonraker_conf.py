#!/usr/bin/env python3
"""Repairing one [update_manager] section without disturbing the file it is in.

This is the only thing in the repo that edits a file somebody else owns, live,
on a machine that is probably printing. So the tests worth having are less about
the repair working and more about its blast radius: the section next door keeps
its own `path`, a fork's `origin` survives, comments stay where they were, and
running it twice changes nothing the second time.

    python tests/test_moonraker_conf.py       # or: pytest tests/
"""

import os
import subprocess
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_AWK = os.path.join(_ROOT, "scripts", "moonraker_section.awk")

#: What install.sh passes on a normal run. Individual tests override one at a
#: time, so what a test is about is the argument it names.
_DEFAULTS = {
    "section": "knomi_serial",
    "path": "/home/pi/knomi_serial",
    "sysdeps": "scripts/moonraker-system-dependencies.json",
    "services": "klipper knomi_serial",
    "branch": "main",
    "home": "/home/pi",
}


def check(label, got, want):
    if got != want:
        raise AssertionError(f"{label}:\n  got  {got!r}\n  want {want!r}")


def repair(text, **overrides):
    """Run the awk over a config, returning (stdout, stderr)."""
    args = dict(_DEFAULTS, **overrides)
    cmd = ["awk", "-f", _AWK]
    for key, value in args.items():
        cmd += ["-v", f"{key}={value}"]
    done = subprocess.run(cmd, input=text, capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError(f"awk failed: {done.stderr}")
    return done.stdout, done.stderr


#: A section that is already right, given the defaults above. Most tests break
#: one thing in it, so what a test is about is the line it replaces.
SECTION = """\
[update_manager knomi_serial]
type: git_repo
origin: https://github.com/someone/their-fork.git
path: /home/pi/knomi_serial
primary_branch: main
managed_services: klipper knomi_serial
system_dependencies: scripts/moonraker-system-dependencies.json
"""


def test_a_correct_section_is_left_exactly_as_it_was():
    """The common case, and the one that must not churn a config every update."""
    out, err = repair(SECTION)
    check("unchanged", out, SECTION)
    check("said nothing", err, "")


def test_a_tilde_path_is_recognised_as_the_path_it_is():
    """Moonraker expands it, so rewriting it long-form is churn for nothing.

    And churn here is not free: it means a backup file and a Moonraker restart,
    on every run, for a config that was already right.
    """
    out, err = repair(SECTION.replace("/home/pi/knomi_serial", "~/knomi_serial"))
    assert "path: ~/knomi_serial" in out, out
    check("said nothing", err, "")


def test_a_genuinely_wrong_path_is_still_repaired():
    """The expansion must not turn the check off, only make it accurate."""
    out, err = repair(SECTION.replace("/home/pi/knomi_serial", "~/somewhere_else"))
    assert "path: /home/pi/knomi_serial" in out, out
    assert "changed path" in err, err


def test_a_forks_origin_survives():
    """Running a fork is a choice, and the local checkout does not contradict it."""
    out, _ = repair(SECTION)
    assert "origin: https://github.com/someone/their-fork.git" in out, out


def test_the_relocated_dependencies_path_is_repaired():
    """The 03a083a move left every existing config pointing at a file that went."""
    stale = SECTION.replace(
        "scripts/moonraker-system-dependencies.json",
        "moonraker-system-dependencies.json")
    out, err = repair(stale)
    assert "system_dependencies: scripts/moonraker-system-dependencies.json" in out
    assert "changed system_dependencies" in err, err


def test_the_branch_follows_the_checkout():
    """git checkout <branch> && ./install.sh is meant to be the whole flip."""
    out, _ = repair(SECTION, branch="dev")
    assert "primary_branch: dev" in out, out


def test_no_branch_leaves_the_branch_alone():
    """A detached HEAD has no answer, and `primary_branch: HEAD` is worse than none.

    Moonraker's recover() does checkout(primary_branch) then a hard reset, so a
    made-up value there is somebody's working tree.
    """
    out, err = repair(SECTION, branch="")
    assert "primary_branch: main" in out, out
    check("said nothing about it", "primary_branch" in err, False)


def test_a_missing_key_is_added_inside_the_section():
    """Not at the end of the file, which is inside whatever section came last."""
    without = SECTION.replace(
        "system_dependencies: scripts/moonraker-system-dependencies.json\n", "")
    text = without + "\n[update_manager mainsail]\ntype: web\n"
    out, _ = repair(text)
    lines = out.splitlines()
    added = lines.index(
        "system_dependencies: scripts/moonraker-system-dependencies.json")
    check("before the next section", added < lines.index("[update_manager mainsail]"),
          True)


def test_a_neighbouring_section_keeps_its_own_path():
    """The whole reason this is a parser and not a sed one-liner."""
    text = (
        "[update_manager mainsail]\n"
        "type: web\n"
        "path: ~/mainsail\n"
        "\n"
        + SECTION +
        "\n[update_manager crowsnest]\n"
        "type: git_repo\n"
        "path: ~/crowsnest\n"
        "primary_branch: master\n"
    )
    out, _ = repair(text)
    assert "path: ~/mainsail" in out, out
    assert "path: ~/crowsnest" in out, out
    assert "primary_branch: master" in out, out


def test_comments_and_unknown_keys_survive_in_place():
    """An installer that eats somebody's notes is not one they will run twice."""
    text = SECTION.replace(
        "primary_branch: main\n",
        "# pinned deliberately, see the forum thread\n"
        "primary_branch: main\n"
        "refresh_interval: 24\n"
        "info_tags:\n"
        "    desc=Knomi Serial\n")
    out, _ = repair(text)
    assert "# pinned deliberately, see the forum thread" in out, out
    assert "refresh_interval: 24" in out, out
    assert "    desc=Knomi Serial" in out, out


def test_a_replaced_key_takes_its_continuation_lines_with_it():
    """Otherwise the orphan is read as more of whatever we just wrote."""
    text = SECTION.replace(
        "managed_services: klipper knomi_serial\n",
        "managed_services: klipper\n    something_else\n")
    out, _ = repair(text)
    check("no orphan", "something_else" in out, False)
    assert "managed_services: klipper knomi_serial" in out, out


def test_a_differently_named_section_is_not_told_to_manage_our_service():
    """Moonraker only accepts the section's own name, klipper, or moonraker."""
    text = SECTION.replace("[update_manager knomi_serial]", "[update_manager knomi]")
    out, _ = repair(text, section="knomi", services="klipper")
    assert "managed_services: klipper\n" in out, out
    check("not ours", "klipper knomi_serial" in out, False)


def test_a_type_that_says_something_else_is_reported_not_overwritten():
    """Not a typo. Somebody meant it, and an installer does not know better."""
    text = SECTION.replace("type: git_repo", "type: zip")
    out, err = repair(text)
    assert "type: zip" in out, out
    assert "kept type" in err, err


def test_running_it_twice_changes_nothing_the_second_time():
    """Idempotence is what lets install.sh run on every update without churn.

    Starting from a section that genuinely needs repairing, so the first pass
    has something to do and the second has to decide not to redo it.
    """
    broken = SECTION.replace("/home/pi/knomi_serial", "/wrong/place")
    once, first = repair(broken)
    assert first, "the first pass should have had something to fix"
    twice, err = repair(once)
    check("stable", twice, once)
    check("said nothing", err, "")


def main():
    tests = [(n, f) for n, f in sorted(globals().items())
             if n.startswith("test_") and callable(f)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f"  ok    {name}")
        except Exception as e:
            failed += 1
            print(f"  FAIL  {name}\n          {type(e).__name__}: {e}")
    print(f"\n  {len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
