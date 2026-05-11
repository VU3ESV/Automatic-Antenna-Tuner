#pragma once

// HAL interface — all platform-specific peripheral access goes through
// these namespaces. Implementations are split per-target under hal/ and
// gated by TARGET_TEENSY41 / TARGET_STM32H743 / TARGET_NATIVE macros
// driven from platformio.ini build_flags.
//
// Application logic (src/app/) MUST NOT include platform headers
// directly. If you need a peripheral that isn't exposed here yet, add
// it to this file first, then implement all three targets.
//
// Motor / encoder / relay / safety currently have a single "sim"
// implementation that compiles on every target. Real drivers (TMC2209
// over UART, hardware QEI, GPIO + opto + HV bias, AD8307 thresholding)
// land under M1b.2 hardware integration; until then the sim keeps the
// control loop and the master ↔ controller protocol honest end-to-end.

#include <cstdint>

namespace hal {

namespace led {
    void init();
    void set(bool on);
    void toggle();
}

// Two stepper-actuated axes, one each for the roller inductor (L) and the
// vacuum-variable capacitor (C).
enum class Axis : uint8_t { L = 0, C = 1 };

// L-network capacitor side select — mirrors app::Side / PROTOCOL.md.
enum class SideSel : uint8_t { HiZ, LoZ };

namespace motor {
    void    init();

    // Drive towards an absolute target / a relative delta. Each call
    // replaces any in-flight target on that axis. Targets are open-loop
    // step counters; the encoder is the position truth (invariant #3).
    void    move_to(Axis a, int32_t target_steps);
    void    move_by(Axis a, int32_t delta_steps);

    // Current open-loop step count and active target.
    int32_t position(Axis a);
    int32_t target(Axis a);

    // Truthy while position != target (the motor task is still ticking
    // it forward).
    bool    busy(Axis a);

    // Abort an in-flight move. After stop(), busy(a) is false and
    // target(a) == position(a).
    void    stop(Axis a);

    // Anchor the open-loop step counter to a known value. Used after
    // a successful home routine, mirroring the
    // `stepper.setCurrentPosition()` call on real driver libraries.
    // Both the position and the active target are set to `value` so
    // the motor sits still at the new reference.
    void    set_position(Axis a, int32_t value);

    // Advance the motion sim / real drivers by one cycle. Called by
    // app::motion::tick(); not normally called directly by anyone else.
    void    tick();
}

namespace encoder {
    void    init();

    // Latest quadrature count for the axis. Sim couples this to the
    // motor's position; real implementations read the QEI peripheral.
    int32_t count(Axis a);

    // Anchor the encoder count to a known value (after homing, or on
    // restore from the NVRAM clean-shutdown record). See
    // docs/ARCHITECTURE.md §5.2.
    void    set_count(Axis a, int32_t value);
}

namespace relay {
    void    init();

    void    set_side(SideSel s);
    SideSel side();

    // K3 — the only relay verb safe to toggle while RF is present.
    void    set_bypass(bool on);
    bool    bypass();
}

namespace safety {
    void    init();

    // True when the latest forward-power reading would refuse motion
    // verbs. Threshold is the tx_lockout_w from config (default 5 W).
    bool    rf_present();

    // Latest smoothed forward-power reading (watts).
    float   fwd_w();

    // Inject a fake forward-power reading. Tests + a master debug verb
    // use this to exercise the lockout path without a transmitter
    // (PROTOCOL.md / PLAN.md M1b.2 "fake fwd_w"). Real implementations
    // ignore this once an AD8307 feeds the ADC task in M2.
    void    inject_fwd_w(float w);
}

} // namespace hal
