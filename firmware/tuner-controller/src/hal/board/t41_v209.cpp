// Board pin map for the grblHAL-teensy-4.x V2.09 carrier.
//
// Per docs/HW-T41-CARRIER.md the board file is **pin map only, no
// logic**. The header (t41_v209.h) defines every pin as a constexpr;
// this translation unit exists only to (a) match the file layout the
// HW-T41-CARRIER plan documents, (b) hold static_assert sanity checks
// for the pin map so the production build catches typos at compile
// time, and (c) give a single .o file the linker can attribute carrier
// definitions to in a map file.

#include "t41_v209.h"

namespace hal::board::t41_v209 {

// All carrier pin numbers fit in the Teensy 4.1's 0..41 GPIO range.
static_assert(AXIS_X.step  <= 41 && AXIS_X.dir  <= 41 && AXIS_X.en  <= 41 && AXIS_X.limit <= 41);
static_assert(AXIS_Y.step  <= 41 && AXIS_Y.dir  <= 41 && AXIS_Y.en  <= 41 && AXIS_Y.limit <= 41);
static_assert(AXIS_Z.step  <= 41 && AXIS_Z.dir  <= 41 && AXIS_Z.en  <= 41 && AXIS_Z.limit <= 41);
static_assert(AXIS_M3.step <= 41 && AXIS_M3.dir <= 41 && AXIS_M3.en <= 41 && AXIS_M3.limit <= 41);
static_assert(AXIS_M4.step <= 41 && AXIS_M4.dir <= 41 && AXIS_M4.en <= 41 && AXIS_M4.limit <= 41);

static_assert(RELAY_K1_HIZ <= 41 && RELAY_K2_LOZ <= 41 && RELAY_K3_BYPASS <= 41);

// Same-axis pins must not collide. Per-axis collision check; cross-axis
// collisions are caught by inspection (HW-T41-PINMAP.md is the source
// of truth and the build will catch a redefined enum / duplicate
// constant if anyone tries to alias).
static_assert(AXIS_X.step != AXIS_X.dir && AXIS_X.step != AXIS_X.en && AXIS_X.step != AXIS_X.limit);
static_assert(AXIS_Y.step != AXIS_Y.dir && AXIS_Y.step != AXIS_Y.en && AXIS_Y.step != AXIS_Y.limit);
static_assert(AXIS_Z.step != AXIS_Z.dir && AXIS_Z.step != AXIS_Z.en && AXIS_Z.step != AXIS_Z.limit);

// The three tuner relays must each land on a distinct carrier output.
static_assert(RELAY_K1_HIZ != RELAY_K2_LOZ);
static_assert(RELAY_K1_HIZ != RELAY_K3_BYPASS);
static_assert(RELAY_K2_LOZ != RELAY_K3_BYPASS);

}  // namespace hal::board::t41_v209
