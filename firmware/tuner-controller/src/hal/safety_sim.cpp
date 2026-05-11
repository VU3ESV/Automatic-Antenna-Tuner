// Simulation backend for hal::safety. The forward-power reading is
// held in a single float that defaults to 0 W. The master can inject
// a non-zero value via the debug `set_fwd_w` verb to exercise the
// lockout path without keying a transmitter (PLAN.md M1b.2 "fake
// fwd_w").
//
// Real implementations (M2) replace this with the AD8307 fwd-channel
// ADC sample fed through the same threshold compare. Invariant #1 is
// enforced in app::motion before any motor::move_* call.

#include "hal/hal.h"

namespace hal::safety {

namespace {

// Threshold matches the default tx_lockout_w in CLAUDE.md "Invariants".
// The master can lower it via config; the firmware-side default is
// intentionally conservative.
constexpr float kLockoutW = 5.0f;

float current_fwd_w = 0.0f;

}

void init() {
    current_fwd_w = 0.0f;
}

bool rf_present() {
    return current_fwd_w >= kLockoutW;
}

float fwd_w() {
    return current_fwd_w;
}

void inject_fwd_w(float w) {
    current_fwd_w = w;
}

} // namespace hal::safety
