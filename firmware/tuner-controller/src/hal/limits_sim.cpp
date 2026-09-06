// Simulation backend for hal::limits (native tests, STM32 placeholder):
// no switch is ever asserted. The Teensy 4.1 build reads the carrier
// opto inputs in limits_teensy41.cpp.

#if !defined(TARGET_TEENSY41)

#include "hal/hal.h"

namespace hal::limits {

void init() {}
bool active(Axis a) { (void)a; return false; }

} // namespace hal::limits

#endif // !TARGET_TEENSY41
