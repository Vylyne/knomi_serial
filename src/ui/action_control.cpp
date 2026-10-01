#include "action_control.h"

#include <stdint.h>

#include "user_conf.h"

namespace ui {
namespace action_control {

namespace {

struct TouchState {
  lv_obj_t *owner;
  printer::ButtonSlot slot;
  input::button::page_action_t action;
  void *context;
  bool pressed;
};

void _dispatch(TouchState *state, input::button::Phase phase) {
  if (state->slot != printer::ButtonSlot::kNone) {
    input::button::page_event(state->owner, state->slot, phase);
  } else if (state->action) {
    state->action(phase, state->context);
  }
}

void _touch_event(lv_event_t *event) {
  TouchState *state = static_cast<TouchState *>(lv_event_get_user_data(event));
  lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED) {
    if (!state->pressed) {
      state->pressed = true;
      _dispatch(state, input::button::Phase::kPress);
    }
    return;
  }
  if (state->pressed) {
    state->pressed = false;
    _dispatch(state, code == LV_EVENT_RELEASED
                         ? input::button::Phase::kRelease
                         : input::button::Phase::kCancel);
  }
  if (code == LV_EVENT_DELETE) {
    lv_free(state);
  }
}

}

Control create(
    lv_obj_t *parent, int32_t x, int32_t y, int32_t target_size,
    int32_t mark_size, const char *symbol, const lv_font_t *font,
    lv_color_t ink, bool touch, printer::ButtonSlot page_slot,
    input::button::page_action_t action, void *context) {
  Control result;
  result.target = lv_obj_create(parent);
  lv_obj_remove_style_all(result.target);
  lv_obj_remove_flag(result.target, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(result.target, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(result.target, target_size, target_size);
  lv_obj_align(result.target, LV_ALIGN_CENTER, x, y);
  if (touch) {
    TouchState *state = static_cast<TouchState *>(lv_malloc(sizeof(TouchState)));
    if (state) {
      *state = {parent, page_slot, action, context, false};
      lv_obj_add_flag(result.target, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(result.target, _touch_event, LV_EVENT_PRESSED, state);
      lv_obj_add_event_cb(result.target, _touch_event, LV_EVENT_RELEASED, state);
      lv_obj_add_event_cb(result.target, _touch_event, LV_EVENT_PRESS_LOST, state);
      lv_obj_add_event_cb(result.target, _touch_event, LV_EVENT_DELETE, state);
    }
  } else {
    lv_obj_remove_flag(result.target, LV_OBJ_FLAG_CLICKABLE);
  }

  result.mark = lv_obj_create(parent);
  lv_obj_remove_style_all(result.mark);
  lv_obj_remove_flag(result.mark, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(result.mark, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(result.mark, mark_size, mark_size);
  lv_obj_set_style_radius(result.mark, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(result.mark, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(result.mark, SCRIM_OPA, LV_PART_MAIN);
  lv_obj_align(result.mark, LV_ALIGN_CENTER, x, y);

  result.label = lv_label_create(result.mark);
  lv_label_set_text(result.label, symbol);
  lv_obj_set_style_text_font(result.label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(result.label, ink, LV_PART_MAIN);
  lv_obj_center(result.label);
  return result;
}

void set_enabled(const Control &control, bool enabled) {
  if (control.label) {
    lv_obj_set_style_text_opa(
        control.label, enabled ? LV_OPA_COVER : CORNER_DISABLED_OPA,
        LV_PART_MAIN);
  }
}

void set_fill(const Control &control, lv_color_t fill, lv_color_t ink) {
  if (!control.mark || !control.label) {
    return;
  }
  lv_obj_set_style_bg_color(control.mark, fill, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(control.mark, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(control.label, ink, LV_PART_MAIN);
}

}
}
