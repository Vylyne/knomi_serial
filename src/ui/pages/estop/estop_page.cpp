#include "estop_page.h"

#include "board_conf.h"
#include "printer/send/send_cmd.h"
#include "ui/pages/page_helper.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace estop_page {

static lv_obj_t *_ring = nullptr;
static lv_obj_t *_button = nullptr;
static lv_obj_t *_label = nullptr;
static lv_timer_t *_timer = nullptr;
static uint32_t _held_from = 0;
static bool _fired = false;

static void _press_handler(lv_event_t *e);
static void _release_handler(lv_event_t *e);
static void _tick(lv_timer_t *timer);
static void _reset();

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, RES_H, RES_V);

  // The ring is the whole feedback mechanism: it says a hold is being counted,
  // and how much is left. Without it a hold is indistinguishable from a control
  // that has stopped responding.
  _ring = lv_arc_create(page);
  lv_obj_set_size(_ring, 176, 176);
  lv_obj_center(_ring);
  lv_arc_set_rotation(_ring, 270);
  lv_arc_set_bg_angles(_ring, 0, 360);
  lv_arc_set_range(_ring, 0, ESTOP_HOLD_MS);
  lv_arc_set_value(_ring, 0);
  lv_obj_set_style_arc_width(_ring, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(_ring, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(_ring, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
  lv_obj_set_style_arc_color(_ring, lv_color_hex(0xE04A32), LV_PART_INDICATOR);
  lv_obj_set_style_opa(_ring, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_remove_flag(_ring, LV_OBJ_FLAG_CLICKABLE);

  _button = lv_button_create(page);
  lv_obj_set_size(_button, 132, 132);
  lv_obj_center(_button);
  lv_obj_set_style_radius(_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);

  _label = lv_label_create(_button);
  lv_obj_set_style_text_font(_label, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_align(_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(_label);

  page_helper::set_button_color(_button, COLOR_STOP_BG);

  // PRESSED rather than CLICKED, and RELEASED and PRESS_LOST both unwind it -
  // a finger that slides off the control has to abandon the hold, not complete
  // it.
  lv_obj_add_event_cb(_button, _press_handler, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(_button, _release_handler, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(_button, _release_handler, LV_EVENT_PRESS_LOST, nullptr);

  _reset();
  printer_update(state);
  return page;
}

void printer_update(const printer::State &state) {
  // Nothing here changes with printer state. Once fired, the machine is down
  // and the shutdown screen takes over, so there is nothing to hold any more.
  if (_fired && state.status != printer::Status::kShutdown) {
    _fired = false;
    _reset();
  }
}

static void _reset() {
  _held_from = 0;
  if (_timer) {
    lv_timer_delete(_timer);
    _timer = nullptr;
  }
  if (_ring) {
    lv_arc_set_value(_ring, 0);
  }
  if (_label) {
    lv_label_set_text(_label, "HOLD\nTO STOP");
  }
}

static void _press_handler(lv_event_t *e) {
  if (_fired) {
    return;
  }
  _held_from = lv_tick_get();
  if (!_timer) {
    // Only while held. A timer left running would repaint the ring forever for
    // a control nobody is touching.
    _timer = lv_timer_create(_tick, 30, nullptr);
  }
}

static void _release_handler(lv_event_t *e) {
  if (_fired) {
    return;
  }
  _reset();
}

static void _tick(lv_timer_t *timer) {
  if (_held_from == 0) {
    return;
  }

  uint32_t held = lv_tick_elaps(_held_from);
  if (held < ESTOP_HOLD_MS) {
    lv_arc_set_value(_ring, (int32_t)held);
    return;
  }

  _fired = true;
  _held_from = 0;
  lv_arc_set_value(_ring, ESTOP_HOLD_MS);
  lv_label_set_text(_label, "STOPPED");
  if (_timer) {
    lv_timer_delete(_timer);
    _timer = nullptr;
  }
  printer::send::send_stop();
}

}
}
