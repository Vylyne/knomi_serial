#include "haze.h"

#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace haze {

void apply(lv_obj_t *scr, const printer::State &state) {
  if (!scr) {
    return;
  }

  // Anchored to the target, not to an absolute temperature: 60C on the way to
  // 65 is nearly there, and 60C on the way to 250 has barely started. With no
  // target there is nothing being asked of the heater, so there is no glow.
  int step = 0;
  if (state.hotend_target > 0) {
    step = (int)((int32_t)state.hotend_temp * HEAT_STEPS / state.hotend_target);
    if (step < 0) {
      step = 0;
    }
    if (step > HEAT_STEPS) {
      step = HEAT_STEPS;
    }
  }

  // Restyling a screen background invalidates everything on it, and the hotend
  // moves about a degree per packet - following that faithfully would be a
  // couple of thousand full repaints across a heat-up. Quantising to HEAT_STEPS
  // makes it a couple of dozen, and nothing once the temperature settles.
  //
  // Stored as step+1 so that a freshly loaded screen, whose user data is zero,
  // always paints once rather than inheriting the last screen's rung.
  intptr_t stored = (intptr_t)lv_obj_get_user_data(scr);
  if (stored == (intptr_t)step + 1) {
    return;
  }
  lv_obj_set_user_data(scr, (void *)((intptr_t)step + 1));

  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);

  if (step == 0) {
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    return;
  }

  // Mixed toward black rather than faded with opacity: this is the bottom-most
  // thing drawn, so there is nothing behind it for an alpha to reveal. Rebuilt
  // from the rung rather than the raw temperature, so colour and strength land
  // on the same value and cannot drift apart.
  int32_t at = (int32_t)state.hotend_target * step / HEAT_STEPS;
  lv_color_t warm = lv_color_mix(
      theme::heat(at, state.hotend_target),
      lv_color_black(),
      (uint8_t)(HEAT_HAZE_OPA * step / HEAT_STEPS));

  lv_obj_set_style_bg_grad_color(scr, warm, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
}

}
}
