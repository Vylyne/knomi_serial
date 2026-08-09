#ifndef UI_H
#define UI_H

#include <functional>
#include <lvgl.h>

#include "printer/printer.h"

namespace ui {

typedef std::function<lv_obj_t*(const printer::State&)> scr_init_t;
typedef void (*printer_update_cb_t)(const printer::State&);

void init();
void update(const printer::State &state);

//: Show or hide the "nothing is talking to me" mark.
//:
//: Not part of any screen. It lives on LVGL's top layer, which survives a
//: screen load and does not belong to the scrolling page row - so it says the
//: same thing wherever you have swiped to, and stays put while you swipe.
void set_link_stale(bool stale);

//: Hand the last known state to the pages again.
//:
//: Needed because pages are only updated when the state changes, and only the
//: page in view and its neighbours are updated at all. Swipe two pages while
//: the printer is sitting still and the one arriving was never a neighbour, so
//: it would show whatever it last heard. Called when a swipe settles.
void refresh();

// Name of the screen currently loaded, and the index of the page scrolled to
// within it. Both are for reporting back to the host; call from the UI task.
const char *screen_name();
int page_index();

namespace control {

void register_printer_update_cb(lv_obj_t *obj, printer_update_cb_t cb);

}

}

#endif