# Tuning Algorithm

How the master decides "what L, C, side to be at" for a given operating
frequency, and how that decision survives invariant #1 from
[`../CLAUDE.md`](../CLAUDE.md) — *no mechanical motion while RF is
present.*

This document covers:

1. The **universal per-tune protocol** — the safety-gated outer loop
   every algorithm must obey because of invariant #1.
2. **Per-band starting conditions** for a 102′ Doublet on 600 Ω ladder
   line — the heuristic table that seeds search-on-miss.
3. **Four candidate algorithm strategies** (A–D) with pros / cons.
4. The project's **chosen strategy** and what it costs to implement.

For the underlying L-network math — closed-form `(R, jX) → (L, C, side)`
for both Hi-Z and Lo-Z topologies — see
[`RF-DESIGN.md`](RF-DESIGN.md) §3 (scaffold during M0; written
alongside M4).

---

## 1. Universal per-tune protocol

You can't move L or C with RF present (firmware-enforced; see
[`../CLAUDE.md`](../CLAUDE.md) "Invariants" #1), and you can't measure Z
without RF. **Every tuning strategy therefore reduces to the same
outer loop:**

```
operator presses TUNE on freq F
        │
        ▼
master → controller: set_bypass true                (K3 in — safe topology)
master → operator:   "key TX at low power"          (banner; ≤ 5 W)
        │
        ▼  (TX keyed; safety.fwd_w ≥ tx_lockout_w)
controller: AD8302 / AD8307 → R, X, |Z|, ∠Z, SWR streamed at ~30 Hz
master: average over ≥ 500 ms window for SNR → Z_measured
master → operator:   "unkey"
        │
        ▼  (TX unkeyed; fwd_w decays < threshold for 3 s)
master: strategy.next_target(Z_measured, history) → (L_tgt, C_tgt, side?)
master → controller: set_side(side)                 (only if side changes)
master → controller: move_l(L_tgt) + move_c(C_tgt)  (parallel)
controller: motion::tick() drives both axes; ack ok when stopped
        │
        ▼
master → controller: set_bypass false               (engage the L-network)
master → operator:   "key TX at low power"          (verification)
master: measure SWR
        ├─ ≤ swr_done_threshold (default 1.10) → save? → DONE
        └─ > swr_done_threshold                → re-enter loop (bypass on)
```

**Why each step is the way it is**

| Step                              | Reason                                                                                              |
|-----------------------------------|-----------------------------------------------------------------------------------------------------|
| `set_bypass true` first           | Invariant #2 (bypass on power-up) extended: bypass is the *only* safe configuration during reconfig. |
| Measure in bypass                 | The bypass topology presents the *load impedance through the balun* at the tuner port — exactly the Z the L-network has to match. With the L-network engaged, you'd measure the network's output, not its input. |
| `set_side` before any motion      | K1/K2 are mutually exclusive (`relay_fault` if both close). The verb itself refuses under RF. |
| Move L and C in parallel          | Both axes can travel without interaction — they're mechanically independent. Halves wall-clock tune time. |
| Re-key after move to verify SWR   | The analytic / heuristic solution assumes lossless components; real losses + parasitic C show up at engagement. The verification is also what catches "the operator changed bands while we were moving." |
| Save is operator-confirmed        | Invariant #4 — memory writes are explicit. `auto_save` exists in config but defaults off. |

**Timing budget per iteration** (target, set in M4):

- key-down + measure: 0.5 – 1 s
- compute + move (worst case, full-range axis): 3 – 5 s
- engage + verify: 0.5 – 1 s
- **Total: ~5 s per iteration.** Recall path bypasses the inner
  loop; most tunes complete in 1 iteration once memory is built.

---

## 2. Per-band starting conditions

The Doublet + ladder-line impedance transformation is highly
band-dependent. These hints exist to (a) seed the search when memory
misses, (b) keep the operator's first key-down brief, and (c) avoid
scanning the entire 0 – 30 000 step range every time.

**Values are typical for a 102′ flat-top Doublet on ~50′ of 600 Ω
window line.** Real installations vary — these are the *seed*; M5
commissioning replaces them with measured truth.

| Band  | Expected Z at tuner port | Likely side  | L hint     | C hint     | Notes                                                |
|-------|--------------------------|--------------|------------|------------|------------------------------------------------------|
| 160 m | very low R, large +jX    | **Lo-Z**     | high (≈70 %) | medium     | Doublet is short relative to λ; high reactance.     |
| 80 m  | low-moderate R, ±jX      | **Lo-Z** first, fall through to Hi-Z | medium-high | medium-high | Sometimes near-resonant, sometimes not.              |
| 40 m  | near 50 Ω, mild jX       | either; try Lo-Z first | low-medium | low-medium | Usually the *easy* band.                            |
| 30 m  | moderate, mildly inductive | **Hi-Z**   | medium     | low-medium |                                                      |
| 20 m  | high R, mild ±jX         | **Hi-Z**     | medium     | low        | Doublet length is ~1λ → high feedpoint Z.            |
| 17 m  | moderate-high            | **Hi-Z**     | medium     | low-medium |                                                      |
| 15 m  | high R                   | **Hi-Z**     | low-medium | low        |                                                      |
| 12 m  | high R                   | **Hi-Z**     | low-medium | low        |                                                      |
| 10 m  | very high R              | **Hi-Z**     | low        | very low   | Common SWR null already near minimum L/C.            |
| 6 m   | highly antenna-dependent | **measure first** | n/a    | n/a        | No good closed-form hint — Doublet is rarely a useful 6 m antenna. |

L/C hints are quantitative once M5 builds the memory table; pre-M5
they're rough percentage-of-travel anchors that prevent the algorithm
from spending the first 30 s sweeping infeasible regions.

---

## 3. Four candidate algorithm strategies

Each strategy is a specific way of populating the *"strategy.next_target"*
step in the universal protocol. The trade-offs are real — pick the one
whose cons you can tolerate.

### Proposal A — *Memory-first with fallback search*

Commercial-tuner style (Elecraft KAT-series, Palstar). Memory recall
is the default path; search only happens on a miss.

```
QRG event → master.recall(band, bucket)
  ├─ hit:  set_bypass true → move (L, C, side) → set_bypass false → verify
  │         ├─ ≤ done_threshold → DONE  (no save — slot was already correct)
  │         └─ > done_threshold → hill-climb refine → re-save
  └─ miss: side  := per-band hint table (§2)
           seed := per-band L/C hint
           hill-climb from seed → save
```

**Pros**
- Sub-second on a memory hit (the 99 % case in normal operation).
- Operator workflow matches every commercial auto-tuner — no learning curve.
- Cheapest in motor wear: most tunes are one parallel move and stop.
- Robust to detector miscalibration — recall doesn't depend on `Z` quality.

**Cons**
- Needs the M5 band-walk to build the initial memory table.
- Stale on weather / ice / season changes — operator notices SWR creep, presses TUNE, refine step rebuilds the slot.
- 6 m is awkward — no per-band hint without measurement.
- New antennas mean re-walking everything.

### Proposal B — *Pure analytic solve, no memory required*

Closed-form L-network theory, no remembered state. Every tune is a
fresh derivation.

```
QRG event → prompt key TX
  measure Z = R + jX  (in bypass)
  side := (R > 50) ? Hi-Z : Lo-Z
  X_L, X_C := analytic L-network solution absorbing jX into the chosen branch
              (closed form per RF-DESIGN.md §3)
  L_tgt := L_steps(X_L, F)    ← from install calibration sweep L(steps, f)
  C_tgt := C_steps(X_C, F)    ← from install calibration sweep C(steps, f)
  move → engage → verify
  if SWR > done → one round of hill-climb (cleans up parasitic / lossy match)
```

**Pros**
- Works first-time on any band, including 6 m and any new antenna.
- Mathematically optimal seeding — usually one move per axis plus a touch-up.
- Robust to environment changes (always remeasures).
- No memory table to build or invalidate.

**Cons**
- AD8302 needs a clean Z measurement at low power — M2 calibration must be cross-checked against a reactive-load fixture across all bands.
- Requires `L(steps, freq)` and `C(steps, freq)` curves from a one-time install sweep; if those drift the algorithm misseeds.
- Lossless equations ignore roller-inductor I²R losses and stray feed-through C — small but the touch-up step has to clean it up.
- Slightly slower than a memory hit (always at least one TX cycle for the measurement).

### Proposal C — *Coarse grid then hill-climb*

No calibration curves, no remembered state — pure search.

```
QRG event → side := per-band hint
  coarse pass: ~16 (L, C) grid points in the band's likely region
               key TX briefly at each, record SWR
  pick the best grid cell → hill-climb within it
  save best to memory
```

**Pros**
- Doesn't need any `L(steps)` / `C(steps)` calibration curves — just the §2 hint table.
- Tolerant to component aging and parasitic surprises.
- Easy to explain and debug; no detector-math dependency.

**Cons**
- 16+ key-down cycles per fresh tune is audible and tedious — operator notices on every band change.
- Slow on first tune of any band (10 – 30 s).
- Still depends on the per-band hint table to bound the coarse window; without it the search space is intractable.
- Doesn't degrade gracefully to anything faster on repeated tunes.

### Proposal D — *Hybrid: recall → analytic seed if miss → hill-climb refine*

The strategy `PLAN.md` M4 already targets. Combines A's fast-path with
B's robustness; the refine step is C's fallback when both stale.

```
QRG event → master.recall(band, bucket)
  ├─ memory hit: move → engage → verify
  │     ├─ SWR ≤ done_threshold                → DONE
  │     ├─ SWR > done but ≤ recall_threshold  → hill-climb refine → re-save
  │     └─ SWR > recall_threshold (stale)      → fall through to "miss"
  └─ memory miss / stale:
        prompt key-down → measure Z (Proposal B path)
        compute analytic (L_seed, C_seed, side)
        move → verify
          ├─ SWR ≤ done → save → DONE
          └─ SWR > done → hill-climb refine → save
```

**Pros**
- Fast on the 99 % memory-hit case (Proposal A pro).
- Robust on the 1 % miss / new-band case (Proposal B pro).
- **Self-improving** — every tune writes a better slot; the table converges to the antenna's truth over weeks.
- Same hill-climb path doubles as stale-memory recovery — no separate "rebuild memory" workflow needed.

**Cons**
- Three code paths to test (hit-good, hit-stale, miss).
- Needs both M2 detector calibration *and* the M3 memory plumbing — but those are paid down in M3 + M4 anyway, not extra cost here.
- The stale-detection threshold `swr_recall_threshold` is a knob with no obvious default; likely needs operator tuning per installation. Suggested starting value: 1.5.

---

## 4. Project decision

**Proposal D is the chosen strategy.** Rationale:

- The M5 band-walk memory table makes the common case sub-second (matches Alpha RF / Elecraft UX); this is the path the operator hits 99 % of the time.
- The analytic seed makes a fresh band — including 6 m and any antenna change — converge in one or two TX cycles, even with an empty memory table.
- The hill-climb refinement absorbs every real-world deviation (component drift, weather, antenna icing) without any operator ceremony.
- It is the only proposal that handles "the antenna got iced over and the slot is now stale" gracefully — the same code path that runs on cold-start also runs on stale-recall, just with a better seed.

The cost is implementation complexity contained in the master-side
state machine; the controller protocol surface (verb set, frame
shapes) is unchanged across all four proposals, so the choice is
revisit-able later without firmware impact.

### Thresholds (initial defaults; tuned in M5)

| Threshold              | Default | Where used                                                  |
|------------------------|---------|-------------------------------------------------------------|
| `swr_done_threshold`   | 1.10    | Hill-climb success exit; "DONE" decision.                   |
| `swr_recall_threshold` | 1.50    | Above this on a memory hit → treat slot as stale.           |
| `safety.tx_lockout_w`  | 5 W     | Hard motion-refusal threshold (also enforced in firmware).  |
| `tune_power_w`         | 5 W     | Recommended low-power TX during the iterate loop.           |
| `measure_window_ms`    | 500     | Averaging window for AD8302 / AD8307 samples per key-down.  |
| `coarse_search_grid`   | 4 × 4   | Fallback only — if analytic seed converges, never used.     |

These land in `deploy/config.example.toml` at M3 and get refined
during M5 commissioning.

### Per-band tune-timing budget (target post-M5)

| Path                            | Time   |
|---------------------------------|--------|
| Memory hit, slot good           | < 4 s   |
| Memory hit, slot needs refine   | ~10 s  |
| Memory miss, analytic + refine  | 10 – 30 s |
| Cold-start auto-tune, no memory | 15 – 60 s |
| 6 m or surprise antenna change  | up to 90 s (no analytic seed possible) |

If any path systematically exceeds these in commissioning, the
detector calibration or the per-band hint table is wrong — fix the
data, not the algorithm.

---

## 5. What changes in the protocol / firmware

**Nothing.** The verb set in [`PROTOCOL.md`](PROTOCOL.md) §3 already
covers every move/measure step every strategy needs. The choice of
strategy is entirely a master-side decision and changes no wire
contract or firmware code path.

The only firmware contributions to tuning are:

- **`safety::rf_present()`** — refuses motion verbs while keyed
  (invariant #1; already wired against the sim HAL in M1b.2; replaced
  by real AD8307 sample in M2).
- **`tuner_server` ack semantics** — every verb returns a stable
  `code` that the master's algorithm can branch on (`rf_lockout`,
  `bad_args`, `unknown_action`).

Everything else — the recall path, the analytic seed, the hill-climb
refinement, the threshold tracking, the memory schema — is Go code in
the master.
