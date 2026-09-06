#pragma once

// A refusal code + human message returned by app-layer verbs that decline
// to act. Codes are stable strings shared by the master protocol
// (docs/PROTOCOL.md error codes) and the HTTP surface; messages are for
// people and may change.

namespace app {

struct Refusal {
    const char *code;   // "rf_lockout", "bad_axis", "not_anchored", "at_limit", ...
    const char *msg;
};

} // namespace app
