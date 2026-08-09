#include "send_task.h"

#include <Arduino.h>
#include <string>
#include <vector>

#include "send_cmd.h"

static std::vector<std::string> _queue;
static bool _stop = false;
static SemaphoreHandle_t _semaphore = nullptr;

namespace printer {
namespace send {

namespace Commands {

const char *kPrefix = "KNOMI_CMD:";
const char *kStop = "STOP";
const char *kRestart = "RESTART";
const char *kGcode = "GCODE:";
const char *kMove = "MOVE:";
const char *kReport = "RPT:";
const char *kConfigRequest = "CFG?";

}


void _send(const char *cmd);

void send_task(void *param) {
  _semaphore = xSemaphoreCreateMutex();
  while (true) {
    xSemaphoreTake(_semaphore, portMAX_DELAY);
    if (_stop) {
      _send(Commands::kStop);
      _stop = false;
      _queue.clear();
    }
    if (!_queue.empty()) {
      for (const std::string &cmd : _queue) {
        _send(cmd.c_str());
      }
      _queue.clear();
    }
    xSemaphoreGive(_semaphore);
    delay(5);
  }
}

void send_stop() {
  xSemaphoreTake(_semaphore, portMAX_DELAY);
  _stop = true;
  xSemaphoreGive(_semaphore);
}

void send_gcode(const char *gcode) {
  std::string cmd = Commands::kGcode;
  cmd.append(gcode);
  send_cmd(cmd.c_str());
}

void send_move(const char *dir) {
  std::string cmd = Commands::kMove;
  cmd.append(dir);
  send_cmd(cmd.c_str());
}

void send_report(const char *fields) {
  std::string cmd = Commands::kReport;
  cmd.append(fields);
  send_cmd(cmd.c_str());
}

void send_config_request() {
  send_cmd(Commands::kConfigRequest);
}

void send_line(const char *body) {
  if (!_semaphore) {
    return;
  }
  xSemaphoreTake(_semaphore, portMAX_DELAY);
  _send(body);
  xSemaphoreGive(_semaphore);
}

void send_cmd(const char *cmd) {
  // The mutex is created by send_task, which starts alongside the task that
  // receives packets - so a config request fired by the very first frame can
  // arrive before there is anything to take. Dropping that one request is
  // correct: the device asks again a second later, and taking a null semaphore
  // is undefined behaviour.
  if (!_semaphore) {
    return;
  }
  xSemaphoreTake(_semaphore, portMAX_DELAY);
  _queue.push_back(cmd);
  xSemaphoreGive(_semaphore);
}

void _send(const char *cmd) {
  Serial.printf("%s%s\n", Commands::kPrefix, cmd);
}

}
}