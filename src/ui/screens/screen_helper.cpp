#include "screen_helper.h"

#include "board_conf.h"
#include "user_conf.h"

namespace ui {
namespace screen_helper {

namespace {

//: Set while the wrap adjusts the scroll itself, because that adjustment ends a
//: scroll and would otherwise call this straight back.
bool _wrapping = false;

void _wrap_handler(lv_event_t *e) {
  if (_wrapping) {
    return;
  }

  lv_obj_t *scr = (lv_obj_t *)lv_event_get_target(e);
  uint32_t count = lv_obj_get_child_count(scr);

  // Two pages cannot wrap: whichever one you settle on is both the first and
  // the last, so every rotation would immediately qualify for the opposite
  // rotation. With two pages a swipe each way already reaches everything.
  if (count < 3) {
    return;
  }

  const int32_t page_w = RES_H;
  int32_t scroll = lv_obj_get_scroll_x(scr);
  int32_t index = (scroll + page_w / 2) / page_w;

  int32_t shift = 0;
  if (index <= 0) {
    // Sitting on the first page: bring the last one round to sit before it, so
    // there is something to swipe back to.
    lv_obj_move_to_index(lv_obj_get_child(scr, count - 1), 0);
    shift = page_w;
  } else if (index >= (int32_t)count - 1) {
    lv_obj_move_to_index(lv_obj_get_child(scr, 0), count - 1);
    shift = -page_w;
  } else {
    return;
  }

  // The row is the same width as before, but everything in it moved by one
  // page, so the scroll has to move with it or the page under your finger jumps
  // sideways. Layout first, or the scroll is clamped against stale geometry.
  _wrapping = true;
  lv_obj_update_layout(scr);
  lv_obj_scroll_to_x(scr, scroll + shift, LV_ANIM_OFF);
  _wrapping = false;
}

}

lv_obj_t *create_screen() {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_set_scroll_dir(scr, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(scr, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(scr, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);

  lv_obj_add_event_cb(scr, _wrap_handler, LV_EVENT_SCROLL_END, nullptr);
  return scr;
}

}
}
