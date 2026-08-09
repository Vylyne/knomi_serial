#include "ui_task.h"

#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

#include "board_conf.h"
#include "display/display.h"
#include "display/cst816s.h"
#include "printer/config.h"
#include "printer/printer.h"
#include "printer/recv/recv_state.h"
#include "printer/recv/recv_task.h"
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
  void _send_snapshot();

  void ui_task(void *param)
  {
    init();
    display::init();
    display::set_backlight(printer::config::get().brightness);

    static int32_t _last_hotend_temp = 0;
    static printer::Status _last_status = printer::Status::kDisconnected;
    static bool _last_used = true;

    uint32_t last_active = millis();
    uint32_t last_report = millis();
    bool sleeping = false;
    bool dimmed = false;

    uint32_t busy_us = 0;
    uint32_t peak_us = 0;

    bool _snapshot_pending = false;

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

      // Timings and brightness come from the host so they can be changed in
      // printer.cfg without a reflash, and fall back to what this build was
      // compiled with until it says otherwise.
      const printer::Config &conf = printer::config::get();

      if (display::cst816s::consume_touched())
      {
        last_active = millis();
        if (sleeping || dimmed)
        {
          display::set_backlight(conf.brightness);
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

      // Every reason to stay awake is a claim about the machine, and a link
      // that has gone quiet is no longer entitled to make one. The hot-nozzle
      // net in particular says a hot nozzle must never sit behind a dark
      // screen - but once nothing is arriving we do not know that the nozzle
      // is hot, only that it was. Holding the backlight on forever off the
      // back of a reading from an hour ago is the wrong kind of caution.
      bool stale = printer::recv::link_age_ms() > STALE_TIMEOUT_MS;
      set_link_stale(stale);

      bool keep_awake = !stale && (is_hot || in_job || alarm);

      // Wake for the printer, not only for a finger. This tested `dimmed` alone,
      // so once the screen had gone fully dark the only way back was a touch:
      // the machine could start heating, begin a print, or shut down in front of
      // a display that stayed off.
      if ((sleeping || dimmed) && keep_awake)
      {
        display::set_backlight(conf.brightness);
        sleeping = false;
        dimmed = false;
        last_active = millis();
      }

      if (!sleeping && !dimmed && !keep_awake)
      {
        if (millis() - last_active > conf.dim_ms)
        {
          display::set_backlight(conf.dim_brightness);
          dimmed = true;
        }
      }
      if (!sleeping && !keep_awake)
      {
        if (millis() - last_active > conf.sleep_ms)
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

      // A screenshot is two steps a frame apart: ask LVGL to redraw everything,
      // then collect it once the flush callback has seen the whole screen.
      if (printer::recv::consume_snapshot_request())
      {
        _snapshot_pending = display::capture_begin();
        if (!_snapshot_pending)
        {
          printer::send::send_line("SNAP:ERR:no psram");
        }
      }
      if (_snapshot_pending && display::capture_complete())
      {
        _snapshot_pending = false;
        _send_snapshot();
      }

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

  // A frame of the glass, as base64 over the same line-based uplink everything
  // else uses.
  //
  // Raw RGB565 rather than anything compressed: PNG on the device would want a
  // deflate implementation and the RAM to run it, to save maybe half of a
  // transfer that happens when somebody types a command. The host turns it into
  // a PNG, where zlib is already sitting in the standard library.
  //
  // 384 bytes a line, which is 512 of base64. Small enough that the line buffer
  // is stack-sized, large enough that the ten-byte prefix is noise rather than
  // a third of the traffic.
  void _send_snapshot()
  {
    static const char *kB64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const uint32_t kRaw = 384;

    const uint16_t *frame = display::capture_frame();
    if (!frame)
    {
      printer::send::send_line("SNAP:ERR:no frame");
      return;
    }

    char begin[64];
    snprintf(
        begin, sizeof(begin), "SNAP:BEGIN:%d,%d,RGB565LE", (int)RES_H, (int)RES_V);
    printer::send::send_line(begin);

    const uint8_t *src = (const uint8_t *)frame;
    uint32_t total = (uint32_t)RES_H * RES_V * sizeof(uint16_t);
    char line[6 + (kRaw / 3) * 4 + 8];

    for (uint32_t at = 0; at < total; at += kRaw)
    {
      uint32_t n = total - at < kRaw ? total - at : kRaw;
      char *out = line;
      *out++ = 'S'; *out++ = 'N'; *out++ = 'A'; *out++ = 'P'; *out++ = ':';

      for (uint32_t i = 0; i < n; i += 3)
      {
        uint32_t bits = (uint32_t)src[at + i] << 16;
        if (i + 1 < n) bits |= (uint32_t)src[at + i + 1] << 8;
        if (i + 2 < n) bits |= (uint32_t)src[at + i + 2];
        *out++ = kB64[(bits >> 18) & 0x3F];
        *out++ = kB64[(bits >> 12) & 0x3F];
        *out++ = (i + 1 < n) ? kB64[(bits >> 6) & 0x3F] : '=';
        *out++ = (i + 2 < n) ? kB64[bits & 0x3F] : '=';
      }
      *out = '\0';
      printer::send::send_line(line);

      // The UART blocks once its buffer is full, so this is already paced by
      // the link - but yielding keeps the receive task fed and the watchdog
      // quiet through the fourteen seconds this takes.
      delay(1);
    }

    printer::send::send_line("SNAP:END");
    display::capture_end();
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
        // CRC of the config actually in force, so the host can see that what it
        // sent is what the device is running rather than assuming the push
        // landed.
        "cfg=%08x;"
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
        (unsigned int)printer::config::held_crc(),
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