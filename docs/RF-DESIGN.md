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

## §4 Detector chain math

To be filled in during M2 detector commissioning. The chain
converts:

```
AD8307 forward channel → V_fwd → P_fwd (W)
        via calibrated per-band slope / intercept (set in M2 cal)

AD8307 reverse channel → V_rev → P_rev (W)
        ρ = √( P_rev / P_fwd )
        SWR = (1 + ρ) / (1 − ρ)

AD8302 V_mag → |Z| / R_s ratio in dB → |Z| (Ω)
        |Z| = R_s · 10^( V_mag_dB / 20 )

AD8302 V_phs → ∠Z in degrees, ambiguous in ±90° quadrant,
        full ±180° resolved by reactance-sign correlation with
        the Stockton-CT phase reference (see HARDWARE.md §5).

R = |Z| · cos(∠Z)
X = |Z| · sin(∠Z)
```

The per-band slope / intercept calibration is the M2 deliverable;
without it the analytic seed (§3) is too inaccurate to be useful
and the algorithm falls through to the manual-procedure escape
hatch ([`TUNING.md`](TUNING.md) §4.1).

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
