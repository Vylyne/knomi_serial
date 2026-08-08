#ifndef SCREEN_HELPER_H
#define SCREEN_HELPER_H

#include <lvgl.h>

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
// The row wraps: swipe past the last page and the first comes round. Done by
// moving a page from one end of the row to the other and scrolling to its new
// index, rather than by duplicating anything - pages hold their widgets in file
// statics, so a second copy of one would quietly overwrite the first.
//
// The rotation waits for the row to stop moving. LVGL reports a scroll as ended
// while its snap animation is still easing, so acting on the position at that
// moment both reads the wrong page and cancels the snap - which parked the row
// between two pages.
//
// Works with two pages as well as many: the wrap's own scroll re-enters this
// synchronously, and is suppressed, so the rotation cannot chase its own tail.
lv_obj_t *create_screen();

}
}

#endif
