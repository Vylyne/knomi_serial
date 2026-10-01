#include "theme.h"

#include "printer/config.h"
#include "ui/contrast.h"
#include "user_conf.h"

namespace ui
{
  namespace theme
  {

    namespace
    {
      struct Stop
      {
        //: Position along the ramp, 0-255.
        uint8_t at;
        uint8_t r, g, b;
      };

      // Steel while cold, warming through amber, tipping orange-red at target.
      // The gap between the last two stops is deliberately narrow: the useful
      // distinction is "nearly there" versus "there", not the long climb.
      const Stop kHeatRamp[] = {
          {0, 91, 107, 118},
          {140, 140, 132, 110},
          {217, 217, 150, 62},
          {255, 232, 110, 70},
      };

      contrast::AccentInkCache _machine_palette;
      lv_color_t _machine_color;
      lv_color_t _machine_ink;

      uint8_t lerp(uint8_t a, uint8_t b, uint8_t num, uint8_t den)
      {
        if (den == 0)
        {
          return a;
        }
        return (uint8_t)(a + ((int32_t)b - a) * num / den);
      }
    }

    void refresh_machine()
    {
      // config::begin() has already loaded NVS before the UI task starts. The
      // derived ink is UI-only state, kept out of the fixed wire/NVS payload.
      if (_machine_palette.update(printer::config::get().color_machine))
      {
        _machine_color = lv_color_hex(_machine_palette.rgb());
        _machine_ink = _machine_palette.use_black()
            ? lv_color_black() : lv_color_white();
      }
    }

    lv_color_t machine()
    {
      return _machine_color;
    }

    lv_color_t machine_ink()
    {
      return _machine_ink;
    }

    lv_color_t ink_on(lv_color_t ground)
    {
      uint32_t rgb = ((uint32_t)ground.red << 16) |
                     ((uint32_t)ground.green << 8) | ground.blue;
      return contrast::use_black(rgb) ? lv_color_black() : lv_color_white();
    }

    lv_color_t heat(int32_t temp, int32_t target)
    {
      if (target <= 0)
      {
        return lv_color_make(kHeatRamp[0].r, kHeatRamp[0].g, kHeatRamp[0].b);
      }

      int32_t k = temp * 255 / target;
      if (k < 0)
      {
        k = 0;
      }
      if (k > 255)
      {
        k = 255;
      }

      const size_t count = sizeof(kHeatRamp) / sizeof(kHeatRamp[0]);
      for (size_t i = 1; i < count; i++)
      {
        if (k <= kHeatRamp[i].at)
        {
          const Stop &a = kHeatRamp[i - 1];
          const Stop &b = kHeatRamp[i];
          uint8_t num = (uint8_t)(k - a.at);
          uint8_t den = (uint8_t)(b.at - a.at);
          return lv_color_make(
              lerp(a.r, b.r, num, den),
              lerp(a.g, b.g, num, den),
              lerp(a.b, b.b, num, den));
        }
      }

      const Stop &last = kHeatRamp[count - 1];
      return lv_color_make(last.r, last.g, last.b);
    }

    lv_color_t heat_ink(int32_t temp, int32_t target)
    {
      // Lifted toward white rather than given its own stop table, so the two
      // ramps cannot drift apart in hue - only in weight.
      return lv_color_mix(lv_color_white(), heat(temp, target), HEAT_INK_LIFT);
    }

    bool has_filament(const printer::State &state)
    {
      return state.filament_color != 0;
    }

    lv_color_t filament(const printer::State &state)
    {
      if (!has_filament(state))
      {
        // Not black: zero means the host has not told us, and rendering that as
        // black would be indistinguishable from genuinely black filament.
        return lv_color_hex(printer::config::get().color_filament_unknown);
      }
      return lv_color_hex(state.filament_color);
    }

  }
}
