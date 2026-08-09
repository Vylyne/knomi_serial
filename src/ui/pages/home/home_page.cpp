#include "home_page.h"

#include "ui/pages/page_helper.h"
#include "printer/send/send_cmd.h"
#include "user_conf.h"

namespace ui {
namespace home_page {

static const char *kZTA = "ZTA";
static const char *kQGL = "QGL";

lv_obj_t *_all = nullptr;
lv_obj_t *_tram = nullptr;
lv_obj_t *_x = nullptr;
lv_obj_t *_y = nullptr;
lv_obj_t *_z = nullptr;
printer::TramType _tram_type = printer::TramType::kNone;

//: Last values written. Setting a local style property invalidates the object
//: whether or not the value changed, and this page was restating four button
//: colours - eight style writes, since the ink follows the fill - on every
//: packet, to say the axes were still homed exactly as they had been.
//:
//: It cost almost nothing while the page was off-screen, because LVGL discards
//: an invalidation outside the clip area. It cost the full four buttons the
//: moment you swiped to it, which is the only time anyone would notice.
//:
//: Impossible sentinels, so the first update after a rebuild always writes.
int8_t _shown_x = -1;
int8_t _shown_y = -1;
int8_t _shown_z = -1;

void _all_click_handler(lv_event_t *e);
void _tram_click_handler(lv_event_t *e);
void _x_click_handler(lv_event_t *e);
void _y_click_handler(lv_event_t *e);
void _z_click_handler(lv_event_t *e);

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  lv_obj_t *page = page_helper::create_page(parent, "HOME");

  // These are not part of the page and outlive it, so a rebuilt page would
  // otherwise be told it is already showing what the last one showed.
  _shown_x = -1;
  _shown_y = -1;
  _shown_z = -1;

  int all_x, all_width;
  const char *tram_label;
  switch (state.tram_type) {
  case printer::TramType::kZTA:
    all_x = -41;
    all_width = 76;
    tram_label = kZTA;
    break;
  case printer::TramType::kQGL:
    all_x = -41;
    all_width = 76;
    tram_label = kQGL;
    break;
  default:
    all_x = 0;
    all_width = 160;
    tram_label = nullptr;
    break;
  }
  _tram_type = state.tram_type;

  _all = page_helper::create_center_button(
      page,
      all_x, -38,
      all_width, 60,
      "ALL",
      _all_click_handler
  );
  if (tram_label) {
    _tram = page_helper::create_center_button(
        page,
        41, -38,
        76, 60,
        tram_label,
        _tram_click_handler
    );
  }
  _x = page_helper::create_center_button(
      page,
      -55, 28,
      50, 60,
      "X",
      _x_click_handler
  );
  _y = page_helper::create_center_button(
      page,
      0, 28,
      50, 60,
      "Y",
      _y_click_handler
  );
  _z = page_helper::create_center_button(
      page,
      55, 28,
      50, 60,
      "Z",
      _z_click_handler
  );

  return page;
}

void printer_update(const printer::State &state) {
  lv_obj_set_state(_all, LV_STATE_DISABLED, state.working);
  if (_tram) {
    lv_obj_set_state(_tram, LV_STATE_DISABLED, state.working);
  }
  lv_obj_set_state(_x, LV_STATE_DISABLED, state.working);
  lv_obj_set_state(_y, LV_STATE_DISABLED, state.working);
  lv_obj_set_state(_z, LV_STATE_DISABLED, state.working);

  int8_t x = state.homed_x ? 1 : 0;
  int8_t y = state.homed_y ? 1 : 0;
  int8_t z = state.homed_z ? 1 : 0;
  if (x == _shown_x && y == _shown_y && z == _shown_z) {
    return;
  }
  bool all = _shown_x == 1 && _shown_y == 1 && _shown_z == 1;
  _shown_x = x;
  _shown_y = y;
  _shown_z = z;

  page_helper::set_button_color(_x, x ? COLOR_HOMED_BG : COLOR_BTN_BG);
  page_helper::set_button_color(_y, y ? COLOR_HOMED_BG : COLOR_BTN_BG);
  page_helper::set_button_color(_z, z ? COLOR_HOMED_BG : COLOR_BTN_BG);
  if ((x && y && z) != all) {
    page_helper::set_button_color(
        _all, (x && y && z) ? COLOR_HOMED_BG : COLOR_BTN_BG);
  }
}

void _all_click_handler(lv_event_t *e) {
  printer::send::send_gcode("G28");
}

void _tram_click_handler(lv_event_t *e) {
  switch (_tram_type) {
  case printer::TramType::kZTA:
    printer::send::send_gcode("Z_TILT_ADJUST");
    break;
  case printer::TramType::kQGL:
    printer::send::send_gcode("QUAD_GANTRY_LEVEL");
    break;
  default:
    break;
  }
}

void _x_click_handler(lv_event_t *e) {
  printer::send::send_gcode("G28 X");
}
void _y_click_handler(lv_event_t *e) {
  printer::send::send_gcode("G28 Y");
}
void _z_click_handler(lv_event_t *e) {
  printer::send::send_gcode("G28 Z");
}

}
}