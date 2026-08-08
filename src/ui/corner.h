#ifndef CORNER_H
#define CORNER_H

#include <lvgl.h>

namespace ui {
namespace corner {

// Round controls at the four diagonals of the glass.
//
// The diagonals are where a round layout has room to spare - everything else
// competes with the readout down the middle - and they are where the physical
// keys are going. Putting the touch targets there first means the screen
// teaches the button positions before the buttons exist, and when they arrive
// they take over something already learned rather than introducing it.
//
// The lower two are the context-aware pair: load and unload while idle, pause
// and cancel while printing. Feed and retract are the fixed pair and are wired
// to the buffer, so they never appear here.
//
// CORNER_KEYS_TOUCH decides whether these are controls or captions. With
// physical keys fitted they become legends - the symbol stays exactly where it
// was, the button chrome and the touch target go, and the screen's job narrows
// to saying what the key under your finger will do. Nothing about the layout
// moves in that transition, which is the point of putting them here early.
enum class Slot {
  kNW,
  kNE,
  kSW,
  kSE,
};

lv_obj_t *create(
    lv_obj_t *parent, Slot slot, const char *symbol, lv_color_t color,
    lv_event_cb_t cb);

//: Change what a corner says and means. Its position never moves - a control
//: that relocates is a different control, and these have to stay where the
//: physical keys are.
void set(lv_obj_t *btn, const char *symbol, lv_color_t color);

}
}

#endif
