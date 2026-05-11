// Simulation backend for hal::relay. Three relays in the L-network
// (CLAUDE.md "RF topology"): K1/K2 select Hi-Z vs Lo-Z (mutually
// exclusive), K3 latches the network in bypass.
//
// The sim is pure state — no settle time, no contact-bounce model.
// Real implementations gate these behind opto-isolated MOSFETs feeding
// the HV bias supply, and enforce K1/K2 mutual exclusion in hardware
// (M1b.2 + M5).

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
