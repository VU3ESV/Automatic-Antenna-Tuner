# Phase 2 Extensions — Multi-Antenna, Multi-Transceiver, SO2R

Captures the scope, architecture options, and trade-offs for the
Phase 2 extension set:

1. **Multiple antennas** — user selects which antenna is active; new
   types (HexBeam, Vertical, Yagi, EFHW, …) supported alongside the
   Phase 1 Doublet.
2. **Multiple transceivers** — two or more rigs share the tuner /
   antenna farm, each with its own routing rules.
3. **SO2R** — Single Operator Two Radios contesting workflow: both
   rigs may transmit simultaneously on different antennas, with the
   interlock + band-pass filtering that requires.

Design reference: the **4O3A TGXL / Antenna Genius** family —
commercial SO2R-capable auto-tuner + antenna-switch combos used in
contest stations. This document describes a build that satisfies the
same use cases.

**Status: design proposal, not yet scheduled.** Phase 1 (M0 – M6,
single Doublet, single rig) is the committed scope; Phase 2 milestones
(M7+) below are gated on Phase 1 commissioning being clean.

---

## 1. Phase 1 recap and Phase 2 scope boundary

**Phase 1 (committed)** — single L-network tuner driving a single
Doublet on 460 / 600 Ω ladder line, single transceiver via CAT. One
master, one tuner-controller, one balun.

**Phase 2 (this document)** — the same master orchestrates:

- **N antennas** of varying types, each with its own feed and tuning
  characteristics. The Doublet stays; HexBeam joins as the second
  case study; the framework is general.
- **M transceivers** sharing the antenna farm, each running its own
  CAT link to the master.
- **Routing rules** mapping `(radio, band, frequency)` to an antenna.
- **SO2R interlock** preventing two radios from keying the same
  antenna simultaneously, with band-pass filtering between paths.

The Phase 1 wire protocol survives largely intact; Phase 2 adds
verbs and an antenna dimension to memory but doesn't break v1
clients. Versioning lives in [`PROTOCOL.md`](PROTOCOL.md) §7.

---

## 2. Antenna abstraction

Every antenna in the system is described by a small typed record on
the master (TOML config + per-installation overrides) so the tuning
algorithm and the routing logic don't hard-code the Doublet.

```toml
[[antenna]]
id            = "doublet-102ft"        # operator-visible name
type          = "doublet_ladder"        # see "Antenna types" below
bands         = ["160m", "80m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"]
feed          = "balanced_600ohm"
needs_tuner   = true                    # always
balun_ratio   = "1:1"                   # or "4:1"
max_power_w   = 1500
notes         = "Phase 1 default; doc'd in HARDWARE.md §9"

[[antenna]]
id            = "hexbeam-k4kio"
type          = "hexbeam"
bands         = ["20m", "17m", "15m", "12m", "10m", "6m"]
feed          = "unbalanced_50ohm"
needs_tuner   = false                   # near-50Ω on every supported band
balun_ratio   = "none"
max_power_w   = 1500
```

### 2.1 Antenna types (initial catalogue)

| `type`              | Feed                 | `needs_tuner` typical | Notes                                                                           |
|---------------------|----------------------|------------------------|--------------------------------------------------------------------------------|
| `doublet_ladder`    | balanced ladder line | **yes (always)**       | Wildly varying Z across bands — the project's Phase 1 case.                    |
| `hexbeam`           | coax, 50 Ω           | no (bypass)            | K4KIO / G3TXQ design. Near-50 Ω on 20–6 m. Tuner passes through in bypass.     |
| `vertical_quarter`  | coax, ~36 Ω          | maybe                  | Often needs a small match — tuner in low-Q Lo-Z mode.                          |
| `yagi_monoband`     | coax, 50 Ω           | no (bypass)            | Single band. Routing logic excludes from other bands.                          |
| `efhw`              | coax via 49:1 unun   | rarely                 | Already pre-matched by the unun on harmonic bands.                             |
| `loop_full_wave`    | coax via 4:1         | sometimes              | Behaviour depends on circumference vs λ.                                       |
| `beverage_rx`       | coax, 450 Ω term     | no                     | RX-only; routing must refuse TX to this antenna (hard interlock).             |

The `type` is a discriminator for:

- the **tuner action** at engage time (bypass vs full L-network match);
- the **per-band hint table** the algorithm uses to seed
  (see [`TUNING.md`](TUNING.md) §2) — each type has its own;
- the **memory schema key** (see §6);
- routing **defaults** (which bands are sensible on which antenna);
- safety policy (RX-only antennas refuse TX verbs).

### 2.2 Per-antenna tuning behaviour

The Phase 1 tuning strategy (TUNING.md Proposal D) extends naturally
to any antenna whose `needs_tuner = true`. For `needs_tuner = false`
antennas, the master skips the entire tuning state machine: on
selection it just sends `set_bypass true`, performs a `set_side`
to a configured default (typically Hi-Z, irrelevant in bypass), and
considers the antenna engaged.

The HexBeam case is the cheap path:

```
QRG event → master.route(radio, band) → antenna = "hexbeam-k4kio"
            antenna.needs_tuner == false
            → set_bypass true; report SWR as measured; done.
```

No motion, no memory hit, no tuning cycle. The motor wear budget
for a HexBeam-favouring operator is effectively zero.

---

## 3. Multi-transceiver support

### 3.1 Radio context per command

Every browser command and CAT event carries a `radio_id` so the
master knows whose state to mutate. Phase 1 implicitly has
`radio_id = "rig-1"`; Phase 2 makes the ID explicit and adds the
config block:

```toml
[[radio]]
id           = "k3-main"
cat_device   = "/dev/ttyUSB0"
cat_protocol = "elecraft_k3"
cat_baud     = 38400
display_name = "K3 (left)"

[[radio]]
id           = "ic7300-second"
cat_device   = "/dev/ttyUSB1"
cat_protocol = "icom_civ"
cat_baud     = 19200
display_name = "IC-7300 (right)"
```

The master spins one `cat-poller` goroutine per `[[radio]]` block,
emitting QRG events tagged with the radio ID. Memory recall, the
Operate page, the auto-tune flow — all per-radio.

### 3.2 Browser UI per radio

The web UI gains a radio-context selector (top-left, like the CAT
spec field) so the operator knows which rig's state they're
mutating. Two browsers, one per radio, work the same as today —
each browser stays on its radio context, each gets the fan-out for
*all* radios but renders only its own.

---

## 4. Routing rules — `(radio, band, frequency) → antenna`

Routing is a **configurable matrix** that the master evaluates on
every QRG event. The operator declares rules in TOML; the master
applies them in priority order.

```toml
[[routing]]
radio    = "k3-main"
bands    = ["20m", "17m", "15m", "12m", "10m", "6m"]
antenna  = "hexbeam-k4kio"
priority = 100              # higher = wins

[[routing]]
radio    = "k3-main"
bands    = ["160m", "80m", "40m", "30m"]
antenna  = "doublet-102ft"
priority = 100

[[routing]]
radio    = "ic7300-second"
bands    = "*"              # any band
antenna  = "doublet-102ft"
priority = 50               # always falls through to doublet
```

Rules are evaluated `radio + band` first, then fall through to band
wildcards, then to radio wildcards. Conflicts (two rules match)
resolve by `priority`. The master surfaces the resolved route in
`state` so the operator always sees which antenna is active for
which radio.

### 4.1 Operator override

The operator can override the routing for a (radio, band) by
selecting an antenna manually in the UI. The override:

- persists only for the current QRG bucket (not saved unless saved
  explicitly);
- triggers a re-tune cycle for the new antenna;
- clears when the QRG leaves the bucket;
- emits `status info code:routing_override` so subsequent operators
  see the deviation in the log.

---

## 5. SO2R mode

### 5.1 What SO2R needs (the constraints)

Single Operator Two Radios contesting:

- Both radios may **transmit simultaneously**, typically on different
  bands.
- Both radios must be **on different antennas** — same antenna into
  two transmitters would couple their PAs, blow the balun, or both.
- **Switching is fast** — between QSOs, often < 100 ms. The operator
  can't wait for stepper-motor motion.
- **Inter-radio isolation** is critical — Radio A's TX must not
  desense Radio B's RX. Achieved with per-antenna **band-pass
  filters** (BPFs) on the radio side of the antenna switch.

### 5.2 What this implies for the hardware

| Requirement                                | Phase 1 supports? | Phase 2 adds                                                                       |
|--------------------------------------------|-------------------|-------------------------------------------------------------------------------------|
| One antenna, one radio, one tuner          | ✅                | —                                                                                  |
| Multi-antenna, single radio, sequential    | ❌                | Antenna selector matrix (M × N) + per-antenna tuning state                         |
| Multi-radio, sequential (one-at-a-time TX) | ❌                | Radio input matrix + per-radio routing                                              |
| Multi-radio, **simultaneous** TX (SO2R)    | ❌                | Per-antenna **tuner banks** so positions are pre-set; per-antenna BPF; interlock matrix |

The critical architectural fork is: **do we share one tuner across
all antennas (with motion required on each switch), or do we run a
tuner per antenna with positions pre-staged?** Sections 7 and 8
cover the trade-offs.

### 5.3 Interlock invariants (new — added to CLAUDE.md at Phase 2)

The Phase 1 invariant set extends with these:

**Invariant #7 (Phase 2): One radio per antenna at a time.** The
master refuses any routing rule that would connect two radios to the
same antenna simultaneously. Attempted routing of Radio B to
Antenna X while Radio A is keyed into Antenna X returns
`ack ok:false code:antenna_busy`.

**Invariant #8 (Phase 2): Band-pass filter required for SO2R.** SO2R
mode (both radios may key simultaneously) is operator-enabled in
config (`mode = "so2r"`). When `mode = "so2r"`, every antenna in the
routing matrix must have a BPF entry; the master refuses to enter
SO2R mode if any active antenna lacks a BPF declaration.

**Invariant #9 (Phase 2): No motion on the in-use antenna's tuner.**
While Radio A is keyed into Antenna X, the master refuses motion
verbs on Antenna X's tuner. This is the Phase 1 invariant #1
applied per-tuner instead of globally — the firmware safety lockout
must be replicated on every tuner-controller in the bank.

### 5.4 The 4O3A TGXL reference

The 4O3A TGXL / Antenna Genius family illustrates one mature
realisation of these constraints:

- **2 radio inputs**, 6 – 8 antenna outputs.
- **Built-in BPFs** per band per radio path.
- **Hardware interlock** in the antenna selector so the
  "two-radios-one-antenna" condition is physically impossible —
  the relay matrix layout precludes it, not just the firmware.
- **Auto band detection** via CAT to each radio.
- **Pre-staged tuning** per antenna so band changes are switch-time,
  not motion-time.
- **Web UI** for live state + manual override.

This project's Phase 2 doesn't have to match the TGXL feature for
feature — but it does have to satisfy the same invariants (§5.3) to
be usable for SO2R contesting.

---

## 6. Memory schema extension

Phase 1 memory key: `(band, freq_bucket)`. Phase 2 adds the antenna
dimension:

```sql
-- Phase 2 schema (additive; Phase 1 rows migrate by setting antenna_id
-- to the Phase-1 default antenna name).
ALTER TABLE slot ADD COLUMN antenna_id TEXT NOT NULL DEFAULT 'doublet-102ft';
DROP INDEX slot_band_freq;
CREATE UNIQUE INDEX slot_antenna_band_freq ON slot(antenna_id, band, freq_hz);
```

Recall logic becomes:

```
recall(radio, freq_hz):
  antenna  := routing.resolve(radio, band(freq_hz), freq_hz)
  bucket   := bucket_for(antenna.type, band, freq_hz)
  slot     := SELECT FROM slot WHERE antenna_id=antenna AND band=band AND freq_hz=bucket
  if slot found and antenna.needs_tuner: drive to (l_steps, c_steps, side)
  else:                                  set_bypass true (HexBeam-class)
```

Memory backup / restore (M3 work) needs to handle the
`antenna_id` column. A Phase 1 export imported into a Phase 2
master defaults all slots to the Phase 1 antenna name.

---

## 7. Protocol extension

The Phase 1 verb set survives. Phase 2 adds:

| `action`              | `args`                                                            | Hop       | Effect                                                                                            |
|-----------------------|-------------------------------------------------------------------|-----------|---------------------------------------------------------------------------------------------------|
| `set_antenna`         | `{ "antenna_id": str }`                                            | master    | Operator override of the routing rule for the current `(radio, band)`. Triggers re-tune.         |
| `set_routing`         | `{ "radio": str, "band": str, "antenna_id": str, "priority"?: int }` | master   | Mutate the routing table at runtime. Persisted to TOML on success.                                |
| `query_routing`       | `{ "radio"?: str }`                                                | master    | Returns the resolved routing matrix.                                                              |
| `set_so2r_mode`       | `{ "enabled": bool }`                                              | master    | Operator toggles SO2R. Refused if BPFs not configured (invariant #8).                            |
| `query_antennas`      | `{}`                                                               | master    | Lists configured antennas + per-antenna state.                                                    |

State frames extend with a `routing` sub-object (per-radio resolved
antenna + lockout flags). The `state` frame from each tuner-
controller carries an extra `antenna_id` field so the master can
fan-out the right tuner's state to the right browser context.

Sequence numbering, ack semantics, and connection management stay
unchanged. Versioning: Phase 2 wire format is **v2**, negotiated via
the `Sec-WebSocket-Protocol` header per
[`PROTOCOL.md`](PROTOCOL.md) §7.

---

## 8. Architecture options

Four ways to get to multi-antenna + SO2R. Trade-offs are real.

### Option A — *Single tuner + antenna selector matrix*

```
                          ┌── antenna sel ──┬── A1 (Doublet)
Radio 1 ──┐               │                 ├── A2 (HexBeam)
          ├── Tuner ──────┤                 ├── A3 (Vertical)
Radio 2 ──┘               │                 └── …
                          └── routed at switch time
```

One L-network, one set of motors, one tuner-controller. Radios are
multiplexed at the input; antennas at the output.

**Pros**
- Minimum hardware cost (one tuner).
- Phase 1 firmware survives mostly unchanged (just adds two relay sets).
- Easiest to reason about — single safety lockout.

**Cons**
- **Cannot do SO2R.** Only one radio can use the tuner at a time, and
  the tuner can only be tuned to one (antenna, frequency) at a time.
- Antenna switch requires re-tuning if the new antenna needs match — operator-visible delay.
- 4O3A TGXL doesn't work this way for a reason.

### Option B — *Tuner-per-antenna bank* (the TGXL approach)

```
Radio 1 ──┐        ┌── Tuner-1 ── Antenna 1 (Doublet)
          ├── BPF ─┤── Tuner-2 ── Antenna 2 (HexBeam, bypass-only)
Radio 2 ──┘        ├── Tuner-3 ── Antenna 3 (Vertical)
                   └── …
```

One tuner per multi-band antenna; bypass-only tuners for matched
antennas (HexBeam, Yagi); a radio→antenna switch matrix in front.

**Pros**
- **Native SO2R** — Radio 1 can be on Antenna 1, Radio 2 on Antenna 3, simultaneously.
- Antenna switches are instant (just relays, no motion) — positions pre-staged.
- Each tuner can pre-position for the next expected band of its antenna while the other tuner is in use.
- Matches the TGXL design philosophy.

**Cons**
- Hardware cost: N tuners instead of one. The L-network electronics dominate the tuner BoM; this multiplies it by N for multi-band antennas.
- Bypass-only antennas (HexBeam, Yagi) need cheaper "tuner-shaped" boxes — just a balun, a relay, optional small fixed L/C. Plan a separate cheap-path BoM.
- Coordination logic on master is more complex (one tunerclient per controller).
- Calibration sweep per antenna at install.

### Option C — *Single tuner + pre-staged positions on a band-detect*

```
Radio 1 ──┐
          ├── Tuner with band-detect ── antenna sel ── A1 / A2 / A3 / …
Radio 2 ──┘                           (positions pre-staged for next QRG)
```

One tuner, but it constantly pre-positions for the next likely band
based on CAT polling, so the user-perceived switch is instant.

**Pros**
- Cheaper than B.
- Faster than A on band changes.
- Some SO2R capability — *sequential* SO2R where both radios share the tuner but don't TX simultaneously.

Cons**
- Still no parallel SO2R.
- High mechanical wear — the motors are always moving anticipating.
- Pre-staging algorithm is complex (which band next? Heuristic on band-map + recent operator behaviour).
- Doesn't solve the BPF-isolation problem.

### Option D — *Hybrid: full tuner for the demanding antenna, bypass paths for matched antennas*

```
Radio 1 ──┐        ┌── Full tuner ── Doublet
          ├── BPF ─┤── Bypass ────── HexBeam
Radio 2 ──┘        ├── Bypass ────── Yagi 20m
                   └── …
```

Only the Doublet (or other always-needs-tuner antennas) gets the
full L-network. HexBeam-class antennas use a simple bypass path
with optional fixed match.

**Pros**
- Cost-effective for typical contest-station antenna farms (most antennas in a contest farm are 50-Ω-resonant).
- **SO2R-capable** if Radio 1 uses the Doublet path and Radio 2 uses any bypass path simultaneously.
- Phase 1 firmware extends naturally — same tuner-controller, same protocol, just additional antenna-selector hardware.
- Reasonable BoM scaling: one expensive tuner, N − 1 cheap bypass paths.

**Cons**
- Two-Doublet-class antennas can't run SO2R against each other (they'd contend for the single full tuner).
- Routing rules need to be aware of which antennas are tuner-class vs bypass-class.
- Doesn't generalise to multi-tuner farms (a 4-Doublet contest station would need to fall back to Option B).

---

## 9. Recommended evolution path

```
Phase 1 (M0 – M6, current scope):
    Single Doublet. Single radio. Get the protocol, the algorithm,
    and the deploy pipeline solid against one antenna + one rig.

Phase 2a (M7 — multi-antenna, single radio):
    Architecture: Option D — full tuner for the Doublet, bypass paths
    for HexBeam-class antennas.
    Adds: antenna abstraction (§2), routing rules (§4), memory schema
    extension (§6), antenna selector hardware.
    Use case: HexBeam in summer, Doublet in winter, no contesting.

Phase 2b (M8 — multi-radio, sequential):
    Architecture: still Option D, plus radio input matrix.
    Adds: per-radio CAT, per-radio routing, per-radio Operate panel,
    interlock invariant #7.
    Use case: K3 + IC-7300 sharing the antenna farm; one operates at
    a time but the second can be listening on a different band.

Phase 2c (M9 — SO2R, parallel):
    Architecture: extend Option D with BPF per radio path, plus
    add tuner-bank capability for multi-band antennas the operator
    wants on both radios (effectively migrating to Option B for
    those antennas).
    Adds: BPF declaration in TOML, invariants #8 and #9, the
    `set_so2r_mode` verb, hardware interlock.
    Use case: contesting.
```

**Why this order, and why Option D first:**

- Option D is the smallest Phase 1 → Phase 2 jump; it preserves the
  Phase 1 firmware unchanged for the Doublet path and adds only
  selector hardware for the bypass paths.
- It gets multi-antenna value early (M7) for the common case (most
  stations have one demanding antenna + 1 – 2 resonant antennas).
- It defers the expensive part (tuner-bank, BPFs, SO2R interlock) to
  M9 only if the operator's use case actually needs it.
- M8 sits between as a natural step — adding a second radio without
  the SO2R complexity is half the value at a quarter of the cost.

If the user's primary use case is contesting (SO2R from day one),
skip to **Option B + M9** directly and absorb the hardware-cost hit.
The two paths converge at the same architecture eventually; the
evolution path above just spreads the cost.

---

## 10. Milestone proposal

These are placeholder milestones for the Phase 2 extension; they're
gated on Phase 1 commissioning (M5) being clean.

- **M7 — Multi-antenna (single radio).** Antenna abstraction +
  routing + bypass-path hardware. Operator can switch between
  Doublet and HexBeam in the UI. Memory schema migrated to include
  `antenna_id`.
- **M8 — Multi-transceiver (sequential).** Second CAT link + radio
  input matrix + per-radio routing + per-radio Operate panel.
  Interlock invariant #7 enforced; SO2R remains off by default.
- **M9 — SO2R mode.** BPF per radio path declared in TOML.
  Invariants #8 and #9 enforced. `set_so2r_mode` verb and lockout
  logic. Optional: migrate the Doublet to tuner-bank if SO2R demands
  it on both radios.

Detailed task lists for each fill in when Phase 1 commissioning
exposes whatever it exposes — there's no point planning M7 in detail
until M5 is on-air, because RF surprises in commissioning may rework
some of these assumptions.

---

## 11. What this changes in the existing docs

When Phase 2 is committed (i.e., the user signs off on the evolution
path in §9), the following propagate:

- **[`../CLAUDE.md`](../CLAUDE.md)**: extend invariants list with
  #7–#9 (§5.3 above); soften the "single Doublet" assumption.
- **[`PLAN.md`](PLAN.md)**: M7 – M9 entries with task checklists.
- **[`ARCHITECTURE.md`](ARCHITECTURE.md)**: extend §5 software
  architecture to cover multi-tunerclient and per-radio routing;
  extend §6 failure modes with antenna-busy / BPF-missing / multi-
  tuner-resync scenarios.
- **[`PROTOCOL.md`](PROTOCOL.md)**: v2 negotiated header; add the
  §7 verbs above; extend `state` and `memory` with `antenna_id`.
- **[`TUNING.md`](TUNING.md)**: per-antenna hint tables instead of
  per-band-only; the algorithm itself doesn't change.
- **[`HARDWARE.md`](HARDWARE.md)**: per-antenna BoM, selector matrix,
  BPF spec, per-tuner power budget.
- **[`RF-DESIGN.md`](RF-DESIGN.md)**: per-antenna feed math; balun
  variants per antenna type; BPF insertion-loss budget.
- **[`../PROPOSAL.md`](../PROPOSAL.md)**: update non-goals (the
  "single Doublet" non-goal is removed at Phase 2 commit).

This doc stays the index for the extension scope; the propagation
above is the contract for what "committing Phase 2" actually entails.
