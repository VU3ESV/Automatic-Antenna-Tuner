#pragma once

// Wire-protocol serializers for the tuner controller.
//
// One function per outbound frame type. Each takes a destination buffer
// + size and returns the number of bytes written (not including a
// trailing '\n' which the caller appends). Buffers should be at least
// 256 bytes for state/telemetry frames; 96 is enough for heartbeat.
//
// The JSON shape matches docs/PROTOCOL.md. The transport layer
// (tuner_server) is responsible for newline-framing and writing to a
// TCP client.

#include <cstddef>
#include <cstdint>

#include "app/state.h"

namespace app {

// Maximum reasonable size of any single outbound frame.
constexpr size_t kFrameBufferSize = 512;

// Returns bytes written, or -1 on serialization failure.
int serialize_heartbeat(char *out, size_t out_size, uint32_t seq);

// Returns bytes written. Includes the full `state` frame body with
// data sub-object.
int serialize_state(char *out, size_t out_size, uint32_t seq,
                    const Snapshot &snap);

// Returns bytes written. Status frame with level + code + msg.
int serialize_status(char *out, size_t out_size, uint32_t seq,
                     const char *level, const char *code, const char *msg);

// Returns bytes written. Positive ack for a command identified by ref.
int serialize_ack_ok(char *out, size_t out_size, const char *ref);

// Returns bytes written. Negative ack with stable error code + free-form msg.
int serialize_ack_err(char *out, size_t out_size, const char *ref,
                      const char *code, const char *msg);

// Inbound: parse a single line of JSON into a Command structure. Returns
// false if the line is malformed or not a command frame.
struct InboundCommand {
    char id[40]    = {0};
    char action[24] = {0};
    // args is left as a raw substring within the input buffer; not parsed
    // here. Per-verb dispatch handles its own argument extraction.
    const char *args_json = nullptr;
    size_t      args_len  = 0;
};

bool parse_command(const char *line, size_t line_len, InboundCommand &out);

} // namespace app
