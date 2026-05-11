// Host-side unit tests for app::motion against the sim HAL. Runs via
// `pio test -e native`. Proves the motion-and-safety glue layer
// behaves regardless of MCU / motor driver.

#include <unity.h>

#include "app/motion.h"
#include "app/state.h"
#include "hal/hal.h"

void setUp()    { app::motion::init(); }
void tearDown() {}

void test_init_state() {
    app::Snapshot s;
    app::motion::tick(100, s);
    TEST_ASSERT_EQUAL_UINT32(0, s.l_steps);
    TEST_ASSERT_EQUAL_UINT32(0, s.c_steps);
    TEST_ASSERT_FALSE(s.moving);
    TEST_ASSERT_FALSE(s.homed);
    TEST_ASSERT_TRUE(s.bypass);
    TEST_ASSERT_TRUE(s.side == app::Side::HiZ);
}

void test_move_l_delta_advances_position() {
    app::motion::Refusal err = {nullptr, nullptr};
    TEST_ASSERT_TRUE(app::motion::move_l(1000, /*is_delta=*/true, err));
    // Tick the sim until the motor reports !moving.
    app::Snapshot s;
    for (int i = 0; i < 200; ++i) {
        app::motion::tick(i, s);
        if (!s.moving) break;
    }
    TEST_ASSERT_EQUAL_UINT32(1000, s.l_steps);
    TEST_ASSERT_EQUAL_INT32(1000, s.l_enc);
    TEST_ASSERT_FALSE(s.moving);
}

void test_set_side_changes_side() {
    app::motion::Refusal err = {nullptr, nullptr};
    TEST_ASSERT_TRUE(app::motion::set_side(app::Side::LoZ, err));
    app::Snapshot s;
    app::motion::tick(0, s);
    TEST_ASSERT_TRUE(s.side == app::Side::LoZ);
}

void test_set_bypass_off_then_on() {
    app::motion::Refusal err = {nullptr, nullptr};
    TEST_ASSERT_TRUE(app::motion::set_bypass(false, err));
    app::Snapshot s;
    app::motion::tick(0, s);
    TEST_ASSERT_FALSE(s.bypass);
    TEST_ASSERT_TRUE(app::motion::set_bypass(true, err));
    app::motion::tick(0, s);
    TEST_ASSERT_TRUE(s.bypass);
}

void test_rf_lockout_refuses_motion() {
    app::motion::Refusal err = {nullptr, nullptr};
    // Inject TX above threshold (5 W default in safety_sim.cpp).
    TEST_ASSERT_TRUE(app::motion::set_fwd_w_fake(50.0f, err));
    TEST_ASSERT_FALSE(app::motion::move_l(100, true, err));
    TEST_ASSERT_EQUAL_STRING("rf_lockout", err.code);
    TEST_ASSERT_FALSE(app::motion::move_c(100, true, err));
    TEST_ASSERT_FALSE(app::motion::set_side(app::Side::LoZ, err));
    TEST_ASSERT_FALSE(app::motion::home(err));
}

void test_set_bypass_allowed_under_rf() {
    app::motion::Refusal err = {nullptr, nullptr};
    TEST_ASSERT_TRUE(app::motion::set_fwd_w_fake(100.0f, err));
    // Invariant: bypass is the only relay verb safe while RF is keyed.
    TEST_ASSERT_TRUE(app::motion::set_bypass(false, err));
    TEST_ASSERT_TRUE(app::motion::set_bypass(true, err));
}

void test_home_anchors_at_zero_and_marks_homed() {
    app::motion::Refusal err = {nullptr, nullptr};
    // Move somewhere first.
    TEST_ASSERT_TRUE(app::motion::move_l(500, true, err));
    TEST_ASSERT_TRUE(app::motion::move_c(-300, true, err));
    app::Snapshot s;
    for (int i = 0; i < 200; ++i) {
        app::motion::tick(i, s);
        if (!s.moving) break;
    }
    TEST_ASSERT_TRUE(app::motion::home(err));
    app::motion::tick(1000, s);
    TEST_ASSERT_EQUAL_UINT32(0, s.l_steps);
    TEST_ASSERT_EQUAL_UINT32(0, s.c_steps);
    TEST_ASSERT_EQUAL_INT32(0, s.l_enc);
    TEST_ASSERT_EQUAL_INT32(0, s.c_enc);
    TEST_ASSERT_TRUE(s.homed);
}

void test_move_to_absolute_target() {
    app::motion::Refusal err = {nullptr, nullptr};
    TEST_ASSERT_TRUE(app::motion::move_l(2000, /*is_delta=*/false, err));
    app::Snapshot s;
    for (int i = 0; i < 200; ++i) {
        app::motion::tick(i, s);
        if (!s.moving) break;
    }
    TEST_ASSERT_EQUAL_UINT32(2000, s.l_steps);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_init_state);
    RUN_TEST(test_move_l_delta_advances_position);
    RUN_TEST(test_set_side_changes_side);
    RUN_TEST(test_set_bypass_off_then_on);
    RUN_TEST(test_rf_lockout_refuses_motion);
    RUN_TEST(test_set_bypass_allowed_under_rf);
    RUN_TEST(test_home_anchors_at_zero_and_marks_homed);
    RUN_TEST(test_move_to_absolute_target);
    return UNITY_END();
}
