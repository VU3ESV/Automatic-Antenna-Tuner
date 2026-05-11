# Wire protocol

JSON, one endpoint per process, named verbs only. Same payload shape on
both hops:

```
Browser  ── WebSocket /ws ─►  Master  ── TCP :8089 (line-JSON) ─►  Tuner Controller
                            (Go, Pi)                              (Teensy / STM32)
```

The master is the boundary. Each hop independently honours the rules
below — there's no "raw passthrough" mode. The master synthesises some
frames (memory, qrg) that the controller doesn't know about and filters
some verbs (save, recall) that the controller doesn't accept directly.

This document is the source of truth. Anything in `CLAUDE.md` is a
summary of what's here.

## 1.0 Transport

Two hops, two transports, **identical JSON payloads**:

| Hop                       | Transport              | Framing                                                   |
|---------------------------|------------------------|-----------------------------------------------------------|
| Browser ↔ Master           | WebSocket on `/ws`     | RFC 6455 text frames. One protocol frame per WS frame.    |
| Master ↔ Controller        | Plain TCP on port 8089 | One protocol frame per line, terminated by `\n`. No null bytes inside frames. |

The controller-side transport is plain TCP for M1b because the WS
handshake (SHA1 + Base64 + RFC 6455 framing) is non-trivial work on
the Teensy and the master↔controller hop is internal LAN. A future
milestone (M6 hardening) may upgrade it to true WebSocket for uniformity;
the JSON payloads do not change — only the framing does. Clients that
need to talk directly to the controller (debug tools, `nc`, `socat`)
should use the TCP transport.

The controller listens on a single TCP port (`8089` by default,
configurable on the master via `[tuner].port`). The master is the only
expected client; the controller currently accepts up to 4 concurrent
clients for debug-tool convenience.

## 1. Framing

- Browser↔Master: one WebSocket text frame per protocol frame. No
  fragmentation, no binary frames at the protocol layer.
- Master↔Controller: one protocol frame per `\n`-terminated line. No
  embedded newlines in JSON values; the controller's serializer escapes
  them as `\\n`.
- UTF-8 JSON. Compact (no pretty-printing) on the wire.
- Maximum frame size 64 KiB; anything larger is a protocol error.
- Field names are `snake_case`. Numbers are JSON numbers (no string
  encoding of floats). Timestamps are ISO-8601 with `Z` suffix.

Every frame has a `type` discriminator. Server→client frames also
carry a `seq` (monotonic uint32, wraps at 2³²−1 → 0) and `ts` (server
clock). Client→server frames carry an optional `id` (echoed back in
`ack`).

## 2. Server → client frames

### 2.1 `telemetry`

Live RF measurements. Sent at most ~30 Hz, only when any field has
changed beyond its deadband.

```json
{
  "type": "telemetry",
  "seq": 12345,
  "ts": "2026-05-11T09:14:22.103Z",
  "data": {
    "fwd_w": 5.0,
    "rev_w": 0.04,
    "swr": 1.13,
    "z_mag": 56.2,
    "z_phase": -8.4,
    "r": 55.6,
    "x": -8.2,
    "mode": "rx"
  }
}
```

| Field      | Type    | Units    | Notes                                              |
|------------|---------|----------|----------------------------------------------------|
| `fwd_w`    | float   | watts    | Forward power at tuner input port.                 |
| `rev_w`    | float   | watts    | Reverse power.                                     |
| `swr`      | float   | —        | Voltage SWR; ∞ encoded as `null`.                  |
| `z_mag`    | float   | ohms     | \|Z\| at tuner input port.                         |
| `z_phase`  | float   | degrees  | ∠Z; ±180°.                                         |
| `r`        | float   | ohms     | Re(Z).                                             |
| `x`        | float   | ohms     | Im(Z).                                             |
| `mode`     | enum    | —        | `"rx"` \| `"tx"` \| `"tune"` — derived from `fwd_w`.|

Deadbands: `fwd_w`/`rev_w` 0.5 W, `swr` 0.01, `z_mag` 0.5 Ω,
`z_phase` 0.5°, `r`/`x` 0.5 Ω.

### 2.2 `state`

Mechanical and relay state. Emitted after every state-changing event
(move complete, relay flip, bypass change) and on every new client
connect.

```json
{
  "type": "state",
  "seq": 12346,
  "ts": "2026-05-11T09:14:22.103Z",
  "data": {
    "l_steps": 18432,
    "c_steps": 9216,
    "l_enc": 18434,
    "c_enc": 9214,
    "side": "hi_z",
    "bypass": false,
    "last_move": "2026-05-11T09:14:21.998Z",
    "moving": false,
    "homed": true
  }
}
```

| Field        | Type    | Notes                                                      |
|--------------|---------|------------------------------------------------------------|
| `l_steps`    | uint32  | Commanded L position (microsteps from home).               |
| `c_steps`    | uint32  | Commanded C position.                                      |
| `l_enc`      | int32   | Measured L position (encoder counts). Source of truth.     |
| `c_enc`      | int32   | Measured C position.                                       |
| `side`       | enum    | `"hi_z"` \| `"lo_z"`.                                      |
| `bypass`     | bool    | K3 engaged.                                                |
| `last_move`  | string  | ISO-8601 of last completed motion.                         |
| `moving`     | bool    | True while a stepper is in motion.                         |
| `homed`      | bool    | False until `home` has run since power-up.                 |

### 2.3 `memory`

Result of a `save` / `recall` / memory-table-edit. Emitted by the
master only (the controller has no memory).

```json
{
  "type": "memory",
  "seq": 12347,
  "ts": "2026-05-11T09:14:22.103Z",
  "data": {
    "op": "recall",
    "band": "20m",
    "freq_hz": 14175000,
    "bucket_hz": 50000,
    "slot": {
      "l_steps": 18432,
      "c_steps": 9216,
      "side": "hi_z",
      "swr_at_save": 1.08,
      "saved_at": "2026-04-30T15:22:10Z",
      "label": "phone net"
    },
    "found": true
  }
}
```

`op` is one of `recall` | `save` | `delete` | `list`. `slot` is null
when `found` is false.

### 2.4 `qrg`

Transceiver frequency, polled via CAT. Master-only.

```json
{
  "type": "qrg",
  "seq": 12348,
  "ts": "2026-05-11T09:14:22.103Z",
  "data": { "freq_hz": 14175000, "mode": "USB", "rig": "icom-civ", "stale": false }
}
```

`stale` is `true` if the master hasn't heard from the CAT link in
`cat.poll_ms × 5`. Auto-recall is disabled when stale.

### 2.5 `status`

Free-form events: warnings, errors, lockouts, reconnects. Both master
and controller emit these.

```json
{
  "type": "status",
  "seq": 12349,
  "ts": "2026-05-11T09:14:22.103Z",
  "level": "warn",
  "code": "rf_lockout",
  "msg": "motion refused: forward power 12.4 W exceeds 5 W lockout"
}
```

`level` is `info` | `warn` | `error`. `code` is a stable enum:

| Code            | Meaning                                                 |
|-----------------|---------------------------------------------------------|
| `rf_lockout`    | Motion refused while RF present.                        |
| `enc_resync`    | Encoder count diverged from step counter; re-synced.    |
| `stall`         | StallGuard triggered during motion.                     |
| `relay_fault`   | K1+K2 closed simultaneously, or readback mismatch.      |
| `link_lost`     | Master lost WS to controller (or vice versa).           |
| `cat_stale`     | CAT poll timed out.                                     |
| `cal_missing`   | Per-axis calibration absent; auto-tune disabled.        |
| `ws_overrun`    | Client too slow; frames dropped.                        |

**Warm-start status.** When a new WS client connects, the master
synthesises a `status` frame carrying the current controller-link
state (`connected` / `connecting` / `disconnected`) in `msg`. This is
how a browser that opens after the controller is already connected
learns to render its pill green — without it, the WS subscriber would
wait forever for a non-existent transition. The synthesised frame's
`level` is `info` and its `code` is omitted.

### 2.6 `heartbeat`

Sent every `heartbeat_ms` when no other server→client frame would be
sent. Lets clients detect a dead link before TCP keepalives would.

```json
{ "type": "heartbeat", "seq": 12350, "ts": "2026-05-11T09:14:22.103Z" }
```

### 2.7 `ack`

Reply to a client command. Always carries the originating `id` if the
client provided one.

```json
{
  "type": "ack",
  "ref": "abc-7",
  "ok": true
}
```

On failure:

```json
{
  "type": "ack",
  "ref": "abc-7",
  "ok": false,
  "err": { "code": "rf_lockout", "msg": "forward power 12 W" }
}
```

`code` reuses the `status.code` enum so clients only need one error
table.

## 3. Client → server frames

All client frames share this envelope:

```json
{ "type": "command", "id": "abc-7", "action": "<verb>", "args": { ... } }
```

`id` is optional but recommended — without it, the server still sends
an `ack`, but with `ref: null`.

| `action`     | `args`                                            | Hop       | Notes                                                |
|--------------|---------------------------------------------------|-----------|------------------------------------------------------|
| `move_l`     | `{ "delta_steps": int }` *or* `{ "target_steps": uint }` | controller| Refused if RF present or `bypass:false`.       |
| `move_c`     | same shape                                        | controller| Same rules.                                           |
| `set_side`   | `{ "side": "hi_z" \| "lo_z" }`                     | controller| Refused if RF present. Auto-engages bypass first.    |
| `set_bypass` | `{ "bypass": bool }` *or* `{ "on": bool }`         | controller| The only relay verb accepted while RF is present.    |
| `recall`     | `{ "freq_hz": uint }`                             | master    | Master expands into a sequence of controller verbs.  |
| `save`       | `{ "freq_hz": uint, "label"?: string }`           | master    | Persists current `state` for `(band, bucket)`.       |
| `auto_tune`  | `{ "freq_hz": uint, "power_w": float }`           | master    | Master orchestrates analytic + hill-climb.           |
| `home`       | `{}`                                              | controller| Drives both axes to mechanical home.                 |
| `resync`     | `{}`                                              | either    | Server re-emits current `state` + `telemetry`.       |
| `noop`       | `{}`                                              | either    | Connection check; `ack ok:true` only.                |
| `set_fwd_w`  | `{ "w": number }`                                 | controller| **Debug.** Injects a synthetic Fwd power reading so the RF-lockout path can be exercised without keying a transmitter. Replaced by real ADC samples once the AD8307 chain lands in M2. |

"Hop" indicates where the verb is *terminated*. Browsers send all
verbs to the master; the master forwards controller-bound verbs and
handles master-bound verbs locally.

## 4. Sequence numbering

- `seq` starts at 1 on each connection (it is *not* persisted across
  reconnects). Clients use it only to detect drops within a single
  session — `seq + 1` always follows `seq` from the same server.
- The master's `seq` to its browsers is *not* the same as the
  controller's `seq` to the master. Each WS hop has an independent
  counter.

## 5. Error handling

| Situation                                    | Behaviour                                                                    |
|---------------------------------------------|-------------------------------------------------------------------------------|
| Malformed JSON                              | Close WS with code 1003 (unsupported data).                                  |
| Unknown `type` on server→client             | Client logs + ignores; never close.                                          |
| Unknown `action` on client→server           | `ack ok:false` with code `unknown_action`; never close.                      |
| Verb args fail validation                   | `ack ok:false` with code `bad_args`.                                         |
| Verb refused due to invariant (RF lockout)  | `ack ok:false` with the relevant `code` (e.g. `rf_lockout`).                 |
| Server overloaded / client too slow         | `status code:ws_overrun`, close with code 1013 (try again later).            |
| Heartbeat absent for 3× `heartbeat_ms`      | Either side closes; client-side reconnect loop kicks in (1 s→30 s backoff). |

## 6. Reserved / out of scope

- **Binary frames.** Reserved for future ADC IQ-capture streams (M6+).
  At v1, any binary frame is a protocol error.
- **Backpressure beyond drop-and-warn.** No flow control protocol;
  slow clients are disconnected, not throttled.
- **Encryption / auth.** None. LAN-only, documented per CLAUDE.md.

## 7. Versioning

This is v1. Any breaking change increments to v2 in a parallel doc;
the WS subprotocol header (`Sec-WebSocket-Protocol`) is the version
selector once v2 ever ships. v1 negotiates no subprotocol — absence of
the header means v1.
