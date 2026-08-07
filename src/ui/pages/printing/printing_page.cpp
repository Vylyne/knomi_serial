#include "printing_page.h"

#include <lvgl.h>

#include "board_conf.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace printing_page {

static lv_obj_t *_fill = nullptr;
static lv_obj_t *_pct = nullptr;
static lv_obj_t *_sub = nullptr;
static lv_obj_t *_tool = nullptr;
static lv_obj_t *_dot_l = nullptr;
static lv_obj_t *_dot_r = nullptr;
static lv_obj_t *_scrim_pct = nullptr;
static lv_obj_t *_scrim_sub = nullptr;
static lv_obj_t *_scrim_tool = nullptr;

static lv_obj_t *_init_dot(lv_obj_t *parent);
static lv_obj_t *_init_scrim(lv_obj_t *parent);
static void _fit_scrim(lv_obj_t *scrim, lv_obj_t *label, int32_t pad_x, int32_t pad_y);

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, RES_H, RES_V);

  // The fill is a plain rectangle rising from the bottom, with no circular
  // mask: the GC9A01 is a round panel, so pixels outside the inscribed circle
  // do not exist on the glass. The hardware does the clipping for free, and a
  // full-screen mask every frame is exactly what this screen cannot afford.
  _fill = lv_obj_create(page);
  lv_obj_remove_style_all(_fill);
  lv_obj_remove_flag(_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(_fill, RES_H);
  lv_obj_set_height(_fill, 0);
  lv_obj_align(_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(_fill, LV_OPA_COVER, LV_PART_MAIN);

  // Every readout gets a scrim - a soft dark pill sized to the text, drawn over
  // the fill and under the label.
  //
  // Flipping the ink black or white by luminance cannot work here. Each label
  // crosses the rising waterline at a different progress, and the 48px numeral
  // takes about a fifth of a print to cross it: for all of that it is half over
  // fill and half over black, where neither ink survives. A blend mode is no
  // better - difference inverts, and a mid-grey filament inverts to mid-grey
  // and disappears.
  //
  // Black on black is a no-op, so a scrim is invisible until the fill is
  // actually behind it. It costs nothing while the screen is dark and darkens
  // only the few hundred pixels the text needs.
  _scrim_tool = _init_scrim(page);
  _tool = lv_label_create(page);
  lv_obj_set_style_text_font(_tool, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(_tool, LV_ALIGN_TOP_MID, 0, 30);

  // The active marker is two drawn dots rather than characters around the
  // label. LVGL's Montserrat carries ASCII and its own symbols, so a middot
  // would render as a placeholder box - and flanking dots keep the label
  // optically centred instead of shoving it sideways when a tool goes active.
  _dot_l = _init_dot(page);
  _dot_r = _init_dot(page);

  _scrim_pct = _init_scrim(page);
  _pct = lv_label_create(page);
  lv_obj_set_style_text_font(_pct, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_align(_pct, LV_ALIGN_CENTER, 0, -6);

  _scrim_sub = _init_scrim(page);
  _sub = lv_label_create(page);
  lv_obj_set_style_text_font(_sub, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(_sub, LV_ALIGN_CENTER, 0, 34);

  // White throughout: the scrim guarantees the ground, so nothing has to be
  // chosen against the filament colour.
  lv_obj_set_style_text_color(_pct, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_color(_sub, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(_sub, LV_OPA_80, LV_PART_MAIN);
  lv_obj_set_style_text_color(_tool, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(_tool, LV_OPA_80, LV_PART_MAIN);

  printer_update(state);
  return page;
}

static lv_obj_t *_init_scrim(lv_obj_t *parent) {
  lv_obj_t *scrim = lv_obj_create(parent);
  lv_obj_remove_style_all(scrim);
  lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(scrim, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(scrim, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scrim, SCRIM_OPA, LV_PART_MAIN);
  return scrim;
}

static void _fit_scrim(lv_obj_t *scrim, lv_obj_t *label, int32_t pad_x, int32_t pad_y) {
  if (lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(scrim, LV_OBJ_FLAG_HIDDEN);
  // The label's size is only correct once layout has caught up with the text
  // that was just set into it.
  lv_obj_update_layout(label);
  lv_obj_set_size(
      scrim,
      lv_obj_get_width(label) + pad_x * 2,
      lv_obj_get_height(label) + pad_y * 2);
  lv_obj_align_to(scrim, label, LV_ALIGN_CENTER, 0, 0);
}

static lv_obj_t *_init_dot(lv_obj_t *parent) {
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 5, 5);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(dot, theme::machine(), LV_PART_MAIN);
  return dot;
}

void printer_update(const printer::State &state) {
  lv_obj_set_style_bg_color(_fill, theme::filament(state), LV_PART_MAIN);

  int32_t pct = state.progress;
  if (pct < 0) {
    pct = 0;
  }
  if (pct > 100) {
    pct = 100;
  }
  lv_obj_set_height(_fill, pct * RES_V / 100);

  lv_label_set_text_fmt(_pct, "%d%%", (int)pct);

  if (state.filament_type[0] != '\0') {
    lv_label_set_text_fmt(_sub, "%s   %d", state.filament_type, (int)state.hotend_temp);
  } else {
    lv_label_set_text_fmt(_sub, "%d", (int)state.hotend_temp);
  }

  if (state.tool_number >= 0) {
    lv_label_set_text_fmt(_tool, "T%d", (int)state.tool_number);
    lv_obj_remove_flag(_tool, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(_tool, LV_OBJ_FLAG_HIDDEN);
  }

  bool show_dots = state.active && state.tool_number >= 0;
  lv_obj_align_to(_dot_l, _tool, LV_ALIGN_OUT_LEFT_MID, -7, 1);
  lv_obj_align_to(_dot_r, _tool, LV_ALIGN_OUT_RIGHT_MID, 7, 1);
  if (show_dots) {
    lv_obj_remove_flag(_dot_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(_dot_r, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(_dot_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_dot_r, LV_OBJ_FLAG_HIDDEN);
  }

  _fit_scrim(_scrim_pct, _pct, 14, 4);
  _fit_scrim(_scrim_sub, _sub, 12, 3);
  // Wide enough to take the dots too: machine pink over pink filament would
  // otherwise disappear exactly when the fill reaches the top of the screen.
  _fit_scrim(_scrim_tool, _tool, show_dots ? 26 : 12, 4);
}

}
}
