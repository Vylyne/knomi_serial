#ifndef PRINTER_CONFIG_H
#define PRINTER_CONFIG_H

#include <stdint.h>

#include "printer/printer.h"

namespace printer {
namespace config {

//: The settings in force. Never null, and valid from before the first packet:
//: it starts at the compile-time defaults in user_conf.h, so a printer.cfg that
//: overrides nothing behaves exactly as the firmware was built to.
//:
//: Read it fresh each time rather than holding the reference. Adopting a new
//: config swaps which of two buffers is live, so a reference kept across a swap
//: goes stale - it stays readable, it just stops being current.
const Config &get();

//: Adopt a config payload as it arrived: kConfigWireSize bytes, network order.
//: Returns false if the payload is the wrong size.
bool apply(const void *payload, uint32_t len);

//: CRC32 of the payload currently held, or zero before the host has sent one.
//: Computed here from the bytes actually received rather than taken on trust
//: from the frame, so a config that arrived damaged cannot match the host's and
//: will be asked for again.
uint32_t held_crc();

//: Whether to ask the host for config, given what it says it is holding.
//:
//: True when the CRCs disagree and the last ask was long enough ago. Rate
//: limiting is here rather than at the call site because this is the only thing
//: that knows whether an ask is already outstanding, and a device that could
//: never adopt the config - a length mismatch against an incompatible host -
//: would otherwise ask ten times a second forever.
bool should_request(uint32_t host_crc, uint32_t now_ms);

//: zlib-compatible CRC32. Bitwise: this runs over 276 bytes when config
//: changes, which is a few times a day at most, so a 1KB table would cost more
//: than it saves.
uint32_t crc32(const void *data, uint32_t len);

}
}

#endif
