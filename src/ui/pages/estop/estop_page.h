#ifndef ESTOP_PAGE_H
#define ESTOP_PAGE_H

#include <lvgl.h>

#include "printer/printer.h"

namespace ui {
namespace estop_page {

// One page, one control: stop the machine where it stands.
//
// Not the same kind of thing as cancelling a print. Cancel is graceful - Klipper
// finishes the move it is on, runs the cancel macro, parks. This kills the
// service: invoke_shutdown, motors off wherever they happen to be, firmware
// restart to recover. It exists to limit physical damage, so every moment
// between deciding and stopping is a moment something is still moving.
//
// That is why it asks twice rather than being held. A hold reads as safer and
// is slower precisely when slowness costs, and the second tap is the smallest
// guard that still stops a brush against the glass from firing it.
//
// Replaces the STOP button that used to ride the overlay on every screen, one
// stray touch away at all times. That overlay was deliberately dropped from the
// toolchanger build - an emergency stop present on every screen of every
// display is not a safety feature, it is four more surfaces to catch a sleeve
// on. The gap that left was accidental rather than intended: nothing replaced
// it, so those builds ended up with no software stop at all.
//
// A page answers the original objection without leaving the gap. It is not
// underfoot, because you have to swipe to it; and swiping to it is deliberate
// without being slow.
//
// None of this is a substitute for a real one. A stop that travels display to
// serial to host to MCU is a convenience that happens to be red; the machine's
// actual emergency stop belongs on a latching switch that cuts power.
lv_obj_t *init(lv_obj_t *parent, const printer::State &state);
void printer_update(const printer::State &state);

}
}

#endif
