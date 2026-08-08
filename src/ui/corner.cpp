#include "corner.h"

#include "board_conf.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace corner {

namespace {

//: Diagonal offset from centre. At 62 the control's outer edge lands about
//: 114px out on a 120px radius - clear of the bezel, clear of the readout.
const int32_t kOffset = 62;

void slot_offset(Slot slot, int32_t *x, int32_t *y) {
  switch (slot) {
    case Slot::kNW: *x = -kOffset; *y = -kOffset; return;
    case Slot::kNE: *x = kOffset;  *y = -kOffset; return;
    case Slot::kSW: *x = -kOffset; *y = kOffset;  return;
    case Slot::kSE: *x = kOffset;  *y = kOffset;  return;
  }
  *x = 0;
  *y = 0;
}

}

lv_obj_t *create(
    lv_obj_t *parent, Slot slot, const char *symbol, lv_color_t color,
    lv_event_cb_t cb) {
  int32_t x = 0, y = 0;
  slot_offset(slot, &x, &y);

#if CORNER_KEYS_TOUCH
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_size(btn, CORNER_SIZE, CORNER_SIZE);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  if (cb) {
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  }
#else
  // A legend, not a control: the key beside the glass is the control. No
  // chrome, nothing to press, and nothing that invites a press.
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, CORNER_SIZE, CORNER_SIZE);
  lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  (void)cb;
#endif

  lv_obj_align(btn, LV_ALIGN_CENTER, x, y);

  lv_obj_t *label = lv_label_create(btn);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_center(label);

  set(btn, symbol, color);
  return btn;
}

void set(lv_obj_t *btn, const char *symbol, lv_color_t color) {
  if (!btn) {
    return;
  }
  lv_obj_t *label = lv_obj_get_child(btn, 0);
  if (label) {
    lv_label_set_text(label, symbol);
  }

#if CORNER_KEYS_TOUCH
  // Filled, so the ink has to survive whatever the fill is - the same reason
  // every other button on this device derives its text colour.
  lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  if (label) {
    lv_obj_set_style_text_color(label, theme::ink_on(color), LV_PART_MAIN);
  }
#else
  // Unfilled, so the symbol carries the colour itself and sits over whatever
  // the screen is doing without a plate around it.
  if (label) {
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
  }
#endif
}

}
}
