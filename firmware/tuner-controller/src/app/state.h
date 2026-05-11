#pragma once

#include <cstdint>

// Tuner-controller state snapshot. Same shape as the `state` + `telemetry`
// frames in docs/PROTOCOL.md (which doesn't exist yet — fill in alongside).
// Diffing semantics match docs/ARCHITECTURE.md §5.1 (telemetry diffing).

namespace app {

enum class Side : uint8_t { HiZ, LoZ };

struct Snapshot {
    // Mechanical state.
    uint32_t l_steps  = 0;
    uint32_t c_steps  = 0;
    int32_t  l_enc    = 0;
    int32_t  c_enc    = 0;
    Side     side     = Side::HiZ;
    bool     bypass   = true;     // invariant #2: bypass on power-up.

    // Motion / homing status — see docs/PROTOCOL.md §2.2.
    bool     moving       = false;
    bool     homed        = false;
    // millis() value of the last completed move (0 = never moved this boot).
    uint32_t last_move_ms = 0;

    // Measurements (smoothed; raw values bypass diffing).
    float fwd_w   = 0.0f;
    float rev_w   = 0.0f;
    float swr     = 0.0f;
    float z_mag   = 0.0f;
    float z_phase = 0.0f;

    // Returns true if any field has changed beyond its deadband.
    bool differs(const Snapshot& other) const;
};

} // namespace app
