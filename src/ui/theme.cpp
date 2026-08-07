#include "theme.h"

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

      uint8_t lerp(uint8_t a, uint8_t b, uint8_t num, uint8_t den)
      {
        if (den == 0)
        {
          return a;
        }
        return (uint8_t)(a + ((int32_t)b - a) * num / den);
      }
    }

    lv_color_t machine()
    {
      return lv_color_hex(COLOR_MACHINE);
    }

    lv_color_t ink_on(lv_color_t ground)
    {
      // Weighted for perceived brightness rather than raw average - green
      // carries most of it, so a saturated yellow reads as light and needs dark
      // ink while a saturated blue of the same raw sum does not. The full sRGB
      // luminance would want a pow() per channel; this integer approximation is
      // close enough and runs inside the UI task's frame budget.
      //
      // The threshold is where black and white contrast *equally*, not where
      // the colour looks "light". That crossover is a relative luminance of
      // 0.179, which for a neutral maps back to sRGB 117 - noticeably darker
      // than the midpoint. Putting it at 128 or above picks white over saturated
      // oranges and blues, where black in fact scores two to three times the
      // contrast.
      uint32_t y = (299u * ground.red + 587u * ground.green + 114u * ground.blue) / 1000u;
      return y > 117 ? lv_color_black() : lv_color_white();
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
        return lv_color_hex(COLOR_FILAMENT_UNKNOWN);
      }
      return lv_color_hex(state.filament_color);
    }

  }
}
