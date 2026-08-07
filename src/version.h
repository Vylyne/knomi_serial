#ifndef VERSION_H
#define VERSION_H

// Both of these are injected by scripts/version.py at build time. The fallbacks
// only apply if the firmware is built without that pre-build script.
#ifndef KNOMI_FW_VERSION
#define KNOMI_FW_VERSION "unknown"
#endif

#ifndef KNOMI_BUILD_VARIANT
#define KNOMI_BUILD_VARIANT "unknown"
#endif

#endif
