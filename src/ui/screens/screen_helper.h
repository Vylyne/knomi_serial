#ifndef SCREEN_HELPER_H
#define SCREEN_HELPER_H

#include <lvgl.h>

#include "printer/printer.h"

namespace ui {
namespace screen_helper {

// A screen is two axes. Horizontally it is a row of full-width pages you swipe
// between, snapping one at a time. Vertically it is that row and one thing
// underneath it - the emergency stop, which is therefore one pull-down from
// every page rather than up to four swipes along the row.
//
// That costs no pixels until it is asked for, which was the objection to the
// overlay it replaces: an e-stop occupying permanent chrome on every screen of
// every display, alongside whatever Klipper's own UI is already showing.
//
// Swiping is the only way through them. There used to be a pair of tap arcs at
// the left and right edges as well, which had to claim the full height of their
// side to be reachable - and so swallowed every touch near a corner, including
// the controls that now live there. Removing them gave the edges back.
//
// The row does not wrap. It briefly did: pages were rotated after each settle
// so the current one always had a neighbour either side. It worked, but the
// idle screen read as laggy with it in and read fine without, and it was the
// only screen rotating - the printing screen has two pages, which cannot wrap
// symmetrically, and that screen got faster over the same period. Not worth
// carrying an unexplained cost for a convenience.
//
// The row still remembers which page is which. tag_pages and visible_page look
// redundant while nothing reorders, and are kept because the alternative -
// inferring the page from the scroll offset - is what silently broke when
// rotation existed, and would break again the same way.
lv_obj_t *create_screen();

//: The row inside the screen. Pages are parented to this, not to the screen -
//: the screen's own children are the row and whatever is pulled down to.
lv_obj_t *page_row(lv_obj_t *scr);
lv_obj_t *estop_slot(lv_obj_t *scr);

//: The container the e-stop is built into - the screen's other child, placed
//: above or below the row as `estop_at:` says. It exists from create_screen so
//: that neither child's identity depends on the order they were added in.

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
