// Simulation backend for hal::feedback (native tests, STM32 placeholder).
// Models powered, healthy iHSS60 drives by default: PED follows the motor
// sim (arrived whenever the axis is not moving) and ALM is clear. Tests cut
// a drive's power, stall it or raise the alarm through the sim_* hooks.
// The Teensy 4.1 build reads the carrier opto inputs in feedback_teensy41.cpp.

#if !defined(TARGET_TEENSY41)

#include "hal/hal.h"

namespace hal::feedback {

namespace {
bool powered[kMaxAxes] = {true, true, true};
bool stalled[kMaxAxes] = {false, false, false};
bool alarm_on          = false;
}

// Test hooks. State survives init() so a test can "reboot" the controller
// with a drive still unpowered.
void sim_set_powered(Axis a, bool on) { if (a < kMaxAxes) powered[a] = on; }
void sim_set_stalled(Axis a, bool on) { if (a < kMaxAxes) stalled[a] = on; }
void sim_set_alarm(bool on)           { alarm_on = on; }
void sim_reset() {
    for (uint8_t a = 0; a < kMaxAxes; a++) { powered[a] = true; stalled[a] = false; }
    alarm_on = false;
}

void init() {}

bool arrived(Axis a) { return a < kMaxAxes && powered[a] && !stalled[a] && !hal::motor::busy(a); }
bool alarm()         { return alarm_on; }

} // namespace hal::feedback

#endif // !TARGET_TEENSY41
