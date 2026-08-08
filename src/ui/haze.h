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

}
}

#endif
