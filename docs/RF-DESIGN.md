# RF Design

L-network theory, component sizing, detector math, balun spec.
Math reference for the auto-tune algorithm
([`TUNING.md`](TUNING.md)) and the detector calibration procedure
([`HARDWARE.md`](HARDWARE.md) §6).

## Status

Scaffold (2026-05-11). §3 (closed-form L-network solution) is filled
in because [`TUNING.md`](TUNING.md) §3 Proposal B/D and
[`ARCHITECTURE.md §4`](ARCHITECTURE.md) reference it directly. The
remaining sections fill in during M2 detector commissioning and M4
algorithm validation.

---

## §1 L-network topology recap

See [`../CLAUDE.md`](../CLAUDE.md) "RF topology" for the locked-in
topology — single series L + single shunt C, with K1 / K2 vacuum
relays switching the C between the load side (Hi-Z mode) and the
source side (Lo-Z mode), plus K3 latching the network out of circuit
in bypass.

What follows assumes that topology. Lossy-element corrections are
absorbed by the hill-climb refinement step downstream
([`TUNING.md`](TUNING.md) §4); the math here is the lossless
first-order seed.

---

## §2 Component sizing

To be filled in during M2 with measured values across all amateur
bands on the target Doublet. Short-form sizing guidance (from BoM
in [`HARDWARE.md`](HARDWARE.md)):

- **Roller inductor**: ~300 nH minimum, ≥ 30 µH maximum. Continuous
  travel — no taps.
- **Vacuum-variable capacitor**: 10 – 2000 pF. Voltage rating
  ≥ 5 kV (≥ 7.5 kV for US legal-limit operation — open decision #4
  in [`PLAN.md`](PLAN.md)).
- Both reactive elements rated for full-legal-limit dissipation
  with 3 dB safety margin per the L-network Q at the worst-case
  band (typically 160 m on a short Doublet — high Q means high
  circulating current and large RMS V across the C).

Per-band L and C target ranges live in [`TUNING.md`](TUNING.md) §2.

---

## §3 Closed-form L-network solution

The auto-tune algorithm
([`TUNING.md`](TUNING.md) Proposal B and the analytic-seed path of
Proposal D) needs to compute `(L_target, C_target, side)` from a
measured load impedance `Z_load = R + jX` looking into the tuner
output port with bypass engaged. Source impedance is `R_s = 50 Ω`.

### §3.1 Side selection

| Load                | Side    | Topology (source → load)                                  |
|---------------------|---------|------------------------------------------------------------|
| `R > R_s` (high-Z)  | **Hi-Z** | source — series L — shunt C across load — load           |
| `R < R_s` (low-Z)   | **Lo-Z** | source — shunt C across source — series L — load         |
| `R == R_s` (rare)   | either  | `X_L = −X`, `X_C → ∞` (C at minimum); either side works   |

The shunt belongs on the **high-impedance side**; the series belongs
on the **low-impedance side**. This is the textbook rule and is
opposite of what some older manual-tuner references state — verify
on a known reactive load during M2 calibration before adopting any
alternative.

### §3.2 Hi-Z mode (R > 50)

Loaded Q of the matching network:

```
Q = √( R / R_s − 1 )
```

Required reactances (lossless, absorbing the load reactance `X`
into the series element):

```
X_L = Q · R_s − X       (positive — inductive)
X_C = R / Q             (positive — magnitude of capacitive reactance)
```

Component values at operating frequency `f` (Hz):

```
L = X_L / ( 2π · f )           (henries)
C = 1 / ( 2π · f · X_C )       (farads)
```

### §3.3 Lo-Z mode (R < 50)

Symmetric to Hi-Z:

```
Q = √( R_s / R − 1 )
X_L = Q · R − X         (positive)
X_C = R_s / Q           (positive)
L = X_L / ( 2π · f )
C = 1 / ( 2π · f · X_C )
```

### §3.4 Reactance-to-steps mapping

The above gives `(L, C)` in SI units. The controller's `move_l` /
`move_c` verbs take **step counts**. The master applies the
per-axis calibration curves derived in the M2 install sweep:

```
L_steps = L_to_steps( L, f )      # interpolates a measured (steps → henries) table at f
C_steps = C_to_steps( C, f )      # mirror
```

These tables live in TOML on the master, served to the controller
on connect (per [`PLAN.md`](PLAN.md) M4). Until the install sweep
runs they're absent; the algorithm falls back to the per-band hint
table in [`TUNING.md`](TUNING.md) §2.

### §3.5 Bounds and degeneracy

Before commanding the move, the master bounds-checks the computed
values against the physical reactive-component ranges (§2). If
either is out of bounds:

- `L > L_max`: load impedance is too high for this network. Fall to
  Lo-Z and re-derive — sometimes a high-Z load on a Doublet is the
  *transformed* low-Z feedpoint, and the alternate side works.
- `C > C_max` *or* `C < C_min` similar.
- `R` exactly 50 Ω (within detector resolution): trivial match,
  `X_L = −X`, `C` at minimum. Often happens in the verify step
  after the hill-climb converges.

If both sides exhaust bounds, the algorithm logs
`status warn:cal_missing` and falls into the manual-procedure
escape hatch ([`TUNING.md`](TUNING.md) §4.1).

### §3.6 What the closed form does *not* handle

Real L-network match accuracy is limited by effects the lossless
equations ignore:

- **Roller-inductor I²R losses** (typical Q ~ 200 at HF — non-trivial
  in high-Q matches at 160 m where Q of the network itself can
  exceed 50).
- **Stray capacitance across the inductor** (typically 5 – 20 pF —
  matters most at 10 / 6 m where it competes with the intended C).
- **Balun insertion loss + reflection** (small but present;
  cross-checked with LP-100A-Server in M5).
- **Common-mode current effects** on the measured `R + jX` — show
  up as feed-asymmetric impedance that the closed form treats as
  signal.

The closed form is **the seed**. The match is only "tuned" after
the hill-climb refinement and the verification key-down with the
L-network engaged. This is why [`TUNING.md`](TUNING.md) Proposal D
always includes hill-climb refinement after the analytic seed —
even on a fresh band with a "good" analytic answer.

---

## §4 Detector chain — from RF to (R, X, SWR)

The detector chain converts the RF on the antenna feedline into the
`(R, X, SWR)` values the auto-tune algorithm reads. It is the single
most labour-intensive piece of **M2 commissioning**; getting it right
is what makes the analytic seed in §3 trustworthy. Without an accurate
chain, the algorithm falls back to the manual-procedure escape hatch
([`TUNING.md`](TUNING.md) §4.1) — slower, audibly key-cycling, but
still safe.

### §4.1 Signal-flow overview

```
                       antenna feedline ─────────────────────────────►
                                  │
              ┌───────────────────┴───────────────────────────────┐
              │           Tandem-match directional coupler         │
              └──┬───────────┬───────────┬───────────────┬───────┘
                 │           │           │               │
                 ▼           ▼           ▼               ▼
              Fwd port   Rev port    V tap (cap div)   I tap (Stockton CT)
              ≈ -30 dB   ≈ -30 dB    high-Z, scaled    low-Z, scaled
                 │           │           │               │
                 ▼           ▼           └──────┬──┬─────┘
              AD8307      AD8307                ▼  ▼
            (log power) (log power)            AD8302  (gain / phase)
                 │           │                  │           │
              V_fwd       V_rev              V_mag       V_phs
                 │           │                  │           │
                 └───────────┴──────────────────┴───────────┘
                                  │
                                  ▼
                  RC LPF + op-amp buffer per channel
                                  │
                                  ▼
                 Teensy 4.1 ADC, 4 channels, DMA 10 kSPS
                                  │
                                  ▼
                Calibrated decode → R, X, SWR, P_fwd, P_rev
                                  │
                                  ▼
                 telemetry frame at 30 Hz (PROTOCOL.md §2.1)
```

Every block has a calibration step (§4.9) and a sanity gate (§4.10);
the algorithm refuses to engage the L-network on telemetry that
fails any gate.

### §4.2 RF front-end — coupler + V/I sample taps

**Directional coupler** (Stockton or Tandem-match):

- Located between the L-network and the balun on the tuner output.
- Coupling factor ≈ −30 dB (1 W on the main line → 1 mW at each
  coupled port).
- **Directivity ≥ 25 dB** across 1.8 – 54 MHz. Below this, Fwd / Rev
  isolation breaks down and SWR readings get unreliable on heavy
  mismatch — the dominant cal headache.
- All unused ports terminated in precision 50 Ω loads (1 %, non-
  inductive).
- Provides the two RF samples the **AD8307 chain** reads (Fwd /
  Rev power → SWR + delivered power).

**V (voltage) sample tap** for the AD8302:

- High-impedance capacitive divider (typically 1 : 10 to 1 : 100)
  across the main line at the same physical point as the coupler.
- Presents > 5 kΩ at the lowest band so it does not load the line.
- 50 Ω-matched into the AD8302 V input.

**I (current) sample tap** for the AD8302:

- Toroidal current transformer (Stockton CT, or single-turn primary
  through a small ferrite core), secondary terminated in 50 Ω.
- 50 Ω-matched into the AD8302 I input.

**Critical constraint — matched electrical length.** The V and I
paths to the AD8302 must be equal length within ≈ λ / 100 at 30 MHz
(≈ 1 cm of FR4 microstrip). Any unmatched length adds a spurious
phase error that masquerades as load reactance — exactly the thing
we're trying to measure. §4.9 includes a path-length residual
calibration step against a pure resistive load.

### §4.3 AD8307 forward / reverse power chains

One AD8307 per coupler port. Per chip:

- **Input** — 50 Ω terminated, DC-blocked with a ~10 nF series cap.
  Resistive attenuator pad (3 – 10 dB) before the chip to keep the
  input below the AD8307's absolute-max +17 dBm under fault
  conditions. Worst-case excursion is **legal-limit drive into the
  coupler with directivity-failure margin** — size the pad
  conservatively, *especially on the Fwd port*.
- **Output** — log-domain DC voltage. Slope ≈ 25 mV/dB, intercept
  ≈ −84 dBm. Usable range −75 dBm → 0 dBm at the chip pin (25 mV
  → 2 V output).
- **Output filter** — RC LPF at ~1 kHz; envelope-detects the
  modulation and rejects any RF that gets past the chip's own filter.
- **Buffer** — op-amp follower (e.g. MCP6022, one half) scaling
  0 – 3.3 V into the Teensy ADC.

**Dynamic-range gotcha.** AD8307 needs ≈ −70 dBm to read above its
noise floor. At 5 W tune power and −30 dB coupling, Fwd port sees
−25 dBm — well within range. At full legal limit with a clean
match (Rev port < −60 dBm), the Rev reading approaches the AD8307
sensitivity floor, and SWR readings of 1.05 or better get noisy.
**The algorithm treats `P_rev / P_fwd < −50 dB` as "SWR < 1.05;
don't fight it"** — better to declare victory than chase noise.

### §4.4 AD8302 |Z| / ∠Z chain

One AD8302 reading the V and I samples (§4.2):

- **V_mag output** — DC voltage proportional to `20·log₁₀(V/I)`,
  scaled by 30 mV/dB. Centred at 900 mV when `V/I = 1` (a 50 Ω load).
- **V_phs output** — DC voltage proportional to the absolute phase
  difference `|∠V − ∠I|`, scaled by 10 mV/°. Centred at 900 mV for
  90° phase difference.
- **Filtering / buffering** — same RC LPF + op-amp follower as the
  AD8307 chain.

**Phase ambiguity — the §4.4 headache.** The AD8302 reports
`|∠V − ∠I|` only. For an inductive load (X > 0), current lags
voltage → ∠V − ∠I > 0. For a capacitive load (X < 0), current
leads → ∠V − ∠I < 0. `| · |` folds both to the same V_phs reading.

Three options to recover the sign, in order of effort:

1. **PFLT-based primary disambiguation.** The AD8302 has a
   phase-quadrant indicator pin (PFLT) that flips on the 0/180°
   boundary. With an asymmetric V tap and consistent I-tap
   orientation on the PCB, the sign of `∠V − ∠I` is recoverable
   from PFLT directly. Cheap, but requires a careful layout.
2. **Frequency-dither fallback.** Briefly shift TX frequency by
   ≈ 1 % and observe the sign of `d|Z|/dω`. Inductive loads → |Z|
   rises with frequency; capacitive → falls. Needs CAT cooperation
   and a small TX dwell.
3. **Two-point measurement** (calibration only). Sample twice with
   V/I orientations swapped via a relay-controlled reference path.
   Most accurate; used at install to cross-check the other two.

**M2 builds option 1 as primary, option 2 as fallback; option 3 is
reserved for the install calibration sweep.**

### §4.5 ADC + firmware sampling

- **Teensy 4.1 ADC** — 12-bit usable, 4 channels (V_fwd, V_rev,
  V_mag, V_phs).
- **DMA-driven sampling** at 10 kSPS per channel (40 kSPS
  aggregate).
- **Sample buffer** — ring of the last 100 ms of data per channel
  (1000 samples × 4 channels × 2 bytes = 8 KB SRAM).
- **Averaging window** — 500 ms default (`measure_window_ms` per
  [`TUNING.md`](TUNING.md) §4 thresholds) → 5000 samples per
  channel per readout. Standard error of the mean for
  noise-limited measurements: `σ_mean = σ_sample / √N` → 70×
  noise reduction. Plenty.
- **Output** — telemetry frames at 30 Hz (PROTOCOL.md §2.1)
  carrying the smoothed values.
- **Raw access** — a debug verb (`get_raw_adc`) returns the
  underlying sample ring for calibration sweeps.

### §4.6 Math — closed-form decoding

```
AD8307 forward → V_fwd → P_fwd (W)
                  via calibrated per-band slope / intercept (§4.9)

AD8307 reverse → V_rev → P_rev (W)
                  ρ   = √( P_rev / P_fwd )
                  SWR = (1 + ρ) / (1 − ρ)

AD8302 V_mag → 20·log₁₀(|Z| / R_s) (dB) → |Z| (Ω)
                  |Z| = R_s · 10^( V_mag_dB / 20 )

AD8302 V_phs (PFLT-disambiguated, §4.4) → ∠Z in degrees, full ±180°

R = |Z| · cos(∠Z)
X = |Z| · sin(∠Z)
```

**Sanity ranges** (post-calibration; gate at §4.10):

| Quantity | Expected range            | Gate-trip condition                                       |
|----------|---------------------------|-----------------------------------------------------------|
| `P_fwd`  | 0 – legal-limit W         | < 0.1 W → can't trust other readings yet                  |
| `P_rev`  | 0 – `P_fwd`               | `P_rev > P_fwd` → coupler swapped or directivity fail      |
| `SWR`    | 1.00 – ∞                  | `SWR > 20` → coupler / detector saturation likely          |
| `R`      | 5 – 5 000 Ω (per Doublet) | Outside band → load problem, not algorithm problem         |
| `X`      | −5 000 to +5 000 Ω        | Sign disambiguation must be wired (§4.4) before trusting   |
| `\|Z\|`  | 5 – 5 000 Ω               | `\|Z\| ≈ R_s` exactly → V/I sample paths swapped?          |

A gate trip emits `status warn:cal_missing`; the algorithm falls
through to the manual escape hatch ([`TUNING.md`](TUNING.md) §4.1).

### §4.7 Power supply + grounding

- **Clean +5 V** for the AD8307s and AD8302 from a low-noise LDO
  (e.g. ADP7142) off the main rail.
- **Star ground** on the detector board — analog and digital grounds
  joined at one point, at the ADC ground reference.
- **Per-chip decoupling** — 10 nF + 1 µF ceramic at every Vcc pin,
  physically adjacent to the pin.
- **Separate supply rail** from the motor drivers and relay coils —
  they switch large currents and inject noise on shared rails.

### §4.8 RF immunity on the detector board

Cross-references [`ARCHITECTURE.md §5.1.1`](ARCHITECTURE.md) for
the enclosure-level practices the firmware assumes. Detector-
board-specific additions:

- **Inner shield can** over the AD8302 and AD8307 if any one of
  them sits within λ/4 of the L-network at the highest band.
- **Feed-through caps** on every wire entering / leaving the
  detector board (V_fwd, V_rev, V_mag, V_phs out; +5 V in; ground).
- **Ferrite beads** on the ADC signal lines at the detector-board
  output.
- **Common-mode chokes** on the V and I sample lines from the
  coupler — the high-impedance V tap is the worst common-mode
  pickup victim on the whole board.

### §4.9 Calibration procedure (M2 deliverable)

Per-band calibration of every chip in the chain. The procedure
is documented step-by-step in [`HARDWARE.md`](HARDWARE.md) §6;
this section is the *what* and *why*.

1. **AD8307 slope / intercept** — drive the coupler input from a
   signal generator + calibrated step attenuator at known levels
   `[−50, −40, −30, −20, −10, 0] dBm` at band centre **and** band
   edges for every amateur band 1.8 – 54 MHz. Fit a line per band;
   store `(slope_mV_per_dB, intercept_dBm)` per band on the master.
2. **AD8302 |Z| calibration** — drive the coupler input at a known
   power into known resistive loads `[10, 25, 50, 100, 250, 1000] Ω`
   at each band centre. Verify V_mag matches expected
   `20·log₁₀(R/50)`; fit per-band offsets.
3. **AD8302 ∠Z calibration** — drive the coupler input into known
   reactive loads at each band: `[50−j100, 50−j50, 50, 50+j50,
   50+j100]`. Verify V_phs matches the expected angle of `(R+jX)/50`;
   fit per-band offsets. **Crucially** also verify the PFLT
   disambiguation correctly reports `sign(X)` on the ±jX pairs.
4. **Path-length residual** — with a pure 50 Ω load, V_phs should
   read 0° at every frequency. Any residual is V↔I path-length
   error; store as a per-band phase offset added to every reading.

Calibration is stored in TOML on the master and pushed to the
controller on connect (per [`../CLAUDE.md`](../CLAUDE.md) "Config").

### §4.10 Self-check + sanity gates (boot + runtime)

The controller runs three gates:

- **Boot baseline.** With no RF (Fwd reading below −70 dBm
  equivalent), log V_mag, V_phs, V_fwd, V_rev DC offsets. Persist
  in NVRAM as the per-installation zero. If the next-boot baseline
  drifts by more than ±50 mV from the persisted value, emit
  `status warn:cal_missing` — likely a temperature shift, a
  component change, or a cable fault.
- **Saturation detection.** Any ADC channel pegging above 95 % of
  full scale → `status error:cal_missing` with the channel ID and
  refuse to engage the L-network. Almost always means coupler
  attenuation insufficient or chip-side pad missing (§4.3).
- **Path consistency.** If the AD8302 reports `|Z| ≈ 50 Ω` but the
  AD8307s report `SWR > 1.5`, the V/I sample paths and the coupler
  disagree — likely path-length error or a swapped tap. Emit
  `status warn:cal_missing` and route to the manual escape hatch.

A clean detector chain reports **zero sanity warnings during a
48 h on-air soak at full legal limit** — the M5 commissioning
exit criterion for the chain.

### §4.11 What "live" looks like (M2 exit criteria)

Putting the parts list together — the chain is "live" when:

| Item                                                                  | Source / status              |
|-----------------------------------------------------------------------|------------------------------|
| Tandem-match coupler, directivity ≥ 25 dB across 1.8 – 54 MHz         | M2 build, [`HARDWARE.md`](HARDWARE.md) §5 |
| V (cap divider) + I (Stockton CT) sample taps, matched length         | M2 build                     |
| Two AD8307s with input pad, RC LPF, op-amp buffer per chip            | M2 build                     |
| AD8302 with PFLT brought out to GPIO for sign disambiguation          | M2 build                     |
| 4-channel ADC DMA loop at 10 kSPS, 500 ms averaging window            | M2 firmware (`hal::adc`)     |
| Per-band slope / intercept tables for AD8307                          | M2 cal, [`HARDWARE.md`](HARDWARE.md) §6 |
| Per-band offset tables for AD8302 V_mag, V_phs                        | M2 cal                       |
| Path-length residual offsets per band                                 | M2 cal                       |
| PFLT-based sign(X) disambiguation (primary, §4.4)                     | M2 firmware                  |
| Frequency-dither sign(X) fallback (secondary, §4.4)                   | M3 firmware (uses CAT)       |
| Boot baseline persisted to NVRAM                                      | M2 firmware                  |
| Sanity gates wired to `status warn:cal_missing` / `cal_missing`       | M2 firmware                  |
| Telemetry frames at 30 Hz with `r`, `x`, `swr`, `fwd_w`, `rev_w`,     |                              |
| `z_mag`, `z_phase` populated (PROTOCOL.md §2.1)                       | M2 firmware                  |
| Clean 48 h soak at full legal limit, zero sanity trips                | M5 commissioning exit gate    |

Until **every** item in this list is green, the algorithm runs
with the analytic seed disabled (Proposal C / coarse-grid behaviour
from [`TUNING.md`](TUNING.md)) — the chain has to earn its trust.

---

## §5 Balun design

To be filled in during M5 commissioning. Short-form spec from
[`../CLAUDE.md`](../CLAUDE.md):

- **1:1 or 4:1 current balun** on the tuner output (1:1 vs 4:1
  decided at M5 from measured ladder-line impedances — open
  decision #3 in [`PLAN.md`](PLAN.md)).
- **Ferrite**: Fair-Rite **43** mix (broadband 1 – 30 MHz) or **31**
  mix (lower bands and lower-loss high-Q operation), sized for
  ≥ 3 kW continuous dissipation safety margin.
- **Shield bonding**: shield bonded only at the bulkhead, never
  carried to the PCB ground.
- **On-air verification**: ferrite case temperature soak at
  full-legal-limit on the worst-case band (M5).
