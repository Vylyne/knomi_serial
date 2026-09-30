#include "home_page.h"

#include <stdint.h>

#include "board_conf.h"
#include "input/button_input.h"
#include "printer/config.h"
#include "printer/send/send_cmd.h"
#include "ui/action_control.h"
#include "ui/theme.h"
#include "user_conf.h"

static_assert(HOME_TARGET_SIZE <= HOME_CORNER_OFFSET,
              "Home centre and diagonal hit targets must not overlap");
static_assert(
    2 * HOME_CORNER_OFFSET * HOME_CORNER_OFFSET <=
        (RES_H / 2 - (HOME_MARK_SIZE + 2 * HOME_RING_GAP) / 2 - 1) *
            (RES_H / 2 - (HOME_MARK_SIZE + 2 * HOME_RING_GAP) / 2 - 1),
    "Home corner rings must fit within the round display");

namespace ui {
namespace home_page {

namespace {

enum ActionId : uint8_t { kX, kY, kAll, kTram, kZ, kActionCount };

const printer::ButtonSlot kSlots[kActionCount] = {
    printer::ButtonSlot::kNW, printer::ButtonSlot::kNE,
    printer::ButtonSlot::kC, printer::ButtonSlot::kSW,
    printer::ButtonSlot::kSE,
};

action_control::Control _controls[kActionCount] = {};
lv_obj_t *_page = nullptr;
printer::TramType _tram_type = printer::TramType::kNone;
bool _busy = false;
int8_t _shown_busy = -1;
int8_t _shown_homed[4] = {-1, -1, -1, -1};
bool _inner_used[4] = {};

uint8_t _legacy_bit(printer::ButtonSlot slot) {
  switch (slot) {
  case printer::ButtonSlot::kNW: return printer::kKeyNW;
  case printer::ButtonSlot::kNE: return printer::kKeyNE;
  case printer::ButtonSlot::kSW: return printer::kKeySW;
  case printer::ButtonSlot::kSE: return printer::kKeySE;
  case printer::ButtonSlot::kC:
  case printer::ButtonSlot::kNone:
    return 0;
  }
  return 0;
}

void _action(input::button::Phase phase, void *context) {
  if (phase != input::button::Phase::kRelease || _busy) {
    return;
  }
  ActionId id = (ActionId)(uintptr_t)context;
  switch (id) {
  case kX: printer::send::send_gcode("G28 X"); break;
  case kY: printer::send::send_gcode("G28 Y"); break;
  case kAll: printer::send::send_gcode("G28"); break;
  case kZ: printer::send::send_gcode("G28 Z"); break;
  case kTram:
    if (_tram_type == printer::TramType::kQGL) {
      printer::send::send_gcode("QUAD_GANTRY_LEVEL");
    } else if (_tram_type == printer::TramType::kZTA) {
      printer::send::send_gcode("Z_TILT_ADJUST");
    }
    break;
  case kActionCount:
    break;
  }
}

void _page_deleted(lv_event_t *event) {
  if (lv_event_get_target(event) != _page) {
    return;
  }
  _page = nullptr;
  for (action_control::Control &control : _controls) {
    control = {};
  }
}

// Shared corner bindings keep their physical legend on the bezel. The Home
// action they displaced moves to a distinct touch-only position on the inner
// cardinal orbit, clear of the centre and the fixed corner overlay.
void _inner_position(ActionId id, int32_t *x, int32_t *y) {
  // left, top, bottom, right. Keep the fallback near its physical slot when
  // several are displaced, with the centre's primary action taking top first.
  const uint8_t preferred[kActionCount][4] = {
      {0, 1, 2, 3}, {1, 3, 2, 0}, {1, 2, 0, 3},
      {2, 0, 3, 1}, {3, 2, 1, 0},
  };
  uint8_t position = preferred[id][0];
  for (uint8_t attempt = 0; attempt < 4; attempt++) {
    uint8_t candidate = preferred[id][attempt];
    if (!_inner_used[candidate]) {
      position = candidate;
      _inner_used[position] = true;
      break;
    }
  }
  const int32_t at = HOME_INNER_OFFSET;
  const int32_t coords[4][2] = {{-at, 0}, {0, -at}, {0, at}, {at, 0}};
  *x = coords[position][0];
  *y = coords[position][1];
}

void _create_action(
    lv_obj_t *page, ActionId id, const char *symbol, bool available) {
  if (!available) {
    return;
  }
  printer::ButtonSlot slot = kSlots[id];
  bool shared = input::button::slot_claimed(slot);
  int32_t x = 0, y = 0;
  int32_t target_size = HOME_TARGET_SIZE;
  int32_t mark_size = id == kAll ? HOME_CENTRE_MARK_SIZE : HOME_MARK_SIZE;
  const lv_font_t *font = id == kAll ? &lv_font_montserrat_24 : &lv_font_montserrat_16;
  printer::ButtonSlot route = slot;

  if (shared) {
    _inner_position(id, &x, &y);
    target_size = HOME_INNER_TARGET_SIZE;
    mark_size = HOME_INNER_MARK_SIZE;
    font = &lv_font_montserrat_16;
    route = printer::ButtonSlot::kNone;
  } else {
    switch (slot) {
    case printer::ButtonSlot::kNW: x = -HOME_CORNER_OFFSET; y = -HOME_CORNER_OFFSET; break;
    case printer::ButtonSlot::kNE: x = HOME_CORNER_OFFSET; y = -HOME_CORNER_OFFSET; break;
    case printer::ButtonSlot::kSW: x = -HOME_CORNER_OFFSET; y = HOME_CORNER_OFFSET; break;
    case printer::ButtonSlot::kSE: x = HOME_CORNER_OFFSET; y = HOME_CORNER_OFFSET; break;
    case printer::ButtonSlot::kC:
    case printer::ButtonSlot::kNone:
      break;
    }
    input::button::register_page_action(
        page, slot, _action, (void *)(uintptr_t)id);
  }

  bool touch = shared ||
      (input::button::slot_touch_enabled(slot) &&
       !(printer::config::get().key_mask & _legacy_bit(slot)));
  _controls[id] = action_control::create(
      page, x, y, target_size, mark_size, symbol, font, theme::machine(),
      touch, route, _action, (void *)(uintptr_t)id);
}

}

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, RES_H, RES_V);
  lv_obj_add_event_cb(page, _page_deleted, LV_EVENT_DELETE, nullptr);
  _page = page;
  _tram_type = state.tram_type;
  _shown_busy = -1;
  for (int8_t &shown : _shown_homed) {
    shown = -1;
  }
  for (bool &used : _inner_used) {
    used = false;
  }
  for (action_control::Control &control : _controls) {
    control = {};
  }

  // The centre is both the page identity and its primary action. Construct it
  // first so a shared centre binding claims an inner position before corners.
  _create_action(page, kAll, LV_SYMBOL_HOME, true);
  _create_action(page, kX, "X", true);
  _create_action(page, kY, "Y", true);
  _create_action(page, kTram,
                 _tram_type == printer::TramType::kQGL ? "QGL" : "ZTA",
                 _tram_type != printer::TramType::kNone);
  _create_action(page, kZ, "Z", true);

  printer_update(state);
  return page;
}

void printer_update(const printer::State &state) {
  _busy = state.working;
  int8_t busy = _busy ? 1 : 0;
  if (busy != _shown_busy) {
    _shown_busy = busy;
    for (const action_control::Control &control : _controls) {
      if (control.label) {
        action_control::set_enabled(control, !_busy);
      }
    }
  }

  const bool homed[4] = {
      state.homed_x ? 1 : 0, state.homed_y ? 1 : 0,
      (state.homed_x && state.homed_y && state.homed_z) ? 1 : 0,
      state.homed_z ? 1 : 0,
  };
  const ActionId ids[4] = {kX, kY, kAll, kZ};
  for (uint8_t i = 0; i < 4; i++) {
    if (homed[i] != _shown_homed[i]) {
      _shown_homed[i] = homed[i];
      action_control::set_ring(_controls[ids[i]], homed[i] != 0);
    }
  }
}

}
}
