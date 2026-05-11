#pragma once

// Tuner-controller TCP server.
//
// Listens on a single TCP port (default 8089). Each accepted client
// receives:
//   - a `state` frame immediately on connect (warm start)
//   - a `state` frame whenever the snapshot mutates beyond its deadbands
//   - a `heartbeat` frame every kHeartbeatPeriodMs when nothing else was sent
//   - `ack` frames in reply to inbound `command` frames
//
// Inbound: one JSON object per line, '\n' terminated. M1b interim
// transport per docs/PROTOCOL.md §1.0.

#include <cstdint>

#include "app/state.h"
#include "net_hal.h"

namespace tuner_server {

constexpr uint16_t kListenPort        = 8089;
constexpr uint32_t kHeartbeatPeriodMs = 2000;
constexpr int      kMaxClients        = 4;

// Initialise the listening socket. Must be called after Ethernet is up.
void begin();

// Replace the published snapshot. Subscribers are notified via the
// poll loop (i.e., on the next tick() call).
void publish(const app::Snapshot &snap);

// Drive the server loop. Call from the main loop() on every iteration.
// Handles accept(), per-client read/parse/dispatch, heartbeat ticking,
// and snapshot-change broadcasts.
void tick();

// Returns current count of connected clients (0 if no one is watching).
int connected_clients();

} // namespace tuner_server
