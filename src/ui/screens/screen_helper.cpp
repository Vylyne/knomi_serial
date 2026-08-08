#include "screen_helper.h"

#include "board_conf.h"
#include "ui/ui.h"
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

//: Counted here rather than left to lv_timer_set_repeat_count. LVGL deletes a
//: timer itself once its repeat count runs out, which would leave the pointer
//: above dangling for the next cancel to free a second time.
int32_t _settle_ticks = 0;

void _stop_settle() {
  if (_settle) {
    lv_timer_delete(_settle);
    _settle = nullptr;
  }
  _settle_ticks = 0;
}

//: Where the page you are looking at is kept. One place in from the start, so
//: there is always a page to its left and, with three or more, at least one to
//: its right.
const int32_t kHome = 1;

void _rotate(lv_obj_t *scr, int32_t scroll) {
  uint32_t count = lv_obj_get_child_count(scr);

  // Two pages cannot wrap in both directions. Symmetry needs a page to the left
  // and to the right, and with two the other one can occupy a side or the other
  // but not both at once - that wants a second copy of it, and pages keep their
  // widgets in file statics, so a copy would overwrite the original. Two pages
  // fall back to ordinary bounded scrolling, which already reaches everything
  // in one swipe each way.
  if (count < 3) {
    return;
  }

  const int32_t page_w = RES_H;
  int32_t index = (scroll + page_w / 2) / page_w;
  int32_t shift = index - kHome;
  if (shift == 0) {
    return;
  }

  // Re-centred after every settle, not only on reaching an end. Rotating only
  // at the ends put a page on one side and left the other a wall: you swiped,
  // bounced, waited for the rotation, and swiped again. Both neighbours are
  // present before every gesture now.
  if (shift > 0) {
    for (int32_t i = 0; i < shift; i++) {
      lv_obj_move_to_index(lv_obj_get_child(scr, 0), count - 1);
    }
  } else {
    for (int32_t i = 0; i < -shift; i++) {
      lv_obj_move_to_index(lv_obj_get_child(scr, count - 1), 0);
    }
  }

  // Scroll to the home slot exactly, not to "wherever we were plus a page".
  // Reading the live position and adding to it was what left the row parked
  // between two pages: LV_EVENT_SCROLL_END arrives while the snap animation is
  // still easing, so that position is mid-flight, and scrolling with
  // LV_ANIM_OFF deletes the snap animation on its way past.
  _wrapping = true;
  lv_obj_update_layout(scr);
  lv_obj_scroll_to_x(scr, kHome * page_w, LV_ANIM_OFF);
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
    if (++_settle_ticks < WRAP_SETTLE_TICKS) {
      return;
    }
    // Never came to rest. Leave the row exactly as the user left it rather than
    // rotating against a position that is still moving.
    _stop_settle();
    return;
  }

  _stop_settle();
  _rotate(scr, scroll);

  // The page that just arrived may not have been a neighbour when the state
  // last changed, so show it what it missed.
  refresh();
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
  _settle_ticks = 0;
  _settle = lv_timer_create(
      _settle_tick, WRAP_SETTLE_TICK_MS, (lv_obj_t *)lv_event_get_target(e));
}

void _screen_deleted(lv_event_t *e) {
  // A pending timer holds a pointer to this screen, and screens are deleted out
  // from under it whenever the printer changes state.
  _stop_settle();
}

}

void tag_pages(lv_obj_t *scr) {
  if (!scr) {
    return;
  }
  uint32_t count = lv_obj_get_child_count(scr);
  for (uint32_t i = 0; i < count; i++) {
    // Offset by one so that zero keeps meaning "never stamped".
    lv_obj_set_user_data(lv_obj_get_child(scr, i), (void *)(intptr_t)(i + 1));
  }
}

int visible_page(lv_obj_t *scr) {
  if (!scr) {
    return 0;
  }
  uint32_t count = lv_obj_get_child_count(scr);
  if (count == 0) {
    return 0;
  }

  int32_t slot = (lv_obj_get_scroll_x(scr) + RES_H / 2) / RES_H;
  if (slot < 0) {
    slot = 0;
  }
  if (slot >= (int32_t)count) {
    slot = (int32_t)count - 1;
  }

  intptr_t tag = (intptr_t)lv_obj_get_user_data(lv_obj_get_child(scr, slot));
  return tag > 0 ? (int)(tag - 1) : (int)slot;
}

void update_visible(
    lv_obj_t *scr, const printer::State &state,
    const page_update_t *updates, uint32_t count) {
  if (!scr || !updates) {
    return;
  }
  uint32_t children = lv_obj_get_child_count(scr);
  if (children == 0) {
    return;
  }

  int32_t slot = (lv_obj_get_scroll_x(scr) + RES_H / 2) / RES_H;

  for (int32_t offset = -1; offset <= 1; offset++) {
    int32_t at = slot + offset;
    if (at < 0 || at >= (int32_t)children) {
      continue;
    }
    intptr_t tag = (intptr_t)lv_obj_get_user_data(lv_obj_get_child(scr, at));
    if (tag <= 0 || (uint32_t)(tag - 1) >= count) {
      continue;
    }
    page_update_t fn = updates[tag - 1];
    if (fn) {
      fn(state);
    }
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
