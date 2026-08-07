#include "recv_task.h"
#include "recv_state.h"

#include <Arduino.h>

#include "printer/printer.h"

namespace printer
{
  namespace recv
  {

    static const uint32_t _HEADER = 0x83ad83ad;
    static const uint32_t _FOOTER = 0xf007f007;
    static const int _BUF_SIZE = sizeof(printer::State) + sizeof(_FOOTER);
    static char _buf[_BUF_SIZE];

    static State _state;
    static SemaphoreHandle_t _semaphore = nullptr;

    //: Set by write(), cleared once a reader has seen it. Starts true so the
    //: first read builds the initial screen with no packet having arrived.
    //:
    //: Without this, try_read handed the state to the UI on every pass of the
    //: UI loop - two hundred times a second - and lv_label_set_text_fmt
    //: invalidates its label whether or not the text changed. The screen was
    //: therefore fully repainted every refresh period forever, whether or not
    //: anything had happened. Measured at 85% of the UI task's wall clock with
    //: the printer sending nothing at all.
    static volatile bool _dirty = true;

    bool _validate_footer();

    void recv_task(void *param)
    {
      _state.status = Status::kDisconnected;
      _semaphore = xSemaphoreCreateMutex();

      uint32_t header = 0;
      while (true)
      {
        if (Serial.available())
        {
          header = (header << 8) | Serial.read();
          if (header != _HEADER)
          {
            continue;
          }
          header = 0;

          size_t len = Serial.readBytes(_buf, _BUF_SIZE);
          if (len < _BUF_SIZE || !_validate_footer())
          {
            write([](printer::State *state)
                  {
          state->status = printer::Status::kDisconnected;
          strcpy(state->gcodes, "MALFORMED\nPACKET"); });
            continue;
          }

          write([](printer::State *state)
                {
        memcpy((char*) state, _buf, sizeof(printer::State));
        state->status = (printer::Status) ntohl((uint32_t) state->status);
        state->hotend_temp = ntohl(state->hotend_temp);
        state->hotend_target = ntohl(state->hotend_target);
        state->bed_temp = ntohl(state->bed_temp);
        state->bed_target = ntohl(state->bed_target);
        state->chamber_temp = ntohl(state->chamber_temp);
        state->chamber_target = ntohl(state->chamber_target);
        state->mcu_temp = ntohl(state->mcu_temp);
        state->mcu_target = ntohl(state->mcu_target);
        state->progress = ntohl(state->progress);
        state->tool_number = ntohl(state->tool_number);
        state->filament_color = ntohl(state->filament_color);
        state->tram_type = (printer::TramType) ntohl((uint32_t) state->tram_type);
        // Both strings are fixed-width fields the host zero-pads, but a
        // truncated or malformed packet could still leave them unterminated.
        state->filament_type[printer::kFilamentTypeMaxLen] = '\0';
        state->gcodes[sizeof(state->gcodes) - 1] = '\0'; });
        }
        delay(5);
      }
    }

    void try_read(std::function<void(const State &)> cb)
    {
      if (!_semaphore)
      {
        return;
      }
      if (!_dirty)
      {
        return;
      }
      if (xSemaphoreTake(_semaphore, 0) == pdTRUE)
      {
        _dirty = false;
        cb(_state);
        xSemaphoreGive(_semaphore);
      }
    }

    void write(std::function<void(State *)> cb)
    {
      xSemaphoreTake(_semaphore, portMAX_DELAY);
      cb(&_state);
      _dirty = true;
      xSemaphoreGive(_semaphore);
    }

    bool _validate_footer()
    {
      uint32_t *footer = (uint32_t *)(_buf + _BUF_SIZE - 4);
      return ntohl(*footer) == _FOOTER;
    }

  }
}