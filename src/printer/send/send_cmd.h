#ifndef SEND_CMD_H
#define SEND_CMD_H

namespace printer {
namespace send {

namespace Commands {

extern const char *kPrefix;
extern const char *kStop;
extern const char *kRestart;
extern const char *kGcode;
extern const char *kMove;
extern const char *kReport;
extern const char *kConfigRequest;

}

void send_stop();
void send_gcode(const char *gcode);
void send_move(const char *dir);
void send_report(const char *fields);

//: Ask the host to send config, because the CRC it stamps on every state frame
//: stops matching the config we hold. Rate limited by printer::config.
void send_config_request();

void send_cmd(const char *cmd);

}
}

#endif