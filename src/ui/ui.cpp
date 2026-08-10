#include "ui.h"

#include <Arduino.h>

#include "board_conf.h"
#include "printer/config.h"
#include "printer/printer.h"
#include "ui/haze.h"
#include "ui/screens/idle/idle_screen.h"
#include "ui/screens/screen_helper.h"
#include "ui/screens/init/init_screen.h"
#include "ui/screens/printing/printing_screen.h"
#include "ui/screens/shutdown/shutdown_screen.h"
#include "user_conf.h"

namespace ui {

static lv_event_code_t _lv_event_printer_update;

static printer::Status _status = printer::Status::kDisconnected;
static lv_obj_t *_scr = nullptr;

//: Kept so a swipe can re-show the pages what they missed. A copy, not a
//: reference: the original lives behind the receive task's mutex.
static printer::State _last_state;

static lv_obj_t *_stale_mark = nullptr;
static bool _stale = false;

//: CRC of the config the current screen was built against.
static uint32_t _built_config = 0;

void init() {
  lv_init();
  lv_tick_set_cb([]() { return (uint32_t) millis(); });
  _lv_event_printer_update = static_cast<lv_event_code_t>(lv_event_register_id());
}

void set_link_stale(bool stale) {
  if (stale == _stale && _stale_mark) {
    return;
  }
  _stale = stale;

  if (!_stale_mark) {
    // On the top layer, built once. Every screen is destroyed and rebuilt on a
    // status change, and this has to outlive that - the whole point of it is
    // that it appears when status changes have stopped arriving.
    lv_obj_t *layer = lv_layer_top();
    _stale_mark = lv_obj_create(layer);
    lv_obj_remove_style_all(_stale_mark);
    lv_obj_remove_flag(_stale_mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_stale_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(_stale_mark, STALE_MARK_SIZE, STALE_MARK_SIZE);
    lv_obj_set_style_radius(_stale_mark, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_stale_mark, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_stale_mark, LV_OPA_70, LV_PART_MAIN);
    lv_obj_align(_stale_mark, LV_ALIGN_BOTTOM_MID, 0, STALE_MARK_Y);

    lv_obj_t *icon = lv_label_create(_stale_mark);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, LV_PART_MAIN);
    // The cable, because that is what has gone: this is the serial link, not a
    // network. A warning triangle would claim the printer is in trouble, and
    // the printer is probably fine - it is this screen that has been cut off.
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_STALE), LV_PART_MAIN);
    lv_obj_center(icon);
  }

  if (_stale) {
    lv_obj_remove_flag(_stale_mark, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(_stale_mark, LV_OBJ_FLAG_HIDDEN);
  }
}

void update(const printer::State &state) {
  _last_state = state;

  // A config change rebuilds whatever screen is up. Colours would follow on
  // their own, being read fresh at draw time, but which corners are soft keys
  // is decided when a page is built - so without this a printer.cfg edit would
  // apply to half the screen and wait for a status change for the rest.
  //
  // It costs the scroll position and a 300ms fade, a few times a day at most,
  // in exchange for never having to reason about which settings are live.
  uint32_t config_crc = printer::config::held_crc();
  bool reconfigured = config_crc != _built_config;
  _built_config = config_crc;

  scr_init_t next_scr_init = nullptr;
  if (state.status != _status || !_scr || reconfigured) {
    switch (state.status) {
    case printer::Status::kDisconnected:
      next_scr_init = init_screen::init;
      break;
    case printer::Status::kIdle:
      next_scr_init = idle_screen::init;
      break;
    case printer::Status::kPrinting:
      next_scr_init = printing_screen::init;
      break;
    case printer::Status::kShutdown:
      next_scr_init = shutdown_screen::init;
      break;
    }
    _status = state.status;
  }

  if (next_scr_init) {
    _scr = next_scr_init(state);
    lv_screen_load_anim(_scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
  }

  if (_scr) {
    // Before the page update, so a screen that has just been loaded is painted
    // with the right heat before anything is drawn on top of it.
    haze::apply(_scr, state);
    lv_obj_send_event(_scr, _lv_event_printer_update, (void*) &state);
  }
}

void refresh() {
  if (_scr) {
    lv_obj_send_event(_scr, _lv_event_printer_update, (void *)&_last_state);
  }
}

const char *screen_name() {
  if (!_scr) {
    return "none";
  }
  switch (_status) {
  case printer::Status::kDisconnected:
    return "init";
  case printer::Status::kIdle:
    return "idle";
  case printer::Status::kPrinting:
    return "printing";
  case printer::Status::kShutdown:
    return "shutdown";
  }
  return "unknown";
}

int page_count() {
  return _scr ? (int)lv_obj_get_child_count(_scr) : 0;
}

int page_index() {
  // Asks which page is under the viewport, not where the viewport is. Wrapping
  // reorders the row, so a slot number stopped identifying a page the moment
  // the first rotation happened - and with re-centring it would now report the
  // home slot forever while you swiped past everything on the device.
  return screen_helper::visible_page(_scr);
}

namespace control {

void _printer_update_cb(lv_event_t *e);

void register_printer_update_cb(lv_obj_t *obj, printer_update_cb_t cb) {
  lv_obj_add_event_cb(
      obj,
      _printer_update_cb,
      _lv_event_printer_update,
      (void*) cb
  );
}

void _printer_update_cb(lv_event_t *e) {
  printer::State *state = (printer::State*) lv_event_get_param(e);
  printer_update_cb_t cb = (printer_update_cb_t) lv_event_get_user_data(e);
  cb(*state);
}

}

}