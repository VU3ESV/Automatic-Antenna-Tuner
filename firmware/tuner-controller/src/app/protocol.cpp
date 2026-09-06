#include "app/protocol.h"

#include <cstdio>
#include <cstring>

#include <Arduino.h>
#include <ArduinoJson.h>

namespace app {

namespace {

const char *side_str(Side s) { return s == Side::HiZ ? "hi_z" : "lo_z"; }

// Synthetic ISO-8601 timestamp anchored at 2000-01-01 + millis(). The
// controller has no wall clock; the master only uses `ts` for ordering
// and staleness and re-stamps on its own clock when fanning out.
void synth_iso_ts(uint32_t ms, char *out, size_t n) {
    const uint32_t seconds = ms / 1000U;
    const uint32_t frac_ms = ms - seconds * 1000U;
    snprintf(out, n, "2000-01-01T%02lu:%02lu:%02lu.%03luZ",
             static_cast<unsigned long>((seconds / 3600U) % 24U),
             static_cast<unsigned long>((seconds / 60U) % 60U),
             static_cast<unsigned long>(seconds % 60U),
             static_cast<unsigned long>(frac_ms));
}

void stamp(JsonDocument &doc, const char *type, uint32_t seq) {
    doc["type"] = type;
    doc["seq"]  = seq;
    char ts[32];
    synth_iso_ts(millis(), ts, sizeof(ts));
    doc["ts"] = ts;
}

// Copy an "axis" argument (number or string) into a small text buffer.
bool axis_arg(JsonVariantConst v, char out[kAxisArgLen]) {
    out[0] = '\0';
    if (v.isNull()) return false;
    if (v.is<const char *>()) {
        const char *s = v.as<const char *>();
        if (!s || !*s) return false;
        strncpy(out, s, kAxisArgLen - 1);
        out[kAxisArgLen - 1] = '\0';
        return true;
    }
    if (v.is<int>()) {
        snprintf(out, kAxisArgLen, "%d", v.as<int>());
        return true;
    }
    return false;
}

bool load(JsonDocument &doc, const char *line, size_t line_len) {
    return deserializeJson(doc, line, line_len) == DeserializationError::Ok;
}

} // namespace

// ── Outbound ────────────────────────────────────────────────────────────

int serialize_heartbeat(char *out, size_t out_size, uint32_t seq) {
    JsonDocument doc;
    stamp(doc, "heartbeat", seq);
    return serializeJson(doc, out, out_size);
}

int serialize_state(char *out, size_t out_size, uint32_t seq, const Snapshot &snap) {
    JsonDocument doc;
    stamp(doc, "state", seq);

    JsonObject data = doc["data"].to<JsonObject>();

    // v1 fields the Go master reads today: L and the (first) C element.
    const int la = snap.axis_of("L");
    int       ca = snap.axis_of("C");
    if (ca < 0) ca = snap.axis_of("C1");
    data["l_steps"] = la >= 0 ? static_cast<uint32_t>(snap.axes[la].steps < 0 ? 0 : snap.axes[la].steps) : 0U;
    data["c_steps"] = ca >= 0 ? static_cast<uint32_t>(snap.axes[ca].steps < 0 ? 0 : snap.axes[ca].steps) : 0U;
    data["l_enc"]   = la >= 0 ? snap.axes[la].enc : 0;
    data["c_enc"]   = ca >= 0 ? snap.axes[ca].enc : 0;
    data["side"]    = side_str(snap.side);
    data["bypass"]  = snap.bypass;
    data["moving"]  = snap.moving;
    data["homed"]   = snap.homed;
    char last_move[32];
    synth_iso_ts(snap.last_move_ms, last_move, sizeof(last_move));
    data["last_move"] = last_move;

    // Additive fields (CLAUDE.md protocol summary): topology + per-axis detail.
    data["topology"]   = topology_kind_name(snap.topology.kind);
    data["rf_lockout"] = snap.rf_lockout;
    data["estop_all"]  = snap.estop_all;
    data["fwd_w"]      = snap.fwd_w;
    data["sd_present"] = snap.sd_present;
    data["sd_ok"]      = snap.sd_ok;
    data["settings_source"] = snap.settings_source == SettingsSource::Sd ? "sd"
                            : snap.settings_source == SettingsSource::Eeprom ? "eeprom" : "defaults";

    JsonArray axes = data["axes"].to<JsonArray>();
    for (uint8_t a = 0; a < hal::kMaxAxes; a++) {
        const AxisSnapshot &s = snap.axes[a];
        JsonObject ax = axes.add<JsonObject>();
        ax["axis"] = a;
        if (s.element >= 0 && s.element < static_cast<int8_t>(snap.topology.n)) {
            const ElementBinding &b = snap.topology.elements[s.element];
            ax["name"] = b.name;
            ax["type"] = b.type == ElementType::L ? "L" : "C";
            ax["pair"] = b.pair;
        }
        ax["steps"]      = s.steps;
        ax["enc"]        = s.enc;
        ax["moving"]     = s.moving;
        ax["enabled"]    = s.enabled;
        ax["limit_sw"]   = s.limit_sw;
        ax["kind"]       = element_kind_name(s.kind);
        ax["home_set"]   = s.home_set;
        ax["anchored"]   = s.anchored;
        ax["max_rev"]    = s.max_rev;
        ax["max_steps"]  = s.max_steps;
        ax["travel"]     = travel_name(s.travel);
        ax["last_clamp"] = s.last_clamp > 0 ? "max" : (s.last_clamp < 0 ? "home" : "none");
        ax["estop"]      = s.estop;
        ax["speed"]      = s.speed;
        ax["accel"]      = s.accel;
    }

    return serializeJson(doc, out, out_size);
}

int serialize_status(char *out, size_t out_size, uint32_t seq,
                     const char *level, const char *code, const char *msg) {
    JsonDocument doc;
    stamp(doc, "status", seq);
    doc["level"] = level;
    if (code && code[0] != '\0') doc["code"] = code;
    doc["msg"] = msg ? msg : "";
    return serializeJson(doc, out, out_size);
}

int serialize_ack_ok(char *out, size_t out_size, const char *ref) {
    JsonDocument doc;
    doc["type"] = "ack";
    if (ref && ref[0] != '\0') doc["ref"] = ref;
    doc["ok"] = true;
    return serializeJson(doc, out, out_size);
}

int serialize_ack_err(char *out, size_t out_size, const char *ref,
                      const char *code, const char *msg) {
    JsonDocument doc;
    doc["type"] = "ack";
    if (ref && ref[0] != '\0') doc["ref"] = ref;
    doc["ok"] = false;
    JsonObject err = doc["err"].to<JsonObject>();
    err["code"] = code ? code : "unknown";
    err["msg"]  = msg ? msg : "";
    return serializeJson(doc, out, out_size);
}

// ── Inbound ─────────────────────────────────────────────────────────────

bool parse_command(const char *line, size_t line_len, InboundCommand &out) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    const char *type = doc["type"] | "";
    if (strcmp(type, "command") != 0) return false;
    const char *id     = doc["id"]     | "";
    const char *action = doc["action"] | "";
    if (action[0] == '\0') return false;
    strncpy(out.id,     id,     sizeof(out.id) - 1);
    strncpy(out.action, action, sizeof(out.action) - 1);
    out.id[sizeof(out.id) - 1]         = '\0';
    out.action[sizeof(out.action) - 1] = '\0';
    return true;
}

namespace {
bool value_from(JsonVariantConst args, int32_t &value, bool &is_delta) {
    if (!args["delta_steps"].isNull())  { value = args["delta_steps"].as<int32_t>();  is_delta = true;  return true; }
    if (!args["target_steps"].isNull()) { value = args["target_steps"].as<int32_t>(); is_delta = false; return true; }
    return false;
}
}

bool parse_args_move(const char *line, size_t line_len, int32_t &value, bool &is_delta) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull()) return false;
    return value_from(args, value, is_delta);
}

bool parse_args_move_axis(const char *line, size_t line_len,
                          char axis[kAxisArgLen], int32_t &value, bool &is_delta) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull() || !axis_arg(args["axis"], axis)) return false;
    return value_from(args, value, is_delta);
}

bool parse_args_run(const char *line, size_t line_len, char axis[kAxisArgLen], int &dir) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull() || !axis_arg(args["axis"], axis)) return false;
    JsonVariantConst d = args["dir"];
    if (d.is<const char *>()) {
        const char *s = d.as<const char *>();
        if (strcmp(s, "cw") == 0)  { dir = +1; return true; }
        if (strcmp(s, "ccw") == 0) { dir = -1; return true; }
        return false;
    }
    if (d.is<int>()) { dir = d.as<int>() >= 0 ? +1 : -1; return true; }
    return false;
}

bool parse_args_axis(const char *line, size_t line_len, char axis[kAxisArgLen], bool required) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    axis[0] = '\0';
    JsonVariantConst args = doc["args"];
    if (args.isNull()) return !required;
    if (axis_arg(args["axis"], axis)) return true;
    return !required;
}

bool parse_args_set_element(const char *line, size_t line_len,
                            char axis[kAxisArgLen], ElementKind &kind, float &max_rev) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull() || !axis_arg(args["axis"], axis)) return false;
    char kind_s[kAxisArgLen * 2] = {0};
    JsonVariantConst k = args["kind"];
    if (k.is<const char *>())  strncpy(kind_s, k.as<const char *>(), sizeof(kind_s) - 1);
    else if (k.is<int>())      snprintf(kind_s, sizeof(kind_s), "%d", k.as<int>());
    else return false;
    if (!parse_element_kind(kind_s, kind)) return false;
    max_rev = args["max_rev"] | 0.0f;
    return true;
}

bool parse_args_set_speed(const char *line, size_t line_len,
                          char axis[kAxisArgLen], uint32_t &speed, uint32_t &accel) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull() || !axis_arg(args["axis"], axis)) return false;
    if (args["speed"].isNull()) return false;
    speed = args["speed"].as<uint32_t>();
    accel = args["accel"] | 0U;
    return true;
}

bool parse_args_set_enabled(const char *line, size_t line_len, char axis[kAxisArgLen], bool &on) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull() || !axis_arg(args["axis"], axis)) return false;
    if (args["on"].isNull()) return false;
    on = args["on"].as<bool>();
    return true;
}

bool parse_args_set_topology(const char *line, size_t line_len, Topology &out) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull()) return false;
    const char *kind = args["kind"] | "";
    if (!parse_topology_kind(kind, out.kind)) return false;
    JsonArrayConst els = args["elements"].as<JsonArrayConst>();
    if (els.isNull()) return false;
    out.n = 0;
    for (JsonVariantConst e : els) {
        if (out.n >= hal::kMaxAxes) return false;
        ElementBinding &b = out.elements[out.n];
        const char *name = e["name"] | "";
        const char *type = e["type"] | "";
        if (!*name || strlen(name) > 3) return false;
        strncpy(b.name, name, sizeof(b.name) - 1);
        b.name[sizeof(b.name) - 1] = '\0';
        if (strcmp(type, "L") == 0)      b.type = ElementType::L;
        else if (strcmp(type, "C") == 0) b.type = ElementType::C;
        else return false;
        if (e["axis"].isNull()) return false;
        const int axis = e["axis"].as<int>();
        if (axis < 0 || axis > 255) return false;
        b.axis = static_cast<uint8_t>(axis);
        b.pair = e["pair"] | false;
        out.n++;
    }
    return out.n > 0;
}

bool parse_args_set_side(const char *line, size_t line_len, Side &out) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    const char *s = doc["args"]["side"] | "";
    if (strcmp(s, "hi_z") == 0) { out = Side::HiZ; return true; }
    if (strcmp(s, "lo_z") == 0) { out = Side::LoZ; return true; }
    return false;
}

bool parse_args_set_bypass(const char *line, size_t line_len, bool &out) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull()) return false;
    if (!args["on"].isNull())     { out = args["on"].as<bool>();     return true; }
    if (!args["bypass"].isNull()) { out = args["bypass"].as<bool>(); return true; }
    return false;
}

bool parse_args_set_fwd_w(const char *line, size_t line_len, float &out) {
    JsonDocument doc;
    if (!load(doc, line, line_len)) return false;
    JsonVariantConst w = doc["args"]["w"];
    if (w.isNull()) return false;
    out = w.as<float>();
    return true;
}

} // namespace app
