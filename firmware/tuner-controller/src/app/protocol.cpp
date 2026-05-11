#include "app/protocol.h"

#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

namespace app {

namespace {

const char *side_str(Side s) {
    return s == Side::HiZ ? "hi_z" : "lo_z";
}

// Format a millisecond-resolution monotonic clock as an ISO-8601-like
// timestamp the master can parse. Because the controller has no
// wall-clock source on boot, this is a synthetic timestamp anchored at
// epoch 2000-01-01 + (millis() ms) — enough for ordering / staleness
// detection on the master side, which is the only thing `ts` is used
// for. The master re-stamps on its own clock when fanning out to
// browsers.
void synth_iso_ts(uint32_t millis, char *out, size_t n) {
    const uint32_t seconds = millis / 1000U;
    const uint32_t frac_ms = millis - seconds * 1000U;
    // 2000-01-01T00:00:00 epoch in seconds: 946684800
    // We emit it relative so the format stays valid even far from epoch.
    snprintf(out, n,
             "2000-01-01T%02lu:%02lu:%02lu.%03luZ",
             static_cast<unsigned long>((seconds / 3600U) % 24U),
             static_cast<unsigned long>((seconds / 60U) % 60U),
             static_cast<unsigned long>(seconds % 60U),
             static_cast<unsigned long>(frac_ms));
}

} // namespace

int serialize_heartbeat(char *out, size_t out_size, uint32_t seq) {
    JsonDocument doc;
    doc["type"] = "heartbeat";
    doc["seq"]  = seq;
    char ts[32];
    synth_iso_ts(millis(), ts, sizeof(ts));
    doc["ts"] = ts;
    return serializeJson(doc, out, out_size);
}

int serialize_state(char *out, size_t out_size, uint32_t seq,
                    const Snapshot &snap) {
    JsonDocument doc;
    doc["type"] = "state";
    doc["seq"]  = seq;
    char ts[32];
    synth_iso_ts(millis(), ts, sizeof(ts));
    doc["ts"] = ts;

    JsonObject data = doc["data"].to<JsonObject>();
    data["l_steps"]   = snap.l_steps;
    data["c_steps"]   = snap.c_steps;
    data["l_enc"]     = snap.l_enc;
    data["c_enc"]     = snap.c_enc;
    data["side"]      = side_str(snap.side);
    data["bypass"]    = snap.bypass;
    data["moving"]    = snap.moving;
    data["homed"]     = snap.homed;

    char last_move[32];
    synth_iso_ts(snap.last_move_ms, last_move, sizeof(last_move));
    data["last_move"] = last_move;

    return serializeJson(doc, out, out_size);
}

int serialize_status(char *out, size_t out_size, uint32_t seq,
                     const char *level, const char *code, const char *msg) {
    JsonDocument doc;
    doc["type"]  = "status";
    doc["seq"]   = seq;
    char ts[32];
    synth_iso_ts(millis(), ts, sizeof(ts));
    doc["ts"]    = ts;
    doc["level"] = level;
    if (code && code[0] != '\0') {
        doc["code"] = code;
    }
    doc["msg"] = msg ? msg : "";
    return serializeJson(doc, out, out_size);
}

int serialize_ack_ok(char *out, size_t out_size, const char *ref) {
    JsonDocument doc;
    doc["type"] = "ack";
    if (ref && ref[0] != '\0') {
        doc["ref"] = ref;
    }
    doc["ok"] = true;
    return serializeJson(doc, out, out_size);
}

int serialize_ack_err(char *out, size_t out_size, const char *ref,
                      const char *code, const char *msg) {
    JsonDocument doc;
    doc["type"] = "ack";
    if (ref && ref[0] != '\0') {
        doc["ref"] = ref;
    }
    doc["ok"] = false;
    JsonObject err = doc["err"].to<JsonObject>();
    err["code"] = code ? code : "unknown";
    err["msg"]  = msg ? msg  : "";
    return serializeJson(doc, out, out_size);
}

bool parse_args_move(const char *line, size_t line_len,
                     int32_t &value, bool &is_delta) {
    JsonDocument doc;
    if (deserializeJson(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull()) return false;
    if (!args["delta_steps"].isNull()) {
        value    = args["delta_steps"].as<int32_t>();
        is_delta = true;
        return true;
    }
    if (!args["target_steps"].isNull()) {
        value    = args["target_steps"].as<int32_t>();
        is_delta = false;
        return true;
    }
    return false;
}

bool parse_args_set_side(const char *line, size_t line_len, Side &out) {
    JsonDocument doc;
    if (deserializeJson(doc, line, line_len)) return false;
    const char *s = doc["args"]["side"] | "";
    if (strcmp(s, "hi_z") == 0) { out = Side::HiZ; return true; }
    if (strcmp(s, "lo_z") == 0) { out = Side::LoZ; return true; }
    return false;
}

bool parse_args_set_bypass(const char *line, size_t line_len, bool &out) {
    JsonDocument doc;
    if (deserializeJson(doc, line, line_len)) return false;
    JsonVariantConst args = doc["args"];
    if (args.isNull()) return false;
    if (!args["on"].isNull())     { out = args["on"].as<bool>();     return true; }
    if (!args["bypass"].isNull()) { out = args["bypass"].as<bool>(); return true; }
    return false;
}

bool parse_args_set_fwd_w(const char *line, size_t line_len, float &out) {
    JsonDocument doc;
    if (deserializeJson(doc, line, line_len)) return false;
    JsonVariantConst w = doc["args"]["w"];
    if (w.isNull()) return false;
    out = w.as<float>();
    return true;
}

bool parse_command(const char *line, size_t line_len, InboundCommand &out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line, line_len);
    if (err) {
        return false;
    }
    const char *type = doc["type"] | "";
    if (strcmp(type, "command") != 0) {
        return false;
    }
    const char *id     = doc["id"]     | "";
    const char *action = doc["action"] | "";
    if (action[0] == '\0') {
        return false;
    }
    strncpy(out.id,     id,     sizeof(out.id)     - 1);
    strncpy(out.action, action, sizeof(out.action) - 1);
    out.id[sizeof(out.id) - 1]         = '\0';
    out.action[sizeof(out.action) - 1] = '\0';
    // Args parsing is left to per-verb dispatch; we don't carry the raw
    // pointer because JsonDocument owns the parse arena.
    out.args_json = nullptr;
    out.args_len  = 0;
    return true;
}

} // namespace app
