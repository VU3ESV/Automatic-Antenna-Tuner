#pragma once

#include <cstdint>

#include "app/config.h"

// Tuner-controller state snapshot — the source for the `state` frame on
// the master link (docs/PROTOCOL.md §2.2) and for the HTTP /api/status
// JSON. Diffing semantics match docs/ARCHITECTURE.md §5.1 (telemetry
// diffing): differs() gates a broadcast.

namespace app {

enum class Side : uint8_t { HiZ, LoZ };

// Where an axis sits relative to its software travel window.
enum class Travel : uint8_t {
    Unlimited,   // element kind has no stops (or none declared)
    Unhomed,     // kind has stops but home not declared — window inactive
    BelowHome,   // outside the window, past 0
    Home,        // exactly on the home bound
    InRange,
    Max,         // exactly on the max bound
    AboveMax,    // outside the window, past max
};
const char *travel_name(Travel t);

struct AxisSnapshot {
    int32_t     steps      = 0;
    int32_t     enc        = 0;
    bool        moving     = false;
    bool        enabled    = true;
    bool        limit_sw   = false;   // end-stop opto input level
    int8_t      element    = -1;      // index into Snapshot::topology.elements, -1 = spare
    ElementKind kind       = ElementKind::Unset;
    bool        home_set   = false;
    bool        anchored   = false;   // position trusted (invariant 3)
    float       max_rev    = 0.0f;
    int32_t     max_steps  = 0;
    Travel      travel     = Travel::Unlimited;
    int8_t      last_clamp = 0;       // -1 home / +1 max bound trimmed the last verb
    bool        estop      = false;   // emergency-stop alarm latched on this axis
    uint32_t    speed      = kDefaultSpeed;
    uint32_t    accel      = kDefaultAccel;
};

struct Snapshot {
    Topology     topology;
    AxisSnapshot axes[hal::kMaxAxes];

    Side     side   = Side::HiZ;
    bool     bypass = true;          // invariant #2: bypass on power-up.

    // Aggregates — see docs/PROTOCOL.md §2.2.
    bool     moving       = false;
    bool     homed        = false;   // every topology-bound axis anchored
    bool     rf_lockout   = false;
    bool     estop_all    = false;   // every axis has its E-stop alarm latched (E-STOP ALL pressed)
    bool     sd_present   = false;   // microSD card mounted
    bool     sd_ok        = false;   // /tuner/config.json read or written successfully
    SettingsSource settings_source = SettingsSource::Defaults;
    uint32_t last_move_ms = 0;       // millis() of the last completed move (0 = none this boot)

    // Measurements (smoothed; raw values bypass diffing).
    float fwd_w   = 0.0f;
    float rev_w   = 0.0f;
    float swr     = 0.0f;
    float z_mag   = 0.0f;
    float z_phase = 0.0f;

    // Returns true if any field has changed beyond its deadband.
    bool differs(const Snapshot &other) const;

    // Legacy v1 accessors for the master's l_steps / c_steps fields:
    // the axis bound to element `name`, or -1.
    int axis_of(const char *name) const { return topology.element_by_name(name) >= 0
                                                 ? topology.elements[topology.element_by_name(name)].axis : -1; }
};

} // namespace app
