#include "recv_task.h"
#include "recv_state.h"

#include <Arduino.h>
#include <string.h>

#include "printer/config.h"
#include "printer/printer.h"
#include "printer/send/send_cmd.h"

namespace printer
{
  namespace recv
  {

    static const uint32_t _HEADER = 0x83ad83ad;
    static const uint32_t _FOOTER = 0xf007f007;

    //: The largest payload any frame carries, plus the footer that follows it.
    static const size_t _BUF_SIZE = printer::kMaxPayload + sizeof(_FOOTER);
    static uint8_t _buf[_BUF_SIZE];

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

    //: Arguments for the captureless lambdas below. write() takes a
    //: std::function, and a capturing lambda would put a heap allocation in the
    //: path of every packet.
    static const char *_fault_text = nullptr;
    static size_t _payload_len = 0;

    //: When the last valid state frame landed. Zero means none ever has.
    static volatile uint32_t _last_state_ms = 0;

    //: A snapshot has been asked for and not yet started.
    static volatile bool _snapshot_wanted = false;

    //: Whether the message on screen is one of ours rather than the host's.
    //:
    //: A local fault - a malformed frame, a length that made no sense - is only
    //: true until the link recovers. Left standing it outlives its cause by
    //: hours and turns up on the next shutdown screen as though it were the
    //: reason Klipper stopped. A message the *host* sent is not ours to expire.
    static bool _message_is_fault = false;

    static void _fault(const char *text);
    static bool _footer_at(size_t len);
    static void _take_state(size_t len);
    static void _take_message(size_t len);

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

          // Type and length, so a frame can be skipped without knowing what is
          // in it. Proto 2 had neither: the length was implied by the one
          // struct it could carry, which is why anything the screen needed had
          // to be in the ten-times-a-second packet or nowhere.
          uint8_t head[3];
          if (Serial.readBytes(head, sizeof(head)) < sizeof(head))
          {
            _fault("SHORT\nFRAME");
            continue;
          }
          Frame type = (Frame)head[0];
          size_t len = ((size_t)head[1] << 8) | head[2];

          if (len > printer::kMaxPayload)
          {
            // Either a corrupt length or a host that speaks a protocol with
            // bigger frames than this build knows about. Both are recovered
            // from the same way: drop it and resynchronise on the next header.
            _fault("BAD\nFRAME");
            continue;
          }

          size_t want = len + sizeof(_FOOTER);
          if (Serial.readBytes(_buf, want) < want || !_footer_at(len))
          {
            _fault("MALFORMED\nPACKET");
            continue;
          }

          switch (type)
          {
          case Frame::kState:
            _take_state(len);
            break;
          case Frame::kConfig:
            config::apply(_buf, len);
            break;
          case Frame::kMessage:
            _take_message(len);
            break;
          case Frame::kSnapshot:
            // Flagged, not acted on: capturing means invalidating the screen
            // and reading the flush callback, both of which belong to the
            // LVGL task.
            _snapshot_wanted = true;
            break;
          default:
            // A newer host sending a frame this build has no name for is not an
            // error. Skipping it is the whole point of carrying a length.
            break;
          }
        }
        delay(5);
      }
    }

    static void _take_state(size_t len)
    {
      if (len != printer::kStateWireSize)
      {
        // A well-formed frame of the wrong shape, which means the host is
        // speaking a different version of this protocol. Say so rather than
        // memcpying whatever arrived over the struct.
        _fault("PROTO\nMISMATCH");
        return;
      }

      write([](printer::State *state)
            {
        memcpy((char*) state, _buf, printer::kStateWireSize);
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
        state->flow = ntohl(state->flow);
        state->config_crc = ntohl(state->config_crc);
        // tram_type is a single byte now, so there is nothing to swap.
        //
        // A fixed-width field the host zero-pads, but a frame that arrived
        // short could still leave it unterminated.
        state->filament_type[printer::kFilamentTypeMaxLen] = '\0';

        // A good frame is proof the fault that raised the last message is over.
        if (_message_is_fault) {
          _message_is_fault = false;
          state->message[0] = '\0';
        } });

      _last_state_ms = millis();

      // Read back without the lock, which is safe because this task is the only
      // writer - the lock exists to stop the UI seeing a half-updated struct,
      // not to protect us from ourselves.
      if (config::should_request(_state.config_crc, millis()))
      {
        send::send_config_request();
      }
    }

    bool consume_snapshot_request()
    {
      if (!_snapshot_wanted)
      {
        return false;
      }
      _snapshot_wanted = false;
      return true;
    }

    uint32_t link_age_ms()
    {
      uint32_t at = _last_state_ms;
      if (at == 0)
      {
        return UINT32_MAX;
      }
      return millis() - at;
    }

    static void _take_message(size_t len)
    {
      if (len > printer::kMessageMaxLen)
      {
        len = printer::kMessageMaxLen;
      }
      _payload_len = len;
      _message_is_fault = false;
      write([](printer::State *state)
            {
        memcpy(state->message, _buf, _payload_len);
        state->message[_payload_len] = '\0'; });
    }

    static void _fault(const char *text)
    {
      _fault_text = text;
      _message_is_fault = true;
      write([](printer::State *state)
            {
        state->status = printer::Status::kDisconnected;
        strncpy(state->message, _fault_text, printer::kMessageMaxLen);
        state->message[printer::kMessageMaxLen] = '\0'; });
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

    static bool _footer_at(size_t len)
    {
      // memcpy rather than a cast: the footer sat at a fixed, aligned offset
      // when there was one frame size, and now sits wherever the payload ends.
      uint32_t footer = 0;
      memcpy(&footer, _buf + len, sizeof(footer));
      return ntohl(footer) == _FOOTER;
    }

  }
}
