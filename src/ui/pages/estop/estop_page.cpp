#include "estop_page.h"

#include "board_conf.h"
#include "printer/send/send_cmd.h"
#include "ui/pages/page_helper.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace estop_page {

static lv_obj_t *_button = nullptr;
static lv_obj_t *_label = nullptr;
static lv_timer_t *_timer = nullptr;
static bool _armed = false;

static void _click_handler(lv_event_t *e);
static void _disarm(lv_timer_t *timer);
static void _show();

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, RES_H, RES_V);

  _button = lv_button_create(page);
  lv_obj_set_size(_button, 168, 168);
  lv_obj_center(_button);
  lv_obj_set_style_radius(_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_add_event_cb(_button, _click_handler, LV_EVENT_CLICKED, nullptr);

  _label = lv_label_create(_button);
  lv_obj_set_style_text_font(_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_align(_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(_label);

  _armed = false;
  if (_timer) {
    lv_timer_delete(_timer);
    _timer = nullptr;
  }
  _show();

  printer_update(state);
  return page;
}

void printer_update(const printer::State &state) {
  // Coming back from a shutdown leaves the control armed if the second tap was
  // never given. Nothing else here depends on printer state.
  if (_armed && state.status == printer::Status::kShutdown) {
    _disarm(nullptr);
  }
}

// Two taps, like cancel. Not a press-and-hold: a hold reads as safer but is
// slower exactly when it matters, and this is not the machine's real emergency
// stop regardless - that belongs on a latching switch that cuts power, rather
// than on a display asking a host to ask an MCU.
static void _click_handler(lv_event_t *e) {
  if (!_armed) {
    _armed = true;
    _show();
    if (!_timer) {
      _timer = lv_timer_create(_disarm, CONFIRM_MS, nullptr);
      lv_timer_set_repeat_count(_timer, 1);
    }
    return;
  }

  _armed = false;
  if (_timer) {
    lv_timer_delete(_timer);
    _timer = nullptr;
  }
  _show();
  printer::send::send_stop();
}

static void _disarm(lv_timer_t *timer) {
  _timer = nullptr;
  _armed = false;
  _show();
}

static void _show() {
  if (!_button || !_label) {
    return;
  }
  lv_label_set_text(_label, _armed ? "CONFIRM\nSTOP" : "E-STOP");
  page_helper::set_button_color(
      _button, _armed ? COLOR_CONFIRM_BG : COLOR_STOP_BG);
}

}
}
