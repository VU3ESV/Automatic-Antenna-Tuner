// Host-side unit tests for drive feedback supervision (iHSS60 PED / ALM) in
// app::motion against the sim HAL: off by default and persisted; with PED
// on a move is anchored only after arrival, a stall or a motor-power cut
// clears home, a drive lost at rest clears home, and a boot with motor
// power off keeps home but refuses motion; with ALM on an alarm stops every
// axis and clears home everywhere, while a glitch only stops.

#include <unity.h>

#include <cstring>

#include "app/config.h"
#include "app/motion.h"
#include "app/settings.h"
#include "app/state.h"
#include "hal/hal.h"

using app::DriveFault;
using app::motion::MoveResult;

// Sim-only hooks (hal/sdcard_sim.cpp, hal/feedback_sim.cpp).
namespace hal::sdcard {
void sim_set_present(bool inserted);
void sim_format();
}
namespace hal::feedback {
void sim_set_powered(hal::Axis a, bool on);
void sim_set_stalled(hal::Axis a, bool on);
void sim_set_alarm(bool on);
void sim_reset();
}

namespace {

app::motion::Refusal err;
app::Snapshot s;
uint32_t now = 0;

// Advance the controller `ms` loop iterations, one millisecond each.
void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) app::motion::tick(++now, s);
}

// Tick until nothing moves.
void settle() {
    for (int i = 0; i < 20000; ++i) {
        app::motion::tick(++now, s);
        if (!s.moving) return;
    }
}

// A controller (re)boot over whatever the stores hold.
void boot() {
    app::motion::init();
    run(1);
}

app::FeedbackConfig fb(bool ped, bool alm) {
    app::FeedbackConfig f;
    f.ped = ped;
    f.alm = alm;
    return f;
}

// Balanced Pi: C1 (vacuum cap, 40 rev), L (inductor, 22 rev) and C2 (free
// rotation) declared and anchored, feedback set as given.
void anchored_pi(app::FeedbackConfig f) {
    TEST_ASSERT_TRUE(app::motion::set_topology(app::Topology::default_balanced_pi(), err));
    TEST_ASSERT_TRUE(app::motion::set_element(0, app::ElementKind::VacuumCap, 40.0f, err));
    TEST_ASSERT_TRUE(app::motion::set_element(1, app::ElementKind::Inductor, 22.0f, err));
    TEST_ASSERT_TRUE(app::motion::set_element(2, app::ElementKind::VarCapFree, 0.0f, err));
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) TEST_ASSERT_TRUE(app::motion::set_home(a, err));
    TEST_ASSERT_TRUE(app::motion::set_feedback(f, err));
    run(1);
    TEST_ASSERT_TRUE(s.homed);
}

} // namespace

void setUp() {
    err = {nullptr, nullptr};
    hal::nvs::init();
    hal::sdcard::sim_set_present(true);
    hal::sdcard::sim_format();
    hal::feedback::sim_reset();
    app::Persisted p;
    p.topology = app::Topology::default_balanced_l();
    app::nvs_format(p);
    boot();
}
void tearDown() { hal::sdcard::sim_set_present(true); }

// ── Off by default, persisted ───────────────────────────────────────────

void test_off_by_default_levels_reported_but_ignored() {
    TEST_ASSERT_FALSE(s.feedback_ped);
    TEST_ASSERT_FALSE(s.feedback_alm);
    anchored_pi(fb(false, false));
    // Nothing wired yet: a dead PED and a stuck ALM change nothing.
    hal::feedback::sim_set_powered(0, false);
    hal::feedback::sim_set_alarm(true);
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 6400, true, err)));
    settle();
    run(3000);
    TEST_ASSERT_EQUAL_INT32(6400, s.axes[0].steps);
    TEST_ASSERT_TRUE(s.axes[0].anchored);
    TEST_ASSERT_TRUE(s.axes[0].drive_fault == DriveFault::None);
    // Live levels are still reported, to check the wiring before enabling.
    TEST_ASSERT_FALSE(s.axes[0].ped);
    TEST_ASSERT_TRUE(s.axes[1].ped);
    TEST_ASSERT_TRUE(s.drive_alarm);
}

void test_setting_persists_in_eeprom_and_card() {
    TEST_ASSERT_TRUE(app::motion::set_feedback(fb(true, false), err));
    run(1000);                                    // debounced card write
    static char card[2048];
    const int n = app::settings::read_card_copy(card, sizeof(card));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(card, "\"feedback\""));
    TEST_ASSERT_NOT_NULL(strstr(card, "\"ped\": true"));
    boot();                                       // power cycle, card present
    TEST_ASSERT_TRUE(s.feedback_ped);
    TEST_ASSERT_FALSE(s.feedback_alm);
    hal::sdcard::sim_set_present(false);
    boot();                                       // no card: the EEPROM copy
    TEST_ASSERT_TRUE(s.feedback_ped);
    hal::sdcard::sim_set_present(true);
    TEST_ASSERT_TRUE(app::motion::set_feedback(fb(false, true), err));
    boot();                                       // before the card write: newer EEPROM wins
    TEST_ASSERT_FALSE(s.feedback_ped);
    TEST_ASSERT_TRUE(s.feedback_alm);
}

void test_set_feedback_refused_while_moving() {
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 32000, true, err)));
    run(10);
    TEST_ASSERT_FALSE(app::motion::set_feedback(fb(true, true), err));
    TEST_ASSERT_EQUAL_STRING("moving", err.code);
    settle();
    TEST_ASSERT_TRUE(app::motion::set_feedback(fb(true, true), err));
}

// ── PED ─────────────────────────────────────────────────────────────────

void test_ped_arrival_saves_the_anchor() {
    anchored_pi(fb(true, false));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 12800, true, err)));
    settle();
    TEST_ASSERT_TRUE(s.axes[0].anchored);
    TEST_ASSERT_FALSE(app::settings::current().dirty[0]);
    TEST_ASSERT_EQUAL_INT32(12800, app::settings::current().position[0]);
    TEST_ASSERT_TRUE(s.axes[0].drive_fault == DriveFault::None);
}

void test_stall_waits_for_arrival_then_clears_home() {
    anchored_pi(fb(true, false));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(1, 6400, true, err)));
    run(10);
    hal::feedback::sim_set_stalled(1, true);      // falls behind mid-move and never catches up
    settle();
    TEST_ASSERT_EQUAL_INT32(6400, s.axes[1].steps);
    // Not recorded while the drive has not confirmed arrival.
    TEST_ASSERT_TRUE(app::settings::current().dirty[1]);
    run(1500);
    TEST_ASSERT_TRUE(s.axes[1].anchored);
    TEST_ASSERT_TRUE(app::settings::current().dirty[1]);
    run(600);                                     // > 2 s after the pulses ended
    TEST_ASSERT_FALSE(s.axes[1].anchored);
    TEST_ASSERT_FALSE(s.homed);
    TEST_ASSERT_TRUE(s.axes[1].drive_fault == DriveFault::NoArrival);
    TEST_ASSERT_FALSE(app::motion::axis_config(1).home_set);
    TEST_ASSERT_TRUE(s.axes[0].anchored);         // other elements untouched
    // PED still off at rest: motion refused.
    TEST_ASSERT_TRUE(app::motion::move_axis(1, 10, true, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("drive_not_ready", err.code);
    // Drive recovers: setup moves work again (bypass, unanchored); the
    // fault stays until home is declared.
    hal::feedback::sim_set_stalled(1, false);
    run(1);
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(1, -10, true, err)));
    settle();
    TEST_ASSERT_TRUE(s.axes[1].drive_fault == DriveFault::NoArrival);
    TEST_ASSERT_TRUE(app::motion::set_home(1, err));
    run(1);
    TEST_ASSERT_TRUE(s.axes[1].drive_fault == DriveFault::None);
    TEST_ASSERT_TRUE(s.homed);
}

void test_motor_power_cut_mid_move_clears_home() {
    // The bench incident: 12 V removed during a run while the Teensy stays up.
    anchored_pi(fb(true, false));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 32000, false, err)));
    run(100);
    hal::feedback::sim_set_powered(0, false);
    settle();
    TEST_ASSERT_EQUAL_INT32(32000, s.axes[0].steps);   // the pulse counter believes it arrived
    run(2100);
    TEST_ASSERT_FALSE(s.axes[0].anchored);
    TEST_ASSERT_TRUE(s.axes[0].drive_fault == DriveFault::NoArrival);
    TEST_ASSERT_FALSE(s.homed);
    boot();                                       // and it stays cleared across a reboot
    TEST_ASSERT_FALSE(s.axes[0].home_set);
}

void test_drive_lost_at_rest_clears_home() {
    anchored_pi(fb(true, false));
    hal::feedback::sim_set_powered(2, false);
    run(100);
    TEST_ASSERT_TRUE(s.axes[2].anchored);         // debounced
    TEST_ASSERT_TRUE(app::motion::move_axis(2, 10, true, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("drive_not_ready", err.code);
    run(200);
    TEST_ASSERT_FALSE(s.axes[2].anchored);
    TEST_ASSERT_TRUE(s.axes[2].drive_fault == DriveFault::DriveLost);
    TEST_ASSERT_TRUE(s.axes[0].anchored);
    TEST_ASSERT_TRUE(s.axes[1].anchored);
}

void test_boot_with_motor_power_off_keeps_home_but_refuses_motion() {
    anchored_pi(fb(true, false));
    hal::feedback::sim_set_powered(0, false);
    boot();                                       // e.g. the Teensy restarts on its own 5 V
    run(1000);
    TEST_ASSERT_TRUE(s.axes[0].anchored);
    TEST_ASSERT_TRUE(s.axes[0].drive_fault == DriveFault::None);
    TEST_ASSERT_TRUE(app::motion::move_axis(0, 100, true, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("drive_not_ready", err.code);
    TEST_ASSERT_FALSE(app::motion::home(err));
    TEST_ASSERT_EQUAL_STRING("drive_not_ready", err.code);
    hal::feedback::sim_set_powered(0, true);
    run(1);
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 100, true, err)));
    settle();
    TEST_ASSERT_TRUE(s.axes[0].anchored);
}

void test_unbound_axis_is_not_supervised() {
    // Balanced L leaves motor 2 spare: no PED wired there.
    TEST_ASSERT_TRUE(app::motion::set_feedback(fb(true, false), err));
    hal::feedback::sim_set_powered(2, false);
    run(500);
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(2, 100, true, err)));
    settle();
    run(3000);
    TEST_ASSERT_EQUAL_INT32(100, s.axes[2].steps);
    TEST_ASSERT_TRUE(s.axes[2].drive_fault == DriveFault::None);
}

// ── ALM ─────────────────────────────────────────────────────────────────

void test_alarm_stops_every_axis_and_clears_home() {
    anchored_pi(fb(false, true));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 32000, false, err)));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(1, 32000, false, err)));
    run(50);
    TEST_ASSERT_TRUE(s.moving);
    hal::feedback::sim_set_alarm(true);
    run(1);
    TEST_ASSERT_FALSE(s.moving);                  // pulses cut on the first sample
    const int32_t stopped_at = s.axes[0].steps;
    run(30);
    TEST_ASSERT_EQUAL_INT32(stopped_at, s.axes[0].steps);
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        TEST_ASSERT_FALSE(s.axes[a].anchored);
        TEST_ASSERT_TRUE(s.axes[a].drive_fault == DriveFault::Alarm);
    }
    TEST_ASSERT_TRUE(app::motion::move_axis(2, 10, true, err) == MoveResult::Refused);
    TEST_ASSERT_EQUAL_STRING("drive_alarm", err.code);
    hal::feedback::sim_set_alarm(false);
    run(1);
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(2, 10, true, err)));
}

void test_alarm_glitch_stops_but_keeps_home() {
    anchored_pi(fb(false, true));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 32000, false, err)));
    run(50);
    hal::feedback::sim_set_alarm(true);
    run(1);
    hal::feedback::sim_set_alarm(false);
    run(50);
    TEST_ASSERT_FALSE(s.moving);
    TEST_ASSERT_TRUE(s.axes[0].anchored);
    TEST_ASSERT_TRUE(s.axes[0].drive_fault == DriveFault::None);
}

void test_first_fault_cause_is_kept() {
    // Alarm stops the move and the faulted drive never arrives: the report
    // stays "alarm", not the arrival timeout that follows it.
    anchored_pi(fb(true, true));
    TEST_ASSERT_TRUE(app::motion::accepted(app::motion::move_axis(0, 32000, false, err)));
    run(50);
    hal::feedback::sim_set_alarm(true);
    hal::feedback::sim_set_stalled(0, true);
    run(2500);
    TEST_ASSERT_FALSE(s.axes[0].anchored);
    TEST_ASSERT_TRUE(s.axes[0].drive_fault == DriveFault::Alarm);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_off_by_default_levels_reported_but_ignored);
    RUN_TEST(test_setting_persists_in_eeprom_and_card);
    RUN_TEST(test_set_feedback_refused_while_moving);
    RUN_TEST(test_ped_arrival_saves_the_anchor);
    RUN_TEST(test_stall_waits_for_arrival_then_clears_home);
    RUN_TEST(test_motor_power_cut_mid_move_clears_home);
    RUN_TEST(test_drive_lost_at_rest_clears_home);
    RUN_TEST(test_boot_with_motor_power_off_keeps_home_but_refuses_motion);
    RUN_TEST(test_unbound_axis_is_not_supervised);
    RUN_TEST(test_alarm_stops_every_axis_and_clears_home);
    RUN_TEST(test_alarm_glitch_stops_but_keeps_home);
    RUN_TEST(test_first_fault_cause_is_kept);
    return UNITY_END();
}
