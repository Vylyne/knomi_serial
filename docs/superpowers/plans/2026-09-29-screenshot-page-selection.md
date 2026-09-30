# Screenshot Page Selection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the screenshot tool make any configured idle page the landing page with `--page`, without changing the wire protocol.

**Architecture:** Build the screenshot `DeviceConfig` in a pure helper. When `--page` is supplied, encode only that page in `page_order`; otherwise preserve today's default firmware ordering. Driving an idle preset then naturally lands on the selected page.

**Tech Stack:** Python 3, argparse, existing `knomi_serial` encoder, script-style tests.

**Spec:** [Button Inputs and Home/Move Pages Design](../specs/2026-09-29-button-inputs-and-pages-design.md)

## Global Constraints

- Do not add a screenshot-only frame or page-changing protocol command.
- Keep `--all` output unchanged unless a documentation shot explicitly names a page.
- Test behavior through the config helper and encoded payload, not source-text searches.

## Review Focus

- Omitted `--page` must preserve current behavior.
- Invalid page names must be rejected by argparse.
- Page selection must update the config CRC used by driven state.

---

### Task 1: Add a failing page-selection test

**Files:**
- Create: `tests/test_screenshot.py`
- Modify: `scripts/screenshot.py`

- [ ] Import `scripts/screenshot.py` by path in `tests/test_screenshot.py` and call a new pure `build_config(page=None)` helper.
- [ ] Assert `build_config("home").pages == (_PAGES["home"],)` and the same for `move`.
- [ ] Assert `build_config(None)` does not set `_HAS_PAGE_ORDER`, preserving compiled default order.
- [ ] Run `python tests/test_screenshot.py`; expect failure because `build_config` does not exist.

### Task 2: Implement `--page`

**Files:**
- Modify: `scripts/screenshot.py`

- [ ] Extract today's explicit accent/G-code config into `build_config(page=None)`.
- [ ] If a page is supplied, add `_HAS_PAGE_ORDER` and set `pages=(_PAGES[page],)`.
- [ ] Add `--page` with `choices=sorted(k._PAGES)` and explain that it makes the selected idle page the only configured page and therefore the landing page.
- [ ] Build `config`, payload CRC, and driven state from the helper after argument parsing.
- [ ] Run `python tests/test_screenshot.py`; expect all tests to pass.
- [ ] Run `ruff check scripts/screenshot.py tests/test_screenshot.py`; expect clean output.

### Task 3: Document and verify the CLI

**Files:**
- Modify: `README.md`
- Modify: `scripts/screenshot.py`

- [ ] Add Home and Move examples using `--drive idle --page home|move` to the screenshot section and module docstring.
- [ ] Run `python scripts/screenshot.py --help`; verify `--page` lists the four valid page names.
- [ ] Run every `tests/test_*.py`; expect all scripts to pass.
- [ ] If commit authorization is later given, commit with `feat: select pages in screenshot captures`.
