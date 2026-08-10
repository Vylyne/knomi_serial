#ifndef READOUTS_H
#define READOUTS_H

#include <stddef.h>

#include "printer/printer.h"

namespace ui {
namespace readouts {

// The secondary temperatures, formatted the same way wherever they appear.
//
// Which ones show is `readouts:` in printer.cfg rather than a fixed pair. Four
// tool screens each restating the one bed temperature is four copies of
// something none of them owns; the MCU inside the chamber is a reading only
// that tool can give. Neither is the right default for the other machine, so
// neither is hardcoded.
//
// Shared between the tool page and the printing page so the two cannot drift
// into formatting the same numbers differently.

//: Write the configured readouts into `out`, e.g. "BED 100/100   CHM 50".
//: Always NUL-terminates, including when nothing is configured or nothing
//: configured is present - an empty line is the correct output then, and it
//: must be an empty *string* rather than an untouched buffer.
void format(char *out, size_t n, const printer::State &state);

//: The widest string format() could produce for the current config, for sizing
//: a scrim once instead of re-measuring as the digits change. Three-digit
//: values throughout, so nothing moves when a temperature crosses 99.
void widest(char *out, size_t n);

}
}

#endif
