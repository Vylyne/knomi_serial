#include "screen_helper.h"

#include "board_conf.h"
#include "user_conf.h"

namespace ui {
namespace screen_helper {

namespace {

//: Set across the wrap's own scroll. That call ends a scroll and sends
//: LV_EVENT_SCROLL_END back synchronously, which would otherwise queue another
//: wrap on top of the one still running.
bool _wrapping = false;

//: Runs after a scroll to watch for the row coming to rest. Cancelled if the
//: user starts another, and if the screen it belongs to is deleted.
lv_timer_t *_settle = nullptr;

void _stop_settle() {
  if (_settle) {
    lv_timer_delete(_settle);
    _settle = nullptr;
  }
}

void _rotate(lv_obj_t *scr, int32_t scroll) {
  uint32_t count = lv_obj_get_child_count(scr);
  if (count < 2) {
    return;
  }

  const int32_t page_w = RES_H;
  int32_t index = (scroll + page_w / 2) / page_w;

  int32_t landed;
  if (index <= 0) {
    // Resting on the first page: bring the last one round to sit before it, so
    // there is something to the left to swipe onto.
    lv_obj_move_to_index(lv_obj_get_child(scr, count - 1), 0);
    landed = index + 1;
  } else if (index >= (int32_t)count - 1) {
    lv_obj_move_to_index(lv_obj_get_child(scr, 0), count - 1);
    landed = index - 1;
  } else {
    return;
  }

  // Scroll to the page's new index exactly, not to "wherever we were plus a
  // page". Reading the live position and adding to it was what left the row
  // parked between two pages: LV_EVENT_SCROLL_END arrives while the snap
  // animation is still easing, so that position is mid-flight, and scrolling
  // with LV_ANIM_OFF deletes the snap animation on its way past.
  _wrapping = true;
  lv_obj_update_layout(scr);
  lv_obj_scroll_to_x(scr, landed * page_w, LV_ANIM_OFF);
  _wrapping = false;
}

void _settle_tick(lv_timer_t *timer) {
  lv_obj_t *scr = (lv_obj_t *)lv_timer_get_user_data(timer);
  const int32_t page_w = RES_H;

  int32_t scroll = lv_obj_get_scroll_x(scr);
  int32_t off = scroll % page_w;
  if (off < 0) {
    off += page_w;
  }

  if (off > WRAP_SNAP_TOLERANCE && off < page_w - WRAP_SNAP_TOLERANCE) {
    // Still easing. LVGL snaps by at most one page, from mid-drag to a
    // boundary, so the only time this reads as aligned is when it has arrived.
    return;
  }

  _settle = nullptr;
  lv_timer_delete(timer);
  _rotate(scr, scroll);
}

void _scroll_begin(lv_event_t *e) {
  _stop_settle();
}

void _scroll_end(lv_event_t *e) {
  if (_wrapping) {
    return;
  }
  _stop_settle();

  // Polled rather than delayed by a fixed amount: the snap runs anywhere from
  // 200 to 400ms, so a wait long enough to always be safe is long enough to be
  // felt between quick swipes.
  _settle = lv_timer_create(
      _settle_tick, WRAP_SETTLE_TICK_MS, (lv_obj_t *)lv_event_get_target(e));
  lv_timer_set_repeat_count(_settle, WRAP_SETTLE_TICKS);
}

void _screen_deleted(lv_event_t *e) {
  // A pending timer holds a pointer to this screen, and screens are deleted out
  // from under it whenever the printer changes state.
  _stop_settle();
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

  lv_obj_add_event_cb(scr, _scroll_begin, LV_EVENT_SCROLL_BEGIN, nullptr);
  lv_obj_add_event_cb(scr, _scroll_end, LV_EVENT_SCROLL_END, nullptr);
  lv_obj_add_event_cb(scr, _screen_deleted, LV_EVENT_DELETE, nullptr);
  return scr;
}

}
}
