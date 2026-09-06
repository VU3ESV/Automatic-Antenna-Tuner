#pragma once

// HAL interface — all platform-specific peripheral access goes through
// these namespaces. Implementations are split per-target under hal/ and
// gated by TARGET_TEENSY41 / TARGET_STM32H743 / TARGET_NATIVE macros
// driven from platformio.ini build_flags.
//
// Application logic (src/app/) MUST NOT include platform headers
// directly. If you need a peripheral that isn't exposed here yet, add
// it to this file first, then implement every target.
//
// Backends (docs/ARCHITECTURE.md §5.1.3):
//   motor    sim (native / stm32 placeholder)  | teensy41: FlexPWM pulse trains,
//                                              |   trapezoidal ramp, iHSS60 timing
//   relay    sim                               | teensy41: carrier relay-driver outputs
//   limits   sim                               | teensy41: carrier opto inputs
//   nvs      sim (RAM)                         | teensy41: emulated EEPROM
//   sdcard   sim (RAM files)                   | teensy41: built-in microSD (SD.h)
//   encoder  passthrough of the motor counter on every target (the iHSS60
//            has no readable encoder; an external QEI slots in here later)
//   safety   sim on every target until the AD8307 chain lands (M2)

#include <cstddef>
#include <cstdint>

namespace hal {

namespace led {
    void init();
    void set(bool on);
    void toggle();
}

// Stepper channels on the carrier: 0 = X, 1 = Y, 2 = Z. Which tuner
// element each one drives is the operator's `set_topology` choice
// (app::Topology), not a HAL property — the HAL only knows pins.
constexpr uint8_t kMaxAxes = 3;
using Axis = uint8_t;

// Balanced-L capacitor side select — mirrors app::Side / PROTOCOL.md.
enum class SideSel : uint8_t { HiZ, LoZ };

namespace motor {
    // Pulse-rate ceiling (FlexPWM / iHSS60: 200 kHz) and the speed /
    // ramp a driver runs at before the app restores its persisted
    // settings. app/config.h derives its defaults from these so the
    // numbers exist in exactly one place.
    constexpr uint32_t kMaxSpeed     = 200000;   // steps/s
    constexpr uint32_t kDefaultSpeed = 25600;    // steps/s — 4 rev/s at 6400 p/r (bench-proven cruise)
    constexpr uint32_t kDefaultAccel = 25600;    // steps/s²

    // Configure pins / pulse generators; drivers come up ENABLED and
    // stay enabled while powered (CLAUDE.md invariant 3 — a direct-
    // coupled vacuum capacitor must never be free to back-drive).
    void     init();

    // Drive towards an absolute target / a relative delta. Each call
    // replaces any in-flight target on that axis. Real backends run a
    // trapezoidal ramp from the axis' accel setting up to its speed
    // setting; the end position is exact (ISR-counted).
    void     move_to(Axis a, int32_t target_steps);
    void     move_by(Axis a, int32_t delta_steps);

    // Cut the pulse train immediately (no deceleration ramp — the drive
    // is closed-loop and this is also the limit-trip path). After
    // stop(), busy(a) is false and target(a) == position(a).
    void     stop(Axis a);

    // Open-loop step counter (exact — counted per pulse) and active target.
    int32_t  position(Axis a);
    int32_t  target(Axis a);

    // True while a pulse train is being emitted on the axis.
    bool     busy(Axis a);

    // Anchor the step counter to a known value (after the operator
    // declares home, or when restoring the NVRAM record at boot). Both
    // position and target are set so the axis sits still.
    void     set_position(Axis a, int32_t value);

    // Per-axis cruise speed (steps/s, 1..kMaxSpeed) and ramp rate
    // (steps/s²). Persisted by the app layer, not the HAL.
    void     set_speed(Axis a, uint32_t steps_per_s);
    void     set_accel(Axis a, uint32_t steps_per_s2);
    uint32_t speed(Axis a);
    uint32_t accel(Axis a);

    // Driver ENA line. Default on. Turning a driver off is a deliberate
    // operator action for setup (turning an element by hand), never an
    // idle-release optimisation.
    void     set_enabled(Axis a, bool on);
    bool     enabled(Axis a);

    // Service the ramp (real) / advance the sim. Called from
    // app::motion::tick() once per main-loop iteration.
    void     tick();
}

namespace encoder {
    void     init();

    // Position source of record for the axis. With the iHSS60 the drive
    // closes its loop internally and the controller sees no counts, so
    // this returns the motor's step counter; an external QEI replaces it
    // behind the same call. Anchoring rules: docs/ARCHITECTURE.md §5.2.
    int32_t  count(Axis a);
    void     set_count(Axis a, int32_t value);
}

namespace limits {
    void     init();

    // Per-axis end-stop opto input (carrier limit-X/Y/Z). True when the
    // input is asserted — switch closed, or with the production NC-in-
    // series wiring, a switch open / cable fault. Direction-latched
    // trip handling (invariant 7) lands with the lead-screw mechanism;
    // for now the app reports the level.
    bool     active(Axis a);
}

namespace relay {
    // Latches K3 into BYPASS before anything else runs (invariant 2) and
    // parks the Balanced-L side select at Hi-Z.
    void     init();

    void     set_side(SideSel s);
    SideSel  side();

    // K3 — the only relay verb safe to toggle while RF is present.
    void     set_bypass(bool on);
    bool     bypass();
}

namespace safety {
    void     init();

    // True when the latest forward-power reading would refuse motion
    // verbs. Threshold is tx_lockout_w (default 5 W).
    bool     rf_present();

    // Latest smoothed forward-power reading (watts).
    float    fwd_w();

    // Inject a fake forward-power reading. Tests + a debug verb use this
    // to exercise the lockout path without a transmitter. Real
    // implementations ignore this once the AD8307 feeds the ADC (M2).
    void     inject_fwd_w(float w);
}

namespace sdcard {
    // Teensy 4.1 built-in microSD (BUILTIN_SDCARD). Holds the human-
    // readable settings copy /tuner/config.json (app/settings.h); the
    // emulated EEPROM below stays the always-present, synchronous store.
    bool     init();                                              // mount; false when no card
    bool     present();                                           // card still inserted
    int      read_file(const char *path, char *buf, size_t max);  // bytes read (NUL-terminated), -1 if missing
    bool     write_file(const char *path, const char *data, size_t len);   // atomic replace
}

namespace nvs {
    // Byte-addressable non-volatile store for the per-axis records,
    // topology block and clean-shutdown position anchors (invariant 3).
    // Layout is owned by app/config.cpp. write() has update semantics:
    // bytes that already hold the value are not rewritten, so frequent
    // saves of an unchanged position cost no wear.
    constexpr size_t kSize = 1024;

    void     init();
    void     read(size_t offset, void *buf, size_t n);
    void     write(size_t offset, const void *buf, size_t n);
}

} // namespace hal
