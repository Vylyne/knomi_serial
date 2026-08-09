#ifndef TOOL_PAGE_H
#define TOOL_PAGE_H

#include <lvgl.h>

#include "printer/printer.h"

namespace ui {
namespace tool_page {

// The tool at rest. What was two pages - a table of temperatures you could not
// touch, and two buttons that said nothing about the machine.
//
// They merged because neither was whole. The temperature page was pure readout
// with the corners sitting empty, and the filament page knew the spool was
// there but never said what it was or how hot it had got. Together they are one
// question - what is loaded and is it ready - with its two answers, and the
// only two actions that question leads to are on the corners beside it.
//
// It is built to read as the printing page's other half. Same tool tag in the
// same place, same hero numeral down the middle, and the filament sits in a
// shallow pool at the bottom - the fill at rest, waiting to rise.
lv_obj_t *init(lv_obj_t *parent, const printer::State &state);
void printer_update(const printer::State &state);

}
}

#endif
