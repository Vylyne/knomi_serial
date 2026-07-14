#include "ui_task.h"

#include <lvgl.h>

#include "display/display.h"
#include "display/cst816s.h"
#include "printer/printer.h"
#include "printer/recv/recv_state.h"
#include "ui/ui.h"
#include "ui/screens/init/init_screen.h"
#include "user_conf.h"

namespace ui
{

  void ui_task(void *param)
  {
    init();
    display::init();
    display::set_backlight(DISPLAY_BRIGHTNESS);

    static int32_t _last_hotend_temp = 0;
    static printer::Status _last_status = printer::Status::kDisconnected;

    uint32_t last_active = millis();
    bool sleeping = false;
    bool dimmed = false;

    while (true)
    {
      lv_task_handler();

      if (display::cst816s::consume_touched())
      {
        last_active = millis();
        if (sleeping || dimmed)
        {
          display::set_backlight(DISPLAY_BRIGHTNESS);
          sleeping = false;
          dimmed = false;
        }
      }

      bool is_hot = _last_hotend_temp > SLEEP_HOT_THRESHOLD;
      bool is_printing = _last_status == printer::Status::kPrinting;

      // restore brightness if printer wakes up while dimmed
      if (dimmed && (is_hot || is_printing))
      {
        display::set_backlight(DISPLAY_BRIGHTNESS);
        dimmed = false;
        last_active = millis();
      }

      if (!sleeping && !dimmed && !is_hot && !is_printing)
      {
        if (millis() - last_active > SLEEP_DIM_MS)
        {
          display::set_backlight(SLEEP_DIM_BRIGHTNESS);
          dimmed = true;
        }
      }
      if (!sleeping && !is_hot && !is_printing)
      {
        if (millis() - last_active > SLEEP_TIMEOUT_MS)
        {
          display::set_backlight(0);
          sleeping = true;
          dimmed = false;
        }
      }

      printer::recv::try_read([](const printer::State &state)
                              {
            _last_hotend_temp = state.hotend_temp;
            _last_status = state.status;
            ui::update(state); });
      delay(5);
    }
  }
}