#include "app/state.h"

#include <cmath>
#include <cstring>

namespace app {

namespace {
    constexpr float SWR_DEADBAND   = 0.01f;
    constexpr float POWER_DEADBAND = 0.5f;   // watts
    constexpr float Z_MAG_DEADBAND = 0.5f;   // ohms
    constexpr float Z_PHS_DEADBAND = 0.5f;   // degrees

    bool topology_differs(const Topology &a, const Topology &b) {
        if (a.kind != b.kind || a.n != b.n) return true;
        for (uint8_t i = 0; i < a.n && i < hal::kMaxAxes; i++) {
            if (strcmp(a.elements[i].name, b.elements[i].name) != 0) return true;
            if (a.elements[i].type != b.elements[i].type) return true;
            if (a.elements[i].axis != b.elements[i].axis) return true;
            if (a.elements[i].pair != b.elements[i].pair) return true;
        }
        return false;
    }

    bool axis_differs(const AxisSnapshot &a, const AxisSnapshot &b) {
        return a.steps != b.steps || a.enc != b.enc || a.moving != b.moving ||
               a.enabled != b.enabled || a.limit_sw != b.limit_sw ||
               a.element != b.element || a.kind != b.kind ||
               a.home_set != b.home_set || a.anchored != b.anchored ||
               a.max_steps != b.max_steps || a.travel != b.travel ||
               a.last_clamp != b.last_clamp || a.estop != b.estop || a.speed != b.speed ||
               a.accel != b.accel || std::fabs(a.max_rev - b.max_rev) > 1e-6f;
    }
}

const char *travel_name(Travel t) {
    switch (t) {
    case Travel::Unlimited: return "unlimited";
    case Travel::Unhomed:   return "unhomed";
    case Travel::BelowHome: return "below_home";
    case Travel::Home:      return "home";
    case Travel::InRange:   return "in_range";
    case Travel::Max:       return "max";
    case Travel::AboveMax:  return "above_max";
    }
    return "?";
}

bool Snapshot::differs(const Snapshot &o) const {
    if (topology_differs(topology, o.topology)) return true;
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        if (axis_differs(axes[a], o.axes[a])) return true;
    }
    if (side         != o.side)         return true;
    if (bypass       != o.bypass)       return true;
    if (moving       != o.moving)       return true;
    if (homed        != o.homed)        return true;
    if (rf_lockout   != o.rf_lockout)   return true;
    if (estop_all    != o.estop_all)    return true;
    if (sd_present   != o.sd_present)   return true;
    if (sd_ok        != o.sd_ok)        return true;
    if (settings_source != o.settings_source) return true;
    if (last_move_ms != o.last_move_ms) return true;

    if (std::fabs(fwd_w   - o.fwd_w)   > POWER_DEADBAND) return true;
    if (std::fabs(rev_w   - o.rev_w)   > POWER_DEADBAND) return true;
    if (std::fabs(swr     - o.swr)     > SWR_DEADBAND)   return true;
    if (std::fabs(z_mag   - o.z_mag)   > Z_MAG_DEADBAND) return true;
    if (std::fabs(z_phase - o.z_phase) > Z_PHS_DEADBAND) return true;
    return false;
}

} // namespace app
