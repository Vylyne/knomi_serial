#include "button_input.h"

#include <Arduino.h>
#include <string.h>

#include "board_conf.h"
#include "printer/config.h"
#include "printer/send/send_cmd.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace input {
namespace button {

namespace {

struct QueuedEvent {
  uint32_t config_crc;
  uint8_t index;
  bool pressed;
};

struct PageAction {
  lv_obj_t *owner;
  printer::ButtonSlot slot;
  page_action_t action;
  void *context;
  bool active;
};

static const unsigned int kQueueLength = 16;
static const unsigned int kMaxPageActions = 16;
static const int32_t kSharedTouchSize = 72;

QueueHandle_t _queue = nullptr;
volatile bool _overflow = false;
bool _active[printer::kMaxButtons] = {false};
printer::ButtonConfig _active_binding[printer::kMaxButtons] = {};
uint32_t _active_crc[printer::kMaxButtons] = {0};
lv_obj_t *_active_touch_target[printer::kMaxButtons] = {nullptr};
lv_obj_t *_active_page_owner[printer::kMaxButtons] = {nullptr};
PageAction _page_actions[kMaxPageActions] = {};
PageAction *_active_page_action[printer::kMaxButtons] = {nullptr};
lv_obj_t *_marks[printer::kMaxButtons] = {nullptr};
lv_obj_t *_touch_regions[printer::kMaxButtons] = {nullptr};
bool _overlay_touch_enabled = true;
bool _overlay_touch_compact = false;
uint32_t _config_crc = 0;

bool _valid_slot(printer::ButtonSlot slot) {
  uint8_t value = (uint8_t)slot;
  return value > 0 && value <= (uint8_t)printer::ButtonSlot::kSE;
}

int _binding_index_for_slot(
    const printer::Config &conf, printer::ButtonSlot slot) {
  for (uint8_t i = 0; i < printer::kMaxButtons; i++) {
    if (conf.buttons[i].source == printer::ButtonSource::kNone) {
      break;
    }
    if (conf.buttons[i].slot == slot) {
      return i;
    }
  }
  return -1;
}

void _set_mark(uint8_t index, bool active) {
  if (!_marks[index]) {
    return;
  }
  lv_obj_t *label = lv_obj_get_child(_marks[index], 0);
  if (label) {
    lv_obj_set_style_text_opa(
        label, active ? LV_OPA_COVER : LV_OPA_60, LV_PART_MAIN);
  }
}

bool _on_glass(lv_obj_t *owner) {
  if (!owner) {
    return false;
  }
  lv_area_t area;
  lv_obj_get_coords(owner, &area);
  return area.x1 <= RES_H / 2 && area.x2 >= RES_H / 2 &&
         area.y1 <= RES_V / 2 && area.y2 >= RES_V / 2;
}

PageAction *_page_action(lv_obj_t *owner, printer::ButtonSlot slot) {
  PageAction *visible = nullptr;
  for (PageAction &candidate : _page_actions) {
    if (!candidate.action || candidate.slot != slot) {
      continue;
    }
    if (owner && candidate.owner == owner) {
      return &candidate;
    }
    if (!owner && _on_glass(candidate.owner)) {
      visible = &candidate;
    }
  }
  return visible;
}

void _page_dispatch(
    uint8_t index, printer::ButtonSlot slot, Phase phase, lv_obj_t *owner) {
  if (phase == Phase::kPress) {
    _active_page_action[index] = _page_action(owner, slot);
  }
  PageAction *entry = _active_page_action[index];
  if (entry && entry->action) {
    entry->active = phase == Phase::kPress;
    entry->action(phase, entry->context);
  }
  if (phase != Phase::kPress) {
    _active_page_action[index] = nullptr;
  }
}

void _dispatch(
    uint8_t index, uint32_t config_crc,
    const printer::ButtonConfig &binding, Phase phase,
    lv_obj_t *owner) {
  switch (binding.resolver) {
  case printer::ButtonResolver::kPage:
    _page_dispatch(index, binding.slot, phase, owner);
    break;
  case printer::ButtonResolver::kGcodeMacro:
    if (phase == Phase::kPress && (binding.flags & printer::kButtonPress)) {
      printer::send::send_button_event(
          config_crc, index, 'P');
    }
    if (phase == Phase::kRelease &&
        (binding.flags & (printer::kButtonBare | printer::kButtonRelease))) {
      printer::send::send_button_event(
          config_crc, index, 'R');
    }
    break;
  case printer::ButtonResolver::kObserve:
    _set_mark(index, phase == Phase::kPress);
    break;
  case printer::ButtonResolver::kInternal:
  case printer::ButtonResolver::kNone:
    // Host validation currently refuses internal actions. Keep the wire value
    // inert until one has an explicit lifecycle contract.
    break;
  }
}

void _lifecycle(uint8_t index, Phase phase, lv_obj_t *owner = nullptr) {
  if (index >= printer::kMaxButtons) {
    return;
  }
  if (phase == Phase::kPress) {
    if (_active[index]) {
      return;
    }
    printer::Config conf;
    uint32_t crc;
    printer::config::snapshot(&conf, &crc);
    if (crc != _config_crc) {
      return;
    }
    _active_binding[index] = conf.buttons[index];
    if (printer::config::held_crc() != crc) {
      return;
    }
    _active_crc[index] = crc;
    _active[index] = true;
    _dispatch(index, _active_crc[index], _active_binding[index], phase, owner);
    return;
  }
  if (!_active[index]) {
    return;
  }
  if (phase == Phase::kRelease &&
      _active_crc[index] != printer::config::held_crc()) {
    phase = Phase::kCancel;
  }
  _active[index] = false;
  _dispatch(index, _active_crc[index], _active_binding[index], phase, owner);
  memset(&_active_binding[index], 0, sizeof(_active_binding[index]));
  _active_crc[index] = 0;
  _active_touch_target[index] = nullptr;
  _active_page_owner[index] = nullptr;
}

void _touch_event(lv_event_t *event) {
  uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
  if (index >= printer::kMaxButtons) {
    return;
  }
  lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
  switch (lv_event_get_code(event)) {
  case LV_EVENT_PRESSED:
    if (_active[index]) {
      return;
    }
    _active_touch_target[index] = target;
    _lifecycle(index, Phase::kPress);
    break;
  case LV_EVENT_RELEASED:
    if (_active_touch_target[index] != target) {
      return;
    }
    _lifecycle(index, Phase::kRelease);
    break;
  case LV_EVENT_PRESS_LOST:
  case LV_EVENT_DELETE:
    if (_active_touch_target[index] != target) {
      return;
    }
    _lifecycle(index, Phase::kCancel);
    break;
  default:
    break;
  }
}

void _mark_deleted(lv_event_t *event) {
  uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
  if (index < printer::kMaxButtons &&
      _marks[index] == lv_event_get_target(event)) {
    _marks[index] = nullptr;
  }
}

void _slot_offset(printer::ButtonSlot slot, int32_t *x, int32_t *y) {
  *x = 0;
  *y = 0;
  switch (slot) {
  case printer::ButtonSlot::kNW: *x = -CORNER_OFFSET; *y = -CORNER_OFFSET; break;
  case printer::ButtonSlot::kNE: *x = CORNER_OFFSET; *y = -CORNER_OFFSET; break;
  case printer::ButtonSlot::kSW: *x = -CORNER_OFFSET; *y = CORNER_OFFSET; break;
  case printer::ButtonSlot::kSE: *x = CORNER_OFFSET; *y = CORNER_OFFSET; break;
  case printer::ButtonSlot::kC:
  case printer::ButtonSlot::kNone:
    break;
  }
}

void _legend(const printer::ButtonConfig &binding, char out[5]) {
  memset(out, 0, 5);
  if (binding.resolver == printer::ButtonResolver::kObserve) {
    if (binding.argument == (uint8_t)printer::ButtonProfile::kFeed) {
      strcpy(out, "F");
    } else if (binding.argument == (uint8_t)printer::ButtonProfile::kRetract) {
      strcpy(out, "R");
    }
    return;
  }
  memcpy(out, binding.legend, sizeof(binding.legend));
}

}

void begin() {
  _queue = xQueueCreate(kQueueLength, sizeof(QueuedEvent));
  _config_crc = printer::config::held_crc();
}

bool enqueue(uint8_t index, bool pressed, uint32_t config_crc) {
  if (!_queue || index >= printer::kMaxButtons) {
    return false;
  }
  QueuedEvent event = {config_crc, index, pressed};
  if (xQueueSend(_queue, &event, 0) == pdTRUE) {
    return true;
  }
  _overflow = true;
  return false;
}

void drain() {
  if (!_queue) {
    return;
  }
  if (_overflow) {
    _overflow = false;
    cancel_all();
    xQueueReset(_queue);
    return;
  }
  QueuedEvent event;
  while (xQueueReceive(_queue, &event, 0) == pdTRUE) {
    if (event.config_crc != _config_crc ||
        event.config_crc != printer::config::held_crc()) {
      continue;
    }
    _lifecycle(
        event.index, event.pressed ? Phase::kPress : Phase::kRelease);
  }
}

void sync_config() {
  uint32_t crc = printer::config::held_crc();
  if (crc == _config_crc) {
    return;
  }
  cancel_all();
  if (_queue) {
    xQueueReset(_queue);
  }
  _overflow = false;
  _config_crc = crc;
}

void cancel_all() {
  for (uint8_t i = 0; i < printer::kMaxButtons; i++) {
    _lifecycle(i, Phase::kCancel);
  }
  for (PageAction &action : _page_actions) {
    if (action.action && action.active) {
      action.active = false;
      action.action(Phase::kCancel, action.context);
    }
  }
}

bool register_page_action(
    lv_obj_t *owner, printer::ButtonSlot slot,
    page_action_t action, void *context) {
  if (!owner || !_valid_slot(slot) || !action || slot_claimed(slot)) {
    return false;
  }
  for (PageAction &entry : _page_actions) {
    if (!entry.action) {
      entry = {owner, slot, action, context, false};
      return true;
    }
  }
  return false;
}

void clear_page_actions() {
  for (uint8_t i = 0; i < printer::kMaxButtons; i++) {
    if (_active[i] &&
        (_active_binding[i].source == printer::ButtonSource::kTouch ||
         _active_binding[i].resolver == printer::ButtonResolver::kPage)) {
      _lifecycle(i, Phase::kCancel);
    }
  }
  for (PageAction &action : _page_actions) {
    if (action.action && action.active) {
      action.active = false;
      action.action(Phase::kCancel, action.context);
    }
  }
  memset(_page_actions, 0, sizeof(_page_actions));
}

bool slot_claimed(printer::ButtonSlot slot) {
  printer::Config conf;
  printer::config::snapshot(&conf, nullptr);
  int index = _binding_index_for_slot(conf, slot);
  return index >= 0 &&
         conf.buttons[index].resolver != printer::ButtonResolver::kPage;
}

bool slot_touch_enabled(printer::ButtonSlot slot) {
  printer::Config conf;
  printer::config::snapshot(&conf, nullptr);
  int index = _binding_index_for_slot(conf, slot);
  return index < 0 ||
         (conf.buttons[index].resolver == printer::ButtonResolver::kPage &&
          conf.buttons[index].source == printer::ButtonSource::kTouch);
}

void page_event(lv_obj_t *owner, printer::ButtonSlot slot, Phase phase) {
  if (!_valid_slot(slot)) {
    return;
  }
  printer::Config conf;
  printer::config::snapshot(&conf, nullptr);
  int index = _binding_index_for_slot(conf, slot);
  if (index >= 0) {
    if (conf.buttons[index].resolver != printer::ButtonResolver::kPage ||
        conf.buttons[index].source != printer::ButtonSource::kTouch) {
      return;
    }
    if (phase == Phase::kPress) {
      if (_active[index]) {
        return;
      }
      _active_page_owner[index] = owner;
    } else if (_active_page_owner[index] != owner) {
      return;
    }
    _lifecycle(index, phase, owner);
    return;
  }

  PageAction *action = _page_action(owner, slot);
  if (!action) {
    return;
  }
  if (phase == Phase::kPress) {
    if (action->active) {
      return;
    }
    action->active = true;
    action->action(phase, action->context);
    return;
  }
  if (!action->active) {
    return;
  }
  action->active = false;
  action->action(phase, action->context);
}

bool profile_active(printer::ButtonProfile profile) {
  for (uint8_t i = 0; i < printer::kMaxButtons; i++) {
    if (_active[i] &&
        _active_binding[i].resolver == printer::ButtonResolver::kObserve &&
        _active_binding[i].argument == (uint8_t)profile) {
      return true;
    }
  }
  return false;
}

void build_overlay(lv_obj_t *parent) {
  memset(_marks, 0, sizeof(_marks));
  memset(_touch_regions, 0, sizeof(_touch_regions));
  _overlay_touch_enabled = true;
  _overlay_touch_compact = false;
  printer::Config conf;
  printer::config::snapshot(&conf, nullptr);
  for (uint8_t i = 0; i < printer::kMaxButtons; i++) {
    const printer::ButtonConfig &binding = conf.buttons[i];
    if (binding.source == printer::ButtonSource::kNone) {
      break;
    }
    if (binding.slot == printer::ButtonSlot::kNone ||
        binding.resolver == printer::ButtonResolver::kPage) {
      continue;
    }

    int32_t x, y;
    _slot_offset(binding.slot, &x, &y);

    lv_obj_t *region = lv_obj_create(parent);
    lv_obj_remove_style_all(region);
    lv_obj_remove_flag(region, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(region, kSharedTouchSize, kSharedTouchSize);
    lv_obj_align(region, LV_ALIGN_CENTER, x, y);
    if (binding.source == printer::ButtonSource::kTouch) {
      _touch_regions[i] = region;
      lv_obj_add_flag(region, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(region, _touch_event, LV_EVENT_PRESSED, (void *)(uintptr_t)i);
      lv_obj_add_event_cb(region, _touch_event, LV_EVENT_RELEASED, (void *)(uintptr_t)i);
      lv_obj_add_event_cb(region, _touch_event, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)i);
      lv_obj_add_event_cb(region, _touch_event, LV_EVENT_DELETE, (void *)(uintptr_t)i);
    } else {
      lv_obj_remove_flag(region, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *mark = lv_obj_create(parent);
    lv_obj_remove_style_all(mark);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mark, CORNER_SIZE, CORNER_SIZE);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mark, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mark, SCRIM_OPA, LV_PART_MAIN);
    lv_obj_align(mark, LV_ALIGN_CENTER, x, y);
    lv_obj_add_event_cb(mark, _mark_deleted, LV_EVENT_DELETE, (void *)(uintptr_t)i);

    char legend[5];
    _legend(binding, legend);
    lv_obj_t *label = lv_label_create(mark);
    lv_label_set_text(label, legend);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, ui::theme::machine(), LV_PART_MAIN);
    lv_obj_set_style_text_opa(label, LV_OPA_60, LV_PART_MAIN);
    lv_obj_center(label);
    _marks[i] = mark;
    if (_active[i] &&
        _active_binding[i].resolver == printer::ButtonResolver::kObserve) {
      _set_mark(i, true);
    }
  }
}

void set_overlay_touch_enabled(bool enabled) {
  if (_overlay_touch_enabled == enabled) {
    return;
  }
  _overlay_touch_enabled = enabled;
  for (uint8_t i = 0; i < printer::kMaxButtons; i++) {
    if (!_touch_regions[i]) {
      continue;
    }
    if (enabled) {
      lv_obj_remove_flag(_touch_regions[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      if (_active_touch_target[i] == _touch_regions[i]) {
        _lifecycle(i, Phase::kCancel);
      }
      lv_obj_add_flag(_touch_regions[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void set_overlay_touch_compact(bool compact) {
  if (_overlay_touch_compact == compact) {
    return;
  }
  _overlay_touch_compact = compact;
  for (uint8_t i = 0; i < printer::kMaxButtons; i++) {
    if (!_touch_regions[i]) {
      continue;
    }
    if (_active_touch_target[i] == _touch_regions[i]) {
      _lifecycle(i, Phase::kCancel);
    }
    int32_t size = compact ? CORNER_SIZE : kSharedTouchSize;
    lv_obj_set_size(_touch_regions[i], size, size);
  }
}

}
}
