// hal::limits on the Teensy 4.1 / grblHAL V2.09 carrier: the per-axis
// end-stop opto inputs (limit-X/Y/Z, pins 20/21/22 per
// docs/HW-T41-PINMAP.md §2). Opto conducting = input LOW = asserted
// (board::LIMIT_ACTIVE_LOW). With the production NC-in-series wiring
// of the lead-screw home + max switches, "asserted" means a switch has
// opened — at a limit, or a cable fault.
//
// Read-only for now: the level is reported in the snapshot. Direction-
// latched trip handling (cut pulses, homed:false — CLAUDE.md invariant
// 7) lands together with the lead-screw mechanism (PLAN.md M1b.2).

#ifdef TARGET_TEENSY41

#include "hal/hal.h"

#include <Arduino.h>

#include "hal/board/t41_v209.h"

namespace hal::limits {

namespace {
namespace board = hal::board::t41_v209;
const uint8_t pins[kMaxAxes] = { board::AXIS_X.limit, board::AXIS_Y.limit, board::AXIS_Z.limit };
}

void init() {
    for (uint8_t p : pins) pinMode(p, INPUT_PULLUP);
}

bool active(Axis a) {
    if (a >= kMaxAxes) return false;
    const int level = digitalRead(pins[a]);
    return board::LIMIT_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
}

} // namespace hal::limits

#endif // TARGET_TEENSY41
