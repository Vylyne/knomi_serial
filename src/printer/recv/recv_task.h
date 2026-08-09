#ifndef RECV_TASK_H
#define RECV_TASK_H

#include <stdint.h>

namespace printer {
namespace recv {

void recv_task(void *param);

//: Milliseconds since the last valid state frame, or UINT32_MAX if none has
//: ever arrived.
//:
//: The state carries no timestamp and the host has no way to say goodbye if it
//: is killed rather than closed - so a screen that stops being sent to has,
//: until now, sat holding its last frame indefinitely. A print that finished an
//: hour ago looks exactly like one at 55%.
uint32_t link_age_ms();

//: Whether the host has asked for a screenshot since this was last called.
//: Reading it clears it. Called from the LVGL task, which is the only place a
//: capture can be started from.
bool consume_snapshot_request();

}
}

#endif