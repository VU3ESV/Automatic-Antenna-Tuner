#include "app/motion.h"

#include <cstring>

#include "app/ota.h"
#include "app/settings.h"
#include "hal/hal.h"

namespace app::motion {

namespace {

// A target far outside any plausible window; clamp_target() turns it
// into "the bound in that direction" for run-to-end moves.
constexpr int32_t kFarAway = 1000000000L;

Persisted cfg;                            // runtime copy of the persisted config
int8_t    last_clamp[hal::kMaxAxes]    = {0, 0, 0};
bool      prev_busy[hal::kMaxAxes]     = {false, false, false};
bool      dirty_written[hal::kMaxAxes] = {false, false, false};
bool      estop_latched[hal::kMaxAxes] = {false, false, false};
uint32_t  last_move_ms                 = 0;

// Drive feedback supervision (iHSS60 PED / ALM). Inert unless cfg.feedback
// enables it — see set_feedback() and docs/HW-T41-PINMAP.md §2.2.
constexpr uint32_t kArrivalTimeoutMs = 2000;   // PED must confirm a finished move within this
constexpr uint32_t kDriveLostMs      = 250;    // PED off this long at rest = drive lost
constexpr uint32_t kAlarmConfirmMs   = 20;     // ALM held this long clears home (pulses stop at once)
bool       awaiting_arrival[hal::kMaxAxes] = {false, false, false};
uint32_t   train_end_ms[hal::kMaxAxes]     = {0, 0, 0};
uint32_t   ped_off_since[hal::kMaxAxes]    = {0, 0, 0};
bool       drive_ready[hal::kMaxAxes]      = {false, false, false};   // PED seen on at rest since boot / enable
DriveFault drive_fault[hal::kMaxAxes]      = {DriveFault::None, DriveFault::None, DriveFault::None};
uint32_t   alarm_since_ms                  = 0;
bool       alarm_handled                   = false;

bool refuse(Refusal &e, const char *code, const char *msg) {
    e.code = code;
    e.msg  = msg;
    return false;
}

bool refuse_if_rf(Refusal &e) {
    if (hal::safety::rf_present()) {
        refuse(e, "rf_lockout", "fwd_w exceeds tx_lockout_w");
        return true;
    }
    return false;
}

// Between an accepted firmware_apply and the reboot (app::ota::tick) no
// verb may start motion or take the network out of bypass: the reboot
// would cut a move dead and leave the invariant-3 dirty marker set.
bool refuse_if_updating(Refusal &e) {
    if (app::ota::apply_pending()) {
        refuse(e, "updating", "a firmware update is being applied - the controller is about to reboot");
        return true;
    }
    return false;
}

bool refuse_if_bad_axis(uint8_t a, Refusal &e) {
    if (a >= hal::kMaxAxes) {
        refuse(e, "bad_axis", "axis index out of range");
        return true;
    }
    return false;
}

bool anchored(uint8_t a) { return cfg.axis[a].home_set; }

hal::SideSel to_hal(Side s)          { return s == Side::HiZ ? hal::SideSel::HiZ : hal::SideSel::LoZ; }
Side         from_hal(hal::SideSel s){ return s == hal::SideSel::HiZ ? Side::HiZ : Side::LoZ; }

Travel travel_of(uint8_t a) {
    const AxisConfig &c = cfg.axis[a];
    if (!kind_has_stops(c.kind) || c.max_steps() <= 0) return Travel::Unlimited;
    if (!c.home_set) return Travel::Unhomed;
    const int32_t p = hal::motor::position(a);
    const int32_t m = c.max_steps();
    if (p < 0)  return Travel::BelowHome;
    if (p == 0) return Travel::Home;
    if (p < m)  return Travel::InRange;
    if (p == m) return Travel::Max;
    return Travel::AboveMax;
}

// Clamp a requested absolute target into [0, max_steps]. `hit` reports
// which bound trimmed it (-1 home, +1 max, 0 none). A move heading back
// *into* the window from outside is always allowed, so an axis whose
// window was tightened after the fact can still be recovered.
int32_t clamp_target(uint8_t a, int32_t target, int &hit) {
    hit = 0;
    const AxisConfig &c = cfg.axis[a];
    if (!c.limits_active()) return target;
    const int32_t pos = hal::motor::position(a);
    const int32_t max = c.max_steps();
    if (target > max && target > pos) { hit = +1; return pos > max ? pos : max; }
    if (target < 0   && target < pos) { hit = -1; return pos < 0 ? pos : 0; }
    return target;
}

// Invariant 3 bookkeeping: a move in flight makes the stored position a
// non-anchor until it completes. One small write per move each way.
void mark_dirty(uint8_t a) {
    if (dirty_written[a]) return;
    settings::save_position(a, hal::motor::position(a), true);
    dirty_written[a] = true;
}
void mark_clean(uint8_t a) {
    cfg.position[a] = hal::motor::position(a);
    cfg.dirty[a]    = false;
    settings::save_position(a, cfg.position[a], false);
    dirty_written[a] = false;
}

// ── Drive feedback (iHSS60 PED / ALM) ───────────────────────────────────

bool bound(uint8_t a) { return cfg.topology.axis_active(a); }

// PED is supervised on an axis when enabled, the axis drives a topology
// element (a spare channel has nothing wired), and its driver is enabled
// (a driver switched off for setup drops PED by design).
bool ped_supervised(uint8_t a) { return cfg.feedback.ped && bound(a) && hal::motor::enabled(a); }

bool alarm_active() { return cfg.feedback.alm && hal::feedback::alarm(); }

// The shaft can no longer be trusted to match the counter: clear home
// (persisted — survives a reboot) and latch why for the operator. The first
// cause is kept: an alarm also stops the drive arriving, and "alarm" is the
// useful report, not the no-arrival timeout that follows it.
void drop_anchor(uint8_t a, DriveFault why) {
    if (drive_fault[a] == DriveFault::None) drive_fault[a] = why;
    if (cfg.axis[a].home_set) {
        cfg.axis[a].home_set = false;
        settings::save_axis(a, cfg.axis[a]);
    }
}

bool refuse_if_drive_fault(uint8_t a, Refusal &e) {
    if (alarm_active()) {
        refuse(e, "drive_alarm", "a drive reports an alarm (ALM) - check the drives' red LEDs; motion refused");
        return true;
    }
    // Not while moving (PED is off by design) or while a finished move is
    // still waiting for arrival (a quick follow-up click is fine).
    if (ped_supervised(a) && !hal::motor::busy(a) && !awaiting_arrival[a] && !hal::feedback::arrived(a)) {
        refuse(e, "drive_not_ready", "the drive does not report arrival (PED off) - motor power, cable or drive fault; motion refused");
        return true;
    }
    return false;
}

// Once per tick, before the axes are read: the first alarm sample cuts
// pulses everywhere (invariant 7 — no ramp); an alarm that persists means
// shaft and counter disagree somewhere, and with the ALMs wired in parallel
// we cannot tell where, so every bound element loses its home. A glitch
// shorter than kAlarmConfirmMs only stops the moves.
void supervise_alarm(uint32_t now_ms) {
    if (!alarm_active()) { alarm_since_ms = 0; alarm_handled = false; return; }
    if (alarm_since_ms == 0) {
        alarm_since_ms = now_ms ? now_ms : 1;
        for (uint8_t a = 0; a < hal::kMaxAxes; a++) hal::motor::stop(a);
        return;
    }
    if (!alarm_handled && now_ms - alarm_since_ms >= kAlarmConfirmMs) {
        alarm_handled = true;
        for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
            if (bound(a)) drop_anchor(a, DriveFault::Alarm);
        }
    }
}

// Per axis, every tick, after the end of a pulse train has been noted.
void supervise_ped(uint8_t a, bool busy, uint32_t now_ms) {
    if (!ped_supervised(a)) {
        if (awaiting_arrival[a]) {
            // Supervision ended before the drive confirmed arrival (its driver
            // was switched off): the position is unconfirmed, not an anchor.
            // Feedback and topology changes, which could also end it, are
            // refused until the wait resolves (any_busy()).
            awaiting_arrival[a] = false;
            if (!hal::feedback::arrived(a)) drop_anchor(a, DriveFault::NoArrival);
            mark_clean(a);
        }
        ped_off_since[a] = 0;
        drive_ready[a]   = false;
        return;
    }
    if (busy) { ped_off_since[a] = 0; return; }              // PED normally drops while moving
    const bool arrived = hal::feedback::arrived(a);
    if (awaiting_arrival[a]) {
        if (arrived) {
            awaiting_arrival[a] = false;
            drive_ready[a]      = true;
            mark_clean(a);                                    // confirmed → clean anchor
        } else if (now_ms - train_end_ms[a] >= kArrivalTimeoutMs) {
            awaiting_arrival[a] = false;
            drop_anchor(a, DriveFault::NoArrival);            // stall, alarm or no motor power
            mark_clean(a);                                    // counter kept as the best estimate, not an anchor
        }
        return;
    }
    if (arrived) { drive_ready[a] = true; ped_off_since[a] = 0; return; }
    // PED off at rest. Before the drive has reported ready (e.g. the
    // controller booted with motor power off) this only refuses motion;
    // once it has, the drive lost power or the shaft was forced.
    if (!drive_ready[a]) return;
    if (ped_off_since[a] == 0) { ped_off_since[a] = now_ms ? now_ms : 1; return; }
    if (now_ms - ped_off_since[a] >= kDriveLostMs) {
        drop_anchor(a, DriveFault::DriveLost);
        drive_ready[a]   = false;
        ped_off_since[a] = 0;
    }
}

// Every bounded move funnels through here so the travel window and the
// anchoring rule are enforced in exactly one place.
MoveResult begin_move(uint8_t a, int32_t target, Refusal &e) {
    if (refuse_if_updating(e)) return MoveResult::Refused;
    if (estop_latched[a]) {
        refuse(e, "estop", "emergency stop latched — release it before moving");
        return MoveResult::Refused;
    }
    if (refuse_if_drive_fault(a, e)) return MoveResult::Refused;
    // Unanchored axis: motion only while the network is bypassed (setup —
    // jogging the element onto its home stop). Bounded moves only.
    if (!anchored(a) && !hal::relay::bypass()) {
        refuse(e, "not_anchored", "home not declared on this axis; engage bypass to jog in setup");
        return MoveResult::Refused;
    }
    int hit = 0;
    const int32_t tgt = clamp_target(a, target, hit);
    last_clamp[a] = static_cast<int8_t>(hit);
    const bool moving = hal::motor::busy(a);
    // Already heading exactly there (e.g. another "+N rev" clamped to the
    // bound the axis is running to): leave the move alone.
    if (moving && tgt == hal::motor::target(a)) return hit ? MoveResult::Clamped : MoveResult::Started;
    // "Already there" only means something at rest. While moving, a target
    // equal to the current position is a request to stop there.
    if (!moving && tgt == hal::motor::position(a)) {
        if (hit) {
            refuse(e, "at_limit", hit > 0 ? "already at max limit" : "already at home limit");
            return MoveResult::AtLimit;
        }
        return MoveResult::Noop;
    }
    mark_dirty(a);
    hal::motor::move_to(a, tgt);
    return hit ? MoveResult::Clamped : MoveResult::Started;
}

// Motion not finished: an axis running, or a finished move whose arrival the
// drive has not confirmed yet (PED supervision) — its position is not
// recorded, so nothing may change the rules that will record it.
bool any_busy() {
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) if (hal::motor::busy(a) || awaiting_arrival[a]) return true;
    return false;
}

// move_l / move_c (v1 master protocol) resolve the element name through
// the Balanced L topology and land on move_axis().
bool move_named(const char *name, int32_t value, bool is_delta, Refusal &e) {
    if (cfg.topology.kind != TopologyKind::BalancedL) {
        return refuse(e, "wrong_topology", "move_l / move_c apply to the Balanced L network only; use move_axis");
    }
    const int i = cfg.topology.element_by_name(name);
    if (i < 0) return refuse(e, "bad_axis", "element not bound");
    return accepted(move_axis(cfg.topology.elements[i].axis, value, is_delta, e));
}

} // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────

void init() {
    // Invariant 2 first: K3 out of circuit before anything else can act.
    hal::relay::init();
    hal::safety::init();
    hal::limits::init();
    hal::feedback::init();
    hal::motor::init();
    hal::encoder::init();

    settings::init(cfg);   // EEPROM + SD card policy — app/settings.h
    last_move_ms   = 0;
    alarm_since_ms = 0;
    alarm_handled  = false;
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        estop_latched[a]    = false;
        awaiting_arrival[a] = false;
        train_end_ms[a]     = 0;
        ped_off_since[a]    = 0;
        drive_ready[a]      = false;
        drive_fault[a]      = DriveFault::None;
        AxisConfig &c = cfg.axis[a];
        hal::motor::set_speed(a, c.speed);
        hal::motor::set_accel(a, c.accel);
        // Restore the position record. If a move was in flight when the
        // record was last written it is not a clean-shutdown anchor:
        // keep the number as the best estimate but drop the anchor so
        // the operator has to re-declare home (invariant 3).
        if (cfg.dirty[a] && c.home_set) {
            c.home_set = false;
            settings::save_axis(a, c);
        }
        hal::motor::set_position(a, cfg.position[a]);
        hal::encoder::set_count(a, cfg.position[a]);
        if (cfg.dirty[a]) {
            cfg.dirty[a] = false;
            settings::save_position(a, cfg.position[a], false);
        }
        dirty_written[a] = false;
        prev_busy[a]     = false;
        last_clamp[a]    = 0;
    }
}

void tick(uint32_t now_ms, Snapshot &out) {
    hal::motor::tick();
    settings::tick(now_ms);
    supervise_alarm(now_ms);   // may cut pulses on every axis before they are read below

    bool any_moving = false;
    bool all_estop  = true;
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        const bool busy = hal::motor::busy(a);
        if (prev_busy[a] && !busy) {
            last_move_ms = now_ms;
            if (ped_supervised(a)) {
                // Anchored only once the drive confirms the shaft got there.
                awaiting_arrival[a] = true;
                train_end_ms[a]     = now_ms;
            } else {
                mark_clean(a);          // move complete → clean anchor
            }
        }
        if (busy) awaiting_arrival[a] = false;   // superseded by a new pulse train
        supervise_ped(a, busy, now_ms);
        prev_busy[a] = busy;
        any_moving  |= busy;

        const AxisConfig &c = cfg.axis[a];
        AxisSnapshot &s = out.axes[a];
        s.steps      = hal::motor::position(a);
        s.enc        = hal::encoder::count(a);
        s.moving     = busy;
        s.enabled    = hal::motor::enabled(a);
        s.limit_sw   = hal::limits::active(a);
        s.element    = static_cast<int8_t>(cfg.topology.element_for_axis(a));
        s.kind       = c.kind;
        s.home_set   = c.home_set;
        s.anchored   = anchored(a);
        s.max_rev    = c.max_rev;
        s.max_steps  = c.max_steps();
        s.travel     = travel_of(a);
        s.last_clamp = last_clamp[a];
        s.estop      = estop_latched[a];
        s.ped        = hal::feedback::arrived(a);
        s.drive_fault = drive_fault[a];
        all_estop   &= estop_latched[a];
        s.speed      = hal::motor::speed(a);
        s.accel      = hal::motor::accel(a);
    }

    out.topology = cfg.topology;
    out.side     = from_hal(hal::relay::side());
    out.bypass   = hal::relay::bypass();
    out.moving   = any_moving;
    bool homed = cfg.topology.n > 0;
    for (uint8_t i = 0; i < cfg.topology.n; i++) homed &= anchored(cfg.topology.elements[i].axis);
    out.homed        = homed;
    out.rf_lockout   = hal::safety::rf_present();
    out.estop_all    = all_estop;
    out.sd_present   = settings::status().sd_present;
    out.sd_ok        = settings::status().sd_ok;
    out.settings_source = settings::status().source;
    out.last_move_ms = last_move_ms;
    out.fwd_w        = hal::safety::fwd_w();
    out.feedback_ped = cfg.feedback.ped;
    out.feedback_alm = cfg.feedback.alm;
    out.drive_alarm  = hal::feedback::alarm();
}

// ── Axis verbs ──────────────────────────────────────────────────────────

MoveResult move_axis(uint8_t a, int32_t value, bool is_delta, Refusal &e) {
    if (refuse_if_bad_axis(a, e) || refuse_if_rf(e)) return MoveResult::Refused;
    if (!is_delta) return begin_move(a, value, e);
    // A relative move is taken from where the axis is already heading, so a
    // second "+N rev" issued before the first finishes adds to it instead of
    // replacing the rest of the first move with N revs from mid-travel.
    const int64_t base   = hal::motor::busy(a) ? hal::motor::target(a) : hal::motor::position(a);
    int64_t       target = base + value;
    if (target > INT32_MAX) target = INT32_MAX;
    if (target < INT32_MIN) target = INT32_MIN;
    return begin_move(a, static_cast<int32_t>(target), e);
}

MoveResult run_to_end(uint8_t a, int dir, Refusal &e) {
    if (refuse_if_bad_axis(a, e) || refuse_if_rf(e)) return MoveResult::Refused;
    if (dir == 0) { refuse(e, "bad_args", "dir must be +1 or -1"); return MoveResult::Refused; }
    const AxisConfig &c = cfg.axis[a];
    if (c.kind == ElementKind::Unset) {
        refuse(e, "kind_unset", "declare the element kind before an unbounded run");
        return MoveResult::Refused;
    }
    if (kind_has_stops(c.kind) && !c.limits_active()) {
        refuse(e, "not_anchored", "declare home (and rated travel) before running an element with stops to its end");
        return MoveResult::Refused;
    }
    return begin_move(a, dir > 0 ? kFarAway : -kFarAway, e);
}

bool stop_axis(uint8_t a, Refusal &e) {
    if (refuse_if_bad_axis(a, e)) return false;
    hal::motor::stop(a);
    return true;
}

void stop_all() {
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) hal::motor::stop(a);
}

bool estop(uint8_t a, Refusal &e) {
    if (refuse_if_bad_axis(a, e)) return false;
    hal::motor::stop(a);
    estop_latched[a] = true;
    return true;
}

void estop_all() {
    stop_all();
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) estop_latched[a] = true;
}

bool estop_reset(uint8_t a, Refusal &e) {
    if (refuse_if_bad_axis(a, e)) return false;
    estop_latched[a] = false;
    return true;
}

void estop_reset_all() {
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) estop_latched[a] = false;
}

bool set_home(uint8_t a, Refusal &e) {
    if (refuse_if_bad_axis(a, e)) return false;
    hal::motor::stop(a);
    hal::motor::set_position(a, 0);
    hal::encoder::set_count(a, 0);
    AxisConfig &c = cfg.axis[a];
    c.home_set    = true;
    last_clamp[a] = 0;
    prev_busy[a]  = false;
    awaiting_arrival[a] = false;
    drive_fault[a]      = DriveFault::None;   // the operator has re-anchored the axis
    settings::save_axis(a, c);
    mark_clean(a);
    return true;
}

bool unset_home(uint8_t a, Refusal &e) {
    if (refuse_if_bad_axis(a, e)) return false;
    cfg.axis[a].home_set = false;
    last_clamp[a] = 0;
    settings::save_axis(a, cfg.axis[a]);
    return true;
}

bool set_element(uint8_t a, ElementKind kind, float max_rev, Refusal &e) {
    if (refuse_if_bad_axis(a, e)) return false;
    if (static_cast<uint8_t>(kind) >= kElementKindCount) return refuse(e, "bad_args", "unknown element kind");
    if (kind_has_stops(kind) && !(max_rev > 0.0f && max_rev < kMaxRatedRev)) {
        return refuse(e, "bad_args", "kinds with stops need max_rev > 0 (revolutions)");
    }
    hal::motor::stop(a);
    AxisConfig &c = cfg.axis[a];
    if (kind != c.kind) c.home_set = false;   // new device on the shaft
    c.kind    = kind;
    c.max_rev = kind_has_stops(kind) ? max_rev : 0.0f;
    last_clamp[a] = 0;
    settings::save_axis(a, c);
    return true;
}

bool set_speed(uint8_t a, uint32_t speed, uint32_t accel, Refusal &e) {
    if (refuse_if_bad_axis(a, e)) return false;
    if (speed < 1 || speed > kMaxSpeedHz)  return refuse(e, "bad_args", "speed out of range (1..200000 steps/s)");
    if (accel < 1 || accel > kMaxAccelHz2) return refuse(e, "bad_args", "accel out of range");
    hal::motor::set_speed(a, speed);
    hal::motor::set_accel(a, accel);
    cfg.axis[a].speed = speed;
    cfg.axis[a].accel = accel;
    settings::save_axis(a, cfg.axis[a]);
    return true;
}

bool set_enabled(uint8_t a, bool on, Refusal &e) {
    if (refuse_if_bad_axis(a, e)) return false;
    if (!on) hal::motor::stop(a);
    hal::motor::set_enabled(a, on);
    return true;
}

bool set_topology(Topology t, Refusal &e) {
    const char *code = validate_topology(t);
    if (code) return refuse(e, code, "topology rejected");
    if (!hal::relay::bypass()) return refuse(e, "not_bypassed", "engage bypass before changing the topology");
    if (any_busy())            return refuse(e, "moving", "stop all axes before changing the topology");
    cfg.topology = t;
    settings::save_topology(t);
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) { drive_ready[a] = false; ped_off_since[a] = 0; }
    return true;
}

bool set_feedback(FeedbackConfig f, Refusal &e) {
    if (any_busy()) return refuse(e, "moving", "stop all axes (and let the drives confirm arrival) before changing drive feedback");
    cfg.feedback = f;
    // Re-learn readiness from the live PED level; a finished move still
    // waiting for arrival is resolved by the next tick either way.
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) { drive_ready[a] = false; ped_off_since[a] = 0; }
    alarm_since_ms = 0;
    alarm_handled  = false;
    settings::save_feedback(f);
    return true;
}

// ── Network verbs ───────────────────────────────────────────────────────

bool set_side(Side s, Refusal &e) {
    if (refuse_if_rf(e)) return false;
    if (refuse_if_updating(e)) return false;
    if (cfg.topology.kind != TopologyKind::BalancedL) {
        return refuse(e, "wrong_topology", "set_side applies to the Balanced L network only");
    }
    hal::relay::set_side(to_hal(s));
    return true;
}

bool set_bypass(bool on, Refusal &e) {
    // Invariant: bypass is the only relay verb safe under RF. No lockout check.
    // Engaging it is always allowed; taking the network out of bypass is
    // refused while a firmware apply is pending (it must reboot in bypass).
    if (!on && refuse_if_updating(e)) return false;
    hal::relay::set_bypass(on);
    return true;
}

bool home(Refusal &e) {
    if (refuse_if_rf(e)) return false;
    if (refuse_if_updating(e)) return false;
    for (uint8_t i = 0; i < cfg.topology.n; i++) {
        if (!anchored(cfg.topology.elements[i].axis)) {
            return refuse(e, "not_anchored", "declare home on every element before homing");
        }
    }
    // Check every drive before starting any of the moves.
    for (uint8_t i = 0; i < cfg.topology.n; i++) {
        if (refuse_if_drive_fault(cfg.topology.elements[i].axis, e)) return false;
    }
    for (uint8_t i = 0; i < cfg.topology.n; i++) {
        Refusal ignored = {nullptr, nullptr};
        const MoveResult r = begin_move(cfg.topology.elements[i].axis, 0, ignored);
        if (r == MoveResult::Refused) { e = ignored; return false; }
    }
    return true;
}

bool set_fwd_w_fake(float w, Refusal &e) {
    (void)e;
    hal::safety::inject_fwd_w(w);
    return true;
}

// ── Legacy aliases ──────────────────────────────────────────────────────

bool move_l(int32_t value, bool is_delta, Refusal &e) { return move_named("L", value, is_delta, e); }
bool move_c(int32_t value, bool is_delta, Refusal &e) { return move_named("C", value, is_delta, e); }

// ── Lookups ─────────────────────────────────────────────────────────────

const Topology   &topology()              { return cfg.topology; }
const AxisConfig &axis_config(uint8_t a)  { return cfg.axis[a < hal::kMaxAxes ? a : 0]; }
const FeedbackConfig &feedback()          { return cfg.feedback; }
bool any_motion_pending()                 { return any_busy(); }

int resolve_axis(const char *s) {
    if (!s || !*s) return -1;
    if (s[0] >= '0' && s[0] <= '9' && s[1] == '\0') {
        const int a = s[0] - '0';
        return a < hal::kMaxAxes ? a : -1;
    }
    const int i = cfg.topology.element_by_name(s);
    return i >= 0 ? cfg.topology.elements[i].axis : -1;
}

} // namespace app::motion
