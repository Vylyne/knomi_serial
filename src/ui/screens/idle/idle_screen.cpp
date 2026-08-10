#include "idle_screen.h"

#include "board_conf.h"
#include "printer/config.h"
#include "ui/pages/estop/estop_page.h"
#include "ui/pages/gcode/gcode_page.h"
#include "ui/pages/home/home_page.h"
#include "ui/pages/move/move_page.h"
#include "ui/pages/none/none_page.h"
#include "ui/pages/tool/tool_page.h"
#include "ui/screens/screen_helper.h"
#include "ui/ui.h"
#include "user_conf.h"

namespace ui
{
  namespace idle_screen
  {

    void _printer_update_handler(const printer::State &state);

    namespace
    {
      struct PageDef
      {
        printer::Page id;
        lv_obj_t *(*init)(lv_obj_t *, const printer::State &);
        void (*update)(const printer::State &);
        //: False if the page would have nothing in it. A page that can only be
        //: empty is worse than one that is absent - it reads as broken.
        bool (*worth_showing)();
      };

      bool _always() { return true; }

      bool _have_macros()
      {
        return printer::config::get().gcodes[0] != '\0';
      }

      const PageDef kPages[] = {
          {printer::Page::kTool, tool_page::init, tool_page::printer_update, _always},
          {printer::Page::kGcode, gcode_page::init, gcode_page::printer_update, _have_macros},
          {printer::Page::kHome, home_page::init, home_page::printer_update, _always},
          {printer::Page::kMove, move_page::init, move_page::printer_update, _always},
      };

      const PageDef *_find(uint8_t id)
      {
        for (const PageDef &page : kPages)
        {
          if ((uint8_t)page.id == id)
          {
            return &page;
          }
        }
        return nullptr;
      }
    }

    //: Update callbacks in the order the pages were actually built, which is
    //: what tag_pages stamps and update_visible indexes by. Built at runtime
    //: now: it used to be a fixed array matching a fixed set of #defines, and
    //: the two had to be kept in step by hand.
    screen_helper::page_update_t _updates[printer::kMaxPages + 1];
    uint32_t _update_count = 0;

    lv_obj_t *_scr = nullptr;

    lv_obj_t *init(const printer::State &state)
    {
      lv_obj_t *scr = screen_helper::create_screen();
      lv_obj_t *row = screen_helper::page_row(scr);
      _scr = scr;
      _update_count = 0;
      control::register_printer_update_cb(scr, _printer_update_handler);

      const printer::Config &conf = printer::config::get();
      for (unsigned int slot = 0; slot < printer::kMaxPages; slot++)
      {
        uint8_t id = conf.page_order[slot];
        if (id == (uint8_t)printer::Page::kNone)
        {
          break;
        }
        const PageDef *page = _find(id);
        // An id this firmware has no page for is skipped rather than refused.
        // A newer host naming a page we do not have is the same situation as a
        // frame type we do not know, and is handled the same way.
        if (!page || !page->worth_showing())
        {
          continue;
        }
        page->init(row, state);
        _updates[_update_count++] = page->update;
      }

      // Off the row rather than at the end of it, so it is one swipe from
      // every page instead of up to four along. Which side is `estop_at:`.
      // Never in `pages:`: a list written while thinking about idle pages would
      // drop it without meaning to, and this is not the setting to learn from.
      estop_page::init(screen_helper::estop_slot(scr), state);

      screen_helper::tag_pages(row);
      // The first page listed, so ordering picks the landing place as well as
      // the sequence rather than needing a second setting that could disagree
      // with it.
      lv_obj_scroll_to_x(row, 0, LV_ANIM_OFF);

      return scr;
    }

    void _printer_update_handler(const printer::State &state)
    {
      screen_helper::update_visible(
          screen_helper::page_row(_scr), state, _updates, _update_count);
      // Not part of the row, so not covered by update_visible - and it is one
      // pull away at all times, so it is never far enough off-screen to skip.
      // Its update does nothing but disarm on a shutdown.
      estop_page::printer_update(state);
    }

  }
}
