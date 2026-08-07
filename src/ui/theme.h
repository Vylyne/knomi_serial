#ifndef THEME_H
#define THEME_H

#include <lvgl.h>

#include "printer/printer.h"

// Three colour roles, kept apart on purpose.
//
//   Machine identity  fixed, the printer's own pink. Chrome that is about the
//                     machine rather than the print: tool tags, key legends.
//   Filament          per-tool data straight off the wire. Must be the most
//                     saturated thing on screen, which is why identity stays
//                     quiet.
//   Heat              a quantity, not a brand. Deliberately off the pink hue so
//                     a hot nozzle can never be mistaken for machine chrome.
//
// user_conf.h holds what a user might reasonably want to change. What lives
// here is the system those choices sit inside.
namespace ui
{
  namespace theme
  {

    //: The printer's pink. Also, as it happens, a filament on the shelf - so
    //: nothing anchors identity by hue alone, or loading pink ABS would erase it.
    lv_color_t machine();

    //: Black or white, whichever survives on `ground`. For text over the fill,
    //: which can be any colour the slicer names - including white and yellow.
    lv_color_t ink_on(lv_color_t ground);

    //: Steel through amber to orange-red as `temp` closes on `target`.
    //: Steel when there is no target: nothing is being asked of the heater.
    lv_color_t heat(int32_t temp, int32_t target);

    //: The loaded filament, or a neutral when the host has not said. Zero means
    //: unknown rather than black, so it must not render as black.
    lv_color_t filament(const printer::State &state);
    bool has_filament(const printer::State &state);

  }
}

#endif
