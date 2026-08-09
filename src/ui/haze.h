#ifndef HAZE_H
#define HAZE_H

#include <lvgl.h>

#include "printer/printer.h"

namespace ui {
namespace haze {

// Heat, as the air above the melt rather than as a number: a gradient rising
// from the bottom of whatever screen is loaded.
//
// Painted onto the screen's own background rather than as an object, so it sits
// under every page without joining the idle screen's flex row, and every screen
// gets it from one call site. Pages are created with remove_style_all, so they
// are transparent and it shows through.
//
// Heat-up is when this matters most - the printer is idle then, so the idle
// screen is what is showing while you wait on the number.
void apply(lv_obj_t *scr, const printer::State &state);

//: What the haze is painting at screen row `y`.
//:
//: For anything that has to draw its own background rather than let this one
//: show through. The printing page's wave band is opaque - it paints every
//: pixel of the strip it covers - so the part above the waterline has to
//: reproduce what would have been behind it. Painting black there punched a
//: black notch through the glow in every trough, worst near the bottom of the
//: glass where the gradient is strongest.
//:
//: Read back off the screen's own style rather than recomputed from
//: temperature, so it cannot disagree with what apply() actually set.
lv_color_t background_at(lv_obj_t *scr, int32_t y);

}
}

#endif
