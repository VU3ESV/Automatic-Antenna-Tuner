#include "app/settings.h"

#include <cstring>

#include <ArduinoJson.h>

#include "hal/hal.h"

namespace app::settings {

namespace {

constexpr uint32_t kDebounceMs   = 750;    // coalesce a burst of settings changes
constexpr uint32_t kCardPollMs   = 2000;   // insertion / removal detection
constexpr size_t   kJsonMax      = 2048;
constexpr int      kJsonVersion  = 1;

Persisted mirror;                 // what the card copy should contain
Status    st;
bool      pending      = false;   // card write scheduled
uint32_t  pending_at   = 0;       // millis() when the pending write was first seen by tick()
uint32_t  last_poll_ms = 0;

void write_card_now() {
    if (!st.sd_present) { pending = false; return; }
    static char buf[kJsonMax];
    const int n = to_json(mirror, buf, sizeof(buf));
    if (n <= 0) { st.sd_ok = false; pending = false; return; }
    st.sd_ok = hal::sdcard::write_file(kConfigPath, buf, static_cast<size_t>(n));
    if (st.sd_ok) st.sd_saves++;
    pending    = false;
    pending_at = 0;
}

void schedule() {
    pending    = true;
    pending_at = 0;
}

// Every *settings* change bumps the generation (EEPROM, synchronously);
// position saves do not — see Persisted::generation.
void bump_generation() {
    mirror.generation++;
    nvs_save_generation(mirror.generation);
}

} // namespace

// ── JSON codec ──────────────────────────────────────────────────────────

int to_json(const Persisted &p, char *out, size_t max) {
    JsonDocument doc;
    doc["version"]    = kJsonVersion;
    doc["generation"] = p.generation;
    doc["_comment"] = "Automatic Antenna Tuner controller settings. Edit with the power off and leave "
                      "'generation' alone; positions are informational (the controller's EEPROM anchors win).";

    JsonObject t = doc["topology"].to<JsonObject>();
    t["kind"] = topology_kind_name(p.topology.kind);
    JsonArray els = t["elements"].to<JsonArray>();
    for (uint8_t i = 0; i < p.topology.n && i < hal::kMaxAxes; i++) {
        const ElementBinding &b = p.topology.elements[i];
        JsonObject e = els.add<JsonObject>();
        e["name"] = b.name;
        e["type"] = b.type == ElementType::L ? "L" : "C";
        e["axis"] = b.axis;
        e["pair"] = b.pair;
    }

    JsonArray axes = doc["axes"].to<JsonArray>();
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        const AxisConfig &c = p.axis[a];
        JsonObject x = axes.add<JsonObject>();
        x["axis"]     = a;
        x["kind"]     = element_kind_name(c.kind);
        x["home_set"] = c.home_set;
        x["max_rev"]  = c.max_rev;
        x["speed"]    = c.speed;
        x["accel"]    = c.accel;
        x["position"] = p.position[a];
        x["dirty"]    = p.dirty[a];
    }

    const size_t need = measureJsonPretty(doc) + 1;
    if (need > max) return -1;
    return static_cast<int>(serializeJsonPretty(doc, out, max));
}

bool from_json(const char *text, size_t len, Persisted &out) {
    JsonDocument doc;
    if (deserializeJson(doc, text, len) != DeserializationError::Ok) return false;
    if ((doc["version"] | 0) != kJsonVersion) return false;

    Persisted p;
    p.generation = doc["generation"] | 0U;
    JsonObjectConst t = doc["topology"].as<JsonObjectConst>();
    if (t.isNull() || !parse_topology_kind(t["kind"] | "", p.topology.kind)) return false;
    JsonArrayConst els = t["elements"].as<JsonArrayConst>();
    if (els.isNull()) return false;
    p.topology.n = 0;
    for (JsonVariantConst e : els) {
        if (p.topology.n >= hal::kMaxAxes) return false;
        ElementBinding &b = p.topology.elements[p.topology.n];
        const char *name = e["name"] | "";
        const char *type = e["type"] | "";
        if (!*name || strlen(name) > 3) return false;
        strncpy(b.name, name, sizeof(b.name) - 1);
        b.name[sizeof(b.name) - 1] = '\0';
        if (strcmp(type, "L") == 0)      b.type = ElementType::L;
        else if (strcmp(type, "C") == 0) b.type = ElementType::C;
        else return false;
        const int axis = e["axis"] | -1;
        if (axis < 0 || axis >= hal::kMaxAxes) return false;
        b.axis = static_cast<uint8_t>(axis);
        b.pair = e["pair"] | false;
        p.topology.n++;
    }
    if (validate_topology(p.topology) != nullptr) return false;

    JsonArrayConst axes = doc["axes"].as<JsonArrayConst>();
    if (!axes.isNull()) {
        for (JsonVariantConst x : axes) {
            const int a = x["axis"] | -1;
            if (a < 0 || a >= hal::kMaxAxes) continue;
            AxisConfig &c = p.axis[a];
            ElementKind k;
            const char *ks = x["kind"] | "unset";
            c.kind     = parse_element_kind(ks, k) ? k : ElementKind::Unset;
            c.home_set = x["home_set"] | false;
            c.max_rev  = x["max_rev"] | 0.0f;
            if (!(c.max_rev > 0.0f) || !(c.max_rev < kMaxRatedRev)) c.max_rev = 0.0f;
            c.speed    = x["speed"] | kDefaultSpeed;
            c.accel    = x["accel"] | kDefaultAccel;
            if (c.speed < 1 || c.speed > kMaxSpeedHz)  c.speed = kDefaultSpeed;
            if (c.accel < 1 || c.accel > kMaxAccelHz2) c.accel = kDefaultAccel;
            p.position[a] = x["position"] | 0;
            p.dirty[a]    = x["dirty"] | false;
        }
    }
    out = p;
    return true;
}

// ── Policy ──────────────────────────────────────────────────────────────

void init(Persisted &cfg) {
    hal::nvs::init();
    st           = Status{};
    pending      = false;
    pending_at   = 0;
    last_poll_ms = 0;

    Persisted ee;
    const bool ee_ok = nvs_load(ee);

    st.sd_present = hal::sdcard::init();
    bool from_card = false;
    if (st.sd_present) {
        static char buf[kJsonMax];
        const int n = hal::sdcard::read_file(kConfigPath, buf, sizeof(buf));
        Persisted card;
        // The card wins for settings only if it is not older than EEPROM:
        // a copy that missed the last (debounced) write must not revert
        // a change made just before a power loss. A card edited on a PC
        // keeps its generation and therefore still wins.
        if (n > 0 && from_json(buf, static_cast<size_t>(n), card) && card.generation >= ee.generation) {
            cfg = card;
            // Positions + the move-in-flight marker are EEPROM's: they are
            // written synchronously around every move, the card copy is
            // debounced and could be stale by one move.
            for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
                cfg.position[a] = ee.position[a];
                cfg.dirty[a]    = ee.dirty[a];
            }
            from_card = true;
            st.sd_ok  = true;
            // Mirror the card's settings into EEPROM for a cardless boot.
            nvs_save_topology(cfg.topology);
            for (uint8_t a = 0; a < hal::kMaxAxes; a++) nvs_save_axis(a, cfg.axis[a]);
            nvs_save_generation(cfg.generation);
        }
    }
    if (!from_card) cfg = ee;
    st.source = from_card ? SettingsSource::Sd : (ee_ok ? SettingsSource::Eeprom : SettingsSource::Defaults);
    mirror    = cfg;
    if (st.sd_present && !from_card) write_card_now();   // create / repair the card copy
}

void save_topology(const Topology &t) {
    mirror.topology = t;
    nvs_save_topology(t);
    bump_generation();
    schedule();
}

void save_axis(uint8_t a, const AxisConfig &c) {
    if (a >= hal::kMaxAxes) return;
    mirror.axis[a] = c;
    nvs_save_axis(a, c);
    bump_generation();
    schedule();
}

void save_position(uint8_t a, int32_t pos, bool dirty) {
    if (a >= hal::kMaxAxes) return;
    mirror.position[a] = pos;
    mirror.dirty[a]    = dirty;
    nvs_save_position(a, pos, dirty);
    if (!dirty) schedule();   // card copy only needs the settled position
}

void tick(uint32_t now_ms) {
    if (now_ms - last_poll_ms >= kCardPollMs) {
        last_poll_ms = now_ms;
        const bool was = st.sd_present;
        st.sd_present = hal::sdcard::present();
        if (!was && !st.sd_present) st.sd_present = hal::sdcard::init();   // card (re)inserted?
        if (!was && st.sd_present) schedule();                             // bring the new card up to date
        if (was && !st.sd_present) st.sd_ok = false;
    }
    if (!pending || !st.sd_present) return;
    if (pending_at == 0) { pending_at = now_ms ? now_ms : 1; return; }
    if (now_ms - pending_at >= kDebounceMs) write_card_now();
}

bool flush() {
    if (!st.sd_present) return false;
    pending = true;
    write_card_now();
    return st.sd_ok;
}

const Status &status() { return st; }

const Persisted &current() { return mirror; }

int read_card_copy(char *buf, size_t max) {
    if (!st.sd_present) return -1;
    return hal::sdcard::read_file(kConfigPath, buf, max);
}

} // namespace app::settings
