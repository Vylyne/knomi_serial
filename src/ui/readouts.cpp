#include "readouts.h"

#include <stdio.h>

#include "board_conf.h"
#include "printer/config.h"
#include "user_conf.h"

// 12 rather than 14. Two readouts with targets at 14 filled a 240px circle at
// this height edge to edge, and the round glass takes the ends off before the
// numbers are done. Small is also right for what these are: context for the
// hero, not competition with it.
#define READOUT_FONT lv_font_montserrat_12

namespace ui {
namespace readouts {

namespace {

//: Padding either side of the text inside a pill, between pills, and clear of
//: the bezel. The last one is not decoration: a pill that ends exactly where
//: the circle does looks cut off even when every pixel of it is drawn.
const int32_t kPadX = 7;
const int32_t kPadY = 3;
const int32_t kGap = 6;
const int32_t kMargin = 10;

const char *_label(uint8_t id) {
  switch ((printer::Readout)id) {
  case printer::Readout::kBed:     return "BED";
  case printer::Readout::kChamber: return "CHM";
  case printer::Readout::kMcu:     return "MCU";
  default: return nullptr;
  }
}

struct Value {
  int32_t now;
  int32_t target;
  //: False when the machine has no such sensor, which is not the same as one
  //: reading zero - an absent bed and a bed at 0C would otherwise look alike.
  bool present;
};

Value _value(uint8_t id, const printer::State &state) {
  switch ((printer::Readout)id) {
  case printer::Readout::kBed:
    return {state.bed_temp, state.bed_target,
            state.bed_temp != 0 || state.bed_target > 0};
  case printer::Readout::kChamber:
    return {state.chamber_temp, state.chamber_target,
            state.chamber_temp != 0 || state.chamber_target > 0};
  case printer::Readout::kMcu:
    // A target only if `sensor_mcu:` names a temperature_fan. A plain
    // temperature_sensor reports zero and the target is simply not drawn.
    return {state.mcu_temp, state.mcu_target,
            state.mcu_temp != 0 || state.mcu_target > 0};
  default:
    return {0, 0, false};
  }
}

}

void build(Row *row, lv_obj_t *parent, int32_t y, bool scrimmed) {
  if (!row) {
    return;
  }
  row->count = 0;
  if (!parent) {
    return;
  }

  const printer::Config &conf = printer::config::get();

  // Measured before anything is created, because centring the row needs its
  // total width and each pill needs its own.
  int32_t width[printer::kMaxReadouts] = {0};
  int32_t height = 0;
  int32_t total = 0;
  uint8_t ids[printer::kMaxReadouts] = {0};
  int count = 0;

  // How much glass there is at the *top* of the pills, which on a round panel
  // is the narrowest line they occupy.
  int32_t r = RES_H / 2;
  int32_t dy = r - (y - kPadY);
  int32_t usable = 2 * lv_sqrt32((uint32_t)(r * r - dy * dy)) - kMargin * 2;

  // Two readouts with targets already fill a 240px circle at this height, so
  // three cannot. Rather than let them run off both edges, the whole row drops
  // its targets - every pill, so they stay a set - and shows the readings
  // alone. That is the same choice a person would make with the space: what it
  // *is* matters more than what it was asked to be.
  bool targets = true;
  for (int pass = 0; pass < 2; pass++) {
    count = 0;
    total = 0;
    height = 0;
    for (unsigned int slot = 0; slot < printer::kMaxReadouts; slot++) {
      uint8_t id = conf.readouts[slot];
      if (id == (uint8_t)printer::Readout::kNone) {
        break;
      }
      const char *label = _label(id);
      if (!label) {
        continue;
      }
      // Always as though it had a target: whether the MCU has one depends on
      // which Klipper object `sensor_mcu:` names, and a pill that grew
      // mid-print would shove its neighbours sideways.
      char widest[24];
      snprintf(widest, sizeof(widest), targets ? "%s 888/888" : "%s 888",
               label);

      lv_point_t size;
      lv_text_get_size(&size, widest, &READOUT_FONT, 0, 0,
                       LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      width[count] = size.x + kPadX * 2;
      if (size.y + kPadY * 2 > height) {
        height = size.y + kPadY * 2;
      }
      total += width[count];
      ids[count] = id;
      count++;
    }
    if (count == 0) {
      return;
    }
    total += kGap * (count - 1);
    if (total <= usable || !targets) {
      break;
    }
    targets = false;
  }
  row->targets = targets;

  for (int i = 0; i < count; i++) {
    lv_obj_t *scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(scrim);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scrim, width[i], height);
    lv_obj_set_style_radius(scrim, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    if (scrimmed) {
      lv_obj_set_style_bg_color(scrim, lv_color_black(), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(scrim, SCRIM_OPA, LV_PART_MAIN);
    }

    lv_obj_t *label = lv_label_create(scrim);
    lv_obj_set_style_text_font(label, &READOUT_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_opa(label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_center(label);
    lv_label_set_text(label, "");

    row->scrim[i] = scrim;
    row->label[i] = label;
    row->id[i] = ids[i];
    row->width[i] = width[i];
  }
  row->count = count;
  row->y = y;
  // Nothing placed yet. Positions are worked out in update(), because they
  // depend on which readings the machine actually reports - and that is not
  // known here, nor fixed afterwards.
  row->laid_out = 0xFFFFFFFFu;
}

namespace {

//: Centre whichever pills are visible.
//:
//: Not done once at build time. Placing all the configured pills and then
//: hiding the absent ones leaves the survivors wherever they happened to fall -
//: a machine with a bed and no chamber got its bed sitting in the left half
//: with a hole beside it. Only run when the visible set changes, which is
//: almost never.
void _place(Row *row, uint32_t shown) {
  int32_t total = 0;
  int visible = 0;
  for (int i = 0; i < row->count; i++) {
    if (shown & (1u << i)) {
      total += row->width[i];
      visible++;
    }
  }
  if (visible > 1) {
    total += kGap * (visible - 1);
  }

  int32_t at = -total / 2;
  for (int i = 0; i < row->count; i++) {
    if (!(shown & (1u << i))) {
      continue;
    }
    lv_obj_align(row->scrim[i], LV_ALIGN_TOP_MID, at + row->width[i] / 2,
                 row->y - kPadY);
    at += row->width[i] + kGap;
  }
  row->laid_out = shown;
}

}

void update(Row *row, const printer::State &state) {
  if (!row) {
    return;
  }

  uint32_t shown = 0;
  for (int i = 0; i < row->count; i++) {
    if (_value(row->id[i], state).present) {
      shown |= 1u << i;
    }
  }
  if (shown != row->laid_out) {
    _place(row, shown);
  }

  for (int i = 0; i < row->count; i++) {
    Value v = _value(row->id[i], state);
    if (!v.present) {
      lv_obj_add_flag(row->scrim[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(row->scrim[i], LV_OBJ_FLAG_HIDDEN);
    if (v.target > 0 && row->targets) {
      lv_label_set_text_fmt(row->label[i], "%s %d/%d", _label(row->id[i]),
                            (int)v.now, (int)v.target);
    } else {
      lv_label_set_text_fmt(row->label[i], "%s %d", _label(row->id[i]),
                            (int)v.now);
    }
  }
}

}
}
