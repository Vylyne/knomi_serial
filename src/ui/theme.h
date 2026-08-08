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

    //: Black or white, whichever survives on `ground`.
    //:
    //: Nothing calls this yet. The printing page went to scrims instead, because
    //: a label crossing the rising fill spends too long half over it for either
    //: ink to work. This is here for the corner key legends, which sit at the
    //: rim directly over the fill and are too small to scrim individually.
    //: Verified against WCAG contrast for ten filament colours.
    lv_color_t ink_on(lv_color_t ground);

    //: Steel through amber to orange-red as `temp` closes on `target`.
    //: Steel when there is no target: nothing is being asked of the heater.
    //:
    //: Tuned for the haze, where it is mixed down toward black and wants depth
    //: rather than brightness. Too dark to read as text - use heat_ink for that.
    lv_color_t heat(int32_t temp, int32_t target);

    //: The same ramp lifted toward white, for text.
    //:
    //: A colour for an ambient gradient and a colour for a readout have
    //: opposite requirements: the cool end of `heat` is a deep steel that gives
    //: the glow somewhere to start from, and is barely legible at 16px on a
    //: scrim. Brightening the one ramp would have made the haze glare, so there
    //: are two.
    lv_color_t heat_ink(int32_t temp, int32_t target);

    //: The loaded filament, or a neutral when the host has not said. Zero means
    //: unknown rather than black, so it must not render as black.
    lv_color_t filament(const printer::State &state);
    bool has_filament(const printer::State &state);

  }
}

#endif
