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

**Bring-up control surface (since 2026-09-06).** The controller also
serves a browser page plus GET verbs on HTTP port 80
(`firmware/tuner-controller/src/http_server.h` lists the routes). It is
the initial means of operating the real tuner before the master is
deployed. Every HTTP verb calls the same `app::motion` entry point as
the corresponding TCP verb, so the two surfaces cannot disagree; the
master remains the long-term operator UI.

**Firmware update over Ethernet (since 2026-09-06).** HTTP only — there
is no firmware verb on the master link. `POST /api/firmware` with the
Intel-HEX file as the body (`Content-Length` required; `Expect:
100-continue` honoured) streams the records into the controller, which
stages the image in free flash above the running program and replies
`200` with `key=value` lines — `lines` (records consumed, EOF included),
`bytes`, `range=0xMIN-0xMAX`, `crc32` (CRC-32 of the staged image, gaps
as `FF`), `target`, and the apply command — or `400 <code>: <msg>` when
the file is rejected (`bad_hex` — malformed, checksum, record after
EOF, descending or overlapping data records, `bad_image` — not at the
flash base or below it, `too_big`, `wrong_target` — the image lacks the
target id `fw_aat_teensy41`, `flash_write`, `flash_read`), `409` when it
cannot start (`moving`, `busy`, `no_buffer`, `unsupported`), `408` when
the client stalls. `GET /api/firmware_apply?lines=N` reboots into the
staged image when `N` equals the staged record count; refused with
`not_staged`, `bad_args` (also `400` when `lines` is missing),
`not_bypassed`, `moving` or `rf_lockout`. The copy runs a few hundred ms
later and re-checks the same gates first: if one fails the state drops
back to `staged` with `code` `apply_aborted` and the request must be
repeated. While the apply is pending every motion and relay verb (other
than engaging bypass) is refused with `updating`. The copy itself takes
several seconds (erase-dominated) — do not power-cycle. `GET
/api/firmware_abort` discards the staged image; refused with `moving`
(the erase masks interrupts) or `busy` once an apply is pending. `GET
/api/firmware` returns the status object that `/api/status` also carries
as `ota` (`state` idle / receiving / staged / applying / error,
`supported`, `target`, `lines`, `bytes`, `capacity`, `crc32`, `min`,
`max`, `code`, `msg`); `/api/status` also carries `build` (`stamp`,
`git`, `env`) so an update can be seen to have landed. Client: `firmware/tuner-controller/tools/ota_upload.py`, used by
the `teensy41_ota` / `teensy41_native_ota` PlatformIO environments.
Rules in CLAUDE.md "Firmware update over Ethernet".

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
    "homed": true,
    "topology": "balanced_l",
    "rf_lockout": false,
    "estop_all": false,
    "fwd_w": 0.0,
    "sd_present": true,
    "sd_ok": true,
    "settings_source": "sd",
    "axes": [
      {"axis": 0, "name": "L", "type": "L", "pair": true, "steps": 18432, "enc": 18432,
       "moving": false, "enabled": true, "limit_sw": false, "kind": "inductor",
       "home_set": true, "anchored": true, "max_rev": 30.0, "max_steps": 192000,
       "travel": "in_range", "last_clamp": "none", "estop": false, "speed": 6400, "accel": 25600},
      {"axis": 1, "name": "C", "type": "C", "pair": false, "steps": 9216, "enc": 9216,
       "moving": false, "enabled": true, "limit_sw": false, "kind": "vacuum_cap",
       "home_set": true, "anchored": true, "max_rev": 40.0, "max_steps": 256000,
       "travel": "in_range", "last_clamp": "none", "speed": 6400, "accel": 25600},
      {"axis": 2, "steps": 0, "enc": 0, "moving": false, "enabled": true, "limit_sw": false,
       "kind": "unset", "home_set": false, "anchored": false, "max_rev": 0, "max_steps": 0,
       "travel": "unlimited", "last_clamp": "none", "speed": 800, "accel": 25600}
    ]
  }
}
```

| Field        | Type    | Notes                                                      |
|--------------|---------|------------------------------------------------------------|
| `l_steps`    | uint32  | **v1 compatibility.** Position of the element named `L` (microsteps from home). |
| `c_steps`    | uint32  | **v1 compatibility.** Position of element `C` (Balanced L) or `C1` (Balanced Pi). |
| `l_enc`      | int32   | Measured L position (encoder counts / drive step counter). |
| `c_enc`      | int32   | Measured C position.                                       |
| `side`       | enum    | `"hi_z"` \| `"lo_z"`. Meaningful on Balanced L only.       |
| `bypass`     | bool    | K3 in the bypass position (network out of circuit).        |
| `last_move`  | string  | ISO-8601 of last completed motion.                         |
| `moving`     | bool    | True while any stepper is in motion.                       |
| `homed`      | bool    | True when every topology-bound axis is **anchored** (home declared and last shutdown clean — invariant 3). |
| `topology`   | enum    | `"balanced_l"` \| `"balanced_pi"` — the declared network (`set_topology`). |
| `rf_lockout` | bool    | Forward power above `tx_lockout_w`; motion verbs are being refused. |
| `estop_all`  | bool    | Every axis has its E-stop alarm latched (what E-STOP ALL produces). Releasing any one axis clears it. |
| `sd_present` | bool    | microSD card mounted.                                       |
| `sd_ok`      | bool    | `/tuner/config.json` was read or written successfully.      |
| `settings_source` | enum | `"sd"` \| `"eeprom"` \| `"defaults"` — where the running settings came from at boot. Settings persist to EEPROM synchronously and to the card (debounced); the card wins for settings at boot, EEPROM always wins for position anchors. |
| `fwd_w`      | number  | Latest forward-power reading (fake-injectable until the AD8307 chain lands). |
| `axes[]`     | array   | One entry per carrier stepper channel (0 = X, 1 = Y, 2 = Z), in axis order. |

Per-axis fields (`axes[]`):

| Field        | Type    | Notes                                                      |
|--------------|---------|------------------------------------------------------------|
| `axis`       | uint8   | Carrier channel index.                                     |
| `name` / `type` / `pair` | string / enum / bool | Present only when the topology binds an element to this axis: element name (`L`, `C`, `C1`, `C2`), `"L"` \| `"C"`, inductor-pair flag. |
| `steps`, `enc` | int32 | Step counter and position source of record.                 |
| `moving`, `enabled`, `limit_sw` | bool | Pulse train active; driver ENA on; end-stop opto input asserted. |
| `kind`       | enum    | Element hardware: `unset`, `inductor`, `vacuum_cap`, `varcap_limited`, `varcap_free`, `variometer`. The first three have mechanical stops and get a travel window. |
| `home_set`, `anchored` | bool | Operator declared home since the kind was set; position trusted (home declared **and** the last power-down happened with no move in flight). |
| `max_rev`, `max_steps` | number, int32 | Rated travel in element revolutions and the derived window upper bound (0 = no window). |
| `travel`     | enum    | `unlimited`, `unhomed`, `below_home`, `home`, `in_range`, `max`, `above_max`. |
| `last_clamp` | enum    | `none`, `home`, `max` — which window bound trimmed the most recent motion verb (the UI's "STOPPED AT MAX" latch). |
| `estop`      | bool    | Emergency-stop alarm latched on this axis (per-axis `estop`, or `estop_all`). The browser page shows it as a flashing beacon. |
| `speed`, `accel` | uint32 | Cruise rate (steps/s) and ramp (steps/s²), persisted per axis. |

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
| `move_l`     | `{ "delta_steps": int }` *or* `{ "target_steps": uint }` | controller| **v1 alias** for `move_axis` on element `L`. Balanced L only (`wrong_topology` otherwise). Refused if RF present. |
| `move_c`     | same shape                                        | controller| **v1 alias** for element `C`. Same rules.             |
| `move_axis`  | `{ "axis": 0..2 \| "L"\|"C"\|"C1"\|"C2", "delta_steps": int }` *or* `target_steps` | controller | Bounded move on one carrier axis, addressed by index or bound element name. Clamped to the axis' travel window; `at_limit` when already on the bound it points at; `not_anchored` for an axis without a declared home unless `bypass:true` (setup). `delta_steps: ±1` is the fine-tuning single step. |
| `run`        | `{ "axis": .., "dir": "cw" \| "ccw" }`             | controller| Run to the end of the travel window in that direction (element with stops), or unbounded for a free-rotating element. `kind_unset` / `not_anchored` otherwise. |
| `stop`       | `{ "axis"?: .. }`                                 | controller| Immediate stop of one axis, or all when `axis` is omitted. Always accepted. No latch. |
| `estop`      | `{ "axis"?: .. }`                                 | controller| **Emergency stop**: immediate stop **and a latched alarm** on the axis (or every axis when omitted). Latched axes refuse every motion verb with `estop` until released — industrial mushroom-button semantics (the browser page shows the button pressed and toggles it). Relay, home-declaration and configuration verbs stay available. Not persisted across a power cycle. |
| `estop_reset`| `{ "axis"?: .. }`                                 | controller| Release the alarm on one axis (even one latched by an all-axes `estop`), or on every axis when omitted. |
| `set_home`   | `{ "axis": .. }`                                  | controller| Declare the current position as home (0): anchors the axis and activates its travel window. |
| `unset_home` | `{ "axis": .. }`                                  | controller| Forget home: window inactive, axis unanchored.        |
| `set_element`| `{ "axis": .., "kind": "vacuum_cap" \| 2, "max_rev": 40 }` | controller | Declare the element hardware on the axis and, for kinds with stops, its rated travel in revolutions. A kind change clears home. Persisted. |
| `set_speed`  | `{ "axis": .., "speed": uint, "accel"?: uint }`    | controller| Cruise steps/s (1..200000) and ramp steps/s². Persisted. |
| `set_enabled`| `{ "axis": .., "on": bool }`                       | controller| Driver ENA. Off is a setup-only action (turn the element by hand); the position is untrusted until home is re-declared. |
| `set_topology` | `{ "kind": "balanced_l" \| "balanced_pi", "elements": [ {"name","type","axis","pair"} ] }` | controller | Declare the wired network and the element→axis map (CLAUDE.md "Topology vs firmware"). Requires `bypass:true` and no motion; refused `duplicate_axis` / `bad_axis` / `bad_elements`. Persisted to NVRAM and applied immediately. |
| `set_side`   | `{ "side": "hi_z" \| "lo_z" }`                     | controller| Refused if RF present. Auto-engages bypass first.    |
| `set_bypass` | `{ "bypass": bool }` *or* `{ "on": bool }`         | controller| The only relay verb accepted while RF is present.    |
| `recall`     | `{ "freq_hz": uint }`                             | master    | Master expands into a sequence of controller verbs.  |
| `save`       | `{ "freq_hz": uint, "label"?: string }`           | master    | Persists current `state` for `(band, bucket)`.       |
| `auto_tune`  | `{ "freq_hz": uint, "power_w": float }`           | master    | Master orchestrates analytic + hill-climb.           |
| `home`       | `{}`                                              | controller| Drives every topology-bound axis back to its declared home (0). Refused `not_anchored` unless all of them are anchored. (The lead-screw limit-switch homing routine replaces this once the mechanism is fitted.) |
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
| Motion refused by the anchoring / travel rules | `ack ok:false` with `not_anchored`, `at_limit`, `kind_unset`, `bad_axis`. |
| Motion refused because an E-stop alarm is latched | `ack ok:false` with `estop`; send `estop_reset` first. |
| Topology / side verbs                       | `ack ok:false` with `wrong_topology`, `not_bypassed`, `moving`, `duplicate_axis`, `bad_axis`, `bad_elements`. |
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
