#include "screen_helper.h"

#include "board_conf.h"
#include "ui/ui.h"
#include "user_conf.h"

namespace ui {
namespace screen_helper {

namespace {

void _scroll_end(lv_event_t *e) {
  // Only the page in view and its neighbours are kept current, and only when
  // the state changes - so a page arriving after two swipes on a still printer
  // was never a neighbour and has heard nothing. Hand it the last state.
  //
  // No need to wait for the snap to finish: the neighbours either side are
  // updated too, so whichever page this rounds to, the one that ends up in view
  // has been refreshed.
  refresh();
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

  lv_obj_add_event_cb(scr, _scroll_end, LV_EVENT_SCROLL_END, nullptr);
  return scr;
}

}
}
