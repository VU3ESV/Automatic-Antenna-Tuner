// Host-side unit tests for app::motion against the sim HAL. Runs via
// `pio test -e native`. Proves the verb-accept matrix, the RF lockout,
// the software travel window, the anchoring rule (invariant 3) and the
// topology verbs behave regardless of MCU / motor driver.

#include <unity.h>

#include "app/config.h"
#include "app/motion.h"
#include "app/state.h"
#include "hal/hal.h"

using app::motion::MoveResult;

// Sim-only hook (hal/sdcard_sim.cpp): wipe the simulated card between tests.
namespace hal::sdcard { void sim_format(); }

namespace {

app::motion::Refusal err;
app::Snapshot s;

// Tick the sim until nothing moves (or a generous bound).
void settle() {
    for (int i = 0; i < 20000; ++i) {
        app::motion::tick(static_cast<uint32_t>(i + 1), s);
        if (!s.moving) return;
    }
}

// A fresh controller: blank NVS record with the defaults, empty card, boot.
void fresh_boot() {
    hal::nvs::init();
    hal::sdcard::sim_format();
    app::Persisted p;
    p.topology = app::Topology::default_balanced_l();
    app::nvs_format(p);
    app::motion::init();
    app::motion::tick(0, s);
}

} // namespace

void setUp()    { err = {nullptr, nullptr}; fresh_boot(); }
void tearDown() {}

// ── Defaults ────────────────────────────────────────────────────────────

void test_init_state() {
    TEST_ASSERT_TRUE(s.bypass);
    TEST_ASSERT_TRUE(s.side == app::Side::HiZ);
    TEST_ASSERT_FALSE(s.moving);
    TEST_ASSERT_FALSE(s.homed);
    TEST_ASSERT_TRUE(s.topology.kind == app::TopologyKind::BalancedL);
    TEST_ASSERT_EQUAL_UINT8(2, s.topology.n);
    TEST_ASSERT_EQUAL_INT(0, app::motion::resolve_axis("L"));
    TEST_ASSERT_EQUAL_INT(1, app::motion::resolve_axis("C"));
    TEST_ASSERT_EQUAL_INT(2, app::motion::resolve_axis("2"));
    TEST_ASSERT_EQUAL_INT(-1, app::motion::resolve_axis("C2"));
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        TEST_ASSERT_EQUAL_INT32(0, s.axes[a].steps);
        TEST_ASSERT_FALSE(s.axes[a].anchored);
        TEST_ASSERT_TRUE(s.axes[a].kind == app::ElementKind::Unset);
    }
    TEST_ASSERT_EQUAL_INT8(0, s.axes[0].element);
    TEST_ASSERT_EQUAL_INT8(1, s.axes[1].element);
    TEST_ASSERT_EQUAL_INT8(-1, s.axes[2].element);
}

// ── Basic motion + anchoring rule ───────────────────────────────────────

void test_setup_move_allowed_in_bypass_when_unanchored() {
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 1000, true, err)));
    settle();
    TEST_ASSERT_EQUAL_INT32(1000, s.axes[0].steps);
    TEST_ASSERT_EQUAL_INT32(1000, s.axes[0].enc);
    TEST_ASSERT_FALSE(s.moving);
}

void test_unanchored_move_refused_when_engaged() {
    TEST_ASSERT_TRUE(app::motion::set_bypass(false, err));
    TEST_ASSERT_TRUE(app::motion::move_axis(0, 100, true, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("not_anchored", err.code);
    // Declaring home anchors the axis; the same move is then accepted.
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 100, true, err)));
}

void test_move_to_absolute_target() {
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(1, 2000, false, err)));
    settle();
    TEST_ASSERT_EQUAL_INT32(2000, s.axes[1].steps);
}

void test_bad_axis_refused() {
    TEST_ASSERT_TRUE(app::motion::move_axis(7, 10, true, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("bad_axis", err.code);
    TEST_ASSERT_FALSE(app::motion::set_home(3, err));
}

// ── Travel window ───────────────────────────────────────────────────────

void test_window_clamps_and_latches_bound() {
    TEST_ASSERT_TRUE(app::motion::set_element(1, app::ElementKind::VacuumCap, 1.0f, err));
    TEST_ASSERT_TRUE(app::motion::set_home(1, err));
    app::motion::tick(1, s);
    TEST_ASSERT_TRUE(s.axes[1].travel == app::Travel::Home);
    TEST_ASSERT_EQUAL_INT32(app::kStepsPerRev, s.axes[1].max_steps);

    TEST_ASSERT_TRUE(app::motion::move_axis(1, 10000, true, err) == MoveResult::Clamped);
    settle();
    TEST_ASSERT_EQUAL_INT32(app::kStepsPerRev, s.axes[1].steps);
    TEST_ASSERT_TRUE(s.axes[1].travel == app::Travel::Max);
    TEST_ASSERT_EQUAL_INT8(+1, s.axes[1].last_clamp);

    // On the bound heading outward → refused; inward → fine.
    TEST_ASSERT_TRUE(app::motion::move_axis(1, +1, true, err) == MoveResult::AtLimit);
    TEST_ASSERT_EQUAL_STRING("at_limit", err.code);
    TEST_ASSERT_TRUE(app::motion::move_axis(1, -1, true, err) == MoveResult::Started);
    settle();
    TEST_ASSERT_EQUAL_INT32(app::kStepsPerRev - 1, s.axes[1].steps);

    // Absolute target below home is clamped to 0.
    TEST_ASSERT_TRUE(app::motion::move_axis(1, -500, false, err) == MoveResult::Clamped);
    settle();
    TEST_ASSERT_EQUAL_INT32(0, s.axes[1].steps);
    TEST_ASSERT_EQUAL_INT8(-1, s.axes[1].last_clamp);
}

void test_window_inactive_until_home_declared() {
    TEST_ASSERT_TRUE(app::motion::set_element(0, app::ElementKind::Inductor, 2.0f, err));
    app::motion::tick(1, s);
    TEST_ASSERT_TRUE(s.axes[0].travel == app::Travel::Unhomed);
    // No clamp while unhomed (setup, in bypass).
    TEST_ASSERT_TRUE(app::motion::move_axis(0, -300, true, err) == MoveResult::Started);
    settle();
    TEST_ASSERT_EQUAL_INT32(-300, s.axes[0].steps);
    // Unset home after declaring it deactivates the window again.
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    TEST_ASSERT_TRUE(app::motion::move_axis(0, -1, true, err) == MoveResult::AtLimit);
    TEST_ASSERT_TRUE(app::motion::unset_home(0, err));
    TEST_ASSERT_TRUE(app::motion::move_axis(0, -1, true, err) == MoveResult::Started);
}

void test_window_recovery_from_outside() {
    TEST_ASSERT_TRUE(app::motion::set_element(2, app::ElementKind::VarCapLimited, 0.5f, err));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(2, 5000, false, err)));   // beyond 3200
    settle();
    TEST_ASSERT_TRUE(app::motion::set_home(2, err));                 // home at 0 … we are at 0 now
    // Force an out-of-window position by tightening the window after a move.
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(2, 3000, false, err)));
    settle();
    TEST_ASSERT_TRUE(app::motion::set_element(2, app::ElementKind::VarCapLimited, 0.25f, err));  // max 1600, keeps home? no: same kind keeps home
    app::motion::tick(1, s);
    TEST_ASSERT_TRUE(s.axes[2].home_set);
    TEST_ASSERT_TRUE(s.axes[2].travel == app::Travel::AboveMax);
    TEST_ASSERT_TRUE(app::motion::move_axis(2, +10, true, err) == MoveResult::AtLimit);     // further out: no
    TEST_ASSERT_TRUE(app::motion::move_axis(2, -10, true, err) == MoveResult::Started);     // back in: yes
}

void test_set_element_validation_and_kind_change_clears_home() {
    TEST_ASSERT_FALSE(app::motion::set_element(0, app::ElementKind::VacuumCap, 0.0f, err));
    TEST_ASSERT_EQUAL_STRING("bad_args", err.code);
    TEST_ASSERT_TRUE(app::motion::set_element(0, app::ElementKind::VacuumCap, 40.0f, err));
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    app::motion::tick(1, s);
    TEST_ASSERT_TRUE(s.axes[0].anchored);
    TEST_ASSERT_TRUE(app::motion::set_element(0, app::ElementKind::Inductor, 30.0f, err));
    app::motion::tick(2, s);
    TEST_ASSERT_FALSE(s.axes[0].anchored);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, s.axes[0].max_rev);
}

// ── Run to end ──────────────────────────────────────────────────────────

void test_run_to_end_rules() {
    TEST_ASSERT_TRUE(app::motion::run_to_end(0, +1, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("kind_unset", err.code);
    TEST_ASSERT_TRUE(app::motion::set_element(0, app::ElementKind::VacuumCap, 2.0f, err));
    TEST_ASSERT_TRUE(app::motion::run_to_end(0, +1, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("not_anchored", err.code);
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    TEST_ASSERT_TRUE(app::motion::run_to_end(0, +1, err) == MoveResult::Clamped);
    settle();
    TEST_ASSERT_EQUAL_INT32(2 * app::kStepsPerRev, s.axes[0].steps);
    // Free-rotating element: unbounded run, stop_axis halts it.
    TEST_ASSERT_TRUE(app::motion::set_element(2, app::ElementKind::Variometer, 0.0f, err));
    TEST_ASSERT_TRUE(app::motion::set_home(2, err));
    TEST_ASSERT_TRUE(app::motion::run_to_end(2, -1, err) == MoveResult::Started);
    for (int i = 0; i < 10; ++i) app::motion::tick(i, s);
    TEST_ASSERT_TRUE(s.axes[2].moving);
    TEST_ASSERT_TRUE(app::motion::stop_axis(2, err));
    app::motion::tick(100, s);
    TEST_ASSERT_FALSE(s.axes[2].moving);
    TEST_ASSERT_TRUE(s.axes[2].steps < 0);
}

// ── RF lockout ──────────────────────────────────────────────────────────

void test_rf_lockout_refuses_motion() {
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    TEST_ASSERT_TRUE(app::motion::set_fwd_w_fake(50.0f, err));
    TEST_ASSERT_TRUE(app::motion::move_axis(0, 100, true, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("rf_lockout", err.code);
    TEST_ASSERT_FALSE(app::motion::move_l(100, true, err));
    TEST_ASSERT_FALSE(app::motion::set_side(app::Side::LoZ, err));
    TEST_ASSERT_FALSE(app::motion::home(err));
    TEST_ASSERT_TRUE(app::motion::run_to_end(0, 1, err) == MoveResult::Refused);
    app::motion::tick(1, s);
    TEST_ASSERT_TRUE(s.rf_lockout);
    // Invariant: bypass is the only relay verb safe while RF is keyed.
    TEST_ASSERT_TRUE(app::motion::set_bypass(false, err));
    TEST_ASSERT_TRUE(app::motion::set_bypass(true, err));
    // Stop is always allowed.
    TEST_ASSERT_TRUE(app::motion::stop_axis(0, err));
}

// ── Relays ──────────────────────────────────────────────────────────────

void test_set_side_and_bypass() {
    TEST_ASSERT_TRUE(app::motion::set_side(app::Side::LoZ, err));
    app::motion::tick(0, s);
    TEST_ASSERT_TRUE(s.side == app::Side::LoZ);
    TEST_ASSERT_TRUE(app::motion::set_bypass(false, err));
    app::motion::tick(0, s);
    TEST_ASSERT_FALSE(s.bypass);
    TEST_ASSERT_TRUE(app::motion::set_bypass(true, err));
    app::motion::tick(0, s);
    TEST_ASSERT_TRUE(s.bypass);
}

// ── Home ────────────────────────────────────────────────────────────────

void test_home_returns_bound_axes_to_zero() {
    TEST_ASSERT_FALSE(app::motion::home(err));
    TEST_ASSERT_EQUAL_STRING("not_anchored", err.code);
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    TEST_ASSERT_TRUE(app::motion::set_home(1, err));
    app::motion::tick(1, s);
    TEST_ASSERT_TRUE(s.homed);                       // both bound axes anchored
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 500, true, err)));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(1, -300, true, err)));
    settle();
    TEST_ASSERT_TRUE(app::motion::home(err));
    settle();
    TEST_ASSERT_EQUAL_INT32(0, s.axes[0].steps);
    TEST_ASSERT_EQUAL_INT32(0, s.axes[1].steps);
    TEST_ASSERT_EQUAL_INT32(0, s.axes[0].enc);
}

// ── Topology ────────────────────────────────────────────────────────────

void test_topology_switch_to_pi() {
    TEST_ASSERT_TRUE(app::motion::set_topology(app::Topology::default_balanced_pi(), err));
    app::motion::tick(1, s);
    TEST_ASSERT_TRUE(s.topology.kind == app::TopologyKind::BalancedPi);
    TEST_ASSERT_EQUAL_UINT8(3, s.topology.n);
    TEST_ASSERT_EQUAL_INT(0, app::motion::resolve_axis("C1"));
    TEST_ASSERT_EQUAL_INT(1, app::motion::resolve_axis("L"));
    TEST_ASSERT_EQUAL_INT(2, app::motion::resolve_axis("C2"));
    TEST_ASSERT_FALSE(app::motion::set_side(app::Side::LoZ, err));
    TEST_ASSERT_EQUAL_STRING("wrong_topology", err.code);
    TEST_ASSERT_FALSE(app::motion::move_l(10, true, err));
    TEST_ASSERT_EQUAL_STRING("wrong_topology", err.code);
    TEST_ASSERT_EQUAL_INT8(2, s.axes[2].element);
}

void test_topology_validation() {
    app::Topology t = app::Topology::default_balanced_l();
    t.elements[1].axis = 0;
    TEST_ASSERT_FALSE(app::motion::set_topology(t, err));
    TEST_ASSERT_EQUAL_STRING("duplicate_axis", err.code);
    t = app::Topology::default_balanced_l();
    t.elements[0].axis = 5;
    TEST_ASSERT_FALSE(app::motion::set_topology(t, err));
    TEST_ASSERT_EQUAL_STRING("bad_axis", err.code);
    t = app::Topology::default_balanced_pi();
    t.kind = app::TopologyKind::BalancedL;          // wrong element set for the kind
    TEST_ASSERT_FALSE(app::motion::set_topology(t, err));
    TEST_ASSERT_EQUAL_STRING("bad_elements", err.code);
    // Requires bypass.
    TEST_ASSERT_TRUE(app::motion::set_bypass(false, err));
    TEST_ASSERT_FALSE(app::motion::set_topology(app::Topology::default_balanced_pi(), err));
    TEST_ASSERT_EQUAL_STRING("not_bypassed", err.code);
    // Operator-chosen map: L on motor 2, C on motor 0.
    TEST_ASSERT_TRUE(app::motion::set_bypass(true, err));
    t = app::Topology::default_balanced_l();
    t.elements[0].axis = 2;
    t.elements[1].axis = 0;
    TEST_ASSERT_TRUE(app::motion::set_topology(t, err));
    TEST_ASSERT_EQUAL_INT(2, app::motion::resolve_axis("L"));
    TEST_ASSERT_TRUE(app::motion::move_l(64, true, err));
    settle();
    TEST_ASSERT_EQUAL_INT32(64, s.axes[2].steps);
}

// ── Persistence (invariant 3) ───────────────────────────────────────────

void test_config_and_position_survive_reboot() {
    TEST_ASSERT_TRUE(app::motion::set_element(2, app::ElementKind::Inductor, 30.0f, err));
    TEST_ASSERT_TRUE(app::motion::set_speed(2, 1600, 8000, err));
    TEST_ASSERT_TRUE(app::motion::set_home(2, err));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(2, 1000, true, err)));
    settle();
    TEST_ASSERT_TRUE(app::motion::set_topology(app::Topology::default_balanced_pi(), err));

    app::motion::init();   // power cycle
    app::motion::tick(1, s);
    TEST_ASSERT_TRUE(s.topology.kind == app::TopologyKind::BalancedPi);
    TEST_ASSERT_TRUE(s.axes[2].kind == app::ElementKind::Inductor);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, s.axes[2].max_rev);
    TEST_ASSERT_EQUAL_UINT32(1600, s.axes[2].speed);
    TEST_ASSERT_EQUAL_UINT32(8000, s.axes[2].accel);
    TEST_ASSERT_TRUE(s.axes[2].home_set);
    TEST_ASSERT_TRUE(s.axes[2].anchored);
    TEST_ASSERT_EQUAL_INT32(1000, s.axes[2].steps);
    TEST_ASSERT_EQUAL_INT32(1000, s.axes[2].enc);
    TEST_ASSERT_TRUE(s.bypass);   // invariant 2 on every boot
}

void test_unclean_shutdown_drops_anchor() {
    TEST_ASSERT_TRUE(app::motion::set_element(0, app::ElementKind::VacuumCap, 10.0f, err));
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 500, true, err)));
    // Power dies mid-move: no settle(), straight to a new boot.
    app::motion::init();
    app::motion::tick(1, s);
    TEST_ASSERT_FALSE(s.axes[0].anchored);
    TEST_ASSERT_FALSE(s.axes[0].home_set);
    TEST_ASSERT_TRUE(s.axes[0].kind == app::ElementKind::VacuumCap);   // kind + travel kept
    TEST_ASSERT_TRUE(s.axes[0].travel == app::Travel::Unhomed);
    // Recovery: declare home again → anchored, and a second boot is clean.
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    app::motion::init();
    app::motion::tick(2, s);
    TEST_ASSERT_TRUE(s.axes[0].anchored);
}

// ── Emergency stop (latched alarm) ──────────────────────────────────────

void test_estop_latches_until_reset() {
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    TEST_ASSERT_TRUE(app::motion::set_home(1, err));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 5000, true, err)));
    for (int i = 0; i < 5; ++i) app::motion::tick(i, s);
    TEST_ASSERT_TRUE(s.axes[0].moving);
    TEST_ASSERT_TRUE(app::motion::estop(0, err));
    app::motion::tick(10, s);
    TEST_ASSERT_FALSE(s.axes[0].moving);              // pulses cut
    TEST_ASSERT_TRUE(s.axes[0].estop);                // alarm latched
    TEST_ASSERT_FALSE(s.axes[1].estop);
    TEST_ASSERT_FALSE(s.estop_all);
    TEST_ASSERT_TRUE(s.axes[0].steps > 0 && s.axes[0].steps < 5000);
    // Latched axis refuses motion; the other axis is unaffected.
    TEST_ASSERT_TRUE(app::motion::move_axis(0, 10, true, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("estop", err.code);
    TEST_ASSERT_TRUE(app::motion::run_to_end(0, 1, err) == MoveResult::Refused);
    TEST_ASSERT_FALSE(app::motion::home(err));        // home touches the latched axis
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(1, 10, true, err)));
    // Config / relay verbs stay available while latched.
    TEST_ASSERT_TRUE(app::motion::set_bypass(false, err));
    TEST_ASSERT_TRUE(app::motion::set_bypass(true, err));
    TEST_ASSERT_TRUE(app::motion::set_element(0, app::ElementKind::Inductor, 5.0f, err));
    // Reset releases it.
    TEST_ASSERT_TRUE(app::motion::estop_reset(0, err));
    app::motion::tick(11, s);
    TEST_ASSERT_FALSE(s.axes[0].estop);
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 10, true, err)));
}

void test_estop_all_and_reset_paths() {
    TEST_ASSERT_TRUE(app::motion::set_home(0, err));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(2, 400, true, err)));
    app::motion::estop_all();
    app::motion::tick(1, s);
    TEST_ASSERT_TRUE(s.estop_all);
    TEST_ASSERT_FALSE(s.moving);
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        TEST_ASSERT_TRUE(s.axes[a].estop);
        TEST_ASSERT_TRUE(app::motion::move_axis(a, 1, true, err) == MoveResult::Refused);
        TEST_ASSERT_EQUAL_STRING("estop", err.code);
    }
    // Releasing one motor works even after E-STOP ALL (toggle semantics):
    // that motor moves again, the others stay latched, estop_all drops.
    TEST_ASSERT_TRUE(app::motion::estop_reset(1, err));
    app::motion::tick(2, s);
    TEST_ASSERT_FALSE(s.estop_all);
    TEST_ASSERT_FALSE(s.axes[1].estop);
    TEST_ASSERT_TRUE(s.axes[0].estop && s.axes[2].estop);
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(1, 1, true, err)));
    TEST_ASSERT_TRUE(app::motion::move_axis(0, 1, true, err) == MoveResult::Refused);
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) TEST_ASSERT_TRUE(app::motion::estop_reset(a, err));
    app::motion::tick(3, s);
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 1, true, err)));
    // Reset-all path, and a power cycle clears latches too.
    app::motion::estop_all();
    app::motion::estop_reset_all();
    app::motion::tick(4, s);
    TEST_ASSERT_FALSE(s.estop_all);
    TEST_ASSERT_FALSE(s.axes[1].estop);
    app::motion::estop_all();
    app::motion::init();
    app::motion::tick(5, s);
    TEST_ASSERT_FALSE(s.estop_all);
    TEST_ASSERT_FALSE(s.axes[0].estop);
}

void test_speed_validation() {
    TEST_ASSERT_FALSE(app::motion::set_speed(0, 0, 100, err));
    TEST_ASSERT_FALSE(app::motion::set_speed(0, 300000, 100, err));
    TEST_ASSERT_TRUE(app::motion::set_speed(0, 25600, 25600, err));
    app::motion::tick(1, s);
    TEST_ASSERT_EQUAL_UINT32(25600, s.axes[0].speed);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_init_state);
    RUN_TEST(test_setup_move_allowed_in_bypass_when_unanchored);
    RUN_TEST(test_unanchored_move_refused_when_engaged);
    RUN_TEST(test_move_to_absolute_target);
    RUN_TEST(test_bad_axis_refused);
    RUN_TEST(test_window_clamps_and_latches_bound);
    RUN_TEST(test_window_inactive_until_home_declared);
    RUN_TEST(test_window_recovery_from_outside);
    RUN_TEST(test_set_element_validation_and_kind_change_clears_home);
    RUN_TEST(test_run_to_end_rules);
    RUN_TEST(test_rf_lockout_refuses_motion);
    RUN_TEST(test_set_side_and_bypass);
    RUN_TEST(test_home_returns_bound_axes_to_zero);
    RUN_TEST(test_topology_switch_to_pi);
    RUN_TEST(test_topology_validation);
    RUN_TEST(test_config_and_position_survive_reboot);
    RUN_TEST(test_unclean_shutdown_drops_anchor);
    RUN_TEST(test_estop_latches_until_reset);
    RUN_TEST(test_estop_all_and_reset_paths);
    RUN_TEST(test_speed_validation);
    return UNITY_END();
}
