#include "tool_page.h"

#include <stdio.h>
#include <string.h>

#include "board_conf.h"
#include "printer/send/send_cmd.h"
#include "ui/corner.h"
#include "ui/readouts.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace tool_page {

static lv_obj_t *_tag = nullptr;
static lv_obj_t *_dot_l = nullptr;
static lv_obj_t *_dot_r = nullptr;
static readouts::Row _aux;
static lv_obj_t *_hero = nullptr;
static lv_obj_t *_target = nullptr;
static lv_obj_t *_pool = nullptr;
static lv_obj_t *_material = nullptr;
static lv_obj_t *_key_load = nullptr;
static lv_obj_t *_key_unload = nullptr;

// Everything below is the last value written to the widget above it, so an
// update that changes nothing touches nothing.
//
// This page used to be two, and one of them realigned all eight of its labels
// on every packet whether or not a row had moved - roughly ten times a second,
// to put things back exactly where they already were. Each align dirties the
// layout, and a dirty layout is a full pass over the page before the next
// frame. The readouts here are fixed to their anchors instead: LVGL keeps an
// alignment once it is set and re-runs it when the text resizes, so a centred
// numeral re-centres itself and the update path never mentions position at all.
//
// Sentinels are impossible values rather than plausible ones, so the first
// update after a rebuild always writes.
static int32_t _hot = INT32_MIN;
static int32_t _hot_target = INT32_MIN;
static int32_t _bed = INT32_MIN;
static int32_t _bed_target = INT32_MIN;
static int32_t _chamber = INT32_MIN;
static int32_t _chamber_target = INT32_MIN;
static int32_t _mcu = INT32_MIN;
static int32_t _tool = INT32_MIN;
static uint32_t _color = 0xFFFFFFFFu;
static char _type[printer::kFilamentTypeMaxLen + 1] = {0};
static int8_t _active = -1;
static int8_t _busy = -1;

static void _load_handler(lv_event_t *e);
static void _unload_handler(lv_event_t *e);
static lv_obj_t *_init_dot(lv_obj_t *parent);

//: Drop every cached value.
//:
//: A status change reloads the screen and builds this page again from nothing,
//: but the statics above are not part of the page and survive it. Left standing
//: they would describe widgets that no longer exist, and the fresh ones would
//: sit empty until the printer happened to change - which on an idle machine
//: can be a long time. This is the bug the printing page's scrim cache had.
static void _forget() {
  _hot = INT32_MIN;
  _hot_target = INT32_MIN;
  _bed = INT32_MIN;
  _bed_target = INT32_MIN;
  _chamber = INT32_MIN;
  _chamber_target = INT32_MIN;
  _mcu = INT32_MIN;
  _tool = INT32_MIN;
  _color = 0xFFFFFFFFu;
  _type[0] = '\0';
  _active = -1;
  _busy = -1;
}

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, RES_H, RES_V);

  _forget();

  // The tool tag, in the same place and the same size as the printing page's.
  // It doubles as the page title - a separate "TOOL" caption above it would say
  // less than the tag does and cost the vertical room the hero numeral needs.
  _tag = lv_label_create(page);
  lv_obj_set_style_text_font(_tag, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(_tag, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(_tag, LV_OPA_80, LV_PART_MAIN);
  lv_obj_align(_tag, LV_ALIGN_TOP_MID, 0, 30);

  _dot_l = _init_dot(page);
  _dot_r = _init_dot(page);

  // The rest of the machine, quiet and in one line. This tool's own heat is the
  // hero below; whatever `readouts:` names is context for it.
  //
  // Unscrimmed: nothing rises behind this page, and these sit near the dark top
  // of the heat haze, so a pill each would be chrome for its own sake.
  readouts::build(&_aux, page, READOUT_Y, false);

  // The hotend, carrying the heat ramp. No scrim: nothing rises behind this
  // page, so the ground under the numeral is always black and the ink can be
  // chosen freely.
  _hero = lv_label_create(page);
  lv_obj_set_style_text_font(_hero, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_align(_hero, LV_ALIGN_CENTER, 0, -12);

  // Under the hero rather than beside it. Beside reads as a fraction and
  // crowds the numeral; under it the two are plainly one reading, and there is
  // nothing rising behind this page to make the vertical room precious.
  _target = lv_label_create(page);
  lv_obj_set_style_text_font(_target, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_opa(_target, TARGET_OPA, LV_PART_MAIN);
  lv_obj_align(_target, LV_ALIGN_CENTER, 0, 30);

  // The fill at rest. On the printing page this rectangle rises with progress;
  // here it sits at the bottom as a shallow pool of whatever is loaded, so the
  // two screens are the same object at two moments rather than two designs.
  //
  // The glass narrows sharply this low down, which does the shaping for free -
  // a plain rectangle reads as a lens of colour without a mask being drawn.
  _pool = lv_obj_create(page);
  lv_obj_remove_style_all(_pool);
  lv_obj_remove_flag(_pool, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(_pool, RES_H, POOL_H);
  lv_obj_align(_pool, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(_pool, LV_OPA_COVER, LV_PART_MAIN);

  // Straight onto the pool, with no scrim. The printing page needs scrims
  // because its labels are crossed by a rising waterline and spend a long while
  // half over it; this one sits wholly inside a band that never moves, so the
  // ground is a single known colour and ink_on can simply answer for it.
  _material = lv_label_create(page);
  lv_obj_set_style_text_font(_material, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_align(_material, LV_ALIGN_BOTTOM_MID, 0, -13);

  // The two actions this page's question leads to, on the same lower diagonals
  // the printing page puts pause and cancel on.
  //
  // The tray arrows rather than plain chevrons: an arrow on its own is just a
  // direction, and up and down are the wrong axis anyway - filament goes in and
  // out, and which way that is depends on where you are standing. An arrow into
  // a tray and an arrow out of one say load and unload without needing the
  // machine's geometry agreed first.
  _key_load = corner::create(
      page, corner::Slot::kSW, LV_SYMBOL_DOWNLOAD, COLOR_LOAD, _load_handler);
  _key_unload = corner::create(
      page, corner::Slot::kSE, LV_SYMBOL_UPLOAD, COLOR_UNLOAD, _unload_handler);

  printer_update(state);
  return page;
}

static lv_obj_t *_init_dot(lv_obj_t *parent) {
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 5, 5);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(dot, theme::machine(), LV_PART_MAIN);
  lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
  return dot;
}

// Both refuse while the machine is working. The corner dims to say so rather
// than the tap being swallowed in silence, and the guard is here rather than on
// the touch region because the region is also what a physical key will replace
// - a key cannot be dimmed, so the refusal has to live behind both of them.
static void _load_handler(lv_event_t *e) {
  if (_busy == 1) {
    return;
  }
  printer::send::send_gcode("LOAD_FILAMENT");
}

static void _unload_handler(lv_event_t *e) {
  if (_busy == 1) {
    return;
  }
  printer::send::send_gcode("UNLOAD_FILAMENT");
}

void printer_update(const printer::State &state) {
  if (state.tool_number != _tool) {
    _tool = state.tool_number;
    if (_tool >= 0) {
      lv_label_set_text_fmt(_tag, "T%d", (int)_tool);
    } else {
      lv_label_set_text(_tag, "TOOL");
    }
    // Only here. The tag is the one label on this page whose width changes
    // without its text changing every packet, so this is the only place the
    // dots can have moved.
    lv_obj_align_to(_dot_l, _tag, LV_ALIGN_OUT_LEFT_MID, -7, 1);
    lv_obj_align_to(_dot_r, _tag, LV_ALIGN_OUT_RIGHT_MID, 7, 1);
  }

  int8_t active = (state.active && state.tool_number >= 0) ? 1 : 0;
  if (active != _active) {
    _active = active;
    if (active) {
      lv_obj_remove_flag(_dot_l, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(_dot_r, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(_dot_l, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(_dot_r, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (state.hotend_temp != _hot || state.hotend_target != _hot_target) {
    _hot = state.hotend_temp;
    _hot_target = state.hotend_target;

    lv_label_set_text_fmt(_hero, "%d", (int)_hot);
    lv_obj_set_style_text_color(
        _hero, theme::heat_ink(_hot, _hot_target), LV_PART_MAIN);

    if (_hot_target > 0) {
      lv_label_set_text_fmt(_target, "/ %d", (int)_hot_target);
      lv_obj_set_style_text_color(
          _target, theme::heat_ink(_hot, _hot_target), LV_PART_MAIN);
      lv_obj_remove_flag(_target, LV_OBJ_FLAG_HIDDEN);
    } else {
      // Nothing is being asked of the heater, so there is nothing to say. A
      // "/ 0" would read as a target of zero rather than as no target.
      lv_obj_add_flag(_target, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Whichever readouts are configured, formatted by ui::readouts so this page
  // and the printing page cannot drift into rendering the same numbers
  // differently.
  if (state.bed_temp != _bed || state.bed_target != _bed_target ||
      state.chamber_temp != _chamber ||
      state.chamber_target != _chamber_target || state.mcu_temp != _mcu) {
    _bed = state.bed_temp;
    _bed_target = state.bed_target;
    _chamber = state.chamber_temp;
    _chamber_target = state.chamber_target;
    _mcu = state.mcu_temp;

    // Initialised, because a machine with neither a bed nor a chamber
    // configured takes neither branch below and this would otherwise be handed
    // to the label as raw stack - read until it happened to find a zero. Every
    // printer I tested against had a bed, which is exactly why it survived.
    readouts::update(&_aux, state);
  }

  if (state.filament_color != _color) {
    _color = state.filament_color;
    lv_color_t pool = theme::filament(state);
    lv_obj_set_style_bg_color(_pool, pool, LV_PART_MAIN);
    // The label's ground just changed under it, so its ink has to be answered
    // again - this is the whole reason ink_on exists.
    lv_obj_set_style_text_color(_material, theme::ink_on(pool), LV_PART_MAIN);
  }

  if (strncmp(_type, state.filament_type, sizeof(_type) - 1) != 0) {
    strncpy(_type, state.filament_type, sizeof(_type) - 1);
    _type[sizeof(_type) - 1] = '\0';
    // An empty pool still shows its colour. Naming it "UNKNOWN" would be a
    // louder claim than the grey already makes.
    lv_label_set_text(_material, _type);
  }

  int8_t busy = state.working ? 1 : 0;
  if (busy != _busy) {
    _busy = busy;
    corner::set_enabled(_key_load, !busy);
    corner::set_enabled(_key_unload, !busy);
  }
}

}
}
