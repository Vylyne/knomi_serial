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
// The two pairs are split by how much they change, and the split follows the
// hardware:
//
//   NW, NE   fixed. Feed and retract, wired straight to the buffer and
//            independent of Klipper - they work with the host down, which is
//            when you most want to move filament by hand. Nothing on screen
//            drives them, so no page creates a mark here.
//   SW, SE   context-aware. Load and unload on the tool page, pause and cancel
//            while printing.
//
// The lower pair got the context because the lower pair is where the meaning
// changes, and pause and stop belong at the bottom of a round face where a
// thumb already rests. The fixed pair went to the top for the same reason in
// reverse: a key that always does one thing does not need to be the one under
// your hand.
//
// Once keys are fitted the top pair could carry static legends too. They would
// have to float above the page row rather than belong to a page, since these
// two work on every screen - see LV_OBJ_FLAG_FLOATING. Not built: there is
// nothing to label until the keys exist.
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

//: Fade a corner to say its action is unavailable right now.
//:
//: Only the appearance. Whether the action fires is the page's call, because a
//: physical key cannot be greyed out - once the keys are fitted this is the
//: only half of "disabled" the screen can still do, and the refusal has to sit
//: behind the handler either way.
//:
//: Opacity rather than colour, so this and set() can each change their own
//: property without having to remember what the other last wrote.
void set_enabled(lv_obj_t *btn, bool enabled);

}
}

#endif
