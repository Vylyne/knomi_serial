#include "gcode_page.h"

#include "ui/pages/page_helper.h"
#include "printer/config.h"
#include "printer/send/send_cmd.h"

namespace ui {
namespace gcode_page {

static lv_obj_t *_roller = nullptr;
static lv_obj_t *_run = nullptr;
static bool _working = false;

//: CRC of the config the roller was filled from, so the list is rebuilt when
//: the host changes it and not otherwise.
//:
//: The macro list used to ride the state packet, so it was simply always there
//: by the time the page was built. It now arrives on its own frame after a
//: round trip, which usually lands before this page is first swiped to but is
//: not guaranteed to - and it can change under a running screen if printer.cfg
//: is edited and Klipper reloaded.
static uint32_t _built_from = 0;

void _run_click_handler(lv_event_t *e);
void _fill_roller();

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  lv_obj_t *page = page_helper::create_page(parent, "GCODE");

  _roller = lv_roller_create(page);
  _built_from = 0;
  _fill_roller();
  lv_obj_set_size(_roller, 160, 110);
  lv_obj_align(_roller, LV_ALIGN_CENTER, 0, -25);
  lv_obj_set_style_text_color(_roller, COLOR_GCODE_UNSELECTED, LV_PART_MAIN);
  lv_obj_set_style_text_align(_roller, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_set_style_text_font(_roller, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(_roller, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(_roller, 0, LV_PART_MAIN);

  lv_obj_set_style_text_align(_roller, LV_TEXT_ALIGN_LEFT, LV_PART_SELECTED);
  lv_obj_set_style_radius(_roller, 10, LV_PART_SELECTED);
  lv_obj_set_style_bg_color(_roller, COLOR_GCODE_HIGHLIGHT, LV_PART_SELECTED);

  _run = page_helper::create_center_button(
      page,
      0, 55,
      160, 40,
      "RUN",
      _run_click_handler
  );

  return page;
}

void _fill_roller() {
  uint32_t crc = printer::config::held_crc();
  if (crc == _built_from) {
    return;
  }
  _built_from = crc;

  const char *gcodes = printer::config::get().gcodes;
  // An empty options string makes an empty roller, which LVGL is happy with but
  // which reads as a broken page. Say what is actually true: the host has not
  // sent a list yet, or printer.cfg declares none.
  lv_roller_set_options(
      _roller,
      gcodes[0] != '\0' ? gcodes : "NO MACROS",
      LV_ROLLER_MODE_INFINITE);
}

void printer_update(const printer::State &state) {
  _fill_roller();
  if (state.working != _working) {
    _working = state.working;
    lv_obj_set_state(_run, LV_STATE_DISABLED, _working);
  }
}

void _run_click_handler(lv_event_t *e) {
  static char selected[printer::kGcodesMaxLen + 1];
  lv_roller_get_selected_str(_roller, selected, sizeof(selected));
  printer::send::send_gcode(selected);
}

}
}