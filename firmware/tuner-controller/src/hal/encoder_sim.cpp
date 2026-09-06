// hal::encoder — passthrough of the motor step counter, on every target.
//
// With the iHSS60 integrated drives the position loop closes inside the
// drive and the controller sees no counts (only ALM / PED), so the
// step counter is the position source of record while the drive is
// healthy (CLAUDE.md invariant 3). This file keeps the interface alive
// so an external QEI (non-integrated motor, Phase-2 fallback) slots in
// behind the same calls; docs/ARCHITECTURE.md §5.2.
//
// set_count() anchors the reported count independently of the motor
// counter (after the operator declares home, or on NVRAM restore), so a
// verified motion loop reads enc == steps after every move.

#include "hal/hal.h"

namespace hal::encoder {

namespace {
int32_t offsets[kMaxAxes] = {0, 0, 0};
}

void init() {
    for (auto &o : offsets) o = 0;
}

int32_t count(Axis a) {
    if (a >= kMaxAxes) return 0;
    return motor::position(a) + offsets[a];
}

void set_count(Axis a, int32_t value) {
    if (a >= kMaxAxes) return;
    offsets[a] = value - motor::position(a);
}

} // namespace hal::encoder
