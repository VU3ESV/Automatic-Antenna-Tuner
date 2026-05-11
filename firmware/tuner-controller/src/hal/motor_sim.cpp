// Simulation backend for hal::motor. Compiles on every target; runs
// instead of (eventually: alongside) real TMC2209 drivers until M1b.2
// hardware integration lands.
//
// Model: each axis has a step counter that advances up to
// kStepsPerTick toward its target on every motor::tick() call. tick()
// is invoked from app::motion::tick() once per main loop iteration —
// so motion rate scales with the loop frequency. The encoder backend
// reads the position directly, so motor/encoder stay in lock-step.

#include "hal/hal.h"

#include <cstddef>

namespace hal::motor {

namespace {

// How many steps a single tick advances per axis. Hand-tuned so that a
// full-range move (~30 000 steps) takes a few seconds at a 1 kHz loop —
// fast enough that the UI feels responsive, slow enough that the
// `moving:true` flag is visible in the browser.
constexpr int32_t kStepsPerTick = 32;

struct AxisState {
    int32_t pos    = 0;
    int32_t tgt    = 0;
};

AxisState axes[2];  // [0] = L, [1] = C

AxisState &axis(Axis a) { return axes[static_cast<size_t>(a)]; }

} // namespace

void init() {
    for (auto &a : axes) {
        a.pos = 0;
        a.tgt = 0;
    }
}

void move_to(Axis a, int32_t target_steps) {
    axis(a).tgt = target_steps;
}

void move_by(Axis a, int32_t delta_steps) {
    axis(a).tgt = axis(a).pos + delta_steps;
}

int32_t position(Axis a) { return axis(a).pos; }
int32_t target(Axis a)   { return axis(a).tgt; }
bool    busy(Axis a)     { return axis(a).pos != axis(a).tgt; }

void stop(Axis a) {
    axis(a).tgt = axis(a).pos;
}

void set_position(Axis a, int32_t value) {
    axis(a).pos = value;
    axis(a).tgt = value;
}

void tick() {
    for (auto &s : axes) {
        if (s.pos == s.tgt) continue;
        const int32_t delta = s.tgt - s.pos;
        if (delta > 0) {
            s.pos += (delta < kStepsPerTick) ? delta : kStepsPerTick;
        } else {
            const int32_t step = (-delta < kStepsPerTick) ? -delta : kStepsPerTick;
            s.pos -= step;
        }
    }
}

} // namespace hal::motor
