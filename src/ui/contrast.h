#ifndef UI_CONTRAST_H
#define UI_CONTRAST_H

#include <math.h>
#include <stdint.h>

namespace ui {
namespace contrast {

inline float linear_channel(uint8_t channel) {
  float srgb = channel / 255.0f;
  return srgb <= 0.04045f ? srgb / 12.92f
                          : powf((srgb + 0.055f) / 1.055f, 2.4f);
}

// Black and white have equal WCAG contrast at relative luminance 0.17912878.
inline bool use_black(uint32_t rgb) {
  float luminance =
      0.2126f * linear_channel((rgb >> 16) & 0xff) +
      0.7152f * linear_channel((rgb >> 8) & 0xff) +
      0.0722f * linear_channel(rgb & 0xff);
  return luminance > 0.17912878f;
}

// Derived UI state, not part of the persisted or serialised config payload.
class AccentInkCache {
 public:
  bool update(uint32_t rgb) {
    rgb &= 0xffffff;
    if (valid_ && rgb_ == rgb) return false;
    rgb_ = rgb;
    black_ = contrast::use_black(rgb);
    valid_ = true;
    return true;
  }

  bool use_black() const { return black_; }
  uint32_t rgb() const { return rgb_; }

 private:
  uint32_t rgb_ = 0;
  bool black_ = false;
  bool valid_ = false;
};

}
}

#endif
