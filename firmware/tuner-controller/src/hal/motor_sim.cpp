// Simulation backend for hal::motor. Builds on every target that has no
// real driver yet (native tests, the STM32 placeholder). The Teensy 4.1
// build uses motor_teensy41.cpp instead.
//
// Model: each axis has a step counter that advances up to kStepsPerTick
// toward its target on every motor::tick() call. tick() is invoked from
// app::motion::tick() once per main loop iteration, so motion rate
// scales with the loop frequency. Speed / accel are stored so the app
// layer can round-trip them, but do not affect the sim rate.

#if !defined(TARGET_TEENSY41)

#include "hal/hal.h"

namespace hal::motor {

namespace {

constexpr int32_t kStepsPerTick = 32;

struct AxisState {
    int32_t  pos     = 0;
    int32_t  tgt     = 0;
    uint32_t speed   = 25600;
    uint32_t accel   = 25600;
    bool     enabled = true;
};

AxisState axes[kMaxAxes];

AxisState &axis(Axis a) { return axes[a < kMaxAxes ? a : 0]; }

} // namespace

void init() {
    for (auto &a : axes) a = AxisState{};
}

void move_to(Axis a, int32_t target_steps) { if (axis(a).enabled) axis(a).tgt = target_steps; }
void move_by(Axis a, int32_t delta_steps)  { move_to(a, axis(a).pos + delta_steps); }
void stop(Axis a)                          { axis(a).tgt = axis(a).pos; }

int32_t position(Axis a) { return axis(a).pos; }
int32_t target(Axis a)   { return axis(a).tgt; }
bool    busy(Axis a)     { return axis(a).pos != axis(a).tgt; }

void set_position(Axis a, int32_t value) {
    axis(a).pos = value;
    axis(a).tgt = value;
}

void     set_speed(Axis a, uint32_t v) { axis(a).speed = v; }
void     set_accel(Axis a, uint32_t v) { axis(a).accel = v; }
uint32_t speed(Axis a)                 { return axis(a).speed; }
uint32_t accel(Axis a)                 { return axis(a).accel; }
void     set_enabled(Axis a, bool on)  { axis(a).enabled = on; if (!on) stop(a); }
bool     enabled(Axis a)               { return axis(a).enabled; }

void tick() {
    for (auto &s : axes) {
        if (s.pos == s.tgt) continue;
        const int32_t delta = s.tgt - s.pos;
        if (delta > 0) {
            s.pos += (delta < kStepsPerTick) ? delta : kStepsPerTick;
        } else {
            s.pos -= (-delta < kStepsPerTick) ? -delta : kStepsPerTick;
        }
    }
}

} // namespace hal::motor

#endif // !TARGET_TEENSY41
