# Home and Move Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the inherited rectangular Home and Move controls with round-display layouts consistent with the current Tool and Printing pages.

**Architecture:** Home binds five semantic slots through the normalized input system and renders diagonal legends plus a centre home-all icon. Move remains a page-local touch layout: an XY pad and separate Z rocker. Shared helpers own only reusable control styling and lifecycle plumbing.

**Tech Stack:** C++17, LVGL 9, PlatformIO, serial screenshot capture.

**Spec:** [Button Inputs and Home/Move Pages Design](../specs/2026-09-29-button-inputs-and-pages-design.md)

## Global Constraints

- No direct rectangular-button carryover from the inherited pages.
- Do not collapse machine, filament, and heat colours.
- Keep touch targets materially larger than visible marks.
- Preserve existing G-code and move command behavior.
- Flash/capture only after explicit user authorization.

## Review Focus

- Legibility and target size on the physical 32.4 mm display.
- Diagonal alignment with existing hardware/soft positions.
- No hit-region overlap or unreachable action.
- Homed/busy state changes should invalidate only affected objects.

---

### Task 1: Add reusable round action styling

**Files:**
- Create: `src/ui/action_control.h`
- Create: `src/ui/action_control.cpp`
- Modify: `src/user_conf.h`
- Modify: `src/ui/theme.h`
- Modify: `src/ui/theme.cpp`

- [ ] Add constants for 72-pixel Home targets, 38-pixel marks, 56-pixel Move targets, ring width, disabled opacity, and inner-orbit offsets.
- [ ] Implement a helper that creates a transparent hit target plus neutral scrim/mark, label/icon, enabled opacity, and optional warm state ring.
- [ ] Keep machine accent use restrained and route foreground contrast through existing theme helpers.
- [ ] Build `pio run -e knomi`; expect success.

### Task 2: Rebuild Home on five semantic positions

**Files:**
- Modify: `src/ui/pages/home/home_page.cpp`
- Modify: `src/ui/pages/home/home_page.h`
- Modify: `src/ui/pages/page_helper.h`

- [ ] Remove the `HOME` title and all five inherited rectangular buttons.
- [ ] Render X at NW, Y at NE, home-all icon at C, QGL/ZTA at SW when available, and Z at SE.
- [ ] Register all actions through the page resolver; a shared slot gets the configured legend and the displaced Home action gets an inward touch-only control.
- [ ] Keep `G28`, `G28 X`, `G28 Y`, `G28 Z`, `QUAD_GANTRY_LEVEL`, and `Z_TILT_ADJUST` unchanged.
- [ ] Update only changed homed rings and busy opacity, with impossible-value cache sentinels after rebuild.
- [ ] Build `pio run -e knomi`; expect success.

### Task 3: Build the Move inner orbit

**Files:**
- Modify: `src/ui/pages/move/move_page.cpp`
- Modify: `src/ui/pages/move/move_page.h`

- [ ] Remove the title and six inherited grid buttons.
- [ ] Place a four-direction XY pad left/centre and a two-direction Z rocker on the right, with clear spatial separation.
- [ ] Use arrow/icon direction plus axis identity without shrinking labels below Montserrat 16.
- [ ] Route committed touch releases to the existing six `send_move` direction strings.
- [ ] Handle press-lost/deletion through normalized cancellation so a gesture never becomes a move.
- [ ] Build `pio run -e knomi`; expect success.

### Task 4: Capture and iterate on real glass

**Files:**
- Add: `docs/img/home.png`
- Add: `docs/img/move.png`
- Modify: `README.md`

- [ ] Run the complete host test suite, Ruff, and firmware build before touching hardware.
- [ ] With explicit flash permission, upload the built firmware to COM5.
- [ ] Capture Home with `python scripts/screenshot.py COM5 --drive idle --page home -o docs/img/home.png`.
- [ ] Capture Move with `python scripts/screenshot.py COM5 --drive idle --page move -o docs/img/move.png`.
- [ ] Inspect both captures at native size and on the physical display; adjust overlap, optical centring, contrast, and target geometry, then repeat build/upload/capture.
- [ ] Exercise every Home and Move action against the simulator or a safe Klipper instance and verify one command per committed tap.

### Task 5: Finish documentation and gates

**Files:**
- Modify: `README.md`

- [ ] Add Home and Move captures and describe their control layout in the operator-facing section.
- [ ] Mark the Home/Move TODO complete only after live captures and interaction checks pass.
- [ ] Run every `tests/test_*.py`; expect all pass.
- [ ] Run `ruff check .`; expect clean output.
- [ ] Run `pio run -e knomi`; expect success and record final RAM/flash usage.
- [ ] Run `git diff --check` and review the complete diff for accidental visual or command regressions.
- [ ] If commit authorization is later given, commit with `feat: redesign home and move pages`.
