# Button and page verification

This is the acceptance checklist for the button inputs and Home/Move pages.
It separates software gates from observations on real hardware. An unchecked
item is not evidence of a defect; it is work that has not been demonstrated.
Update the [README feature/TODO status](../README.md#features) when live
evidence changes. The commands below use the existing configuration and
protocol described in the README; they do not introduce a new API.

## Evidence to date

- Host tests, Ruff, and the `knomi` PlatformIO build passed on the merged
  feature commit `3e5b4d1` (2026-09-30).
- Home and Move captures, plus a script-driven shared-button Move capture, were
  inspected from COM5; Home and Move touch controls were checked on the panel.
  See the [Home](img/home.png), [Move](img/move.png), and
  [shared-button Move](img/move-shared.png) images. The script-driven capture
  does not prove a physical button interaction.
- The unarmed [E-stop page](img/estop.png) was captured on COM5 after a manual
  upward swipe. This does not verify the return gesture or either stop tap.
- The revised Home palette was captured on COM5 [before homing](img/home-unhomed.png),
  [after homing](img/home.png), with [QGL applied](img/home-applied.png), and
  with a [red machine accent](img/home-red.png). These script-driven visual
  captures do not prove Klipper's applied transition or re-verify touch actions.
- The [shutdown screen](img/shutdown.png) was captured with the accent RESTART
  button and contrasting label. Direct captures intermittently omitted static
  labels at short settle times; the linked 30-second-settle capture is complete.
  No live Klipper restart was exercised.
- Wired GPIO switches were exercised on COM5 on 2026-10-19/20 with
  `scripts/simulate.py --button`, against display `19aa44` running
  `0.6.0+20.g69744c3.dirty` (`69744c3` plus the uncommitted board-variant
  change). Four active-low switches on GPIO9/8/6/5 were bound to NW/SW/NE/SE,
  and GPIO47 and GPIO38, with no switch, were bound slotless as controls. All
  six pins carry external 4.7 kΩ pull-ups to U6's 3.3 V, so idle HIGH on this
  rig comes from those resistors, not from the firmware's internal pull-up. Every actuation produced exactly one press and one
  release, matching the operator's count, including a tap shorter than the
  simulator's 0.1 s read loop. The controls never fired, the corner legends
  showed, and tapping a claimed corner on the glass did nothing. After the
  config was replaced without SE (CRC `BD675C3A` to `695472C1`), the SE switch
  produced nothing and the SE corner's touch action (`UNLOAD_FILAMENT`) came
  back. A switch held through a config replacement (GPIO9 rebound from NW to SE,
  CRC `7387E5F2`) produced no release or stale edge from the old config. The
  new binding reported a press for the already-held switch under the new CRC;
  the operator accepts that as intended.
- Still not shown for wired GPIO: the 20 ms debounce threshold itself rather than ordinary actuations, the
  internal pull-up being applied and later removed (the external resistors mask
  both), and a macro run by Klipper rather than an edge reported by the
  simulator.
- Live shared-button routing, swipes, and end-to-end command counts have not
  been verified. A successful build is not a switch test, and a touch hit is not
  proof that Klipper ran exactly one command.

## Software gate for changes

Run all `tests/test_*.py`, `ruff check .`, and `pio run -e knomi` as specified
in [AGENTS.md](../AGENTS.md#verification). Include the protocol parity tests
when changing button records, indexes, or frame fields. Review
`git diff --check`. Record host-test and firmware-build results separately
from any hardware result; do not check off live behavior because a build passed.

## Live acceptance checks

Use a safe Klipper simulator or an idle machine with motion hazards controlled
before testing Home, Move, QGL, or ZTA. Do not infer command success from a
screen animation alone. Note firmware/module versions, active config CRC,
display ID, and the action observed for each run.

- [ ] **Pages and swipes:** With normal page order, swipe into and out of Home
  and Move, including the E-stop vertical route. Check that a swipe does not
  commit the touched control or fire a jog/home command.
- [ ] **Home/Move command count:** Exercise every available Home and Move
  target against a safe command sink or simulator. Confirm exactly one intended
  command per committed tap, and none for a canceled or press-lost gesture.
  Keep the existing busy/disabled guards in force.
- [ ] **Tramming applied state:** With a live Klipper QGL or Z-tilt object,
  confirm the Home tramming mark gains the accent only after `applied` becomes
  true, and returns to neutral when Klipper clears it (including motor-off).
- [ ] **Shutdown restart:** Induce a safe Klipper shutdown with the process and
  serial link still running. Confirm the shutdown reason appears, RESTART
  requests `firmware_restart` exactly once, and the display leaves shutdown
  after recovery. A `--drive shutdown` screenshot proves appearance only.
- [ ] **Shared positions:** Test visible FEED/RETRACT or another shared binding
  at NE and SE. Confirm its legend/action owns the slot, a displaced Home action
  remains reachable as touch-only, and both Move Z controls remain on the
  right edge with usable, non-overlapping targets. A slotless observed input
  must leave the page touch position available. Verify the overlay is hidden
  on E-stop without disabling an external input.
- [ ] **Host-forwarded event:** Forward press and release with `KNOMI_BUTTON`
  to a harmless test macro/observer. Confirm bare macro executes once on a
  committed release, configured edge macros execute only on their edges, and
  duplicate press or unmatched release executes nothing. Test multi-display
  addressing and invalid targets without partial writes.
- [ ] **Config replacement:** Hold a touch, GPIO, or forwarded event while the
  button config changes. Confirm cancel does not fire a release macro, a stale
  CRC/index cannot activate a new binding, and a queued old edge is dropped.
  Check the device adopts the new CRC before judging the new binding.
- [ ] **Wired GPIO:** On a supported pin from [hardware.md](hardware.md#for-the-four-corner-keys),
  wire an active-low switch to Knomi ground. Confirm pull-up idle HIGH, one
  press after 20 ms stable LOW, one release after 20 ms stable HIGH, and no
  action from contact bounce. Check touch suppression for a claimed slot and
  removal of the old pull-up/binding after config replacement. Repeat with a
  harmless macro or observe profile to check one event per lifecycle.

Direct screenshot or flashing tools must not contend with Klipper or the
watcher for the port. Stop the owning service before opening it directly, and
reconfirm the display ID before a write; see [mcu-updater.md](mcu-updater.md)
and [AGENTS.md](../AGENTS.md#project-invariants).
