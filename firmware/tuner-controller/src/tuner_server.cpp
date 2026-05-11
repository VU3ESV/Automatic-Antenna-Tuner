#include "tuner_server.h"

#include <Arduino.h>
#include <cstring>

#include "app/protocol.h"

namespace tuner_server {

namespace {

// Single global server + connection table. EthernetServer/Client types
// are exposed by net_hal.h with the same name regardless of backend.
EthernetServer server(kListenPort);

struct Connection {
    EthernetClient client;
    bool           in_use;
    // Inbound line buffer. Cleared on each '\n'.
    char           rx_buf[512];
    size_t         rx_len;
    // Whether we've sent the warm-start state frame on this connection yet.
    bool           sent_initial_state;
};

Connection conns[kMaxClients];

app::Snapshot   published;
app::Snapshot   last_sent;
bool            published_dirty = false;
uint32_t        seq             = 0;
unsigned long   last_heartbeat_ms = 0;

uint32_t next_seq() {
    seq = seq + 1;
    if (seq == 0) seq = 1;  // wrap rule from PROTOCOL.md §4
    return seq;
}

// Write a complete JSON frame + '\n' to one client. Returns true on
// success. Drops the frame silently if the underlying socket would
// block; the slow-client drop is intentional per PROTOCOL.md §5.
bool send_frame(EthernetClient &c, const char *frame, int n) {
    if (!c.connected()) return false;
    if (n <= 0) return false;
    c.write(reinterpret_cast<const uint8_t *>(frame), n);
    c.write('\n');
    return true;
}

void broadcast(const char *frame, int n) {
    for (auto &conn : conns) {
        if (conn.in_use) {
            send_frame(conn.client, frame, n);
        }
    }
}

// Send the current published snapshot as a `state` frame to one client.
void send_state_to(EthernetClient &c) {
    char frame[app::kFrameBufferSize];
    int n = app::serialize_state(frame, sizeof(frame), next_seq(), published);
    send_frame(c, frame, n);
}

void send_state_broadcast() {
    char frame[app::kFrameBufferSize];
    int n = app::serialize_state(frame, sizeof(frame), next_seq(), published);
    broadcast(frame, n);
    last_sent = published;
}

void send_heartbeat_broadcast() {
    char frame[128];
    int n = app::serialize_heartbeat(frame, sizeof(frame), next_seq());
    broadcast(frame, n);
}

// Dispatch a single parsed command. M1b accepts only the safe verbs;
// motion / relay verbs return `ack ok:false` with `unknown_action` until
// the HAL layers are wired.
void dispatch(Connection &conn, const app::InboundCommand &cmd) {
    char ack[256];
    int  n = 0;

    if (strcmp(cmd.action, "noop") == 0) {
        n = app::serialize_ack_ok(ack, sizeof(ack), cmd.id);
    } else if (strcmp(cmd.action, "resync") == 0) {
        n = app::serialize_ack_ok(ack, sizeof(ack), cmd.id);
        send_frame(conn.client, ack, n);
        // Re-emit state immediately after the ack.
        send_state_to(conn.client);
        return;
    } else if (strcmp(cmd.action, "set_bypass") == 0 ||
               strcmp(cmd.action, "move_l")     == 0 ||
               strcmp(cmd.action, "move_c")     == 0 ||
               strcmp(cmd.action, "set_side")   == 0 ||
               strcmp(cmd.action, "home")       == 0) {
        // Verb is known but hardware is not wired yet (M1b firmware
        // bring-up still has stepper/relay HAL pending).
        n = app::serialize_ack_err(ack, sizeof(ack), cmd.id,
                                   "unknown_action",
                                   "verb known but hardware not wired (M1b in progress)");
    } else {
        n = app::serialize_ack_err(ack, sizeof(ack), cmd.id,
                                   "unknown_action",
                                   cmd.action);
    }
    send_frame(conn.client, ack, n);
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
                    dispatch(conn, cmd);
                } else {
                    char ack[200];
                    int n = app::serialize_ack_err(ack, sizeof(ack), "",
                                                   "bad_args",
                                                   "malformed command");
                    send_frame(conn.client, ack, n);
                }
                conn.rx_len = 0;
            }
            continue;
        }
        if (conn.rx_len + 1 >= sizeof(conn.rx_buf)) {
            // Frame overflow — discard.
            conn.rx_len = 0;
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
            conn.sent_initial_state = false;
        }
    }
    EthernetClient newClient = server.accept();
    if (!newClient) return;
    for (auto &conn : conns) {
        if (!conn.in_use) {
            conn.client             = newClient;
            conn.in_use             = true;
            conn.rx_len             = 0;
            conn.sent_initial_state = false;
            // Send initial state immediately.
            send_state_to(conn.client);
            conn.sent_initial_state = true;
            return;
        }
    }
    // Table full — politely close the new connection.
    newClient.stop();
}

} // namespace

void begin() {
    server.begin();
    for (auto &conn : conns) {
        conn.in_use             = false;
        conn.rx_len             = 0;
        conn.sent_initial_state = false;
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
        if (conn.in_use) {
            read_one(conn);
        }
    }

    if (published_dirty) {
        send_state_broadcast();
        published_dirty   = false;
        last_heartbeat_ms = millis();  // reset HB timer; state counts as a frame
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
        // `connected()` isn't const in either backend's EthernetClient.
        if (conn.in_use && conn.client.connected()) ++n;
    }
    return n;
}

} // namespace tuner_server
