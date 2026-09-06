// Host-side unit tests for app/config: topology validation, element-kind
// helpers and the NVS record round-trip through the sim store.

#include <unity.h>

#include <cstring>

#include "app/config.h"
#include "hal/hal.h"

void setUp()    { hal::nvs::init(); }
void tearDown() {}

void test_defaults_validate() {
    app::Topology l = app::Topology::default_balanced_l();
    TEST_ASSERT_NULL(app::validate_topology(l));
    TEST_ASSERT_TRUE(l.elements[0].pair);
    TEST_ASSERT_FALSE(l.elements[1].pair);
    app::Topology pi = app::Topology::default_balanced_pi();
    TEST_ASSERT_NULL(app::validate_topology(pi));
    TEST_ASSERT_EQUAL_INT(1, pi.element_by_name("L"));
    TEST_ASSERT_EQUAL_INT(2, pi.element_for_axis(2));
    TEST_ASSERT_FALSE(l.axis_active(2));
    TEST_ASSERT_TRUE(pi.axis_active(2));
}

void test_validation_codes() {
    app::Topology t = app::Topology::default_balanced_l();
    t.elements[1].axis = 0;
    TEST_ASSERT_EQUAL_STRING("duplicate_axis", app::validate_topology(t));
    t = app::Topology::default_balanced_l();
    t.elements[1].axis = 3;
    TEST_ASSERT_EQUAL_STRING("bad_axis", app::validate_topology(t));
    t = app::Topology::default_balanced_l();
    strcpy(t.elements[1].name, "C2");
    TEST_ASSERT_EQUAL_STRING("bad_elements", app::validate_topology(t));
    t = app::Topology::default_balanced_l();
    t.elements[1].type = app::ElementType::L;
    TEST_ASSERT_EQUAL_STRING("bad_elements", app::validate_topology(t));
    t = app::Topology::default_balanced_pi();
    t.n = 2;
    TEST_ASSERT_EQUAL_STRING("bad_elements", app::validate_topology(t));
    // pair flag is normalised, not rejected
    t = app::Topology::default_balanced_l();
    t.elements[0].pair = false;
    TEST_ASSERT_NULL(app::validate_topology(t));
    TEST_ASSERT_TRUE(t.elements[0].pair);
}

void test_element_kind_helpers() {
    app::ElementKind k;
    TEST_ASSERT_TRUE(app::parse_element_kind("vacuum_cap", k));
    TEST_ASSERT_TRUE(k == app::ElementKind::VacuumCap);
    TEST_ASSERT_TRUE(app::parse_element_kind("5", k));
    TEST_ASSERT_TRUE(k == app::ElementKind::Variometer);
    TEST_ASSERT_FALSE(app::parse_element_kind("6", k));
    TEST_ASSERT_FALSE(app::parse_element_kind("coil", k));
    TEST_ASSERT_TRUE(app::kind_has_stops(app::ElementKind::Inductor));
    TEST_ASSERT_FALSE(app::kind_has_stops(app::ElementKind::VarCapFree));
    TEST_ASSERT_EQUAL_STRING("varcap_limited", app::element_kind_name(app::ElementKind::VarCapLimited));

    app::AxisConfig c;
    c.kind = app::ElementKind::VacuumCap;
    c.max_rev = 40.0f;
    TEST_ASSERT_EQUAL_INT32(40 * app::kStepsPerRev, c.max_steps());
    TEST_ASSERT_FALSE(c.limits_active());
    c.home_set = true;
    TEST_ASSERT_TRUE(c.limits_active());
    c.kind = app::ElementKind::Variometer;
    TEST_ASSERT_EQUAL_INT32(0, c.max_steps());
    TEST_ASSERT_FALSE(c.limits_active());
}

void test_nvs_round_trip() {
    app::Persisted p;
    p.generation = 42;
    p.topology = app::Topology::default_balanced_pi();
    p.topology.elements[0].axis = 2;
    p.topology.elements[2].axis = 0;
    p.axis[1].kind = app::ElementKind::Inductor;
    p.axis[1].home_set = true;
    p.axis[1].max_rev = 27.5f;
    p.axis[1].speed = 12800;
    p.axis[1].accel = 4000;
    p.position[1] = -4321;
    p.dirty[2] = true;
    app::nvs_format(p);

    app::Persisted q;
    TEST_ASSERT_TRUE(app::nvs_load(q));
    TEST_ASSERT_EQUAL_UINT32(42, q.generation);
    app::nvs_save_generation(43);
    TEST_ASSERT_TRUE(app::nvs_load(q));
    TEST_ASSERT_EQUAL_UINT32(43, q.generation);
    TEST_ASSERT_TRUE(q.topology.kind == app::TopologyKind::BalancedPi);
    TEST_ASSERT_EQUAL_UINT8(2, q.topology.elements[0].axis);
    TEST_ASSERT_EQUAL_STRING("C1", q.topology.elements[0].name);
    TEST_ASSERT_EQUAL_UINT8(0, q.topology.elements[2].axis);
    TEST_ASSERT_TRUE(q.axis[1].kind == app::ElementKind::Inductor);
    TEST_ASSERT_TRUE(q.axis[1].home_set);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 27.5f, q.axis[1].max_rev);
    TEST_ASSERT_EQUAL_UINT32(12800, q.axis[1].speed);
    TEST_ASSERT_EQUAL_UINT32(4000, q.axis[1].accel);
    TEST_ASSERT_EQUAL_INT32(-4321, q.position[1]);
    TEST_ASSERT_FALSE(q.dirty[1]);
    TEST_ASSERT_TRUE(q.dirty[2]);

    // Partial saves update in place.
    app::nvs_save_position(1, 777, true);
    app::AxisConfig c = q.axis[1];
    c.speed = 640;
    app::nvs_save_axis(1, c);
    TEST_ASSERT_TRUE(app::nvs_load(q));
    TEST_ASSERT_EQUAL_INT32(777, q.position[1]);
    TEST_ASSERT_TRUE(q.dirty[1]);
    TEST_ASSERT_EQUAL_UINT32(640, q.axis[1].speed);
}

void test_blank_store_yields_defaults() {
    // Corrupt the magic → load reports "no record", writes defaults.
    const uint32_t junk = 0xDEADBEEF;
    hal::nvs::write(0, &junk, sizeof(junk));
    app::Persisted q;
    TEST_ASSERT_FALSE(app::nvs_load(q));
    TEST_ASSERT_TRUE(q.topology.kind == app::TopologyKind::BalancedL);
    TEST_ASSERT_EQUAL_UINT8(2, q.topology.n);
    TEST_ASSERT_TRUE(q.axis[0].kind == app::ElementKind::Unset);
    TEST_ASSERT_EQUAL_UINT32(app::kDefaultSpeed, q.axis[0].speed);
    TEST_ASSERT_TRUE(app::nvs_load(q));   // defaults were written back
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_validate);
    RUN_TEST(test_validation_codes);
    RUN_TEST(test_element_kind_helpers);
    RUN_TEST(test_nvs_round_trip);
    RUN_TEST(test_blank_store_yields_defaults);
    return UNITY_END();
}
