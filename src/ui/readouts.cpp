#include "readouts.h"

#include <stdio.h>

#include "printer/config.h"

namespace ui {
namespace readouts {

namespace {

struct Source {
  const char *label;
  int32_t value;
  int32_t target;
  //: False when the machine has no such sensor, which is not the same as one
  //: reading zero - an absent bed and a bed at 0C would otherwise look alike.
  bool present;
};

Source _source(uint8_t id, const printer::State &state) {
  switch ((printer::Readout)id) {
  case printer::Readout::kBed:
    return {"BED", state.bed_temp, state.bed_target,
            state.bed_temp != 0 || state.bed_target > 0};
  case printer::Readout::kChamber:
    return {"CHM", state.chamber_temp, state.chamber_target,
            state.chamber_temp != 0 || state.chamber_target > 0};
  case printer::Readout::kMcu:
    // A target only if `sensor_mcu:` names a temperature_fan. A plain
    // temperature_sensor reports zero here and the target is simply not drawn.
    return {"MCU", state.mcu_temp, state.mcu_target,
            state.mcu_temp != 0 || state.mcu_target > 0};
  default:
    return {nullptr, 0, 0, false};
  }
}

}

void format(char *out, size_t n, const printer::State &state) {
  if (!out || n == 0) {
    return;
  }
  out[0] = '\0';

  const printer::Config &conf = printer::config::get();
  size_t at = 0;
  for (unsigned int slot = 0; slot < printer::kMaxReadouts; slot++) {
    uint8_t id = conf.readouts[slot];
    if (id == (uint8_t)printer::Readout::kNone) {
      break;
    }
    Source src = _source(id, state);
    if (!src.label || !src.present || at + 1 >= n) {
      continue;
    }
    const char *gap = at > 0 ? "   " : "";
    int wrote;
    if (src.target > 0) {
      wrote = snprintf(out + at, n - at, "%s%s %d/%d", gap, src.label,
                       (int)src.value, (int)src.target);
    } else {
      wrote = snprintf(out + at, n - at, "%s%s %d", gap, src.label,
                       (int)src.value);
    }
    if (wrote <= 0) {
      break;
    }
    at += (size_t)wrote;
    if (at >= n) {
      break;
    }
  }
}

void widest(char *out, size_t n) {
  if (!out || n == 0) {
    return;
  }
  out[0] = '\0';

  // Every configured readout at its widest, whether or not the machine
  // currently reports it. A sensor that appears mid-print - a chamber heater
  // switched on - must not resize anything it is drawn on.
  const printer::Config &conf = printer::config::get();
  size_t at = 0;
  for (unsigned int slot = 0; slot < printer::kMaxReadouts; slot++) {
    uint8_t id = conf.readouts[slot];
    if (id == (uint8_t)printer::Readout::kNone) {
      break;
    }
    const char *label = nullptr;
    switch ((printer::Readout)id) {
    case printer::Readout::kBed:     label = "BED"; break;
    case printer::Readout::kChamber: label = "CHM"; break;
    case printer::Readout::kMcu:     label = "MCU"; break;
    default: break;
    }
    if (!label || at + 1 >= n) {
      continue;
    }
    // Every one sized as though it had a target, even the MCU - whether it
    // does depends on which Klipper object `sensor_mcu:` names, and a reading
    // that grew a target mid-print must not resize what it is drawn on.
    int wrote = snprintf(out + at, n - at, "%s%s 888/888", at > 0 ? "   " : "",
                         label);
    if (wrote <= 0) {
      break;
    }
    at += (size_t)wrote;
    if (at >= n) {
      break;
    }
  }
}

}
}
