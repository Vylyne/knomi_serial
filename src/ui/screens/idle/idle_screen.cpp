#include "idle_screen.h"

#include "ui/screens/screen_helper.h"
#include "ui/pages/estop/estop_page.h"
#include "ui/pages/gcode/gcode_page.h"
#include "ui/pages/home/home_page.h"
#include "ui/pages/filament/filament_page.h"
#include "ui/pages/move/move_page.h"
#include "ui/pages/none/none_page.h"
#include "ui/pages/temp/temp_page.h"
#include "ui/ui.h"
#include "user_conf.h"

namespace ui
{
  namespace idle_screen
  {

    void _printer_update_handler(const printer::State &state);

    lv_obj_t *init(const printer::State &state)
    {
      lv_obj_t *scr = screen_helper::create_screen();
      control::register_printer_update_cb(scr, _printer_update_handler);

      IDLE_PAGE_0::init(scr, state);
      IDLE_PAGE_1::init(scr, state);
      IDLE_PAGE_2::init(scr, state);

#if !defined(TOOLCHANGER) || TOOLCHANGER == 0
      IDLE_PAGE_3::init(scr, state);
      IDLE_PAGE_4::init(scr, state);
#endif

      // Last page on every screen. The emergency stop used to ride the overlay,
      // one stray touch away at all times, and was compiled out of toolchanger
      // builds entirely - so those had none at all. A page you swipe to is
      // deliberate without being slow.
      estop_page::init(scr, state);

      lv_obj_scroll_to_x(scr, IDLE_PAGE_START * 240, LV_ANIM_OFF);

      return scr;
    }

    void _printer_update_handler(const printer::State &state)
    {
      IDLE_PAGE_0::printer_update(state);
      IDLE_PAGE_1::printer_update(state);
      IDLE_PAGE_2::printer_update(state);
#if !defined(TOOLCHANGER) || TOOLCHANGER == 0
      IDLE_PAGE_3::printer_update(state);
      IDLE_PAGE_4::printer_update(state);
#endif
      estop_page::printer_update(state);
    }

  }
}