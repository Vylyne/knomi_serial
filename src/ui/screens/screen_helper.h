#ifndef SCREEN_HELPER_H
#define SCREEN_HELPER_H

#include <lvgl.h>

#include "printer/printer.h"

namespace ui {
namespace screen_helper {

// A screen is a row of full-width pages you swipe between, snapping one at a
// time.
//
// Swiping is the only way through them. There used to be a pair of tap arcs at
// the left and right edges as well, which had to claim the full height of their
// side to be reachable - and so swallowed every touch near a corner, including
// the controls that now live there. Removing them gave the edges back.
//
// The row wraps in both directions. After every page change the row is rotated
// so the page you are on sits at a fixed slot with a neighbour either side -
// rather than by duplicating anything, since pages hold their widgets in file
// statics and a second copy of one would quietly overwrite the first.
//
// Re-centring every time, instead of only on reaching an end, is what makes it
// symmetrical. Rotating at the ends alone puts a page on one side and leaves
// the other a wall: swipe, bounce, wait for the rotation, swipe again.
//
// The rotation waits for the row to stop moving. LVGL reports a scroll as ended
// while its snap animation is still easing, so acting on the position at that
// moment both reads the wrong page and cancels the snap - which parked the row
// between two pages.
//
// Three pages minimum. Symmetry needs a page to the left and to the right, and
// with two the other one can be on a side or the other but not both. Two pages
// keep ordinary bounded scrolling, which already reaches everything in one
// swipe each way.
lv_obj_t *create_screen();

//: Stamp each page with the position it was built in, once they all exist.
//:
//: Needed because wrapping reorders them. Without it, "which page am I on" can
//: only be answered as "wherever the viewport is", and after a rotation that is
//: a slot in a row whose order has changed - the reported page would sit still
//: at the home slot while you swiped through every page on the device.
void tag_pages(lv_obj_t *scr);

//: The page under the viewport, by the identity stamped above rather than by
//: where it currently sits in the row.
int visible_page(lv_obj_t *scr);

typedef void (*page_update_t)(const printer::State &);

//: Update the page in view and the one either side of it, and no others.
//:
//: A screen used to hand every state change to every page it owned - four here,
//: six without a toolchanger - so most of the text formatting and invalidation
//: was for pages nobody could see. The neighbours are included so a page is
//: current before it scrolls into view rather than after.
//:
//: `updates` is indexed by the identity tag_pages stamped, so it stays correct
//: however the row has been rotated.
void update_visible(
    lv_obj_t *scr, const printer::State &state,
    const page_update_t *updates, uint32_t count);

}
}

#endif
