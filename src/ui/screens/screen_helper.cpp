#include "screen_helper.h"

#include "board_conf.h"
#include "input/button_input.h"
#include "printer/config.h"
#include "ui/ui.h"
#include "user_conf.h"

namespace ui {
namespace screen_helper {

namespace {

//: Marks the page row among the screen's children, so which one it is never
//: depends on what order they were added in. Pages carry their own tags from
//: tag_pages, but those are children of the row rather than of the screen, so
//: the two namespaces cannot collide.
const intptr_t kRowTag = -1;
const intptr_t kOverlayTag = -2;
lv_obj_t *_overlay_exclusion = nullptr;

lv_obj_t *_child_tagged(lv_obj_t *parent, intptr_t tag) {
  if (!parent) {
    return nullptr;
  }
  uint32_t count = lv_obj_get_child_count(parent);
  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t *child = lv_obj_get_child(parent, i);
    if ((intptr_t)lv_obj_get_user_data(child) == tag) {
      return child;
    }
  }
  return nullptr;
}

void _scroll_end(lv_event_t *e) {
  (void)e;
  // Only the page in view and its neighbours are kept current, and only when
  // the state changes - so a page arriving after two swipes on a still printer
  // was never a neighbour and has heard nothing. Hand it the last state.
  //
  // No need to wait for the snap to finish: the neighbours either side are
  // updated too, so whichever page this rounds to, the one that ends up in view
  // has been refreshed.
  refresh();
}

void _scroll(lv_event_t *e) {
  lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
  lv_obj_t *scr = lv_obj_get_screen(target);
  update_overlay(scr);
}

}

void tag_pages(lv_obj_t *scr) {
  if (!scr) {
    return;
  }
  lv_obj_t *overlay = _child_tagged(scr, kOverlayTag);
  if (overlay) {
    // The overlay is a floating row child so a drag on a shared touch button
    // can still reach the row's horizontal scroll. Keep it above page marks.
    lv_obj_move_foreground(overlay);
  }
  uint32_t count = lv_obj_get_child_count(scr);
  for (uint32_t i = 0; i < count; i++) {
    if (lv_obj_get_child(scr, i) == overlay) {
      continue;
    }
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
  _overlay_exclusion = nullptr;
  // Two axes. The screen scrolls vertically between exactly two things - the
  // row of pages, and whatever is pulled down to - while the row inside it
  // scrolls horizontally between the pages themselves.
  //
  // LVGL routes this on its own: a drag walks up from whatever was touched
  // looking for an ancestor that scrolls in the direction the finger started
  // moving, so horizontal finds the row and vertical passes it and finds the
  // screen. Nothing has to arbitrate between them.
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_set_scroll_dir(scr, LV_DIR_VER);
  lv_obj_set_scroll_snap_y(scr, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);

  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, RES_H, RES_V);
  lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_set_scroll_dir(row, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(row, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(row, 0, LV_PART_MAIN);

  lv_obj_add_event_cb(row, _scroll_end, LV_EVENT_SCROLL_END, nullptr);
  lv_obj_add_event_cb(scr, _scroll_end, LV_EVENT_SCROLL_END, nullptr);
  lv_obj_add_event_cb(row, _scroll, LV_EVENT_SCROLL, nullptr);
  lv_obj_add_event_cb(scr, _scroll, LV_EVENT_SCROLL, nullptr);
  lv_obj_set_user_data(row, (void *)kRowTag);

  // Both children exist before anyone asks about either, which is the whole
  // point of building the slot here rather than letting the caller add the
  // e-stop afterwards. That version worked out the row's index from config and
  // was asked for it before the e-stop had been created - so with the stop
  // configured above, `page_row` returned child 1 of a screen that had one
  // child, handed back null, and every page was then built with a null parent.
  lv_obj_t *slot = lv_obj_create(scr);
  lv_obj_remove_style_all(slot);
  lv_obj_set_size(slot, RES_H, RES_V);
  lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
  if (printer::config::get().estop_at == (uint8_t)printer::EstopAt::kTop) {
    lv_obj_move_to_index(slot, 0);
  }

  // Fixed to the glass rather than parented to a horizontally scrolling page:
  // shared bindings label the physical key position on every page.
  lv_obj_t *overlay = lv_obj_create(row);
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, RES_H, RES_V);
  lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_user_data(overlay, (void *)kOverlayTag);
  input::button::build_overlay(overlay);

  // Park on the row whichever side the stop ended up, so a screen never opens
  // already showing it.
  lv_obj_update_layout(scr);
  lv_obj_scroll_to_y(scr, lv_obj_get_y(row), LV_ANIM_OFF);
  return scr;
}

void register_overlay_exclusion(lv_obj_t *page) {
  _overlay_exclusion = page;
}

void update_overlay(lv_obj_t *scr) {
  lv_obj_t *row = page_row(scr);
  lv_obj_t *overlay = _child_tagged(row, kOverlayTag);
  if (!overlay || !row) {
    return;
  }
  lv_area_t screen_area, row_area;
  lv_obj_get_coords(scr, &screen_area);
  lv_obj_get_coords(row, &row_area);
  bool on_row = row_area.y1 <= screen_area.y1 &&
      row_area.y2 >= screen_area.y2;
  bool excluded = false;
  if (on_row && _overlay_exclusion) {
    lv_area_t page_area;
    lv_obj_get_coords(_overlay_exclusion, &page_area);
    int32_t middle = (screen_area.x1 + screen_area.x2) / 2;
    excluded = page_area.x1 <= middle && page_area.x2 >= middle;
  }
  input::button::set_overlay_touch_compact(excluded);
  input::button::set_overlay_touch_enabled(on_row);
  if (on_row) {
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  }
}

lv_obj_t *page_row(lv_obj_t *scr) {
  return _child_tagged(scr, kRowTag);
}

lv_obj_t *estop_slot(lv_obj_t *scr) {
  // The one that is not the row. Found by elimination rather than by index, so
  // it cannot disagree with page_row about which is which.
  if (!scr) {
    return nullptr;
  }
  uint32_t count = lv_obj_get_child_count(scr);
  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t *child = lv_obj_get_child(scr, i);
    intptr_t tag = (intptr_t)lv_obj_get_user_data(child);
    if (tag != kRowTag && tag != kOverlayTag) {
      return child;
    }
  }
  return nullptr;
}

}
}
