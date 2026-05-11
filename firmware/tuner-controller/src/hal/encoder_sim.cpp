// Simulation backend for hal::encoder. Couples the encoder count
// directly to the motor's step counter — a perfectly-calibrated 1:1
// encoder, no missed steps, no noise. Real implementations (M1b.2)
// will read the Teensy's hardware QEI and apply per-axis cal.
//
// The set_count() entry point matches the post-home and NVRAM-restore
// path from docs/ARCHITECTURE.md §5.2: after homing or anchor restore,
// the application sets the encoder reference to a known value and the
// motor position counter is reset to match.

#include "hal/hal.h"

#include <cstddef>

namespace hal::encoder {

namespace {

int32_t counts[2] = {0, 0};

}

void init() {
    counts[0] = 0;
    counts[1] = 0;
}

int32_t count(Axis a) {
    // Sim couples encoder to the motor's open-loop position so a verified
    // motion loop reads `l_enc == l_steps` after every move. Real
    // hardware reads the QEI peripheral instead.
    return motor::position(a) + counts[static_cast<size_t>(a)];
}

void set_count(Axis a, int32_t value) {
    // Offset so future count(a) reads return `value` until the motor moves.
    counts[static_cast<size_t>(a)] = value - motor::position(a);
}

} // namespace hal::encoder
