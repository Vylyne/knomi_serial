#include "printing_screen.h"

#include "ui/screens/screen_helper.h"
#include "ui/pages/estop/estop_page.h"
#include "ui/pages/printing/printing_page.h"
#include "ui/ui.h"

namespace ui {
namespace printing_screen {

void _printer_update_handler(const printer::State &state);

lv_obj_t *init(const printer::State &state) {
  lv_obj_t *scr = screen_helper::create_screen();
  control::register_printer_update_cb(scr, _printer_update_handler);

  printing_page::init(scr, state);
  estop_page::init(scr, state);

  return scr;
}

void _printer_update_handler(const printer::State &state) {
  printing_page::printer_update(state);
  estop_page::printer_update(state);
}

}
}