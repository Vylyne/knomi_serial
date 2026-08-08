#include "corner.h"

#include "board_conf.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace corner {

namespace {

bool is_left(Slot slot) {
  return slot == Slot::kNW || slot == Slot::kSW;
}

bool is_top(Slot slot) {
  return slot == Slot::kNW || slot == Slot::kNE;
}

//: Where the mark goes: on the diagonal, at the bezel, where the key is.
void mark_offset(Slot slot, int32_t *x, int32_t *y) {
  *x = is_left(slot) ? -CORNER_OFFSET : CORNER_OFFSET;
  *y = is_top(slot) ? -CORNER_OFFSET : CORNER_OFFSET;
}

//: Where the hit area goes, in page coordinates. Hugs its corner, stops short
//: of the middle in both axes - so the bottom centre of the glass, where the
//: readout sits, belongs to nothing.
void region_pos(Slot slot, int32_t *x, int32_t *y) {
  *x = is_left(slot) ? CORNER_TOUCH_INSET
                     : RES_H - CORNER_TOUCH_W - CORNER_TOUCH_INSET;
  *y = is_top(slot) ? CORNER_TOUCH_INSET
                    : RES_V - CORNER_TOUCH_H - CORNER_TOUCH_INSET;
}

}

lv_obj_t *create(
    lv_obj_t *parent, Slot slot, const char *symbol, lv_color_t color,
    lv_event_cb_t cb) {
  // The hit area, and nothing to look at. Created before the mark so it never
  // draws over it, and a sibling rather than its parent so the mark can sit on
  // the diagonal without being clipped to the region's box.
  lv_obj_t *region = lv_obj_create(parent);
  lv_obj_remove_style_all(region);
  lv_obj_remove_flag(region, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(region, CORNER_TOUCH_W, CORNER_TOUCH_H);

  int32_t rx = 0, ry = 0;
  region_pos(slot, &rx, &ry);
  lv_obj_align(region, LV_ALIGN_TOP_LEFT, rx, ry);

#if CORNER_KEYS_TOUCH
  if (cb) {
    lv_obj_add_flag(region, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(region, cb, LV_EVENT_CLICKED, nullptr);
  }
#else
  // With a key fitted, the glass beside it should do nothing - two ways to fire
  // the same action, one of them invisible, is a way to fire it by accident.
  lv_obj_remove_flag(region, LV_OBJ_FLAG_CLICKABLE);
  (void)cb;
#endif

  // The mark. A scrim disc so it survives whatever the fill is doing behind it,
  // and a symbol in the colour of the action. Small on purpose - it is a label
  // for a key, not the key.
  lv_obj_t *mark = lv_obj_create(parent);
  lv_obj_remove_style_all(mark);
  lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(mark, CORNER_SIZE, CORNER_SIZE);
  lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(mark, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(mark, SCRIM_OPA, LV_PART_MAIN);

  int32_t mx = 0, my = 0;
  mark_offset(slot, &mx, &my);
  lv_obj_align(mark, LV_ALIGN_CENTER, mx, my);

  lv_obj_t *label = lv_label_create(mark);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_center(label);

  set(mark, symbol, color);
  return mark;
}

void set(lv_obj_t *mark, const char *symbol, lv_color_t color) {
  if (!mark) {
    return;
  }
  lv_obj_t *label = lv_obj_get_child(mark, 0);
  if (!label) {
    return;
  }
  lv_label_set_text(label, symbol);
  // The symbol carries the colour. The disc behind it stays neutral, so a
  // control changing meaning changes one thing rather than two.
  lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
}

}
}
