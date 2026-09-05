# Hardware

BoM, schematic notes, wiring, calibration. This document is the
single source of truth for the **physical build**; pin assignments,
component values, and connector pinouts that the firmware encodes
in `hal/` originate here.

## Status

Scaffold (2026-05-11). The hardware-contract table in
[`../CLAUDE.md`](../CLAUDE.md) "Hardware contract" is the current
short-form spec; this document fills it out with sourcing, part
numbers, photos, and calibration procedures as the build proceeds
(M1b.2 hardware integration → M2 detector commissioning → M5 RF
commissioning).

**2026-09-05 scope change.** The network is now **balanced** — Balanced
L-Network (default) or Balanced Pi-Network — with the 1:1 current balun
on the transceiver side; the two roller inductors are a **synchronized
pair on one motor**; and every motor couples **directly** to its
element, with a **3:1 GT2 belt-driven lead screw** alongside carrying
the home and max limit switches. Contract in
[`../CLAUDE.md`](../CLAUDE.md) "RF topology"; mechanical detail in
"Drive train" below.

The firmware portability rule in [`../CLAUDE.md`](../CLAUDE.md) "MCU
selection" means every pin assignment and connector pinout below
must be honoured by `hal/*_teensy41.cpp` *and* by a future
`hal/*_stm32h7.cpp` Phase 2 port. Keep the physical pinout
abstractions thin and named — e.g., `STEPPER_L_STEP_PIN`, not the
raw GPIO number — in `hal/board_*.h` headers, not hard-coded in
implementation files.

## BoM — short form

Long-form BoM with vendor links, part numbers, and substitute notes
lands during M1b.2 hardware integration. The current short form
(from [`../CLAUDE.md`](../CLAUDE.md) "Hardware contract") is:

| Subsystem              | Choice (default)                                                              |
|------------------------|-------------------------------------------------------------------------------|
| Tuner-side MCU         | Teensy 4.1 (Phase 1); STM32H743 (Phase 2 fallback)                            |
| Tuner-side carrier     | grblHAL Teensy 4.x V2.09 (Phil Barrett, CNC controller board)                  |
| Network topology       | **Balanced L-Network** (default; axes `L`-pair + `C`, relays K1/K2/K3) or **Balanced Pi-Network** (axes `C1` + `L`-pair + `C2`, relay K3). Declared via `set_topology` with an operator-chosen element→motor map |
| L axis                 | **Two matched roller inductors, one per line leg, turned in lock-step by one JMC iHSS60** (NEMA 24 integrated closed-loop stepper) — the motor shaft drives the first coil directly and the second coil is belted 1:1 off the same shaft (GT2). No rear shaft (driver sits there) |
| C axis / axes          | **JMC iHSS60** coupled directly to the vacuum-variable capacitor shaft (e.g. Jennings UCSL-1500, 10–1500 pF, 5 kV). One cap (Balanced L) or two (Balanced Pi) |
| Drive train (per axis) | **Direct coupling** motor → element (6400 steps per element revolution, full motor torque). **Limit mechanism:** small GT2 pulley on the motor shaft → belt → large pulley on a parallel lead screw (3:1, so the screw turns once per three element turns); a traveling nut block / wheel on the screw trips the home and max limit switches. Not in the torque path. See "Drive train" below |
| Limit switches         | **2 per axis (home, max)**, mounted beside the lead screw where the traveling block reaches them when the element is at its home / max position, a safe margin inside its mechanical stops; NC contacts in series to one carrier opto input per axis (fail-safe). 4 switches for Balanced L, 6 for Balanced Pi |
| Stepper drivers        | **Integrated in the iHSS60** (adopted 2026-09-05, replacing TB6600 ×2): 24–50 VDC, 4.5 A, 200 kHz, 6400 p/r DIP, PUL/DIR/ENA 5 V/24 V opto inputs, ALM + PED opto outputs; DIR ≥ 6 µs before PUL, ≥ 2.5 µs per pulse level. Wiring + final-build checks: [`HW-T41-PINMAP.md`](HW-T41-PINMAP.md) §2.2 |
| Step pulse source      | **FlexPWM hardware** on the MCU (no loop-polled software stepping; see CLAUDE.md "Firmware portability rule" + `firmware/t41-stepper-test/src/flexpwm_stepper.h`) |
| Homing                 | Per-axis lead-screw limit switches (traveling block) into V2.09 carrier opto inputs — the iHSS60 has no sensorless homing. Home is approached from one direction at low speed so belt / nut backlash cannot move the reference. The drive's ALM (following-error trip) and PED (arrived) outputs are the stall / arrival signals — final-build check, [`HW-T41-PINMAP.md`](HW-T41-PINMAP.md) §2.2 |
| Position encoder       | **Inside the iHSS60** — its optical encoder closes the loop within the drive; the controller sees only ALM / PED. No rear shaft is available for an external encoder. External incremental / absolute-SSI remains a per-axis HAL option only for a non-integrated motor (Phase-2 fallback) |
| Vacuum relays          | Gigavac G2 / G81 or Kilovac H-series, **two-pole per switch** (2 relays with paralleled coils, or one DPST/DPDT): K1 Hi-Z, K2 Lo-Z, K3 bypass changeover — 6 contacts for Balanced L; K3 only (2 contacts) for Balanced Pi |
| Balun                  | **1:1 Guanella current balun on the transceiver side** (50 Ω unbalanced → 50 Ω balanced, ahead of the network), Fair-Rite 43 / 31 ferrite. Ratio is no longer an M5 decision |
| RF detector            | AD8302 (gain / phase) + AD8307 ×2 (Fwd / Rev)                                 |
| Directional coupler    | Stockton or Tandem-match, 50 Ω, ~50 dB coupling                                |
| Master MCU             | Raspberry Pi 4 / 5 + 7" / 10" capacitive touchscreen                          |
| Master input           | 2 × Adafruit ANO directional encoder (p/n 5735); a 3rd for Balanced Pi        |
| Transceiver link       | USB serial CAT (CI-V / Yaesu / Kenwood / K3 / K4)                              |

See [`../CLAUDE.md`](../CLAUDE.md) "Hardware contract" for the
per-subsystem rationale and detector / driver notes.

Power budget, voltage tree, and HV-bias supply for the vacuum
relays are TBD in §2.

## Drive train — direct coupling + belt-driven lead screw for the limit switches (decided 2026-09-05)

Every reactive-element axis uses the same mechanical arrangement,
replacing the planetary gearbox previously assumed:

```
                        ┌── direct coupling ── ELEMENT SHAFT (roller inductor / vacuum cap)
iHSS60 ── motor shaft ──┤
(6400 p/r)              └── small GT2 pulley ══ belt (3:1) ══ large GT2 pulley ── LEAD SCREW (⅓ speed)
                                                                                        │
                                                    [SW_home] ── traveling nut block / wheel ── [SW_max]
                                                    (block is guided so it translates, not rotates)

L axis only: a 1:1 GT2 belt off the same motor shaft drives the second roller inductor.
```

- **Motor → element: direct.** The motor shaft is coupled straight to
  the element shaft, so one motor turn is one element turn — 6400 steps
  per element revolution with the full motor torque at the element.
  Nothing in the belt path can move the element or affect its position.
- **Motor → lead screw: 3:1 belt.** A small GT2 pulley on the motor
  shaft drives a large pulley on a parallel lead screw, so the screw
  turns once for every three element turns. The lead screw is **not in
  the torque path**; it exists only to carry the limit switches, and
  the reduction compresses the block travel so a long-travel element
  fits a short screw.
- **Traveling block = turns counter for the end stops.** A nut block
  (or wheel) rides the lead screw and is guided so that it translates
  rather than rotates; its travel is `element_turns / 3 × lead`. The
  **home** and **max** switches are mounted beside the screw so that
  the block trips them when the element is at its home / max position,
  a safe margin (`SAFE_MARGIN`, invariant 7) inside the element's own
  mechanical stops. The switches, not the element, take the contact.
  The screw needs a hard stop or enough thread that the nut can never
  run off.
- **Position truth is unchanged** (invariant 3): the position source
  is still the pulse counter validated by the drive's ALM / PED; the
  block only provides the home reference and the end stops. Belt
  backlash and switch hysteresis therefore affect only home
  repeatability, so home is always approached from the same direction
  at low speed.
- **Home repeatability is set by lead and switch.** The block moves
  only `lead / 3` per element turn, so a switch that repeats to `s` mm
  re-homes the element to `s / (lead / 3)` turns. A larger lead or a
  precision / optical switch improves it — see the table. Memory
  recall across power cycles is only as good as this figure unless the
  clean-shutdown NVRAM anchor (invariant 3) is valid.
- **L axis.** The first roller inductor is on the motor shaft directly;
  the second is belted 1:1 off the same shaft with GT2 pulleys, and the
  lead screw is belted 3:1 off it as on every other axis. The two coils
  must be a matched pair (same turns, pitch, and form) and phased so
  that both rollers travel the same way; per-leg tracking is measured
  at M1b.2 (PLAN.md).
- **Limit-switch wiring.** The two switches of an axis are NC contacts
  in series to one carrier opto input (Sig → SW_home → SW_max → Gnd,
  per [`HW-T41-PINMAP.md`](HW-T41-PINMAP.md) §2.1). Either switch
  opening, or a broken cable, reads as "at limit"; the firmware knows
  the travel direction, so it knows which end it hit. Input budget for
  Balanced Pi: 3 limit + 3 ALM + 3 PED = 9 of the carrier's 10 opto
  inputs.

Arithmetic (6400 p/r drive, direct element drive, 3:1 belt to the lead
screw):

| Quantity                                                            | Value                                                   |
|---------------------------------------------------------------------|---------------------------------------------------------|
| Steps per element revolution                                        | 6400                                                    |
| Element speed at 6400 pps / bench-saved 25 600 pps / ≈ 100 kHz ceiling | 1 / 4 / 15.6 rev/s                                   |
| Full travel of a 40-turn element at 6400 / 12 800 / 25 600 pps      | 40 s / 20 s / 10 s                                      |
| Lead-screw turns per element turn                                   | 1/3                                                     |
| Block travel per element turn (= lead / 3)                          | Tr8×2: 0.67 mm; Tr8×8: 2.67 mm                          |
| Block travel for a 40-turn element                                  | Tr8×2: 27 mm; Tr8×8: 107 mm                             |
| Home repeatability with a 0.1 mm mechanical microswitch             | Tr8×2: ≈ 0.15 turn ≈ 960 steps; Tr8×8: ≈ 0.04 turn ≈ 240 steps |
| Same with a 0.02 mm precision snap-action / optical switch          | Tr8×2: ≈ 190 steps; Tr8×8: ≈ 50 steps                   |

Open mechanical items, to land in §2 / §4 as the build proceeds:
lead-screw lead and length per element (block travel must fit the
enclosure; a larger lead buys home repeatability); pulley tooth counts
(e.g. 20T / 60T) and belt length; belt tensioner / idler; lead-screw
bearings and the block's anti-rotation guide; limit-switch type
(mechanical microswitch vs precision snap-action vs optical) against
the repeatability table; motor-to-element coupling (rigid vs Oldham);
each element's rated maximum shaft speed, which sets the production
speed limit now that there is no reduction; inductor-pair tracking
acceptance (PLAN.md M1b.2).

### Risks of the belt-driven lead-screw limit mechanism

The switches sense the lead screw, not the element, and only the belt
ties the two together. That indirection is the root of most of the risks
below. Mitigations are a mix of mechanical choices, firmware rules (now
binding through [`../CLAUDE.md`](../CLAUDE.md) invariant 7) and
commissioning checks (PLAN.md M1b.2 / M5). The soft limits and the
drive's ALM trip remain the primary protection; the switches are the
fallback, and the mechanism must never be the only thing between the
motor and the element's mechanical stop.

1. **Belt path fails — the switches go silent.** A snapped or thrown
   belt, a loose pulley set-screw or a sheared key leaves the element
   turning under direct drive while the block stands still, so neither
   switch ever trips. The switches cannot see this failure themselves.
   *Effect:* the element can be driven into its own stop (vacuum-cap
   bellows, roller-inductor end) at full motor torque.
   *Mitigation:* soft limits with `SAFE_MARGIN` stay primary; the P16
   following-error trip catches a hard stall; **homing travel
   watchdog** — a homing move that has travelled the commissioned
   distance plus margin without a trip aborts with `homed:false`;
   clamp-type or D-shaft pulleys, a belt tensioner, and a §10
   inspection interval.
2. **Tooth skip shifts both trip points.** A GT2 belt does not slip,
   but a slack belt can skip teeth under a jolt or when the block
   binds. One skipped tooth (2 mm pitch) moves the screw by one tooth
   of the large pulley — with 20T / 60T pulleys that is 1/20 element
   turn = 320 steps at both switches.
   *Effect:* the home reference moves, so every memory slot recalls
   the wrong position; if it moves outward, the switch no longer
   protects the stop.
   *Mitigation:* **home→max span check** — the step count between the
   two switches is commissioned once and re-measured on every full
   home (or on demand); a change beyond tolerance flags the mechanism
   and blocks recall until re-commissioned; correct tension and an
   idler; keep the screw's load near zero.
3. **Backlash and switch hysteresis limit home repeatability.** Belt
   tooth clearance, nut backlash and the switch's differential travel
   add up, and because the block moves only lead / 3 per element turn a
   small linear error is a large angular one (table above).
   *Effect:* recall error across power cycles; `SAFE_MARGIN` erosion.
   *Mitigation:* approach home from one direction at low speed; a
   larger lead; a precision or optical switch; prefer the
   clean-shutdown NVRAM anchor to re-homing; measure and record the
   repeatability in steps at M1b.2.
4. **A trip at speed over-runs the switch.** With the bench ramp
   (25 600 steps/s²) a ramped stop from 25 600 pps takes 12 800 steps
   (two element turns), from 12 800 pps 3200 steps (half a turn), from
   6400 pps 800 steps. The default `SAFE_MARGIN` of 100 steps covers
   none of these.
   *Effect:* the element reaches its stop after the switch has
   tripped; the block bottoms the switch lever or stalls the screw and
   the belt skips (risk 2).
   *Mitigation:* on a limit trip **cut pulses immediately** — no
   deceleration ramp (a following-error ALM is an acceptable outcome);
   cap speed inside `2 × SAFE_MARGIN` of a soft limit; size
   `SAFE_MARGIN` to at least the stopping distance at homing speed;
   give the block a **cam / ramp face** so the switch stays actuated
   over the whole over-travel instead of bottoming a lever.
5. **Switch stuck closed, or released by over-travel.** NC-in-series is
   fail-safe for an open cable (reads "at limit"), but a welded or
   jammed switch reads "clear", and a block that pushes past a
   short-lever switch releases it again while the element is beyond
   the limit.
   *Effect:* an unprotected end, or a limit that clears itself.
   *Mitigation:* **latch limit events in firmware** — edge plus travel
   direction while moving, never level-sampled; cam-actuated or
   roller-lever switches with long over-travel; exercise both switches
   at every commissioning and periodically via `home`.
6. **One input for two ends.** On a shared NC-in-series input the
   firmware knows which end tripped only from the travel direction. At
   power-up with the block already on a switch it cannot tell home
   from max.
   *Effect:* a wrong guess drives the element into the stop it is
   already at.
   *Mitigation:* **bounded pull-off** — move toward home a few hundred
   steps; if the input does not clear, reverse; use the NVRAM position
   record to choose the first direction. Balanced L can afford separate
   inputs (4 limits + 2 ALM + 2 PED = 8 of 10); sharing is only forced
   on Balanced Pi.
7. **Block binds or runs off the screw.** Dirt, misalignment or
   corrosion in the anti-rotation guide, or thread that ends just past
   the switches, stalls the screw; the belt then skips (risk 2) or the
   motor trips ALM.
   *Effect:* a lost mechanism or a spurious ALM lockout.
   *Mitigation:* thread longer than travel plus margins, with a
   compliant end stop; a low-friction guide; enclosure sealing;
   inspection.
8. **Mis-set switch position.** A switch mounted so the block reaches
   it after the element's stop, rather than before, protects nothing.
   *Effect:* as risk 1 at that end.
   *Mitigation:* commissioning procedure — with the motor unpowered,
   hand-turn the element to its stop, back off `SAFE_MARGIN` turns and
   fix the switch to trip there; verify by slow powered approach;
   record the step positions to NVRAM.
9. **RF pickup on the switch wiring.** The wiring runs inside the RF
   enclosure; rectified RF at the opto input can read as a trip during
   TX.
   *Effect:* a spurious lockout (safe, but it blocks recall until
   cleared) or a false latched event.
   *Mitigation:* twisted or shielded pairs, a ferrite at the entry, RC
   plus firmware debounce; latch limit events only while an axis is
   moving toward that limit (motion is already forbidden under RF by
   invariant 1).
10. **Same belt technology on the L pair.** The 1:1 belt between the
    two roller inductors has the same skip modes; a skip there
    silently unbalances the two legs.
    *Effect:* leg-current imbalance with no motion symptom.
    *Mitigation:* tension and clamp pulleys as above; the M5 leg-current
    check; index marks on both coil shafts for a quick visual
    alignment check.

### Memory reliability with direct coupling

Direct coupling is the best arrangement for memory recall as far as the
motor's part of the chain goes, but the coupling is not what limits
recall across power cycles — the position anchor is.

- **Why it helps.** With no belt or gearbox between the drive's encoder
  and the element, a given step count lands the element at the same
  angle from either direction: no backlash to compensate, no tooth to
  skip. The iHSS60 closes its loop on the motor shaft, which is now the
  element shaft, so while ALM is clear and PED confirms arrival the
  stored count *is* the element position. Resolution is not a concern:
  one step on a 1500 pF, 40-turn capacitor is ≈ 0.006 pF, far finer
  than the capacitor's own mechanical hysteresis.
- **What still limits recall.** The stored count is only as good as
  the reference it was measured from:

  | Anchor                           | Accuracy                                                   | Weakness                                            |
  |----------------------------------|------------------------------------------------------------|-----------------------------------------------------|
  | Clean-shutdown NVRAM record      | Exact                                                      | Assumes the shaft did not move while unpowered      |
  | Re-home on the lead-screw switch | 240 – 960 steps with a 0.1 mm microswitch (lead-dependent) | Set by the belt-driven lead screw, not by the drive |

  On the capacitor example the re-home figures are ≈ 1.4 – 5.6 pF:
  negligible on 160 – 20 m, but on 10 m / 6 m, where the matching
  capacitance is tens of pF, a re-homed recall may land at a visibly
  higher SWR and need the hill-climb to finish.
- **Where direct coupling is slightly worse.** An unpowered stepper
  holds the shaft only by detent torque and coupling friction. A vacuum
  capacitor's bellows can exert a back-torque, so the shaft may creep
  during a true power-off and the NVRAM anchor goes stale without the
  firmware knowing. A gearbox would have resisted this; a belt in the
  torque path would not have helped much, so this is not an argument
  against direct drive — only a reason to keep the drives enabled
  whenever the tuner has power and to confirm the NVRAM count against
  the home switch after any cold start (required by
  [`../CLAUDE.md`](../CLAUDE.md) invariant 3). The creep is measured at
  M1b.2 (PLAN.md).
- **Making the anchor independent of homing (Phase-2 options).** A
  cheap absolute sensor on the lead-screw block — a linear
  potentiometer or magnetic linear sensor along the block's travel —
  gives a coarse absolute position at power-up without any motion, so
  the NVRAM anchor can be validated or rejected on boot. If a roller
  inductor shaft has a free end, a multi-turn absolute encoder there
  does the same for the L axis (the HAL already allows a per-axis
  external position source, [`ARCHITECTURE.md`](ARCHITECTURE.md)
  §5.2). Open decision 11 in [`PLAN.md`](PLAN.md).

### Cap-safety stack (why the BoM looks this way)

The iHSS60 closes its position loop internally but reports only
pass/fail (ALM, PED), so a vacuum-variable capacitor still cannot be
protected by trusting the pulse counter alone. Protection is by
layered envelopes — drive currents (P8/P9) and following-error limit
(P16) set for the geared load via the HISU tool → ALM into a carrier
opto input (fail-safe P10 = 1), any trip stops pulses and clears
`homed` → `homed:false` motion refusal → per-axis software soft
limits (default 100 steps inside the switch) → per-axis lead-screw
limit switches (both ends) on the V2.09 carrier opto inputs → PED
confirmation
before a move is recorded complete. Wiring and open final-build checks
in [`HW-T41-PINMAP.md`](HW-T41-PINMAP.md) §2.2.
Full enforcement contract in
[`../CLAUDE.md`](../CLAUDE.md) invariant #7; architectural rationale
and layer-by-layer behaviour in
[`ARCHITECTURE.md`](ARCHITECTURE.md) §5.2.5.

The per-build calibration that ties layers 3 and 4 together — the
step counts of `home_low`, `home_high`, and the `SAFE_MARGIN`
inside each limit switch — is an M1b.2 commissioning step (the
switches are positioned physically along the lead screw, then their
trip points recorded in steps) and lands in §9 below when it gets
written up.

## Sections to fill in

These are written incrementally as the build proceeds:

- **§1 Vendor list and part numbers** — *M1b.2 hardware integration*.
- **§2 Power tree and enclosure mechanicals** — *M1b.2*. Includes the
  per-axis drive-train envelope: lead-screw length and block travel,
  pulley / belt layout, and the L-axis 1:1 belt to the second coil.
- **§3 Schematic and PCB notes** — *M1b.2* (Teensy carrier) / *Phase 2*
  (STM32H743 custom carrier per
  [`../CLAUDE.md`](../CLAUDE.md) "MCU selection").
- **§4 Pin assignments and connector pinouts** — *M1b.2* (must align
  with `hal/*_teensy41.cpp` once those land).
- **§4a Closed-loop driver feedback inputs (ALM / PED)** — *final
  build*. Carrier opto-input electrical facts and the selected wiring
  are already in [`HW-T41-PINMAP.md`](HW-T41-PINMAP.md) §2.1–2.2; this
  section records the measured LED current, chosen P10/P14 polarity,
  P16 following-error limit, and per-driver timing table once decided.
- **§5 Tandem-match coupler build notes** — *M2*. Toroid choice,
  primary / secondary turns, port-isolation measurement procedure,
  directivity sweep across 1.8 – 54 MHz on the network analyzer.
  See [`RF-DESIGN.md`](RF-DESIGN.md) §4.2 for the signal-flow role
  and §4.11 for the full M2 "chain is live" checklist.
- **§6 Detector calibration procedure** — *M2*. Step-by-step bench
  procedure for the per-band calibration sweeps documented in
  [`RF-DESIGN.md`](RF-DESIGN.md) §4.9: AD8307 slope / intercept
  against a calibrated signal generator + step attenuator; AD8302
  |Z| against `[10, 25, 50, 100, 250, 1000] Ω` resistive loads;
  AD8302 ∠Z against `[50−j100, 50−j50, 50, 50+j50, 50+j100]`
  reactive loads; path-length residual against a precision 50 Ω
  load. The closed-form decode and the sanity gates that consume
  these cal values live in [`RF-DESIGN.md`](RF-DESIGN.md) §4.6 and
  §4.10 respectively.
- **§7 RF immunity practices in this enclosure** — *M2 / M5*. Cross-
  references [`ARCHITECTURE.md §5.1.1`](ARCHITECTURE.md) for the
  enclosure-level practices the firmware assumes.
- **§8 Balun spec and on-air verification** — *M5*. 1:1 Guanella
  current balun on the transceiver side (ratio fixed by the balanced
  topology, 2026-09-05); verify leg-current balance and ladder-line
  common-mode current at power.
- **§9 Per-band memory build procedure (the M5 walk)** — *M5*.
  Operator procedure for stepping every band in 25 / 50 / 100 kHz
  buckets per [`ARCHITECTURE.md §5.4`](ARCHITECTURE.md) and saving
  a slot at each.
- **§10 Mechanical maintenance** — *M5+*. Belt tension and wear,
  lead-screw nut backlash, limit-switch position check at both ends,
  home→max span check via `home` (risk register above),
  inductor-pair tracking re-check, lead-screw-to-element coupling,
  roller-inductor contact inspection.
