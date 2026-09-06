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
    TEST_ASSERT_FALSE(s.homed);
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) TEST_ASSERT_EQUAL_INT32(0, s.axes[a].steps);
}

void test_step_change_diffs() {
    app::Snapshot a, b;
    b.axes[2].steps = 1;
    TEST_ASSERT_TRUE(a.differs(b));
}

void test_travel_change_diffs() {
    app::Snapshot a, b;
    b.axes[0].travel = app::Travel::Max;
    TEST_ASSERT_TRUE(a.differs(b));
    b = a;
    b.axes[1].last_clamp = -1;
    TEST_ASSERT_TRUE(a.differs(b));
    b = a;
    b.axes[0].estop = true;
    TEST_ASSERT_TRUE(a.differs(b));
    b = a;
    b.estop_all = true;
    TEST_ASSERT_TRUE(a.differs(b));
    b = a;
    b.sd_present = true;
    TEST_ASSERT_TRUE(a.differs(b));
}

void test_swr_deadband() {
    app::Snapshot a, b;
    b.swr = 0.005f;          // below 0.01 deadband
    TEST_ASSERT_FALSE(a.differs(b));
    b.swr = 0.02f;           // above deadband
    TEST_ASSERT_TRUE(a.differs(b));
}

void test_side_and_topology_change_diffs() {
    app::Snapshot a, b;
    b.side = app::Side::LoZ;
    TEST_ASSERT_TRUE(a.differs(b));
    b = a;
    b.topology = app::Topology::default_balanced_pi();
    TEST_ASSERT_TRUE(a.differs(b));
}

void test_axis_of_resolves_topology_names() {
    app::Snapshot s;
    s.topology = app::Topology::default_balanced_pi();
    TEST_ASSERT_EQUAL_INT(0, s.axis_of("C1"));
    TEST_ASSERT_EQUAL_INT(1, s.axis_of("L"));
    TEST_ASSERT_EQUAL_INT(-1, s.axis_of("C"));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_default_starts_in_bypass_hi_z);
    RUN_TEST(test_step_change_diffs);
    RUN_TEST(test_travel_change_diffs);
    RUN_TEST(test_swr_deadband);
    RUN_TEST(test_side_and_topology_change_diffs);
    RUN_TEST(test_axis_of_resolves_topology_names);
    return UNITY_END();
}
