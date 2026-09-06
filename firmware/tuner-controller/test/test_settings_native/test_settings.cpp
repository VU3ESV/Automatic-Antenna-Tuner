// Host-side unit tests for app::settings — the EEPROM + SD card policy
// and the config.json codec — against the sim stores.

#include <unity.h>

#include <cstring>

#include "app/config.h"
#include "app/settings.h"
#include "hal/hal.h"

// Sim-only hooks (hal/sdcard_sim.cpp).
namespace hal::sdcard {
void sim_set_present(bool inserted);
void sim_format();
}

namespace {

char buf[2048];

app::Persisted sample() {
    app::Persisted p;
    p.topology = app::Topology::default_balanced_pi();
    p.topology.elements[0].axis = 2;
    p.topology.elements[2].axis = 0;
    p.axis[1].kind     = app::ElementKind::Inductor;
    p.axis[1].home_set = true;
    p.axis[1].max_rev  = 27.5f;
    p.axis[1].speed    = 12800;
    p.axis[1].accel    = 4000;
    p.axis[2].kind     = app::ElementKind::VacuumCap;
    p.axis[2].max_rev  = 40.0f;
    p.position[1]      = -4321;
    p.position[2]      = 999;
    return p;
}

// Blank EEPROM (defaults) and an empty, inserted card.
void blank_stores() {
    hal::nvs::init();
    app::Persisted d;
    d.topology = app::Topology::default_balanced_l();
    app::nvs_format(d);
    hal::sdcard::sim_set_present(true);
    hal::sdcard::sim_format();
}

} // namespace

void setUp()    { blank_stores(); }
void tearDown() { hal::sdcard::sim_set_present(true); }

void test_json_round_trip() {
    const app::Persisted p = sample();
    const int n = app::settings::to_json(p, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"balanced_pi\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"inductor\""));
    app::Persisted q;
    TEST_ASSERT_TRUE(app::settings::from_json(buf, static_cast<size_t>(n), q));
    TEST_ASSERT_TRUE(q.topology.kind == app::TopologyKind::BalancedPi);
    TEST_ASSERT_EQUAL_UINT8(2, q.topology.elements[0].axis);
    TEST_ASSERT_EQUAL_UINT8(0, q.topology.elements[2].axis);
    TEST_ASSERT_TRUE(q.topology.elements[1].pair);
    TEST_ASSERT_TRUE(q.axis[1].kind == app::ElementKind::Inductor);
    TEST_ASSERT_TRUE(q.axis[1].home_set);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 27.5f, q.axis[1].max_rev);
    TEST_ASSERT_EQUAL_UINT32(12800, q.axis[1].speed);
    TEST_ASSERT_EQUAL_UINT32(4000, q.axis[1].accel);
    TEST_ASSERT_TRUE(q.axis[2].kind == app::ElementKind::VacuumCap);
    TEST_ASSERT_EQUAL_INT32(-4321, q.position[1]);
    TEST_ASSERT_EQUAL_INT32(999, q.position[2]);
    // Too-small buffer is reported, not truncated.
    TEST_ASSERT_EQUAL_INT(-1, app::settings::to_json(p, buf, 64));
}

void test_json_rejects_invalid() {
    app::Persisted q;
    const char *bad_topo = "{\"version\":1,\"topology\":{\"kind\":\"balanced_l\",\"elements\":["
                           "{\"name\":\"L\",\"type\":\"L\",\"axis\":0},{\"name\":\"C\",\"type\":\"C\",\"axis\":0}]}}";
    TEST_ASSERT_FALSE(app::settings::from_json(bad_topo, strlen(bad_topo), q));   // duplicate axis
    const char *bad_ver = "{\"version\":7,\"topology\":{\"kind\":\"balanced_l\",\"elements\":[]}}";
    TEST_ASSERT_FALSE(app::settings::from_json(bad_ver, strlen(bad_ver), q));
    const char *garbage = "not json at all";
    TEST_ASSERT_FALSE(app::settings::from_json(garbage, strlen(garbage), q));
    // Bad per-axis values are defaulted, not fatal.
    const char *odd = "{\"version\":1,\"topology\":{\"kind\":\"balanced_l\",\"elements\":["
                      "{\"name\":\"L\",\"type\":\"L\",\"axis\":0},{\"name\":\"C\",\"type\":\"C\",\"axis\":1}]},"
                      "\"axes\":[{\"axis\":0,\"kind\":\"banana\",\"speed\":900000,\"max_rev\":-3}]}";
    TEST_ASSERT_TRUE(app::settings::from_json(odd, strlen(odd), q));
    TEST_ASSERT_TRUE(q.axis[0].kind == app::ElementKind::Unset);
    TEST_ASSERT_EQUAL_UINT32(app::kDefaultSpeed, q.axis[0].speed);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, q.axis[0].max_rev);
}

void test_card_wins_for_settings_eeprom_wins_for_positions() {
    // Card carries the sample config; EEPROM has defaults but a real position + dirty flag.
    const app::Persisted p = sample();
    const int n = app::settings::to_json(p, buf, sizeof(buf));
    TEST_ASSERT_TRUE(hal::sdcard::write_file(app::settings::kConfigPath, buf, static_cast<size_t>(n)));
    app::nvs_save_position(1, 777, true);
    app::nvs_save_position(2, 5, false);

    app::Persisted cfg;
    app::settings::init(cfg);
    TEST_ASSERT_TRUE(app::settings::status().sd_present);
    TEST_ASSERT_TRUE(app::settings::status().sd_ok);
    TEST_ASSERT_TRUE(app::settings::status().source == app::SettingsSource::Sd);
    TEST_ASSERT_TRUE(cfg.topology.kind == app::TopologyKind::BalancedPi);
    TEST_ASSERT_TRUE(cfg.axis[1].kind == app::ElementKind::Inductor);
    TEST_ASSERT_EQUAL_UINT32(12800, cfg.axis[1].speed);
    TEST_ASSERT_EQUAL_INT32(777, cfg.position[1]);   // EEPROM, not the card's -4321
    TEST_ASSERT_TRUE(cfg.dirty[1]);
    TEST_ASSERT_EQUAL_INT32(5, cfg.position[2]);

    // The card's settings were mirrored into EEPROM.
    app::Persisted ee;
    TEST_ASSERT_TRUE(app::nvs_load(ee));
    TEST_ASSERT_TRUE(ee.topology.kind == app::TopologyKind::BalancedPi);
    TEST_ASSERT_TRUE(ee.axis[1].kind == app::ElementKind::Inductor);
}

void test_no_card_uses_eeprom() {
    app::AxisConfig c;
    c.kind = app::ElementKind::VacuumCap;
    c.max_rev = 12.0f;
    app::nvs_save_axis(0, c);
    hal::sdcard::sim_set_present(false);

    app::Persisted cfg;
    app::settings::init(cfg);
    TEST_ASSERT_FALSE(app::settings::status().sd_present);
    TEST_ASSERT_TRUE(app::settings::status().source == app::SettingsSource::Eeprom);
    TEST_ASSERT_TRUE(cfg.axis[0].kind == app::ElementKind::VacuumCap);
    // Saves still work (EEPROM only) and a later flush reports no card.
    c.max_rev = 13.0f;
    app::settings::save_axis(0, c);
    app::settings::tick(1000);
    app::settings::tick(3000);
    TEST_ASSERT_FALSE(app::settings::flush());
    app::Persisted ee;
    app::nvs_load(ee);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.0f, ee.axis[0].max_rev);
}

void test_empty_or_corrupt_card_gets_eeprom_copy() {
    app::AxisConfig c;
    c.kind = app::ElementKind::Inductor;
    c.max_rev = 30.0f;
    app::nvs_save_axis(2, c);
    const char *junk = "{\"version\":1,\"topology\":{\"kind\":\"nope\"}}";
    TEST_ASSERT_TRUE(hal::sdcard::write_file(app::settings::kConfigPath, junk, strlen(junk)));

    app::Persisted cfg;
    app::settings::init(cfg);
    TEST_ASSERT_TRUE(app::settings::status().source == app::SettingsSource::Eeprom);
    TEST_ASSERT_TRUE(app::settings::status().sd_ok);          // repaired copy written
    TEST_ASSERT_EQUAL_UINT32(1, app::settings::status().sd_saves);
    const int n = hal::sdcard::read_file(app::settings::kConfigPath, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    app::Persisted q;
    TEST_ASSERT_TRUE(app::settings::from_json(buf, static_cast<size_t>(n), q));
    TEST_ASSERT_TRUE(q.axis[2].kind == app::ElementKind::Inductor);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, q.axis[2].max_rev);
}

void test_stale_card_loses_to_newer_eeprom() {
    // Card copy from generation 3; then the device made two more changes
    // (generation 5) that never reached the card before a power loss.
    app::Persisted old = sample();
    old.generation = 3;
    int n = app::settings::to_json(old, buf, sizeof(buf));
    TEST_ASSERT_TRUE(hal::sdcard::write_file(app::settings::kConfigPath, buf, static_cast<size_t>(n)));
    app::Persisted ee = sample();
    ee.generation = 5;
    ee.axis[1].speed = 3200;                      // the newer change
    ee.topology = app::Topology::default_balanced_l();
    app::nvs_format(ee);

    app::Persisted cfg;
    app::settings::init(cfg);
    TEST_ASSERT_TRUE(app::settings::status().source == app::SettingsSource::Eeprom);
    TEST_ASSERT_EQUAL_UINT32(5, cfg.generation);
    TEST_ASSERT_EQUAL_UINT32(3200, cfg.axis[1].speed);
    TEST_ASSERT_TRUE(cfg.topology.kind == app::TopologyKind::BalancedL);
    // …and the card was repaired with the newer content.
    n = hal::sdcard::read_file(app::settings::kConfigPath, buf, sizeof(buf));
    app::Persisted q;
    TEST_ASSERT_TRUE(app::settings::from_json(buf, static_cast<size_t>(n), q));
    TEST_ASSERT_EQUAL_UINT32(5, q.generation);
    TEST_ASSERT_EQUAL_UINT32(3200, q.axis[1].speed);

    // Equal generation (e.g. edited on a PC) → the card wins.
    q.axis[1].speed = 1234;
    n = app::settings::to_json(q, buf, sizeof(buf));
    TEST_ASSERT_TRUE(hal::sdcard::write_file(app::settings::kConfigPath, buf, static_cast<size_t>(n)));
    app::settings::init(cfg);
    TEST_ASSERT_TRUE(app::settings::status().source == app::SettingsSource::Sd);
    TEST_ASSERT_EQUAL_UINT32(1234, cfg.axis[1].speed);

    // Every settings save bumps the generation in EEPROM immediately.
    app::AxisConfig c = cfg.axis[0];
    app::settings::save_axis(0, c);
    app::Persisted ee2;
    app::nvs_load(ee2);
    TEST_ASSERT_EQUAL_UINT32(6, ee2.generation);
}

void test_saves_are_debounced_onto_the_card() {
    app::Persisted cfg;
    app::settings::init(cfg);                         // empty card → copy written (1 save)
    TEST_ASSERT_EQUAL_UINT32(1, app::settings::status().sd_saves);

    app::AxisConfig c = cfg.axis[0];
    c.speed = 3200;
    app::settings::save_axis(0, c);
    c.speed = 6400;
    app::settings::save_axis(0, c);                   // two changes → one card write
    app::settings::tick(10);                          // stamps the pending write
    app::settings::tick(300);                         // inside the debounce: no write yet
    TEST_ASSERT_EQUAL_UINT32(1, app::settings::status().sd_saves);
    app::settings::tick(10 + 800);
    TEST_ASSERT_EQUAL_UINT32(2, app::settings::status().sd_saves);
    const int n = hal::sdcard::read_file(app::settings::kConfigPath, buf, sizeof(buf));
    app::Persisted q;
    TEST_ASSERT_TRUE(app::settings::from_json(buf, static_cast<size_t>(n), q));
    TEST_ASSERT_EQUAL_UINT32(6400, q.axis[0].speed);

    // A position save with the dirty marker does not touch the card; the
    // settled save does.
    app::settings::save_position(1, 100, true);
    app::settings::tick(2000); app::settings::tick(3000); app::settings::tick(4000);
    TEST_ASSERT_EQUAL_UINT32(2, app::settings::status().sd_saves);
    app::settings::save_position(1, 100, false);
    app::settings::tick(5000); app::settings::tick(6000);
    TEST_ASSERT_EQUAL_UINT32(3, app::settings::status().sd_saves);
    TEST_ASSERT_TRUE(app::settings::flush());
    TEST_ASSERT_EQUAL_UINT32(4, app::settings::status().sd_saves);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_json_round_trip);
    RUN_TEST(test_json_rejects_invalid);
    RUN_TEST(test_card_wins_for_settings_eeprom_wins_for_positions);
    RUN_TEST(test_no_card_uses_eeprom);
    RUN_TEST(test_empty_or_corrupt_card_gets_eeprom_copy);
    RUN_TEST(test_stale_card_loses_to_newer_eeprom);
    RUN_TEST(test_saves_are_debounced_onto_the_card);
    return UNITY_END();
}
