#include "init_screen.h"

#include "ui/ui.h"
#include "user_conf.h"

namespace ui
{
  namespace init_screen
  {

    static lv_obj_t *name = nullptr;

    void _printer_update_handler(const printer::State &state);

    lv_obj_t *init(const printer::State &state)
    {
      lv_obj_t *scr = lv_obj_create(nullptr);
      lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
      // A bare screen is scrollable, and one with anything near its edges can
      // be dragged around by a thumb. The page screens set a horizontal scroll
      // direction so they only ever move the way they are meant to; these
      // standalone ones had nothing saying so and slid in both.
      lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
      control::register_printer_update_cb(scr, _printer_update_handler);

      lv_obj_t *title = lv_label_create(scr);
      lv_label_set_text(title, "WAITING");
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

      lv_obj_t *spinner = lv_spinner_create(scr);
      lv_obj_set_size(spinner, 56, 56);
      lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -28);
      lv_obj_set_style_arc_width(spinner, 5, LV_PART_MAIN);
      lv_obj_set_style_arc_width(spinner, 5, LV_PART_INDICATOR);

      // Given a width, so it wraps. Without one a label is a single line as
      // long as its text, and this one carries whatever the host last said -
      // "MCU 'MCU' SHUTDOWN: LOST COMMUNICATION" ran off both sides of a round
      // screen at once.
      //
      // Anchored to the middle rather than the bottom: 170px of text does not
      // fit across the bottom of a 240px circle, where there is only about 130
      // of glass left. Growing downward from here keeps every line inside it.
      name = lv_label_create(scr);
      lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(name, 170);
      lv_label_set_text(name, PRINTER_NAME);
      lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(name, LV_ALIGN_CENTER, 0, 34);

      return scr;
    }

    void _printer_update_handler(const printer::State &state)
    {
      // Anything the host has to say while we are still waiting for it - a
      // shutdown reason, or a local fault like a malformed frame. Otherwise the
      // line keeps the printer name it was built with.
      if (state.message[0] != '\0')
      {
        lv_label_set_text(name, state.message);
      }
    }

  }
}