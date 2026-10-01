#ifndef ACTION_CONTROL_H
#define ACTION_CONTROL_H

#include <lvgl.h>

#include "input/button_input.h"

namespace ui {
namespace action_control {

struct Control {
  lv_obj_t *target = nullptr;
  lv_obj_t *mark = nullptr;
  lv_obj_t *label = nullptr;
};

//: Offsets are measured from the centre of a 240-pixel page. A page slot routes
//: touch through the normalized page resolver; kNone calls action directly.
Control create(
    lv_obj_t *parent, int32_t x, int32_t y, int32_t target_size,
    int32_t mark_size, const char *symbol, const lv_font_t *font,
    lv_color_t ink, bool touch, printer::ButtonSlot page_slot,
    input::button::page_action_t action = nullptr, void *context = nullptr);

void set_enabled(const Control &control, bool enabled);
void set_fill(const Control &control, lv_color_t fill, lv_color_t ink);

}
}

#endif
