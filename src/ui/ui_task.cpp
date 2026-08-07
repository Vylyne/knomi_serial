#include "ui_task.h"

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

#include "display/display.h"
#include "display/cst816s.h"
#include "printer/printer.h"
#include "printer/recv/recv_state.h"
#include "printer/send/send_cmd.h"
#include "ui/ui.h"
#include "ui/screens/init/init_screen.h"
#include "user_conf.h"
#include "version.h"

namespace ui
{

  void _send_report(const char *sleep_state);

  void ui_task(void *param)
  {
    init();
    display::init();
    display::set_backlight(DISPLAY_BRIGHTNESS);

    static int32_t _last_hotend_temp = 0;
    static printer::Status _last_status = printer::Status::kDisconnected;

    uint32_t last_active = millis();
    uint32_t last_report = 0;
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

      // Report our own state upstream. This repeats rather than announcing once
      // at boot so the host recovers the version and UI state after a Klipper
      // restart or a device reset, without needing a handshake.
      if (millis() - last_report >= REPORT_PERIOD_MS)
      {
        last_report = millis();
        _send_report(sleeping ? "off" : (dimmed ? "dim" : "awake"));
      }

      delay(5);
    }
  }

  void _send_report(const char *sleep_state)
  {
    char fields[256];
    snprintf(
        fields,
        sizeof(fields),
        "fw=%s;proto=%u;var=%s;sleep=%s;scr=%s;page=%d;"
        "heap=%u;minheap=%u;up=%u",
        KNOMI_FW_VERSION,
        printer::kProtoVersion,
        KNOMI_BUILD_VARIANT,
        sleep_state,
        screen_name(),
        page_index(),
        (unsigned int)ESP.getFreeHeap(),
        (unsigned int)ESP.getMinFreeHeap(),
        (unsigned int)(millis() / 1000));
    printer::send::send_report(fields);
  }
}