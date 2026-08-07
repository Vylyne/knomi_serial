#ifndef PAGE_HELPER_H
#define PAGE_HELPER_H

#include <lvgl.h>

#include "board_conf.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace page_helper {

// Set a button's fill and let its label follow.
//
// Every button colour on this device is a pale pastel, and LVGL's default label
// ink is white - so every one of them was white on near-white. Deriving the ink
// from the fill means a colour can never be changed without the text keeping up,
// which is what went wrong: the pastels were introduced in one place and the
// labels stayed at their default in another.
inline void set_button_color(lv_obj_t *btn, lv_color_t color) {
  lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);
  lv_obj_t *label = lv_obj_get_child(btn, 0);
  if (label) {
    lv_obj_set_style_text_color(label, theme::ink_on(color), LV_PART_MAIN);
  }
}

inline lv_obj_t *create_page(lv_obj_t *parent, const char *title) {
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, RES_H, RES_V);

  lv_obj_t *title_obj = lv_label_create(page);
  lv_label_set_text(title_obj, title);
  lv_obj_align(title_obj, LV_ALIGN_TOP_MID, 0, 15);

  return page;
}

inline lv_obj_t *create_center_button(
    lv_obj_t *parent,
    int x, int y,
    int w, int h,
    const char *label_text,
    lv_event_cb_t cb) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_size(btn, w, h);
  lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, label_text);
  lv_obj_center(label);

  // After the label exists, so the ink is set with it.
  set_button_color(btn, COLOR_BTN_BG);

  return btn;
}

}
}

#endif