#ifndef ESTOP_PAGE_H
#define ESTOP_PAGE_H

#include <lvgl.h>

#include "printer/printer.h"

namespace ui {
namespace estop_page {

// One page, one control: hold to shut the machine down.
//
// Replaces the STOP button that used to ride the overlay on every screen. That
// button was one stray touch away at all times, and what it calls is
// invoke_shutdown - motors off, firmware restart to recover - which is far too
// much to hang off a brush against the glass. It was also compiled out in
// toolchanger builds, so those had no emergency stop at all.
//
// A page you swipe to, holding a control for a moment, is deliberate without
// being slow. A confirmation dialogue would be the wrong shape: an emergency
// stop should never be waiting for a second tap.
lv_obj_t *init(lv_obj_t *parent, const printer::State &state);
void printer_update(const printer::State &state);

}
}

#endif
