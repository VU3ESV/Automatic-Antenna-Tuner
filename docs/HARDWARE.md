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
| L actuator             | **NEMA 23 stepper, dual-shaft** + planetary gearbox → roller inductor          |
| C actuator             | **NEMA 23 stepper, dual-shaft** + planetary gearbox → vacuum-variable cap shaft|
| Stepper drivers        | **TB6600** ×2 (opto-isolated, active-LOW ENA, ≥ 5 µs STEP pulse width)         |
| Step pulse source      | **FlexPWM hardware** on the MCU (no loop-polled software stepping; see CLAUDE.md "Firmware portability rule" + `firmware/t41-stepper-test/src/flexpwm_stepper.h`) |
| Homing                 | Per-axis mechanical limit switch on V2.09 carrier opto inputs (no StallGuard — TB6600 doesn't expose stall feedback) |
| Position encoder       | **Optional**, per axis. Incremental quadrature optical, ≥ 2000 CPR, mounted on the NEMA 23's rear extended shaft (motor's front shaft drives the gearbox / coupling; rear shaft is free for encoder mounting). Absolute SSI is a per-axis drop-in alternative |
| Vacuum relays          | Gigavac G2 / G81 ×3 (Hi-Z / Lo-Z / bypass)                                    |
| Balun                  | 1:1 or 4:1 current, Fair-Rite 43 / 31 ferrite (decided at M5)                  |
| RF detector            | AD8302 (gain / phase) + AD8307 ×2 (Fwd / Rev)                                 |
| Directional coupler    | Stockton or Tandem-match, 50 Ω, ~50 dB coupling                                |
| Master MCU             | Raspberry Pi 4 / 5 + 7" / 10" capacitive touchscreen                          |
| Master input           | 2 × Adafruit ANO directional encoder (p/n 5735)                               |
| Transceiver link       | USB serial CAT (CI-V / Yaesu / Kenwood / K3 / K4)                              |

See [`../CLAUDE.md`](../CLAUDE.md) "Hardware contract" for the
per-subsystem rationale and detector / driver notes.

Power budget, voltage tree, and HV-bias supply for the vacuum
relays are TBD in §2.

### Cap-safety stack (why the BoM looks this way)

TB6600 has no stall-feedback signal (no StallGuard-equivalent), so a
vacuum-variable capacitor cannot be protected by trusting the step
counter. Protection is by layered envelopes — driver current
dip-switch set to motor-rated → `homed:false` motion refusal →
per-axis software soft limits (default 100 steps inside the
switch) → per-axis mechanical limit switch on the V2.09 carrier
opto inputs → optional rear-shaft encoder for post-move stall
reconciliation. Full enforcement contract in
[`../CLAUDE.md`](../CLAUDE.md) invariant #7; architectural rationale
and layer-by-layer behaviour in
[`ARCHITECTURE.md`](ARCHITECTURE.md) §5.2.5.

The per-build calibration that ties layers 3 and 4 together — the
step counts of `home_low`, `home_high`, and the `SAFE_MARGIN`
inside each limit switch — is an M1b.2 commissioning step and
lands in §9 below when it gets written up.

## Sections to fill in

These are written incrementally as the build proceeds:

- **§1 Vendor list and part numbers** — *M1b.2 hardware integration*.
- **§2 Power tree and enclosure mechanicals** — *M1b.2*.
- **§3 Schematic and PCB notes** — *M1b.2* (Teensy carrier) / *Phase 2*
  (STM32H743 custom carrier per
  [`../CLAUDE.md`](../CLAUDE.md) "MCU selection").
- **§4 Pin assignments and connector pinouts** — *M1b.2* (must align
  with `hal/*_teensy41.cpp` once those land).
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
- **§8 Balun spec and on-air verification** — *M5*. 1:1 vs 4:1
  decision based on measured impedance ranges (open decision #3 in
  [`PLAN.md`](PLAN.md)).
- **§9 Per-band memory build procedure (the M5 walk)** — *M5*.
  Operator procedure for stepping every band in 25 / 50 / 100 kHz
  buckets per [`ARCHITECTURE.md §5.4`](ARCHITECTURE.md) and saving
  a slot at each.
- **§10 Mechanical maintenance** — *M5+*. Roller-inductor end-stop
  inspection, vacuum-cap shaft coupling, gearbox backlash check.
