// Host-side unit tests for app::Snapshot. Runs via `pio test -e native`.
// No Arduino dependency — proves the app/ layer compiles without an MCU.

#include <unity.h>

#include "app/state.h"

void setUp() {}
void tearDown() {}

void test_default_starts_in_bypass_hi_z() {
    app::Snapshot s;
    TEST_ASSERT_TRUE(s.bypass);
    TEST_ASSERT_TRUE(s.side == app::Side::HiZ);
    TEST_ASSERT_EQUAL_UINT32(0, s.l_steps);
    TEST_ASSERT_EQUAL_UINT32(0, s.c_steps);
}

void test_step_change_diffs() {
    app::Snapshot a, b;
    b.l_steps = 1;
    TEST_ASSERT_TRUE(a.differs(b));
}

void test_swr_deadband() {
    app::Snapshot a, b;
    b.swr = 0.005f;          // below 0.01 deadband
    TEST_ASSERT_FALSE(a.differs(b));
    b.swr = 0.02f;           // above deadband
    TEST_ASSERT_TRUE(a.differs(b));
}

void test_side_change_diffs() {
    app::Snapshot a, b;
    b.side = app::Side::LoZ;
    TEST_ASSERT_TRUE(a.differs(b));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_default_starts_in_bypass_hi_z);
    RUN_TEST(test_step_change_diffs);
    RUN_TEST(test_swr_deadband);
    RUN_TEST(test_side_change_diffs);
    return UNITY_END();
}
