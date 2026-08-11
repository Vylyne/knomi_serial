#include "identity.h"

#include <esp_mac.h>
#include <stdio.h>

namespace printer {
namespace identity {

const char *id() {
  static char text[7] = {0};
  if (text[0] != '\0') {
    return text;
  }

  uint8_t mac[6] = {0};
  // esp_read_mac rather than ESP.getEfuseMac(), which returns the six bytes
  // reversed against the printed order. The point of this identifier is that it
  // can be checked against `esptool chip_id` without anyone having to know that,
  // so it has to come out in the same order that prints.
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    // Nothing sensible to invent. An empty id reads on the host as "this device
    // cannot be addressed by identity", which is true, rather than as some
    // other device's.
    return text;
  }

  // The low three bytes only. The top three are Espressif's OUI and are the
  // same on every one of these, so they would be six characters of noise in
  // something a person has to copy into printer.cfg.
  snprintf(text, sizeof(text), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return text;
}

}
}
