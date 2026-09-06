#include "http_server.h"

#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/motion.h"
#include "app/settings.h"
#include "net_hal.h"
#include "web_page.h"

namespace http_server {

namespace {

EthernetServer server(kPort);
bool           ready = false;

// ── Transport helpers ───────────────────────────────────────────────────
// Every write goes through net_hal::write_all (chunked, flushed between
// chunks — the NativeEthernet send-buffer footgun) and tick() flushes
// before stop() for the same reason.

using net_hal::write_all;

void send_header(EthernetClient &c, int code, const char *status, const char *ctype, int len) {
    char hdr[192];
    const int n = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           code, status, ctype, len);
    write_all(c, hdr, n);
}

void send_text(EthernetClient &c, int code, const char *status, const char *body) {
    const int n = static_cast<int>(strlen(body));
    send_header(c, code, status, "text/plain; charset=utf-8", n);
    write_all(c, body, n);
}

void send_ok(EthernetClient &c, const char *body)  { send_text(c, 200, "OK", body); }
void send_bad(EthernetClient &c, const char *body) { send_text(c, 400, "Bad Request", body); }

void send_refusal(EthernetClient &c, const app::motion::Refusal &e) {
    char body[160];
    snprintf(body, sizeof(body), "%s: %s\n", e.code ? e.code : "refused", e.msg ? e.msg : "");
    send_text(c, 409, "Conflict", body);
}

void send_index(EthernetClient &c) {
    const int n = static_cast<int>(sizeof(INDEX_HTML) - 1);
    send_header(c, 200, "OK", "text/html; charset=utf-8", n);
    write_all(c, INDEX_HTML, n);
}

// ── Query helpers ────────────────────────────────────────────────────────

bool get_param(const char *query, const char *key, char *out, size_t outsz) {
    if (!query || !out || outsz == 0) return false;
    out[0] = '\0';
    const size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < outsz) out[i++] = *p++;
            out[i] = '\0';
            return true;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return false;
}

// Resolve ?axis= (index or element name) or reply 400. Returns -1 after replying.
int axis_or_bad(EthernetClient &c, const char *query) {
    char s[8];
    if (!get_param(query, "axis", s, sizeof(s))) { send_bad(c, "missing axis\n"); return -1; }
    const int a = app::motion::resolve_axis(s);
    if (a < 0) send_bad(c, "bad axis (0..2 or a bound element name)\n");
    return a;
}

int dir_of(const char *query) {
    char s[8];
    if (!get_param(query, "dir", s, sizeof(s))) return 0;
    if (strcmp(s, "cw") == 0)  return +1;
    if (strcmp(s, "ccw") == 0) return -1;
    return 0;
}

// Map a MoveResult onto the reply: 200 with the verb echoed (the page
// learns which bound a clamped move stops at from the status poll),
// 409 for a refusal.
void reply_move(EthernetClient &c, app::motion::MoveResult r, const app::motion::Refusal &e,
                const char *verb) {
    using app::motion::MoveResult;
    char body[160];
    switch (r) {
    case MoveResult::Started:
        snprintf(body, sizeof(body), "%s\n", verb); send_ok(c, body); return;
    case MoveResult::Clamped:
        snprintf(body, sizeof(body), "%s: travel window - will stop at the limit\n", verb); send_ok(c, body); return;
    case MoveResult::Noop:
        snprintf(body, sizeof(body), "%s: already there\n", verb); send_ok(c, body); return;
    case MoveResult::AtLimit:
    case MoveResult::Refused:
        send_refusal(c, e); return;
    }
    send_text(c, 500, "Internal Server Error", "unknown move result\n");
}

// ── Status JSON ─────────────────────────────────────────────────────────

const char *axis_letter(uint8_t a) { return a == 0 ? "X" : a == 1 ? "Y" : a == 2 ? "Z" : "?"; }

void send_status(EthernetClient &c, const app::Snapshot &s, int master_clients) {
    static char json[3072];
    const IPAddress ip = Ethernet.localIP();
    int n = snprintf(json, sizeof(json),
        "{\"net\":{\"backend\":\"%s\",\"link\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"master_clients\":%d},"
        "\"topology\":{\"kind\":\"%s\",\"elements\":[",
        net_hal::lib_name(), net_hal::link_state() ? "up" : "down",
        ip[0], ip[1], ip[2], ip[3], master_clients,
        app::topology_kind_name(s.topology.kind));
    for (uint8_t i = 0; i < s.topology.n && n < static_cast<int>(sizeof(json)); i++) {
        const app::ElementBinding &b = s.topology.elements[i];
        n += snprintf(json + n, sizeof(json) - n, "%s{\"name\":\"%s\",\"type\":\"%s\",\"axis\":%u,\"pair\":%s}",
                      i ? "," : "", b.name, b.type == app::ElementType::L ? "L" : "C", b.axis,
                      b.pair ? "true" : "false");
    }
    n += snprintf(json + n, sizeof(json) - n,
        "]},\"side\":\"%s\",\"bypass\":%s,\"moving\":%s,\"homed\":%s,\"rf_lockout\":%s,\"estop_all\":%s,\"fwd_w\":%.1f,"
        "\"settings\":{\"source\":\"%s\",\"sd_present\":%s,\"sd_ok\":%s,\"sd_saves\":%lu},"
        "\"last_move_ms\":%lu,\"axes\":[",
        s.side == app::Side::HiZ ? "hi_z" : "lo_z",
        s.bypass ? "true" : "false", s.moving ? "true" : "false", s.homed ? "true" : "false",
        s.rf_lockout ? "true" : "false", s.estop_all ? "true" : "false", static_cast<double>(s.fwd_w),
        s.settings_source == app::SettingsSource::Sd ? "sd" : s.settings_source == app::SettingsSource::Eeprom ? "eeprom" : "defaults",
        s.sd_present ? "true" : "false", s.sd_ok ? "true" : "false",
        static_cast<unsigned long>(app::settings::status().sd_saves),
        static_cast<unsigned long>(s.last_move_ms));
    for (uint8_t a = 0; a < hal::kMaxAxes && n < static_cast<int>(sizeof(json)); a++) {
        const app::AxisSnapshot &x = s.axes[a];
        const char *name = (x.element >= 0 && x.element < static_cast<int8_t>(s.topology.n))
                             ? s.topology.elements[x.element].name : "";
        n += snprintf(json + n, sizeof(json) - n,
            "%s{\"axis\":%u,\"letter\":\"%s\",\"name\":\"%s\",\"steps\":%ld,\"enc\":%ld,\"moving\":%s,\"enabled\":%s,"
            "\"limit_sw\":%s,\"kind\":\"%s\",\"home_set\":%s,\"anchored\":%s,\"max_rev\":%.3f,\"max_steps\":%ld,"
            "\"travel\":\"%s\",\"last_clamp\":\"%s\",\"estop\":%s,\"turns\":%.3f,\"speed\":%lu,\"accel\":%lu}",
            a ? "," : "", a, axis_letter(a), name, static_cast<long>(x.steps), static_cast<long>(x.enc),
            x.moving ? "true" : "false", x.enabled ? "true" : "false", x.limit_sw ? "true" : "false",
            app::element_kind_name(x.kind), x.home_set ? "true" : "false", x.anchored ? "true" : "false",
            static_cast<double>(x.max_rev), static_cast<long>(x.max_steps), app::travel_name(x.travel),
            x.last_clamp > 0 ? "max" : (x.last_clamp < 0 ? "home" : "none"),
            x.estop ? "true" : "false",
            static_cast<double>(x.steps) / static_cast<double>(app::kStepsPerRev),
            static_cast<unsigned long>(x.speed), static_cast<unsigned long>(x.accel));
    }
    n += snprintf(json + n, sizeof(json) - n, "]}");
    send_header(c, 200, "OK", "application/json", n);
    write_all(c, json, n);
}

// ── Dispatcher ──────────────────────────────────────────────────────────

void dispatch(EthernetClient &c, const char *path, const char *query,
              const app::Snapshot &snap, int master_clients) {
    using app::motion::MoveResult;
    app::motion::Refusal err = {nullptr, nullptr};
    char v[24];

    if (strcmp(path, "/") == 0)           { send_index(c); return; }
    if (strcmp(path, "/api/status") == 0) { send_status(c, snap, master_clients); return; }

    if (strcmp(path, "/api/jog") == 0 || strcmp(path, "/api/goto") == 0) {
        const int a = axis_or_bad(c, query); if (a < 0) return;
        if (!get_param(query, "steps", v, sizeof(v))) { send_bad(c, "missing steps\n"); return; }
        long steps = atol(v);
        const bool is_jog = strcmp(path, "/api/jog") == 0;
        if (is_jog) {
            const int dir = dir_of(query);
            if (dir == 0 || steps <= 0 || steps > 100000000L) { send_bad(c, "bad dir/steps\n"); return; }
            steps *= dir;
        }
        const MoveResult r = app::motion::move_axis(static_cast<uint8_t>(a), static_cast<int32_t>(steps), is_jog, err);
        reply_move(c, r, err, is_jog ? "jog" : "goto"); return;
    }

    if (strcmp(path, "/api/rotate") == 0) {
        const int a = axis_or_bad(c, query); if (a < 0) return;
        const int dir = dir_of(query);
        if (!get_param(query, "revs", v, sizeof(v))) { send_bad(c, "missing revs\n"); return; }
        const long revs = atol(v);
        if (dir == 0 || revs <= 0 || revs > 100000L) { send_bad(c, "bad dir/revs\n"); return; }
        const MoveResult r = app::motion::move_axis(static_cast<uint8_t>(a),
                                                    static_cast<int32_t>(dir * revs * app::kStepsPerRev), true, err);
        reply_move(c, r, err, "rotate"); return;
    }

    if (strcmp(path, "/api/run") == 0) {
        const int a = axis_or_bad(c, query); if (a < 0) return;
        get_param(query, "dir", v, sizeof(v));
        if (strcmp(v, "stop") == 0) { app::motion::stop_axis(static_cast<uint8_t>(a), err); send_ok(c, "stopped\n"); return; }
        const int dir = dir_of(query);
        if (dir == 0) { send_bad(c, "bad dir\n"); return; }
        reply_move(c, app::motion::run_to_end(static_cast<uint8_t>(a), dir, err), err, "run"); return;
    }

    if (strcmp(path, "/api/stop") == 0) {
        if (get_param(query, "axis", v, sizeof(v))) {
            const int a = app::motion::resolve_axis(v);
            if (a < 0) { send_bad(c, "bad axis\n"); return; }
            app::motion::stop_axis(static_cast<uint8_t>(a), err);
        } else {
            app::motion::stop_all();
        }
        send_ok(c, "stop\n"); return;
    }

    // E-STOP latches an alarm (industrial semantics); estop_reset releases it.
    if (strcmp(path, "/api/estop") == 0 || strcmp(path, "/api/estop_reset") == 0) {
        const bool reset = strcmp(path, "/api/estop_reset") == 0;
        if (get_param(query, "axis", v, sizeof(v))) {
            const int a = app::motion::resolve_axis(v);
            if (a < 0) { send_bad(c, "bad axis\n"); return; }
            if (reset) app::motion::estop_reset(static_cast<uint8_t>(a), err);
            else       app::motion::estop(static_cast<uint8_t>(a), err);
            send_ok(c, reset ? "alarm reset\n" : "EMERGENCY STOP - alarm latched on this motor\n"); return;
        }
        if (reset) app::motion::estop_reset_all(); else app::motion::estop_all();
        send_ok(c, reset ? "all alarms reset\n" : "EMERGENCY STOP ALL - alarms latched, reset before moving\n"); return;
    }

    if (strcmp(path, "/api/zero") == 0) {
        const int a = axis_or_bad(c, query); if (a < 0) return;
        if (!app::motion::set_home(static_cast<uint8_t>(a), err)) { send_refusal(c, err); return; }
        send_ok(c, app::motion::axis_config(static_cast<uint8_t>(a)).limits_active()
                       ? "home set - travel window active\n" : "home set\n"); return;
    }

    if (strcmp(path, "/api/unhome") == 0) {
        const int a = axis_or_bad(c, query); if (a < 0) return;
        if (!app::motion::unset_home(static_cast<uint8_t>(a), err)) { send_refusal(c, err); return; }
        send_ok(c, "home unset - travel window INACTIVE, axis unanchored\n"); return;
    }

    if (strcmp(path, "/api/element") == 0) {
        const int a = axis_or_bad(c, query); if (a < 0) return;
        app::ElementKind kind;
        if (!get_param(query, "kind", v, sizeof(v)) || !app::parse_element_kind(v, kind)) { send_bad(c, "bad kind\n"); return; }
        float mr = 0.0f;
        if (get_param(query, "max_rev", v, sizeof(v))) mr = strtof(v, nullptr);
        if (!app::motion::set_element(static_cast<uint8_t>(a), kind, mr, err)) { send_refusal(c, err); return; }
        send_ok(c, app::kind_has_stops(kind)
                       ? (app::motion::axis_config(static_cast<uint8_t>(a)).home_set
                              ? "element set - travel window active\n"
                              : "element set - now declare home to activate the window\n")
                       : "element set - no stops, travel unlimited\n"); return;
    }

    if (strcmp(path, "/api/speed") == 0) {
        const int a = axis_or_bad(c, query); if (a < 0) return;
        if (!get_param(query, "v", v, sizeof(v))) { send_bad(c, "missing v\n"); return; }
        const uint32_t speed = static_cast<uint32_t>(strtoul(v, nullptr, 10));
        uint32_t accel = app::motion::axis_config(static_cast<uint8_t>(a)).accel;
        if (get_param(query, "acc", v, sizeof(v))) accel = static_cast<uint32_t>(strtoul(v, nullptr, 10));
        if (!app::motion::set_speed(static_cast<uint8_t>(a), speed, accel, err)) { send_refusal(c, err); return; }
        send_ok(c, "speed saved\n"); return;
    }

    if (strcmp(path, "/api/enable") == 0) {
        const int a = axis_or_bad(c, query); if (a < 0) return;
        get_param(query, "on", v, sizeof(v));
        if (!app::motion::set_enabled(static_cast<uint8_t>(a), strcmp(v, "1") == 0, err)) { send_refusal(c, err); return; }
        send_ok(c, "driver\n"); return;
    }

    if (strcmp(path, "/api/side") == 0) {
        get_param(query, "v", v, sizeof(v));
        app::Side s;
        if (strcmp(v, "hi_z") == 0) s = app::Side::HiZ;
        else if (strcmp(v, "lo_z") == 0) s = app::Side::LoZ;
        else { send_bad(c, "bad side\n"); return; }
        if (!app::motion::set_side(s, err)) { send_refusal(c, err); return; }
        send_ok(c, "side\n"); return;
    }

    if (strcmp(path, "/api/bypass") == 0) {
        get_param(query, "on", v, sizeof(v));
        app::motion::set_bypass(strcmp(v, "1") == 0, err);
        send_ok(c, strcmp(v, "1") == 0 ? "BYPASS engaged - network out of circuit\n" : "network IN CIRCUIT\n"); return;
    }

    if (strcmp(path, "/api/home") == 0) {
        if (!app::motion::home(err)) { send_refusal(c, err); return; }
        send_ok(c, "homing every bound axis to 0\n"); return;
    }

    if (strcmp(path, "/api/topology") == 0) {
        app::TopologyKind kind;
        if (!get_param(query, "kind", v, sizeof(v)) || !app::parse_topology_kind(v, kind)) { send_bad(c, "bad kind\n"); return; }
        app::Topology t = kind == app::TopologyKind::BalancedL ? app::Topology::default_balanced_l()
                                                               : app::Topology::default_balanced_pi();
        for (uint8_t i = 0; i < t.n; i++) {
            if (get_param(query, t.elements[i].name, v, sizeof(v))) {
                const long ax = atol(v);
                if (ax < 0 || ax > 255) { send_bad(c, "bad axis in map\n"); return; }
                t.elements[i].axis = static_cast<uint8_t>(ax);
            }
        }
        if (!app::motion::set_topology(t, err)) { send_refusal(c, err); return; }
        send_ok(c, "topology set\n"); return;
    }

    if (strcmp(path, "/api/settings") == 0) {
        static char cfg_json[2048];
        if (get_param(query, "src", v, sizeof(v)) && strcmp(v, "card") == 0) {
            const int n = app::settings::read_card_copy(cfg_json, sizeof(cfg_json));
            if (n < 0) { send_text(c, 404, "Not Found", "no SD card or no /tuner/config.json\n"); return; }
            send_header(c, 200, "OK", "application/json", n);
            write_all(c, cfg_json, n); return;
        }
        app::Persisted p = app::settings::current();
        for (uint8_t a = 0; a < hal::kMaxAxes; a++) p.position[a] = snap.axes[a].steps;
        const int n = app::settings::to_json(p, cfg_json, sizeof(cfg_json));
        if (n <= 0) { send_text(c, 500, "Internal Server Error", "settings too large\n"); return; }
        send_header(c, 200, "OK", "application/json", n);
        write_all(c, cfg_json, n); return;
    }

    if (strcmp(path, "/api/settings_save") == 0) {
        if (!app::settings::status().sd_present) { send_text(c, 409, "Conflict", "no SD card\n"); return; }
        send_ok(c, app::settings::flush() ? "config.json written to the SD card\n" : "SD write FAILED\n"); return;
    }

    if (strcmp(path, "/api/fwd_w") == 0) {
        if (!get_param(query, "w", v, sizeof(v))) { send_bad(c, "missing w\n"); return; }
        app::motion::set_fwd_w_fake(strtof(v, nullptr), err);
        send_ok(c, "fwd_w injected\n"); return;
    }

    send_text(c, 404, "Not Found", "no such route\n");
}

// Read the request line; drain headers up to the blank line.
bool read_request(EthernetClient &c, char *line, size_t linesz) {
    const unsigned long start = millis();
    size_t i = 0;
    bool gotLF = false;
    while (c.connected() && (millis() - start) < 500) {
        if (!c.available()) { delay(1); continue; }
        const int b = c.read();
        if (b < 0) break;
        if (b == '\r') continue;
        if (b == '\n') { gotLF = true; break; }
        if (i + 1 < linesz) line[i++] = static_cast<char>(b);
    }
    line[i] = '\0';
    if (!gotLF) return false;
    int state = 1;
    while (c.connected() && (millis() - start) < 500) {
        if (!c.available()) { delay(1); continue; }
        const int b = c.read();
        if (b < 0) break;
        if (b == '\r') continue;
        if (b == '\n') { if (state == 1) return true; state = 1; }
        else state = 0;
    }
    return true;
}

} // namespace

void begin() {
    server.begin();
    ready = true;
}

void tick(const app::Snapshot &snap, int master_clients) {
    if (!ready) return;
    EthernetClient client = server.accept();
    if (!client) return;

    char line[256];
    if (!read_request(client, line, sizeof(line))) { client.stop(); return; }

    char *method = line;
    char *path = strchr(line, ' ');
    if (!path) { client.stop(); return; }
    *path++ = '\0';
    char *httpver = strchr(path, ' ');
    if (httpver) *httpver = '\0';
    char *query = strchr(path, '?');
    if (query) { *query++ = '\0'; } else { query = const_cast<char *>(""); }

    if (strcmp(method, "GET") != 0) send_text(client, 405, "Method Not Allowed", "GET only\n");
    else                            dispatch(client, path, query, snap, master_clients);

    // flush() before stop(): NativeEthernet's stop() discards anything
    // still in FNET's send buffer. Harmless on QNEthernet.
    client.flush();
    client.stop();
}

} // namespace http_server
