#include "app/state.h"

#include <cmath>

namespace app {

namespace {
    constexpr float SWR_DEADBAND   = 0.01f;
    constexpr float POWER_DEADBAND = 0.5f;   // watts
    constexpr float Z_MAG_DEADBAND = 0.5f;   // ohms
    constexpr float Z_PHS_DEADBAND = 0.5f;   // degrees
}

bool Snapshot::differs(const Snapshot& o) const {
    if (l_steps      != o.l_steps)      return true;
    if (c_steps      != o.c_steps)      return true;
    if (l_enc        != o.l_enc)        return true;
    if (c_enc        != o.c_enc)        return true;
    if (side         != o.side)         return true;
    if (bypass       != o.bypass)       return true;
    if (moving       != o.moving)       return true;
    if (homed        != o.homed)        return true;
    if (last_move_ms != o.last_move_ms) return true;

    if (std::fabs(fwd_w   - o.fwd_w)   > POWER_DEADBAND) return true;
    if (std::fabs(rev_w   - o.rev_w)   > POWER_DEADBAND) return true;
    if (std::fabs(swr     - o.swr)     > SWR_DEADBAND)   return true;
    if (std::fabs(z_mag   - o.z_mag)   > Z_MAG_DEADBAND) return true;
    if (std::fabs(z_phase - o.z_phase) > Z_PHS_DEADBAND) return true;
    return false;
}

} // namespace app
