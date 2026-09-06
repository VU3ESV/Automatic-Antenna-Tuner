#include "tuner_server.h"

#include <Arduino.h>
#include <cstring>

#include "app/motion.h"
#include "app/protocol.h"

namespace tuner_server {

namespace {

// Single global server + connection table. EthernetServer/Client types
// are exposed by net_hal.h with the same name regardless of backend.
EthernetServer server(kListenPort);

struct Connection {
    EthernetClient client;
    bool           in_use;
    // Inbound line buffer. Cleared on each '\n'. Large enough for a
    // three-element set_topology frame.
    char           rx_buf[768];
    size_t         rx_len;
};

Connection conns[kMaxClients];

app::Snapshot   published;
app::Snapshot   last_sent;
bool            published_dirty   = false;
uint32_t        seq               = 0;
unsigned long   last_heartbeat_ms = 0;

uint32_t next_seq() {
    seq = seq + 1;
    if (seq == 0) seq = 1;  // wrap rule from PROTOCOL.md §4
    return seq;
}

// Write a complete JSON frame + '\n' to one client. Drops the frame
// silently if the socket would block; the slow-client drop is
// intentional per PROTOCOL.md §5. net_hal::write_all chunks below the
// NativeEthernet socket-buffer ceiling.
bool send_frame(EthernetClient &c, const char *frame, int n) {
    if (n <= 0 || !net_hal::write_all(c, frame, static_cast<size_t>(n))) return false;
    c.write('\n');
    return true;
}

void broadcast(const char *frame, int n) {
    for (auto &conn : conns) {
        if (conn.in_use) send_frame(conn.client, frame, n);
    }
}

void send_state_to(EthernetClient &c) {
    char frame[app::kFrameBufferSize];
    const int n = app::serialize_state(frame, sizeof(frame), next_seq(), published);
    send_frame(c, frame, n);
}

void send_state_broadcast() {
    char frame[app::kFrameBufferSize];
    const int n = app::serialize_state(frame, sizeof(frame), next_seq(), published);
    broadcast(frame, n);
    last_sent = published;
}

void send_heartbeat_broadcast() {
    char frame[128];
    const int n = app::serialize_heartbeat(frame, sizeof(frame), next_seq());
    broadcast(frame, n);
}

void send_ack_ok(EthernetClient &c, const char *id) {
    char ack[128];
    const int n = app::serialize_ack_ok(ack, sizeof(ack), id);
    send_frame(c, ack, n);
}

void send_ack_err(EthernetClient &c, const char *id, const char *code, const char *msg) {
    char ack[256];
    const int n = app::serialize_ack_err(ack, sizeof(ack), id, code, msg);
    send_frame(c, ack, n);
}

// Resolve an axis argument ("0".."2" or an element name) or send the
// bad_axis ack. Returns -1 after replying.
int resolve_or_reply(EthernetClient &c, const char *id, const char *axis_arg) {
    const int a = app::motion::resolve_axis(axis_arg);
    if (a < 0) send_ack_err(c, id, "bad_axis", "axis must be 0..2 or a bound element name");
    return a;
}

// Dispatch a single parsed command. Every verb routes through
// app::motion, which enforces the invariants; the protocol layer only
// decodes arguments and maps results onto acks.
void dispatch(Connection &conn, const char *line, size_t line_len, const app::InboundCommand &cmd) {
    EthernetClient &c = conn.client;
    app::motion::Refusal err = {nullptr, nullptr};
    auto reply = [&](bool ok) {
        if (ok) send_ack_ok(c, cmd.id);
        else    send_ack_err(c, cmd.id, err.code ? err.code : "refused", err.msg ? err.msg : "");
    };
    auto reply_move = [&](app::motion::MoveResult r) { reply(app::motion::accepted(r)); };
    auto bad_args = [&](const char *what) { send_ack_err(c, cmd.id, "bad_args", what); };

    const char *act = cmd.action;
    char axis[app::kAxisArgLen];

    if (strcmp(act, "noop") == 0)   { send_ack_ok(c, cmd.id); return; }
    if (strcmp(act, "resync") == 0) { send_ack_ok(c, cmd.id); send_state_to(c); return; }

    if (strcmp(act, "move_l") == 0 || strcmp(act, "move_c") == 0) {
        int32_t value = 0; bool is_delta = true;
        if (!app::parse_args_move(line, line_len, value, is_delta)) return bad_args("expected delta_steps or target_steps");
        reply(strcmp(act, "move_l") == 0 ? app::motion::move_l(value, is_delta, err)
                                         : app::motion::move_c(value, is_delta, err));
        return;
    }

    if (strcmp(act, "move_axis") == 0) {
        int32_t value = 0; bool is_delta = true;
        if (!app::parse_args_move_axis(line, line_len, axis, value, is_delta)) return bad_args("expected axis + delta_steps|target_steps");
        const int a = resolve_or_reply(c, cmd.id, axis);
        if (a >= 0) reply_move(app::motion::move_axis(static_cast<uint8_t>(a), value, is_delta, err));
        return;
    }

    if (strcmp(act, "run") == 0) {
        int dir = 0;
        if (!app::parse_args_run(line, line_len, axis, dir)) return bad_args("expected axis + dir: cw|ccw");
        const int a = resolve_or_reply(c, cmd.id, axis);
        if (a >= 0) reply_move(app::motion::run_to_end(static_cast<uint8_t>(a), dir, err));
        return;
    }

    if (strcmp(act, "stop") == 0) {
        if (!app::parse_args_axis(line, line_len, axis, /*required=*/false)) return bad_args("malformed args");
        if (axis[0] == '\0') { app::motion::stop_all(); send_ack_ok(c, cmd.id); return; }
        const int a = resolve_or_reply(c, cmd.id, axis);
        if (a >= 0) reply(app::motion::stop_axis(static_cast<uint8_t>(a), err));
        return;
    }

    if (strcmp(act, "estop") == 0 || strcmp(act, "estop_reset") == 0) {
        const bool reset = strcmp(act, "estop_reset") == 0;
        if (!app::parse_args_axis(line, line_len, axis, /*required=*/false)) return bad_args("malformed args");
        if (axis[0] == '\0') {
            if (reset) app::motion::estop_reset_all(); else app::motion::estop_all();
            send_ack_ok(c, cmd.id);
            return;
        }
        const int a = resolve_or_reply(c, cmd.id, axis);
        if (a < 0) return;
        reply(reset ? app::motion::estop_reset(static_cast<uint8_t>(a), err)
                    : app::motion::estop(static_cast<uint8_t>(a), err));
        return;
    }

    if (strcmp(act, "set_home") == 0 || strcmp(act, "unset_home") == 0) {
        if (!app::parse_args_axis(line, line_len, axis, /*required=*/true)) return bad_args("expected axis");
        const int a = resolve_or_reply(c, cmd.id, axis);
        if (a < 0) return;
        reply(strcmp(act, "set_home") == 0 ? app::motion::set_home(static_cast<uint8_t>(a), err)
                                           : app::motion::unset_home(static_cast<uint8_t>(a), err));
        return;
    }

    if (strcmp(act, "set_element") == 0) {
        app::ElementKind kind; float max_rev = 0.0f;
        if (!app::parse_args_set_element(line, line_len, axis, kind, max_rev)) return bad_args("expected axis, kind, max_rev");
        const int a = resolve_or_reply(c, cmd.id, axis);
        if (a >= 0) reply(app::motion::set_element(static_cast<uint8_t>(a), kind, max_rev, err));
        return;
    }

    if (strcmp(act, "set_speed") == 0) {
        uint32_t speed = 0, accel = 0;
        if (!app::parse_args_set_speed(line, line_len, axis, speed, accel)) return bad_args("expected axis, speed[, accel]");
        const int a = resolve_or_reply(c, cmd.id, axis);
        if (a < 0) return;
        if (accel == 0) accel = app::motion::axis_config(static_cast<uint8_t>(a)).accel;
        reply(app::motion::set_speed(static_cast<uint8_t>(a), speed, accel, err));
        return;
    }

    if (strcmp(act, "set_enabled") == 0) {
        bool on = true;
        if (!app::parse_args_set_enabled(line, line_len, axis, on)) return bad_args("expected axis, on");
        const int a = resolve_or_reply(c, cmd.id, axis);
        if (a >= 0) reply(app::motion::set_enabled(static_cast<uint8_t>(a), on, err));
        return;
    }

    if (strcmp(act, "set_topology") == 0) {
        app::Topology t;
        if (!app::parse_args_set_topology(line, line_len, t)) return bad_args("expected kind + elements[{name,type,axis,pair}]");
        reply(app::motion::set_topology(t, err));
        return;
    }

    if (strcmp(act, "set_side") == 0) {
        app::Side s;
        if (!app::parse_args_set_side(line, line_len, s)) return bad_args("expected side: hi_z|lo_z");
        reply(app::motion::set_side(s, err));
        return;
    }

    if (strcmp(act, "set_bypass") == 0) {
        bool on = true;
        if (!app::parse_args_set_bypass(line, line_len, on)) return bad_args("expected on: bool");
        reply(app::motion::set_bypass(on, err));
        return;
    }

    if (strcmp(act, "home") == 0) { reply(app::motion::home(err)); return; }

    if (strcmp(act, "set_fwd_w") == 0) {
        float w = 0.0f;
        if (!app::parse_args_set_fwd_w(line, line_len, w)) return bad_args("expected w: number");
        reply(app::motion::set_fwd_w_fake(w, err));
        return;
    }

    send_ack_err(c, cmd.id, "unknown_action", cmd.action);
}

// Drain pending bytes from one client; on every '\n' parse + dispatch.
void read_one(Connection &conn) {
    while (conn.client.connected() && conn.client.available() > 0) {
        const int byte = conn.client.read();
        if (byte < 0) break;
        if (byte == '\r') continue;
        if (byte == '\n') {
            if (conn.rx_len > 0) {
                conn.rx_buf[conn.rx_len] = '\0';
                app::InboundCommand cmd;
                if (app::parse_command(conn.rx_buf, conn.rx_len, cmd)) {
                    dispatch(conn, conn.rx_buf, conn.rx_len, cmd);
                } else {
                    send_ack_err(conn.client, "", "bad_args", "malformed command");
                }
                conn.rx_len = 0;
            }
            continue;
        }
        if (conn.rx_len + 1 >= sizeof(conn.rx_buf)) {
            conn.rx_len = 0;   // frame overflow — discard
            continue;
        }
        conn.rx_buf[conn.rx_len++] = static_cast<char>(byte);
    }
}

// Reap closed connections and accept new ones.
void manage_connections() {
    for (auto &conn : conns) {
        if (conn.in_use && !conn.client.connected()) {
            conn.client.stop();
            conn.in_use = false;
            conn.rx_len = 0;
        }
    }
    EthernetClient newClient = server.accept();
    if (!newClient) return;
    for (auto &conn : conns) {
        if (!conn.in_use) {
            conn.client = newClient;
            conn.in_use = true;
            conn.rx_len = 0;
            send_state_to(conn.client);   // warm start
            return;
        }
    }
    newClient.stop();   // table full
}

} // namespace

void begin() {
    server.begin();
    for (auto &conn : conns) {
        conn.in_use = false;
        conn.rx_len = 0;
    }
    last_heartbeat_ms = millis();
}

void publish(const app::Snapshot &snap) {
    if (published.differs(snap)) {
        published       = snap;
        published_dirty = true;
    }
}

void tick() {
    manage_connections();
    for (auto &conn : conns) {
        if (conn.in_use) read_one(conn);
    }

    if (published_dirty) {
        send_state_broadcast();
        published_dirty   = false;
        last_heartbeat_ms = millis();
    }

    const unsigned long now = millis();
    if ((now - last_heartbeat_ms) >= kHeartbeatPeriodMs) {
        last_heartbeat_ms = now;
        send_heartbeat_broadcast();
    }
}

int connected_clients() {
    int n = 0;
    for (auto &conn : conns) {
        if (conn.in_use && conn.client.connected()) ++n;
    }
    return n;
}

} // namespace tuner_server
