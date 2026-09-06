// Simulation backend for hal::relay (native tests, STM32 placeholder).
// Pure state — no settle time, no contact-bounce model. The Teensy 4.1
// build uses relay_teensy41.cpp.

#if !defined(TARGET_TEENSY41)

#include "hal/hal.h"

namespace hal::relay {

namespace {

SideSel current_side   = SideSel::HiZ;
bool    bypass_engaged = true;   // invariant #2: bypass on power-up.

}

void init() {
    current_side   = SideSel::HiZ;
    bypass_engaged = true;
}

void set_side(SideSel s) { current_side = s; }
SideSel side()           { return current_side; }

void set_bypass(bool on) { bypass_engaged = on; }
bool bypass()            { return bypass_engaged; }

} // namespace hal::relay

#endif // !TARGET_TEENSY41
