#include "init_screen.h"

#include "printer/identity.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "user_conf.h"

namespace ui
{
  namespace init_screen
  {

    static lv_obj_t *name = nullptr;
    static lv_obj_t *_spinner = nullptr;
    static lv_obj_t *_message = nullptr;

    //: The screen these statics describe, and the timer turning the arc.
    //:
    //: A screen load deletes the outgoing screen only once its fade finishes,
    //: so for 300ms two of these exist and share the statics. Without the
    //: guard the old one's delete handler would stop the new one's timer -
    //: the same trap the printing page's wave timer fell into.
    static lv_obj_t *_screen = nullptr;
    static lv_timer_t *_spin_timer = nullptr;
    static int32_t _phase = 0;

    void _printer_update_handler(const printer::State &state);
    static void _spin(lv_timer_t *timer);
    static void _screen_deleted(lv_event_t *e);

    lv_obj_t *init(const printer::State &state_in)
    {
      lv_obj_t *scr = lv_obj_create(nullptr);
      lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
      // A bare screen is scrollable, and one with anything near its edges can
      // be dragged around by a thumb. The page screens set a horizontal scroll
      // direction so they only ever move the way they are meant to; these
      // standalone ones had nothing saying so and slid in both.
      lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
      control::register_printer_update_cb(scr, _printer_update_handler);

      // The machine names itself at the top, and the state it is in sits at the
      // bottom under the link mark - so the two things you read are the two
      // ends, with what is happening in between.
      name = lv_label_create(scr);
      lv_label_set_text(name, PRINTER_NAME);
      lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 18);

      // The hardware id, under the name. This is the half of identity-based
      // addressing that makes setting it up possible: plug in six displays with
      // no config at all, walk down the row, and read six codes off the glass.
      // Nothing else on the machine can tell you which physical unit is which.
      lv_obj_t *ident = lv_label_create(scr);
      lv_label_set_text(ident, printer::identity::id());
      lv_obj_set_style_text_font(ident, &lv_font_montserrat_12, LV_PART_MAIN);
      lv_obj_set_style_text_opa(ident, LV_OPA_50, LV_PART_MAIN);
      lv_obj_align(ident, LV_ALIGN_TOP_MID, 0, IDENT_Y);

      // An arc turned by hand rather than lv_spinner, in the machine's own
      // colour - which is why config is loaded from flash before any of this
      // runs. Nothing has spoken to the host yet and nothing may for minutes,
      // so an arc that waited for the wire would be the default pink on
      // somebody else's printer for exactly as long as it was the only thing
      // on screen.
      //
      // The spinner animates its start and end angles independently, which is
      // what gives it its breathing sweep - and means the two wrap past 360 at
      // different moments. On the frame between those two wraps the span reads
      // as the whole circle, and on the next as nothing, so the ring flashes
      // full then empty and carries on. The two animation periods beat against
      // each other, so it happens on a long cycle and looks like chance.
      //
      // One angle and a fixed span cannot do that: there is no second value to
      // disagree with. The look is a constant arc chasing round rather than
      // breathing, which is the smaller loss.
      _spinner = lv_arc_create(scr);
      lv_obj_set_size(_spinner, 56, 56);
      lv_obj_align(_spinner, LV_ALIGN_CENTER, 0, -8);
      lv_obj_remove_style(_spinner, nullptr, LV_PART_KNOB);
      lv_obj_remove_flag(_spinner, LV_OBJ_FLAG_CLICKABLE);
      lv_arc_set_bg_angles(_spinner, 0, 360);
      lv_arc_set_rotation(_spinner, 270);
      lv_obj_set_style_arc_width(_spinner, 5, LV_PART_MAIN);
      lv_obj_set_style_arc_width(_spinner, 5, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(_spinner, theme::machine(), LV_PART_INDICATOR);

      _phase = 0;
      lv_arc_set_angles(_spinner, 0, SPINNER_SPAN);
      if (_spin_timer) {
        lv_timer_delete(_spin_timer);
      }
      _spin_timer = lv_timer_create(_spin, SPINNER_TICK_MS, nullptr);
      lv_obj_add_event_cb(scr, _screen_deleted, LV_EVENT_DELETE, nullptr);
      _screen = scr;

      // Given a width, so it wraps. Without one a label is a single line as
      // long as its text, and this one carries whatever the host last said -
      // "MCU 'MCU' SHUTDOWN: LOST COMMUNICATION" ran off both sides of a round
      // screen at once.
      //
      // It takes the spinner's place rather than sitting under it. There is
      // only one clear area on this screen big enough for three lines, and if
      // we know why the host is gone that is worth more than an animation
      // saying we are still waiting - which the label at the bottom says
      // anyway.
      _message = lv_label_create(scr);
      lv_label_set_long_mode(_message, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(_message, 170);
      lv_label_set_text(_message, "");
      lv_obj_set_style_text_align(_message, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(_message, LV_ALIGN_CENTER, 0, -8);
      lv_obj_add_flag(_message, LV_OBJ_FLAG_HIDDEN);

      lv_obj_t *state = lv_label_create(scr);
      lv_label_set_text(state, "WAITING");
      lv_obj_align(state, LV_ALIGN_BOTTOM_MID, 0, -12);

      _printer_update_handler(state_in);
      return scr;
    }

    static void _spin(lv_timer_t *timer)
    {
      if (!_spinner) {
        return;
      }
      _phase += SPINNER_STEP;
      if (_phase >= 360) {
        _phase -= 360;
      }
      // Both ends move together, so the span is always exactly SPINNER_SPAN.
      // LVGL draws through zero when the end has wrapped and the start has not,
      // which is now the only case there is.
      int32_t end = _phase + SPINNER_SPAN;
      if (end >= 360) {
        end -= 360;
      }
      lv_arc_set_angles(_spinner, _phase, end);
    }

    static void _screen_deleted(lv_event_t *e)
    {
      if (lv_event_get_target(e) != _screen) {
        return;
      }
      _screen = nullptr;
      _spinner = nullptr;
      if (_spin_timer) {
        lv_timer_delete(_spin_timer);
        _spin_timer = nullptr;
      }
    }

    void _printer_update_handler(const printer::State &state)
    {
      if (!_spinner) {
        return;
      }
      // Anything the host had to say before it went away - a shutdown reason,
      // or a local fault like a malformed frame. Nothing generates a message
      // here; this is the last one that arrived, which is the only account of
      // why there is no host to talk to.
      bool has_message = state.message[0] != '\0';
      if (has_message)
      {
        lv_label_set_text(_message, state.message);
      }
      if (has_message)
      {
        lv_obj_remove_flag(_message, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_spinner, LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        lv_obj_add_flag(_message, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_spinner, LV_OBJ_FLAG_HIDDEN);
      }
    }

  }
}