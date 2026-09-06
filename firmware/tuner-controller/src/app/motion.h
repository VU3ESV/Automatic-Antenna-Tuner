#pragma once

// Motion + safety orchestration. Sits between the two protocol
// front-ends (tuner_server for the master, http_server for the browser)
// and the HAL: every move / relay / config verb funnels through here so
// the invariants are enforced in one place:
//
//   #1 no motion under RF            — refuse_if_rf()
//   #2 bypass on power-up            — hal::relay::init() latches K3 first
//   #3 position truth is anchored    — per-axis `anchored` (home declared
//                                      and last shutdown clean); unanchored
//                                      axes move only in bypass (setup)
//   #7 travel window (soft limits)   — clamp_target() for every verb
//
// Application-layer code only; no platform headers, no JSON.

#include <cstdint>

#include "app/config.h"
#include "app/state.h"

namespace app::motion {

// Initialise every HAL peripheral, latch bypass, restore the persisted
// configuration + position anchors. Idempotent — re-running it is how
// the native tests simulate a power cycle.
void init();

// Advance one loop iteration: service the motors, persist positions on
// move-complete, rebuild `out` from HAL state.
void tick(uint32_t now_ms, Snapshot &out);

// Refusal codes returned alongside a false / Refused result.
struct Refusal {
    const char *code;   // "rf_lockout", "bad_axis", "not_anchored", "at_limit", ...
    const char *msg;
};

enum class MoveResult : uint8_t {
    Started,    // running to the requested target
    Clamped,    // running, but the target was trimmed to a window bound
    AtLimit,    // refused: already on the bound the move points at
    Refused,    // refused for another reason — see Refusal
    Noop,       // nothing to do (target == position)
};
inline bool accepted(MoveResult r) { return r == MoveResult::Started || r == MoveResult::Clamped || r == MoveResult::Noop; }

// ── Axis-addressed verbs (axis = carrier channel 0..kMaxAxes-1) ─────────

// Bounded move: relative (`is_delta`) or absolute. ±1 is the fine-tuning
// single step. Clamped to the axis' travel window when active.
MoveResult move_axis(uint8_t axis, int32_t value, bool is_delta, Refusal &err);

// "Continuous" run in `dir` (+1 / -1). On an axis with an active window
// this is a bounded run to that end of the window; on a free-rotating
// element it is unbounded. Refused for an element with stops whose home
// is not declared, and for an undeclared element kind.
MoveResult run_to_end(uint8_t axis, int dir, Refusal &err);

bool stop_axis(uint8_t axis, Refusal &err);   // immediate; always allowed
void stop_all();

// Emergency stop: cuts pulses immediately AND latches an alarm on the
// axis (estop) or on every axis (estop_all). A latched axis refuses
// every motion verb with code `estop` until released — the same
// semantics as an industrial E-stop mushroom button, which stays down
// until deliberately released. The latch is purely per axis:
// estop_all() latches every axis, estop_reset(a) releases one of them
// (even if it was latched by estop_all), estop_reset_all() releases
// every one; Snapshot::estop_all reports "every axis latched". Relay
// verbs, home declaration and configuration stay available while
// latched. Latches are not persisted: a power cycle clears them (the
// position anchors are protected separately by invariant 3).
bool estop(uint8_t axis, Refusal &err);
void estop_all();
bool estop_reset(uint8_t axis, Refusal &err);
void estop_reset_all();

// Declare the current position as home (0) — activates the window for
// kinds with stops and marks the axis anchored. Declare it a few steps
// INSIDE the physical stop; the bound is exactly 0.
bool set_home(uint8_t axis, Refusal &err);
// Forget home: window inactive, axis unanchored (setup moves only).
bool unset_home(uint8_t axis, Refusal &err);

// Declare what the motor drives; kinds with stops need max_rev > 0.
// A kind change clears home (a new device is on the shaft).
bool set_element(uint8_t axis, ElementKind kind, float max_rev, Refusal &err);
bool set_speed(uint8_t axis, uint32_t steps_per_s, uint32_t steps_per_s2, Refusal &err);
bool set_enabled(uint8_t axis, bool on, Refusal &err);

// Declare the wired network + element→axis map. Requires bypass and no
// motion; persisted, applied immediately.
bool set_topology(Topology t, Refusal &err);

// ── Network verbs ───────────────────────────────────────────────────────

bool set_side(Side s, Refusal &err);       // Balanced L only; refused under RF
bool set_bypass(bool on, Refusal &err);    // the only relay verb safe under RF
// Drive every topology-bound axis back to its declared home (0). Needs
// every one of them anchored.
bool home(Refusal &err);
bool set_fwd_w_fake(float w, Refusal &err);

// ── Legacy aliases for the v1 master protocol (Balanced L only) ─────────
bool move_l(int32_t value, bool is_delta, Refusal &err);
bool move_c(int32_t value, bool is_delta, Refusal &err);

// ── Lookups ─────────────────────────────────────────────────────────────
const Topology   &topology();
const AxisConfig &axis_config(uint8_t axis);
// Resolve "L" / "C1" / ... via the topology, or "0".."2" as an index. -1 if unknown.
int resolve_axis(const char *name_or_index);

} // namespace app::motion
