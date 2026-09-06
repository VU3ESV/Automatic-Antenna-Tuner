#pragma once

// Install-time configuration of the tuner: which balanced network is
// wired (CLAUDE.md "RF topology"), which carrier axis drives which
// element, what kind of hardware sits on each shaft (decides whether a
// software travel window protects it), per-axis motion settings, and
// the clean-shutdown position anchors (invariant 3). Everything here is
// persisted through hal::nvs; the layout lives in config.cpp.
//
// Application-layer code only — no platform headers.

#include <cstdint>

#include "hal/hal.h"

namespace app {

// Steps per element revolution: iHSS60 DIP set to 6400 p/r, motor
// coupled directly to the element shaft (docs/HARDWARE.md "Drive train").
constexpr int32_t kStepsPerRev = 6400;

// ── Topology ────────────────────────────────────────────────────────────

enum class TopologyKind : uint8_t { BalancedL = 0, BalancedPi = 1 };
enum class ElementType  : uint8_t { L = 0, C = 1 };

const char *topology_kind_name(TopologyKind k);
bool        parse_topology_kind(const char *s, TopologyKind &out);

struct ElementBinding {
    char        name[4] = {0};   // "L", "C", "C1", "C2"
    ElementType type    = ElementType::L;
    uint8_t     axis    = 0;     // carrier stepper channel 0..kMaxAxes-1
    bool        pair    = false; // true for the synchronized inductor pair
};

struct Topology {
    TopologyKind   kind = TopologyKind::BalancedL;
    uint8_t        n    = 0;
    ElementBinding elements[hal::kMaxAxes];

    static Topology default_balanced_l();    // L → axis 0 (pair), C → axis 1
    static Topology default_balanced_pi();   // C1 → 0, L → 1 (pair), C2 → 2

    // Index into elements[] or -1.
    int  element_for_axis(uint8_t axis) const;
    int  element_by_name(const char *name) const;
    bool axis_active(uint8_t axis) const { return element_for_axis(axis) >= 0; }
};

// nullptr when valid, else a stable refusal code: "bad_kind",
// "bad_elements" (wrong names / types for the kind), "bad_axis",
// "duplicate_axis". Normalises the inductor's `pair` flag to true.
const char *validate_topology(Topology &t);

// ── Per-axis element hardware + motion settings ─────────────────────────

// What is bolted to the motor. Kinds with mechanical stops get a
// software travel window [0, max_steps] anchored at the declared home.
enum class ElementKind : uint8_t {
    Unset         = 0,   // nothing declared — no window, UI warns
    Inductor      = 1,   // roller inductor: stops at both ends
    VacuumCap     = 2,   // vacuum-variable capacitor: stops, fragile bellows
    VarCapLimited = 3,   // air variable with end stops (e.g. 180°)
    VarCapFree    = 4,   // air variable that rotates freely
    Variometer    = 5,   // variometer, rotates freely
};
constexpr uint8_t kElementKindCount = 6;

const char *element_kind_name(ElementKind k);
bool        parse_element_kind(const char *s, ElementKind &out);   // name or digit
bool        kind_has_stops(ElementKind k);

constexpr float    kMaxRatedRev   = 100000.0f;   // sanity bound, not a real limit
constexpr uint32_t kMaxSpeedHz    = 200000;      // FlexPWM / iHSS60 pulse ceiling
constexpr uint32_t kMaxAccelHz2   = 10000000;
constexpr uint32_t kDefaultSpeed  = 25600;      // 4 rev/s at 6400 p/r (bench-proven cruise rate)
constexpr uint32_t kDefaultAccel  = 25600;

struct AxisConfig {
    ElementKind kind     = ElementKind::Unset;
    bool        home_set = false;   // operator declared home since the kind was set
    float       max_rev  = 0.0f;    // rated travel, element revolutions
    uint32_t    speed    = kDefaultSpeed;
    uint32_t    accel    = kDefaultAccel;

    int32_t max_steps() const;      // 0 when no window applies
    bool    limits_active() const { return kind_has_stops(kind) && home_set && max_steps() > 0; }
};

// ── Persistence ─────────────────────────────────────────────────────────

// Where the running configuration came from at boot (app/settings.h).
enum class SettingsSource : uint8_t { Defaults = 0, Eeprom = 1, Sd = 2 };

struct Persisted {
    // Bumped on every *settings* change (not on position saves). Stored
    // in EEPROM synchronously and in the card copy; at boot the card wins
    // only if its generation is not older than EEPROM's, so a card copy
    // that missed the last debounced write can never revert a change.
    uint32_t   generation = 0;
    Topology   topology;
    AxisConfig axis[hal::kMaxAxes];
    int32_t    position[hal::kMaxAxes] = {0, 0, 0};
    // true = a move was in flight when this record was last written, so
    // the position is not a clean-shutdown anchor (invariant 3).
    bool       dirty[hal::kMaxAxes]    = {false, false, false};
};

// Load everything. Returns false (and fills `out` with defaults, which
// are also written back) when no valid record exists.
bool nvs_load(Persisted &out);
void nvs_format(const Persisted &p);
void nvs_save_topology(const Topology &t);
void nvs_save_axis(uint8_t axis, const AxisConfig &c);
void nvs_save_generation(uint32_t generation);
void nvs_save_position(uint8_t axis, int32_t pos, bool dirty);

} // namespace app
