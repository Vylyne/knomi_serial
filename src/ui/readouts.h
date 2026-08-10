#ifndef READOUTS_H
#define READOUTS_H

#include <lvgl.h>

#include "printer/printer.h"

namespace ui {
namespace readouts {

// The secondary temperatures, as a centred row of independently scrimmed
// labels.
//
// Which ones show is `readouts:` in printer.cfg rather than a fixed pair. Four
// tool screens each restating the one bed temperature is four copies of
// something none of them owns; the MCU inside the chamber is a reading only
// that tool can give. Neither is the right default for the other machine, so
// neither is hardcoded.
//
// A scrim each rather than one behind the lot. Two short numbers with a gap
// between them under a single pill is mostly dead dark space, and it reads as a
// bar rather than as two readings.
//
// Shared by the tool page and the printing page so the two cannot drift into
// rendering the same numbers differently.

struct Row {
  lv_obj_t *scrim[printer::kMaxReadouts];
  lv_obj_t *label[printer::kMaxReadouts];
  //: Which Readout each slot shows, so update() knows what to put in it.
  uint8_t id[printer::kMaxReadouts];
  //: Each pill's fixed width, kept so the row can be re-centred when one of
  //: them appears or disappears.
  int32_t width[printer::kMaxReadouts];
  //: Bit per slot, which were visible last time the row was laid out.
  //: Impossible to start with, so the first update always places them.
  uint32_t laid_out;
  int32_t y;
  int count;
  //: False when the row would not fit with targets and dropped them. Set for
  //: the whole row rather than per pill, so they stay a matching set.
  bool targets;
};

//: Build the configured readouts as a row centred at `y`.
//:
//: Each pill is sized once, to the widest its reading can ever be - three
//: digits, and as though it had a target whether or not it does. A sensor that
//: appears mid-print, or a fan that gains a setpoint, then resizes nothing and
//: moves nothing beside it.
//:
//: `scrimmed` is for pages with something rising behind them. The tool page has
//: only the heat haze, which these sit near the dark top of, so a pill there
//: would be chrome for the sake of it.
void build(Row *row, lv_obj_t *parent, int32_t y, bool scrimmed);

//: Put the current values in. A reading the machine does not report hides its
//: whole pill rather than showing a zero.
void update(Row *row, const printer::State &state);

}
}

#endif
