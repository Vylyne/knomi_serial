#include "print_control_page.h"

#include "board_conf.h"
#include "ui/pages/page_helper.h"
#include "printer/send/send_cmd.h"
#include "user_conf.h"

namespace ui {
namespace print_control_page {

static bool _paused = false;
static lv_obj_t *_pause = nullptr;
static lv_obj_t *_cancel = nullptr;
static lv_obj_t *_progress = nullptr;

void _update_pause();
void _pause_click_handler(lv_event_t *e);
void _cancel_click_handler(lv_event_t *e);

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  // No "PRINTING" heading. This page is only ever reached by swiping off the
  // printing screen, so the title said nothing the user did not already know
  // while occupying the only band of the circle the buttons leave free. The
  // progress readout takes that space instead - the fill carries progress on
  // the previous page, and it is not visible from here.
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, RES_H, RES_V);

  _paused = state.paused;

  _progress = lv_label_create(page);
  lv_obj_set_style_text_font(_progress, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(_progress, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(_progress, LV_ALIGN_TOP_MID, 0, 14);

  // Symbols rather than words. LVGL carries these glyphs already, so they cost
  // nothing, and a triangle or a square is read faster than PAUSE or CANCEL -
  // which matters most for the button you reach for when a print is going
  // wrong. It also matches what Mainsail shows for the same actions.
  _pause = page_helper::create_center_button(
      page,
      0, -38,
      160, 60,
      LV_SYMBOL_PAUSE,
      _pause_click_handler
  );
  lv_obj_set_style_text_font(lv_obj_get_child(_pause, 0), &lv_font_montserrat_24, LV_PART_MAIN);
  _update_pause();

  _cancel = page_helper::create_center_button(
      page,
      0, 28,
      160, 60,
      LV_SYMBOL_STOP,
      _cancel_click_handler
  );
  lv_obj_set_style_text_font(lv_obj_get_child(_cancel, 0), &lv_font_montserrat_24, LV_PART_MAIN);
  page_helper::set_button_color(_cancel, COLOR_CANCEL_BG);

  return page;
}

void printer_update(const printer::State &state) {
  lv_label_set_text_fmt(_progress, "%d%%", (int)state.progress);

  if (state.paused != _paused) {
    _paused = state.paused;
    _update_pause();
    lv_obj_clear_state(_pause, LV_STATE_DISABLED);
    lv_obj_clear_state(_cancel, LV_STATE_DISABLED);
  };
}

void _update_pause() {
  lv_obj_t *label = lv_obj_get_child(_pause, 0);
  if (_paused) {
    lv_label_set_text(label, LV_SYMBOL_PLAY);
    page_helper::set_button_color(_pause, COLOR_RESUME_BG);
  } else {
    lv_label_set_text(label, LV_SYMBOL_PAUSE);
    page_helper::set_button_color(_pause, COLOR_PAUSE_BG);
  }
}

void _pause_click_handler(lv_event_t *e) {
  if (_paused) {
    printer::send::send_gcode("RESUME");
  } else {
    printer::send::send_gcode("PAUSE");
  }
  lv_obj_add_state(_pause, LV_STATE_DISABLED);
  lv_obj_add_state(_cancel, LV_STATE_DISABLED);
}

void _cancel_click_handler(lv_event_t *e) {
  printer::send::send_gcode("CANCEL_PRINT");
  lv_obj_add_state(_pause, LV_STATE_DISABLED);
  lv_obj_add_state(_cancel, LV_STATE_DISABLED);
}

}
}