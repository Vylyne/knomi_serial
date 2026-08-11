#ifndef PRINTER_IDENTITY_H
#define PRINTER_IDENTITY_H

namespace printer {
namespace identity {

// Who this display is, permanently.
//
// The CH340K carries no USB serial number - verified on the bench, the
// descriptor is empty - so nothing about the USB side of the link distinguishes
// one display from another. Every way of naming a port therefore names a
// *socket*: /dev/ttyUSB0 moves on reboot, a /dev/serial/by-id path for a second
// identical unit is disambiguated by the kernel in enumeration order rather
// than by the device, and a udev rule keyed to KERNELS== is stable only until
// somebody moves a cable. On a toolchanger that last one is the dangerous
// shape: swap two leads and two screens quietly describe the wrong tool, with
// nothing on the glass to say so.
//
// The chip has had a unique number in eFuse the whole time. Reporting it costs
// nothing, needs no storage, and cannot be duplicated, lost or reset - which is
// the whole argument against pairing codes here. A generated code kept in NVS
// survives a firmware upload but not an erase_flash and not a change to the
// partition table; this survives all three, because it is not stored anywhere.
const char *id();

}
}

#endif
