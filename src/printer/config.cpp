#include "config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "user_conf.h"

namespace printer {
namespace config {

namespace {

//: NVS namespace and keys. The stored payload is the wire bytes exactly as they
//: arrived, so loading is the same code path as receiving and the CRC comes out
//: identical - there is no second decoder to disagree with the first.
const char *kStore = "knomi";
const char *kKeyPayload = "cfg";
const char *kKeyProto = "proto";

Preferences _nvs;
bool _nvs_open = false;

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
portMUX_TYPE _snapshot_mux = portMUX_INITIALIZER_UNLOCKED;

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
  c->estop_at = (uint8_t)ESTOP_AT;
  memset(c->buttons, 0, sizeof(c->buttons));

  const Readout readouts[] = DEFAULT_READOUTS;
  memset(c->readouts, 0, sizeof(c->readouts));
  for (size_t i = 0; i < sizeof(readouts) / sizeof(readouts[0]) &&
                     i < kMaxReadouts;
       i++) {
    c->readouts[i] = (uint8_t)readouts[i];
  }

  const Page defaults[] = DEFAULT_PAGES;
  memset(c->page_order, 0, sizeof(c->page_order));
  for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]) &&
                     i < kMaxPages;
       i++) {
    c->page_order[i] = (uint8_t)defaults[i];
  }
  // Empty until the host says otherwise. The macro list is the host's to know -
  // it comes from printer.cfg - so there is no sensible thing to invent here.
  c->gcodes[0] = '\0';
}

bool _initialised = false;

bool _button_pin_allowed(uint8_t pin) {
  switch (pin) {
  case 5: case 6: case 8: case 9: case 11: case 15:
  case 38: case 39: case 40: case 41: case 42: case 47: case 48:
    return true;
  default:
    return false;
  }
}

bool _button_legend_valid(const ButtonConfig &b, bool required) {
  bool ended = false;
  bool have_char = false;
  for (char character : b.legend) {
    uint8_t c = (uint8_t)character;
    if (c == 0) {
      ended = true;
    } else {
      if (ended || c < 0x20 || c > 0x7e) return false;
      have_char = true;
    }
  }
  return required ? have_char : !have_char;
}

bool _buttons_valid(const ButtonConfig *buttons) {
  uint64_t used_pins = 0;
  uint8_t used_slots = 0;
  bool terminated = false;
  for (unsigned int i = 0; i < kMaxButtons; i++) {
    const ButtonConfig &b = buttons[i];
    if (b.source == ButtonSource::kNone) {
      terminated = true;
      const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&b);
      for (unsigned int j = 0; j < sizeof(b); j++) {
        if (bytes[j] != 0) return false;
      }
      continue;
    }
    if (terminated || (uint8_t)b.source > (uint8_t)ButtonSource::kEvent ||
        (uint8_t)b.slot > (uint8_t)ButtonSlot::kSE ||
        b.reserved[0] || b.reserved[1]) {
      return false;
    }
    if (b.slot != ButtonSlot::kNone) {
      uint8_t bit = 1u << (uint8_t)b.slot;
      if (used_slots & bit) return false;
      used_slots |= bit;
    } else if (b.source == ButtonSource::kTouch) {
      return false;
    }
    if (b.source == ButtonSource::kGpio) {
      if (!_button_pin_allowed(b.pin) || (used_pins & (1ull << b.pin))) {
        return false;
      }
      used_pins |= 1ull << b.pin;
    } else if (b.pin != 0xff) {
      return false;
    }
    switch (b.resolver) {
    case ButtonResolver::kPage:
      if (b.slot == ButtonSlot::kNone || b.flags || b.argument ||
          !_button_legend_valid(b, false)) return false;
      break;
    case ButtonResolver::kObserve:
      if (b.source == ButtonSource::kTouch ||
          b.flags || !_button_legend_valid(b, false) ||
          (b.argument != (uint8_t)ButtonProfile::kFeed &&
           b.argument != (uint8_t)ButtonProfile::kRetract)) {
        return false;
      }
      break;
    case ButtonResolver::kGcodeMacro:
      if (!(b.flags & (kButtonBare | kButtonPress | kButtonRelease)) ||
          (b.flags & ~(kButtonBare | kButtonPress | kButtonRelease)) ||
          ((b.flags & kButtonBare) &&
           (b.flags & (kButtonPress | kButtonRelease))) || b.argument ||
          !_button_legend_valid(b, b.slot != ButtonSlot::kNone)) {
        return false;
      }
      break;
    default:
      return false;
    }
  }
  return true;
}

void _ensure() {
  if (_initialised) {
    return;
  }
  _defaults(&_slot[0]);
  _defaults(&_slot[1]);
  _initialised = true;
}

}

void begin() {
  _ensure();
  if (!_nvs.begin(kStore, false)) {
    return;
  }

  // A stored payload from a firmware that spoke a different protocol describes
  // a different struct. The size check below catches most of that, but a change
  // that kept the size and moved a field would not, and there is no way to tell
  // those apart after the fact.
  if (_nvs.getUInt(kKeyProto, 0) == kProtoVersion) {
    static uint8_t stored[kConfigWireSize];
    if (_nvs.getBytes(kKeyPayload, stored, sizeof(stored)) == kConfigWireSize) {
      // Straight through the normal path, so anything wrong with it fails the
      // same way a bad frame would. Writes are still disabled at this point,
      // which is the whole reason they are enabled below rather than above:
      // adopting the stored config is a change by every measure apply() has,
      // and it would otherwise write those bytes back over themselves on every
      // single boot.
      apply(stored, kConfigWireSize);
    }
  }

  _nvs_open = true;
}

const Config &get() {
  _ensure();
  return _slot[_live];
}

void snapshot(Config *out, uint32_t *crc) {
  _ensure();
  portENTER_CRITICAL(&_snapshot_mux);
  if (out) memcpy(out, &_slot[_live], sizeof(*out));
  if (crc) *crc = _held_crc;
  portEXIT_CRITICAL(&_snapshot_mux);
}

uint32_t held_crc() {
  uint32_t crc;
  snapshot(nullptr, &crc);
  return crc;
}

bool apply(const void *payload, uint32_t len) {
  if (len != kConfigWireSize) {
    return false;
  }
  _ensure();

  Config wire;
  memcpy(&wire, payload, kConfigWireSize);
  wire.present = ntohl(wire.present);
  if ((wire.present & kHasButtons) && !_buttons_valid(wire.buttons)) {
    return false;
  }

  // Hash the received bytes before taking the short publication lock.
  uint32_t crc = crc32(payload, len);

  // Start from the defaults and overlay only what the host set, so a setting
  // absent from printer.cfg keeps whatever this firmware was built with.
  portENTER_CRITICAL(&_snapshot_mux);
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
  if (wire.present & kHasPageOrder) {
    memcpy(c->page_order, wire.page_order, sizeof(c->page_order));
  }
  if (wire.present & kHasEstopAt) {
    c->estop_at = wire.estop_at;
  }
  if (wire.present & kHasReadouts) {
    memcpy(c->readouts, wire.readouts, sizeof(c->readouts));
  }
  if (wire.present & kHasButtons) {
    memcpy(c->buttons, wire.buttons, sizeof(c->buttons));
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
  bool changed = crc != _held_crc;
  _held_crc = crc;

  _live = next;
  _ever_asked = false;
  portEXIT_CRITICAL(&_snapshot_mux);

  // Only on a change, and never during begin() - which reached here holding the
  // very bytes it just read back out of flash.
  if (changed && _nvs_open) {
    _nvs.putBytes(kKeyPayload, payload, len);
    _nvs.putUInt(kKeyProto, kProtoVersion);
  }
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
