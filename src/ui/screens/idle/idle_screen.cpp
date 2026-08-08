#include "idle_screen.h"

#include "board_conf.h"
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

    //: Must be in the order the pages are built, because that is the order
    //: tag_pages stamps them in and what update_visible indexes by.
    const screen_helper::page_update_t _updates[] = {
        IDLE_PAGE_0::printer_update,
        IDLE_PAGE_1::printer_update,
        IDLE_PAGE_2::printer_update,
#if !defined(TOOLCHANGER) || TOOLCHANGER == 0
        IDLE_PAGE_3::printer_update,
        IDLE_PAGE_4::printer_update,
#endif
        estop_page::printer_update,
    };

    lv_obj_t *_scr = nullptr;

    lv_obj_t *init(const printer::State &state)
    {
      lv_obj_t *scr = screen_helper::create_screen();
      _scr = scr;
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

      screen_helper::tag_pages(scr);
      lv_obj_scroll_to_x(scr, IDLE_PAGE_START * RES_H, LV_ANIM_OFF);

      return scr;
    }

    void _printer_update_handler(const printer::State &state)
    {
      screen_helper::update_visible(
          _scr, state, _updates, sizeof(_updates) / sizeof(_updates[0]));
    }

  }
}