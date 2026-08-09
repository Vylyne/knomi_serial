#include "config.h"

#include <Arduino.h>
#include <string.h>

#include "user_conf.h"

namespace printer {
namespace config {

namespace {

//: How long to wait before asking again. Long enough that a host which cannot
//: answer is not being interrogated ten times a second, short enough that a
//: screen coming up beside a running Klipper is configured before anyone has
//: finished looking at it.
const uint32_t kRequestPeriodMs = 1000;

// Two buffers and an index, rather than a mutex.
//
// A reader here is the UI task pulling a colour or a timeout out of a frame it
// is already halfway through drawing, and a writer is the serial task adopting
// a config a few times a day. A lock would make every one of those reads pay
// for a collision that essentially never happens. Swapping the index instead
// means a reader is always looking at a whole config, never a half-written one:
// the buffer it is reading is the one that is not being written.
Config _slot[2];
volatile uint8_t _live = 0;

uint32_t _held_crc = 0;
uint32_t _asked_at = 0;
bool _ever_asked = false;

void _defaults(Config *c) {
  c->present = 0;
  c->color_machine = COLOR_MACHINE;
  c->color_filament_unknown = COLOR_FILAMENT_UNKNOWN;
  c->dim_ms = SLEEP_DIM_MS;
  c->sleep_ms = SLEEP_TIMEOUT_MS;
  c->brightness = DISPLAY_BRIGHTNESS;
  c->dim_brightness = SLEEP_DIM_BRIGHTNESS;
  c->key_mask = CORNER_LEGEND_KEYS;
  c->_padding[0] = 0;
  // Empty until the host says otherwise. The macro list is the host's to know -
  // it comes from printer.cfg - so there is no sensible thing to invent here.
  c->gcodes[0] = '\0';
}

bool _initialised = false;

void _ensure() {
  if (_initialised) {
    return;
  }
  _defaults(&_slot[0]);
  _defaults(&_slot[1]);
  _initialised = true;
}

}

const Config &get() {
  _ensure();
  return _slot[_live];
}

uint32_t held_crc() {
  return _held_crc;
}

bool apply(const void *payload, uint32_t len) {
  if (len != kConfigWireSize) {
    return false;
  }
  _ensure();

  Config wire;
  memcpy(&wire, payload, kConfigWireSize);
  wire.present = ntohl(wire.present);

  // Start from the defaults and overlay only what the host set, so a setting
  // absent from printer.cfg keeps whatever this firmware was built with.
  uint8_t next = _live ^ 1;
  Config *c = &_slot[next];
  _defaults(c);
  c->present = wire.present;

  if (wire.present & kHasColorMachine) {
    c->color_machine = ntohl(wire.color_machine);
  }
  if (wire.present & kHasColorUnknown) {
    c->color_filament_unknown = ntohl(wire.color_filament_unknown);
  }
  if (wire.present & kHasDimMs) {
    c->dim_ms = ntohl(wire.dim_ms);
  }
  if (wire.present & kHasSleepMs) {
    c->sleep_ms = ntohl(wire.sleep_ms);
  }
  if (wire.present & kHasBrightness) {
    c->brightness = wire.brightness;
  }
  if (wire.present & kHasDimBrightness) {
    c->dim_brightness = wire.dim_brightness;
  }
  if (wire.present & kHasKeyMask) {
    c->key_mask = wire.key_mask;
  }
  if (wire.present & kHasGcodes) {
    memcpy(c->gcodes, wire.gcodes, sizeof(c->gcodes));
    // A fixed-width field the host zero-pads, but a frame that arrived short
    // could still leave it unterminated.
    c->gcodes[kGcodesMaxLen] = '\0';
  }

  // Over the bytes as received, before any of the swapping above. That is what
  // the host hashed, and hashing our own decoded copy would agree with the host
  // only by accident of endianness.
  _held_crc = crc32(payload, len);

  _live = next;
  _ever_asked = false;
  return true;
}

bool should_request(uint32_t host_crc, uint32_t now_ms) {
  if (host_crc == _held_crc) {
    return false;
  }
  if (_ever_asked && (uint32_t)(now_ms - _asked_at) < kRequestPeriodMs) {
    return false;
  }
  _asked_at = now_ms;
  _ever_asked = true;
  return true;
}

uint32_t crc32(const void *data, uint32_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int bit = 0; bit < 8; bit++) {
      // Branchless: the mask is all ones when the low bit is set, zero when it
      // is not. The polynomial is the reversed 0x04C11DB7 every zlib-compatible
      // CRC32 uses, which is what lets the host use the stdlib and agree.
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
  }
  return ~crc;
}

}
}
