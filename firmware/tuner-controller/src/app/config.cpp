#include "app/config.h"

#include <cmath>
#include <cstring>

namespace app {

// ── Names ───────────────────────────────────────────────────────────────

namespace {

const char *const kKindNames[kElementKindCount] = {
    "unset", "inductor", "vacuum_cap", "varcap_limited", "varcap_free", "variometer"
};

} // namespace

const char *topology_kind_name(TopologyKind k) {
    return k == TopologyKind::BalancedPi ? "balanced_pi" : "balanced_l";
}

bool parse_topology_kind(const char *s, TopologyKind &out) {
    if (!s) return false;
    if (strcmp(s, "balanced_l") == 0)  { out = TopologyKind::BalancedL;  return true; }
    if (strcmp(s, "balanced_pi") == 0) { out = TopologyKind::BalancedPi; return true; }
    return false;
}

const char *element_kind_name(ElementKind k) {
    const uint8_t i = static_cast<uint8_t>(k);
    return i < kElementKindCount ? kKindNames[i] : "?";
}

bool parse_element_kind(const char *s, ElementKind &out) {
    if (!s || !*s) return false;
    if (s[0] >= '0' && s[0] <= '9' && s[1] == '\0') {
        const uint8_t k = static_cast<uint8_t>(s[0] - '0');
        if (k >= kElementKindCount) return false;
        out = static_cast<ElementKind>(k);
        return true;
    }
    for (uint8_t k = 0; k < kElementKindCount; k++) {
        if (strcmp(s, kKindNames[k]) == 0) { out = static_cast<ElementKind>(k); return true; }
    }
    return false;
}

bool kind_has_stops(ElementKind k) {
    return k == ElementKind::Inductor || k == ElementKind::VacuumCap ||
           k == ElementKind::VarCapLimited;
}

int32_t AxisConfig::max_steps() const {
    if (!kind_has_stops(kind) || !(max_rev > 0.0f) || max_rev >= kMaxRatedRev) return 0;
    return static_cast<int32_t>(std::lround(max_rev * static_cast<float>(kStepsPerRev)));
}

// ── Topology ────────────────────────────────────────────────────────────

namespace {

void set_binding(ElementBinding &b, const char *name, ElementType t, uint8_t axis, bool pair) {
    strncpy(b.name, name, sizeof(b.name) - 1);
    b.name[sizeof(b.name) - 1] = '\0';
    b.type = t;
    b.axis = axis;
    b.pair = pair;
}

} // namespace

Topology Topology::default_balanced_l() {
    Topology t;
    t.kind = TopologyKind::BalancedL;
    t.n    = 2;
    set_binding(t.elements[0], "L", ElementType::L, 0, true);
    set_binding(t.elements[1], "C", ElementType::C, 1, false);
    return t;
}

Topology Topology::default_balanced_pi() {
    Topology t;
    t.kind = TopologyKind::BalancedPi;
    t.n    = 3;
    set_binding(t.elements[0], "C1", ElementType::C, 0, false);
    set_binding(t.elements[1], "L",  ElementType::L, 1, true);
    set_binding(t.elements[2], "C2", ElementType::C, 2, false);
    return t;
}

int Topology::element_for_axis(uint8_t axis) const {
    for (uint8_t i = 0; i < n && i < hal::kMaxAxes; i++) {
        if (elements[i].axis == axis) return i;
    }
    return -1;
}

int Topology::element_by_name(const char *name) const {
    if (!name) return -1;
    for (uint8_t i = 0; i < n && i < hal::kMaxAxes; i++) {
        if (strcmp(elements[i].name, name) == 0) return i;
    }
    return -1;
}

const char *validate_topology(Topology &t) {
    if (t.kind != TopologyKind::BalancedL && t.kind != TopologyKind::BalancedPi) return "bad_kind";

    // Required element set per kind: names + types are fixed by the
    // network; only the axis binding is the operator's choice.
    struct Req { const char *name; ElementType type; };
    const Req reqL[]  = { {"L", ElementType::L}, {"C", ElementType::C} };
    const Req reqPi[] = { {"C1", ElementType::C}, {"L", ElementType::L}, {"C2", ElementType::C} };
    const Req *req    = (t.kind == TopologyKind::BalancedL) ? reqL : reqPi;
    const uint8_t nreq = (t.kind == TopologyKind::BalancedL) ? 2 : 3;

    if (t.n != nreq) return "bad_elements";
    for (uint8_t r = 0; r < nreq; r++) {
        const int i = t.element_by_name(req[r].name);
        if (i < 0 || t.elements[i].type != req[r].type) return "bad_elements";
        // The inductor pair is always one synchronized axis.
        t.elements[i].pair = (req[r].type == ElementType::L);
    }
    for (uint8_t i = 0; i < t.n; i++) {
        if (t.elements[i].axis >= hal::kMaxAxes) return "bad_axis";
        for (uint8_t j = i + 1; j < t.n; j++) {
            if (t.elements[i].axis == t.elements[j].axis) return "duplicate_axis";
        }
    }
    return nullptr;
}

// ── NVS layout v2 ───────────────────────────────────────────────────────
//
//   0..3    u32  magic 'ATC1'
//   4       u8   layout version (2)
//   8       u8   topology kind
//   12..15  u32  settings generation (see Persisted::generation)
//   9       u8   topology element count
//   16..39  3 × 8 B element bindings: name[4], type u8, axis u8, pair u8, pad
//   48..143 3 × 32 B axis records:
//             +0  i32  position (clean-shutdown anchor)
//             +4  u32  speed (steps/s)
//             +8  u32  accel (steps/s²)
//             +12 u8   element kind
//             +13 u8   home_set
//             +14 u8   dirty (move in flight when written)
//             +16 f32  rated travel (revolutions)
//
// Every field has update semantics in hal::nvs, so re-saving an
// unchanged position after each move costs no wear.

namespace {

constexpr uint32_t kMagic    = 0x41544331UL;   // 'ATC1'
constexpr uint8_t  kVersion  = 2;              // v2: + generation counter

constexpr size_t OFF_MAGIC      = 0;
constexpr size_t OFF_VERSION    = 4;
constexpr size_t OFF_TOPO_KIND  = 8;
constexpr size_t OFF_TOPO_N     = 9;
constexpr size_t OFF_GENERATION = 12;
constexpr size_t OFF_ELEMENTS  = 16;
constexpr size_t ELEM_REC      = 8;
constexpr size_t OFF_AXES      = 48;
constexpr size_t AXIS_REC      = 32;
constexpr size_t AX_POS = 0, AX_SPEED = 4, AX_ACCEL = 8, AX_KIND = 12, AX_HOME = 13, AX_DIRTY = 14, AX_MAXREV = 16;

static_assert(OFF_AXES + hal::kMaxAxes * AXIS_REC <= hal::nvs::kSize, "NVS layout exceeds store");

template <typename T> void rd(size_t off, T &v)       { hal::nvs::read(off, &v, sizeof(v)); }
template <typename T> void wr(size_t off, const T &v) { hal::nvs::write(off, &v, sizeof(v)); }

size_t axis_off(uint8_t a) { return OFF_AXES + static_cast<size_t>(a) * AXIS_REC; }

} // namespace

void nvs_save_topology(const Topology &t) {
    wr<uint8_t>(OFF_TOPO_KIND, static_cast<uint8_t>(t.kind));
    wr<uint8_t>(OFF_TOPO_N, t.n);
    for (uint8_t i = 0; i < hal::kMaxAxes; i++) {
        const size_t off = OFF_ELEMENTS + i * ELEM_REC;
        const ElementBinding &b = t.elements[i];
        hal::nvs::write(off, b.name, 4);
        wr<uint8_t>(off + 4, static_cast<uint8_t>(b.type));
        wr<uint8_t>(off + 5, b.axis);
        wr<uint8_t>(off + 6, b.pair ? 1 : 0);
    }
}

void nvs_save_axis(uint8_t a, const AxisConfig &c) {
    if (a >= hal::kMaxAxes) return;
    const size_t off = axis_off(a);
    wr<uint32_t>(off + AX_SPEED, c.speed);
    wr<uint32_t>(off + AX_ACCEL, c.accel);
    wr<uint8_t>(off + AX_KIND, static_cast<uint8_t>(c.kind));
    wr<uint8_t>(off + AX_HOME, c.home_set ? 1 : 0);
    wr<float>(off + AX_MAXREV, c.max_rev);
}

void nvs_save_position(uint8_t a, int32_t pos, bool dirty) {
    if (a >= hal::kMaxAxes) return;
    const size_t off = axis_off(a);
    wr<int32_t>(off + AX_POS, pos);
    wr<uint8_t>(off + AX_DIRTY, dirty ? 1 : 0);
}

void nvs_save_generation(uint32_t generation) {
    wr<uint32_t>(OFF_GENERATION, generation);
}

void nvs_format(const Persisted &p) {
    nvs_save_generation(p.generation);
    nvs_save_topology(p.topology);
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        nvs_save_axis(a, p.axis[a]);
        nvs_save_position(a, p.position[a], p.dirty[a]);
    }
    wr<uint8_t>(OFF_VERSION, kVersion);
    wr<uint32_t>(OFF_MAGIC, kMagic);
}

bool nvs_load(Persisted &out) {
    uint32_t magic = 0;
    uint8_t  ver   = 0;
    rd(OFF_MAGIC, magic);
    rd(OFF_VERSION, ver);
    if (magic != kMagic || ver != kVersion) {
        out = Persisted{};
        out.topology = Topology::default_balanced_l();
        nvs_format(out);
        return false;
    }

    rd(OFF_GENERATION, out.generation);

    Topology t;
    uint8_t kind = 0, n = 0;
    rd(OFF_TOPO_KIND, kind);
    rd(OFF_TOPO_N, n);
    t.kind = static_cast<TopologyKind>(kind);
    t.n    = n > hal::kMaxAxes ? hal::kMaxAxes : n;
    for (uint8_t i = 0; i < hal::kMaxAxes; i++) {
        const size_t off = OFF_ELEMENTS + i * ELEM_REC;
        ElementBinding &b = t.elements[i];
        hal::nvs::read(off, b.name, 4);
        b.name[3] = '\0';
        uint8_t type = 0, axis = 0, pair = 0;
        rd(off + 4, type); rd(off + 5, axis); rd(off + 6, pair);
        b.type = type ? ElementType::C : ElementType::L;
        b.axis = axis;
        b.pair = pair != 0;
    }
    // A corrupt topology block falls back to the default rather than
    // leaving the controller with an unusable element map.
    if (validate_topology(t) != nullptr) t = Topology::default_balanced_l();
    out.topology = t;

    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        const size_t off = axis_off(a);
        AxisConfig &c = out.axis[a];
        uint8_t kind8 = 0, home = 0, dirty = 0;
        rd(off + AX_POS,   out.position[a]);
        rd(off + AX_SPEED, c.speed);
        rd(off + AX_ACCEL, c.accel);
        rd(off + AX_KIND,  kind8);
        rd(off + AX_HOME,  home);
        rd(off + AX_DIRTY, dirty);
        rd(off + AX_MAXREV, c.max_rev);
        c.kind     = kind8 < kElementKindCount ? static_cast<ElementKind>(kind8) : ElementKind::Unset;
        c.home_set = home != 0;
        if (!(c.max_rev > 0.0f) || !(c.max_rev < kMaxRatedRev)) c.max_rev = 0.0f;   // NaN / garbage → 0
        if (c.speed < 1 || c.speed > kMaxSpeedHz)  c.speed = kDefaultSpeed;
        if (c.accel < 1 || c.accel > kMaxAccelHz2) c.accel = kDefaultAccel;
        out.dirty[a] = dirty != 0;
    }
    return true;
}

} // namespace app
