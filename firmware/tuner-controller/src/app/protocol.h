#pragma once

// Wire-protocol serializers / parsers for the master link
// (docs/PROTOCOL.md). One function per outbound frame type; each takes a
// destination buffer + size and returns the number of bytes written (not
// including the trailing '\n' the transport appends).
//
// Inbound: parse_command() extracts the envelope; the per-verb decoders
// re-parse the line (cheap — frames are < 1 KiB) and pull only the args
// their verb needs. Axis arguments are returned as text ("0".."2" or an
// element name such as "C1") and resolved by the dispatcher through
// app::motion::resolve_axis(), which knows the topology.

#include <cstddef>
#include <cstdint>

#include "app/config.h"
#include "app/state.h"

namespace app {

// Maximum reasonable size of any single outbound frame. The `state`
// frame carries three axes plus the legacy L/C fields.
constexpr size_t kFrameBufferSize = 1536;

int serialize_heartbeat(char *out, size_t out_size, uint32_t seq);
int serialize_state(char *out, size_t out_size, uint32_t seq, const Snapshot &snap);
int serialize_status(char *out, size_t out_size, uint32_t seq,
                     const char *level, const char *code, const char *msg);
int serialize_ack_ok(char *out, size_t out_size, const char *ref);
int serialize_ack_err(char *out, size_t out_size, const char *ref,
                      const char *code, const char *msg);

struct InboundCommand {
    char id[40]     = {0};
    char action[24] = {0};
};

bool parse_command(const char *line, size_t line_len, InboundCommand &out);

constexpr size_t kAxisArgLen = 8;

// move_l / move_c — "delta_steps" (is_delta=true) or "target_steps".
bool parse_args_move(const char *line, size_t line_len, int32_t &value, bool &is_delta);

// move_axis — {"axis": 0|"L", "delta_steps"|"target_steps": int}.
bool parse_args_move_axis(const char *line, size_t line_len,
                          char axis[kAxisArgLen], int32_t &value, bool &is_delta);

// run — {"axis": .., "dir": "cw"|"ccw"|1|-1}.
bool parse_args_run(const char *line, size_t line_len, char axis[kAxisArgLen], int &dir);

// stop / estop / estop_reset / set_home / unset_home — {"axis": ..}. When
// `required` is false a missing axis is accepted (axis[0] == '\0' = "all").
bool parse_args_axis(const char *line, size_t line_len, char axis[kAxisArgLen], bool required);

// set_element — {"axis": .., "kind": "vacuum_cap"|2, "max_rev": 40}.
bool parse_args_set_element(const char *line, size_t line_len,
                            char axis[kAxisArgLen], ElementKind &kind, float &max_rev);

// set_speed — {"axis": .., "speed": steps/s, "accel"?: steps/s²}. accel = 0 → keep.
bool parse_args_set_speed(const char *line, size_t line_len,
                          char axis[kAxisArgLen], uint32_t &speed, uint32_t &accel);

// set_enabled — {"axis": .., "on": bool}.
bool parse_args_set_enabled(const char *line, size_t line_len, char axis[kAxisArgLen], bool &on);

// set_topology — {"kind": "balanced_l", "elements": [{"name","type","axis","pair"}]}.
// Structural parse only; app::validate_topology() does the semantic check.
bool parse_args_set_topology(const char *line, size_t line_len, Topology &out);

bool parse_args_set_side(const char *line, size_t line_len, Side &out);
bool parse_args_set_bypass(const char *line, size_t line_len, bool &out);
bool parse_args_set_fwd_w(const char *line, size_t line_len, float &out);

} // namespace app
