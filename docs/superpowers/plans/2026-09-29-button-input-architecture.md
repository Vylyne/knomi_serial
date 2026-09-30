# Button Input Architecture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Normalize touch, display GPIO, and host-forwarded events into one lifecycle and resolver system with page, macro, and observe behavior.

**Architecture:** Klipper parses named prefixed options into a host-only binding table and compact fixed wire records. Protocol 6 adds those records to `CONFIG` plus a CRC-scoped event frame. Firmware producers feed a queue; the UI task owns lifecycle dispatch and LVGL-facing resolution. Macro names remain host-side and are selected by CRC plus record index.

**Tech Stack:** Klipper Python extra, Python `struct`, ESP32-S3 Arduino/FreeRTOS, LVGL 9, PlatformIO.

**Spec:** [Button Inputs and Home/Move Pages Design](../specs/2026-09-29-button-inputs-and-pages-design.md)

## Global Constraints

- Follow strict test-first order for host-visible behavior.
- Any wire layout change updates both sides, version 6, assertions, protocol docs, and protocol tests together.
- Deterministic config errors happen at startup and name section, option, property, and invalid value.
- No firmware flash without explicit user authorization.
- Do not claim GPIO complete without a live wired switch test.

## Review Focus

- Stale CRCs and unmatched releases must be inert.
- Cancel must never fire a release macro.
- `observe` must never execute the external action.
- LVGL calls must stay on the UI task.
- Existing page commands and legacy `hardware_keys` behavior must not regress.

---

### Task 1: Specify and test config parsing

**Files:**
- Modify: `tests/test_addressing.py`
- Modify: `klippy_extras/knomi_serial.py`

- [ ] Extend `FakeSection` with `get_prefix_options(prefix)` returning the matching option names, mirroring Klipper's config API.
- [ ] Add failing tests for valid GPIO observe, event macro, visible touch macro, touch page, bare release macro, and edge-specific macro definitions.
- [ ] Add table-driven failing tests for missing/unknown properties, bad source/slot/pin/resolver/legend, duplicate names/slots, unsupported pins, `observe` on touch, `hardware_keys` collisions, bare-plus-edge resolver conflicts, and non-macro edge resolvers.
- [ ] Assert each failure contains the section, `button_<name>`, property, and bad value.
- [ ] Implement frozen `ButtonBinding` and enums/constants, then parse `button_` options in sorted config order into at most eight bindings.
- [ ] Keep macro names in each host binding and produce the compact device record separately.
- [ ] Run `python tests/test_addressing.py`; expect all tests to pass.

### Task 2: Pin protocol 6 in tests

**Files:**
- Modify: `tests/test_protocol.py`
- Modify: `klippy_extras/knomi_serial.py`
- Modify: `src/printer/printer.h`

- [ ] Add failing parity checks for protocol version 6, `_FRAME_BUTTON_EVENT`, `kHasButtons`, button/source/slot/resolver/profile enum values, `kMaxButtons == 8`, `kButtonWireSize == 12`, and config size 388.
- [ ] Add failing byte-level tests for one populated record, zeroed unused records, the new presence bit, and a six-byte big-endian button-event payload.
- [ ] Add the matching Python constants, config dataclasses/packing, `encode_button_event`, C++ enums/records, frame id `0x05`, config field, size constants, and static assertions.
- [ ] Bump `_PROTO_VERSION` and `kProtoVersion` to 6 and document the history comment on both sides.
- [ ] Run `python tests/test_protocol.py`; expect all tests to pass.

### Task 3: Apply button records safely on-device

**Files:**
- Modify: `src/printer/config.cpp`
- Modify: `src/printer/config.h`
- Modify: `src/printer/recv/recv_task.cpp`
- Modify: `src/printer/recv/recv_task.h`

- [ ] Add compiled empty button defaults and overlay records only when `kHasButtons` is present.
- [ ] Preserve raw-payload CRC and protocol-keyed NVS behavior for the enlarged payload.
- [ ] Decode button-event payload fields explicitly, validate exact length, and enqueue only CRC-matching `source=event` indexes.
- [ ] Treat malformed known event payloads as a local protocol fault; continue skipping unknown frame types.
- [ ] Build `pio run -e knomi`; expect static assertions and compilation to pass.

### Task 4: Add normalized lifecycle and resolver dispatch

**Files:**
- Create: `src/input/button_input.h`
- Create: `src/input/button_input.cpp`
- Modify: `src/main.cpp`
- Modify: `src/ui/ui_task.cpp`
- Modify: `src/ui/ui.cpp`
- Modify: `src/ui/ui.h`
- Modify: `src/ui/screens/screen_helper.cpp`
- Modify: `src/ui/screens/screen_helper.h`
- Modify: `src/printer/send/send_cmd.h`
- Modify: `src/printer/send/send_task.cpp`

- [ ] Define phase (`press`, `release`, `cancel`), active-index tracking, a bounded FreeRTOS queue, shared-slot queries, and UI-thread drain API.
- [ ] Implement idempotent press/release handling: ignore duplicate presses and unmatched releases; dispatch cancel on config CRC change and link staleness.
- [ ] Implement bare macro on committed release and edge macro emission as `BUTTON:<crc>:<index>:<P|R>`.
- [ ] Implement observe FEED/RETRACT state callbacks without sending any command.
- [ ] Reserve internal dispatch but return unsupported for all current arguments, matching host-side rejection.
- [ ] Add page-slot registration/unregistration so pages install current callbacks after shared bindings claim their slots.
- [ ] Create a non-scrolling shared-binding overlay for Idle and Printing, rendering profile-derived legends or the configured four-character macro legend; touch bindings attach lifecycle targets there.
- [ ] Drain the queue before LVGL handling in `ui_task`; never invoke page or observe callbacks in recv/GPIO tasks.
- [ ] Build `pio run -e knomi`; expect success.

### Task 5: Adapt touch controls to the lifecycle

**Files:**
- Modify: `src/ui/corner.h`
- Modify: `src/ui/corner.cpp`
- Modify: `src/ui/pages/tool/tool_page.cpp`
- Modify: `src/ui/pages/printing/printing_page.cpp`
- Modify: `src/ui/pages/estop/estop_page.cpp`

- [ ] Replace direct `LV_EVENT_CLICKED` handling for page slots with press, release, and press-lost forwarding to the normalized dispatcher.
- [ ] Preserve the current visible marks and large hit regions on Tool and Printing.
- [ ] Register page actions for SW/SE and preserve their existing busy guards and cancel confirmation behavior.
- [ ] Ensure object deletion unregisters page callbacks and cancels any active touch lifecycle.
- [ ] Build `pio run -e knomi`; expect success.

### Task 6: Implement debounced GPIO inputs

**Files:**
- Create: `src/input/gpio_input.h`
- Create: `src/input/gpio_input.cpp`
- Modify: `src/main.cpp`
- Modify: `src/user_conf.h`

- [ ] Configure only active `source=gpio` records as `INPUT_PULLUP` after config adoption.
- [ ] Poll every 5 ms and require 20 ms of stable state before queuing an edge.
- [ ] Reconfigure pins and cancel active lifecycles when config CRC changes; do not leave an old pin driven or polled.
- [ ] Add named timing constants and comments explaining active-low/pull-up assumptions.
- [ ] Build `pio run -e knomi`; expect success.

### Task 7: Add host event command and safe macro lookup

**Files:**
- Modify: `tests/test_addressing.py`
- Modify: `klippy_extras/knomi_serial.py`

- [ ] Add failing tests for `KNOMI_BUTTON` single/multi-screen addressing, case-insensitive names, source enforcement, `PRESSED=0|1`, all-target validation before writes, and the encoded CRC/index.
- [ ] Add failing `_process_cmd` tests for valid bare/edge macro events and ignored stale CRC, bad index, bad edge, and non-macro bindings.
- [ ] Register `KNOMI_BUTTON` on the cluster and implement it using the existing resolver and `_write` path.
- [ ] Parse `KNOMI_CMD:BUTTON:` defensively and execute only the macro selected by the current binding table.
- [ ] Run `python tests/test_addressing.py`; expect all tests to pass.

### Task 8: Document the public contract

**Files:**
- Modify: `README.md`
- Modify: `docs/protocol.md`
- Modify: `docs/hardware.md`
- Modify: `AGENTS.md`

- [ ] Replace the corner-key section with the three-source/four-resolver model, complete config examples, event command, lifecycle, legacy migration, and safety rules.
- [ ] Document protocol 6 record layout, event frame, macro line, CRC semantics, and byte counts.
- [ ] Document supported/preferred/forbidden GPIOs and active-low pull-up wiring.
- [ ] Add an AGENTS invariant that button names/macros remain host-side and device button indexes are meaningful only with config CRC.
- [ ] Leave the GPIO TODO unchecked until live-switch verification passes.

### Task 9: Run software gates and prepare hardware verification

**Files:**
- Modify as failures require, without weakening assertions.

- [ ] Run all `tests/test_*.py`; expect every script to pass.
- [ ] Run `ruff check .`; expect clean output.
- [ ] Run `pio run -e knomi`; expect success and record RAM/flash usage.
- [ ] Review `git diff --check` and the complete diff for protocol symmetry, cancellation paths, and unrelated changes.
- [ ] With explicit flash permission, upload to COM5 and verify config adoption, touch fallback, stale-event rejection, and macro cardinality.
- [ ] With a switch on a supported GPIO, verify debounce and lifecycle behavior; only then check the GPIO feature in README.
- [ ] If commit authorization is later given, commit with `feat: add configurable button inputs`.
