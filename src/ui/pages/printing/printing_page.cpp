#include "printing_page.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

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

//: Remembers the material the sub-line scrim was last sized for, so it is
//: measured again when the spool changes rather than on every packet.
//:
//: Cleared in init(). It outlives the object it describes - a status change
//: reloads the screen and every scrim on it is built anew - so a cache left
//: standing would report the fresh one as already sized and it would never get
//: a size at all.
static char _sized_for[printer::kFilamentTypeMaxLen + 1] = {0};

static lv_obj_t *_init_dot(lv_obj_t *parent);
static lv_obj_t *_init_scrim(lv_obj_t *parent);
static void _size_scrim(
    lv_obj_t *scrim, const lv_font_t *font, const char *widest,
    int32_t pad_x, int32_t pad_y, lv_align_t align, int32_t y);

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
  // 46, not 34: at 34 the two scrims met at the same pixel and read as one
  // shape with a seam. This clears the percentage scrim by about 12px.
  lv_obj_align(_sub, LV_ALIGN_CENTER, 0, 46);

  // White throughout: the scrim guarantees the ground, so nothing has to be
  // chosen against the filament colour.
  lv_obj_set_style_text_color(_pct, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_color(_sub, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(_sub, LV_OPA_80, LV_PART_MAIN);
  lv_obj_set_style_text_color(_tool, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(_tool, LV_OPA_80, LV_PART_MAIN);

  // Fixed from here on. "100%" is the widest the percentage ever gets, and the
  // tool scrim is always sized as though the active dots were showing - a
  // slightly generous pill on an idle tool costs nothing, while resizing when a
  // tool takes over would be one more thing moving on screen.
  _size_scrim(_scrim_pct, &lv_font_montserrat_48, "100%", 14, 4, LV_ALIGN_CENTER, -6);
  _size_scrim(_scrim_tool, &lv_font_montserrat_16, "T00", 26, 4, LV_ALIGN_TOP_MID, 30);

  // The sub-line scrim is sized from the material, so it is the one printer_update
  // owns. Forget what the previous incarnation of this page was showing, or the
  // scrim created three lines above never gets measured.
  _sized_for[0] = '\0';

  printer_update(state);
  return page;
}

static lv_obj_t *_init_scrim(lv_obj_t *parent) {
  lv_obj_t *scrim = lv_obj_create(parent);
  lv_obj_remove_style_all(scrim);
  lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
  // Start at nothing. An lv_obj defaults to LV_DPI_DEF square at the top-left
  // corner, so a scrim that misses its sizing draws a large circle in the upper
  // left rather than not drawing - which is how this went unnoticed. Failing
  // invisibly is the right failure for a mask.
  lv_obj_set_size(scrim, 0, 0);
  lv_obj_set_style_radius(scrim, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(scrim, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scrim, SCRIM_OPA, LV_PART_MAIN);
  return scrim;
}

// Sized to the widest string the label will ever hold, not to what it holds
// right now. Montserrat is proportional, so measuring live made the scrim
// breathe on every digit - "9%" to "10%" to "100%" - which draws the eye to the
// mask instead of the number. The label stays centred inside a scrim that does
// not move, so only the digits change.
//
// Both are aligned to the same anchor rather than the scrim being aligned to
// the label, which keeps them concentric whatever the text measures and drops a
// forced layout pass out of the update path.
static void _size_scrim(
    lv_obj_t *scrim, const lv_font_t *font, const char *widest,
    int32_t pad_x, int32_t pad_y, lv_align_t align, int32_t y) {
  lv_point_t size;
  lv_text_get_size(&size, widest, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

  // filament_type allows 15 characters. Nothing real is longer than "NYLON",
  // but an odd one would otherwise stretch the pill past the edge of the glass.
  int32_t w = size.x + pad_x * 2;
  if (w > RES_H - 30) {
    w = RES_H - 30;
  }
  lv_obj_set_size(scrim, w, size.y + pad_y * 2);
  if (align == LV_ALIGN_TOP_MID) {
    lv_obj_align(scrim, align, 0, y - pad_y);
  } else {
    lv_obj_align(scrim, align, 0, y);
  }
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

  // The readout takes the heat colour while something is being asked of the
  // heater, and plain ink when nothing is. The scrim guarantees a dark ground
  // underneath, so even the cool end of the ramp stays legible.
  lv_obj_set_style_text_color(
      _sub,
      state.hotend_target > 0 ? theme::heat(state.hotend_temp, state.hotend_target)
                              : lv_color_white(),
      LV_PART_MAIN);

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

  // The sub-line is the one width that genuinely varies, because the material
  // name does. Measured against a three-digit temperature so the digits never
  // move it, and only re-measured when the spool actually changes - which is
  // once a print, not ten times a second.
  if (strncmp(_sized_for, state.filament_type, sizeof(_sized_for) - 1) != 0) {
    strncpy(_sized_for, state.filament_type, sizeof(_sized_for) - 1);
    _sized_for[sizeof(_sized_for) - 1] = '\0';

    char widest[printer::kFilamentTypeMaxLen + 8];
    if (_sized_for[0] != '\0') {
      snprintf(widest, sizeof(widest), "%s   888", _sized_for);
    } else {
      snprintf(widest, sizeof(widest), "888");
    }
    _size_scrim(_scrim_sub, &lv_font_montserrat_16, widest, 12, 3, LV_ALIGN_CENTER, 46);
  }

  if (lv_obj_has_flag(_tool, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_add_flag(_scrim_tool, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(_scrim_tool, LV_OBJ_FLAG_HIDDEN);
  }
}

}
}
