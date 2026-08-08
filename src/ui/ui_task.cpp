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

  struct RenderStats
  {
    //: Share of wall time spent inside lv_task_handler, in permille. The
    //: headroom number: what is left is what a heavier screen can spend.
    uint32_t busy;
    //: Longest single lv_task_handler call in the window, microseconds. A full
    //: frame hides here - the average will not show it.
    uint32_t peak;
  };

  void _send_report(const char *sleep_state, const RenderStats &stats);

  void ui_task(void *param)
  {
    init();
    display::init();
    display::set_backlight(DISPLAY_BRIGHTNESS);

    static int32_t _last_hotend_temp = 0;
    static printer::Status _last_status = printer::Status::kDisconnected;
    static bool _last_used = true;

    uint32_t last_active = millis();
    uint32_t last_report = millis();
    bool sleeping = false;
    bool dimmed = false;

    uint32_t busy_us = 0;
    uint32_t peak_us = 0;

    while (true)
    {
      uint32_t enter = micros();
      lv_task_handler();
      uint32_t spent = micros() - enter;
      busy_us += spent;
      if (spent > peak_us)
      {
        peak_us = spent;
      }

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

      // A job running on *this* tool is what keeps the screen up. The host says
      // so explicitly - it is told which tools a job uses - because temperature
      // cannot tell a docked tool still in the job from one that is simply warm
      // from the chamber. Temperature stays on as a safety net: a hot nozzle
      // should never sit behind a dark screen whatever the host believes.
      bool is_hot = _last_hotend_temp > SLEEP_HOT_THRESHOLD;
      bool in_job = _last_status == printer::Status::kPrinting && _last_used;
      // A shutdown screen exists to say something went wrong. A dark one says
      // nothing, so an alarm wakes the display whatever else is true.
      bool alarm = _last_status == printer::Status::kShutdown;
      bool keep_awake = is_hot || in_job || alarm;

      // Wake for the printer, not only for a finger. This tested `dimmed` alone,
      // so once the screen had gone fully dark the only way back was a touch:
      // the machine could start heating, begin a print, or shut down in front of
      // a display that stayed off.
      if ((sleeping || dimmed) && keep_awake)
      {
        display::set_backlight(DISPLAY_BRIGHTNESS);
        sleeping = false;
        dimmed = false;
        last_active = millis();
      }

      if (!sleeping && !dimmed && !keep_awake)
      {
        if (millis() - last_active > SLEEP_DIM_MS)
        {
          display::set_backlight(SLEEP_DIM_BRIGHTNESS);
          dimmed = true;
        }
      }
      if (!sleeping && !keep_awake)
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
            _last_used = state.used;
            ui::update(state); });

      // Report our own state upstream. This repeats rather than announcing once
      // at boot so the host recovers the version and UI state after a Klipper
      // restart or a device reset, without needing a handshake.
      uint32_t now = millis();
      if (now - last_report >= REPORT_PERIOD_MS)
      {
        uint32_t window_ms = now - last_report;
        last_report = now;

        RenderStats stats;
        // busy_us / (window_ms * 1000) is the fraction; permille cancels the
        // thousand and keeps it in integers.
        stats.busy = window_ms ? busy_us / window_ms : 0;
        stats.peak = peak_us;
        busy_us = 0;
        peak_us = 0;

        _send_report(sleeping ? "off" : (dimmed ? "dim" : "awake"), stats);
      }

      delay(5);
    }
  }

  void _send_report(const char *sleep_state, const RenderStats &stats)
  {
    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);

    uint32_t flush_count = 0, flush_px = 0, flush_us = 0;
    display::take_flush_stats(&flush_count, &flush_px, &flush_us);

    char fields[320];
    snprintf(
        fields,
        sizeof(fields),
        "fw=%s;proto=%u;var=%s;sleep=%s;scr=%s;page=%d;"
        "heap=%u;minheap=%u;up=%u;"
        // Everything past here is for judging what a heavier screen can afford:
        // how much of the frame budget the UI already spends, how long its worst
        // frame took, and whether the PSRAM a full-screen draw buffer would need
        // is actually there.
        "busy=%u;peak=%u;psram=%u;lvfree=%u;lvfrag=%u;"
        "flush=%u;fpx=%u;fus=%u",
        KNOMI_FW_VERSION,
        printer::kProtoVersion,
        KNOMI_BUILD_VARIANT,
        sleep_state,
        screen_name(),
        page_index(),
        (unsigned int)ESP.getFreeHeap(),
        (unsigned int)ESP.getMinFreeHeap(),
        (unsigned int)(millis() / 1000),
        (unsigned int)stats.busy,
        (unsigned int)stats.peak,
        (unsigned int)ESP.getFreePsram(),
        (unsigned int)mem.free_size,
        (unsigned int)mem.frag_pct,
        (unsigned int)flush_count,
        (unsigned int)flush_px,
        (unsigned int)flush_us);
    printer::send::send_report(fields);
  }
}