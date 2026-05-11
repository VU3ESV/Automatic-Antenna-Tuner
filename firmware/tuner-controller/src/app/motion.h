#pragma once

// Motion + safety orchestration. Sits between the protocol dispatcher
// (tuner_server) and the HAL: every move / relay / safety verb funnels
// through here so invariant #1 ("no motion under RF") is enforced in
// one place. Snapshot mutation lives here too — tick() reads the HAL
// each loop iteration and rebuilds the published state.
//
// Application-layer code only; no platform headers, no JSON. The
// per-verb argument decoders live in app::protocol.

#include <cstdint>

#include "app/state.h"

namespace app::motion {

// Initialise every HAL peripheral. Idempotent — safe to call again
// from a resync.
void init();

// Advance one loop iteration. Drives the motor sim, copies HAL state
// into `out`, and tags `last_move_ms` with `now_ms` whenever an axis
// completes a move. Doesn't publish — caller passes `out` to
// tuner_server::publish().
void tick(uint32_t now_ms, Snapshot &out);

// Refusal codes returned alongside `accept=false`. Mirrored to ack.err.
struct Refusal {
    const char *code;   // e.g. "rf_lockout", "bad_args"
    const char *msg;
};

// True on accepted; on refusal, `out_err` is populated.
//
// Verbs:
//   move_l / move_c — `is_delta` selects move_by vs move_to.
//   set_side        — refused under RF.
//   set_bypass      — the only relay verb safe under RF (invariant).
//   home            — refused under RF; sim homes instantly to (0, 0).
//   set_fwd_w_fake  — debug verb so the master can exercise the
//                     lockout path without keying a transmitter.

bool move_l(int32_t value, bool is_delta, Refusal &out_err);
bool move_c(int32_t value, bool is_delta, Refusal &out_err);
bool set_side(Side s, Refusal &out_err);
bool set_bypass(bool on, Refusal &out_err);
bool home(Refusal &out_err);
bool set_fwd_w_fake(float w, Refusal &out_err);

} // namespace app::motion
