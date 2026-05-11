# Proposal — Automatic Antenna Tuner

Following the same shape as
[LP-100A-Server's PROPOSAL.md](https://github.com/VU3ESV/LP-100A-Server)
so the two station projects can be reviewed against the same template.

## Problem

VU3ESV operates a **Doublet antenna on 460 / 600 Ω open-wire ladder
line, 160 m – 6 m**. The feedline impedance at the shack end varies
wildly across bands — sometimes near 50 Ω, more often hundreds or
thousands of ohms with significant reactance — and drifts slightly
with weather, ice, and nearby objects. Every band change requires
re-matching the feedline to the 50 Ω transceiver. The status quo is:

- A manual L-tuner with knobs the operator twiddles by ear.
- Tuning takes 30 – 90 s per band; band changes are friction-heavy.
- No memory: the same band tuned yesterday is tuned again from scratch.
- The operator is the safety interlock; an inattentive key-down during
  re-tune risks the components.

## Goal

Replace the manual tuner with an **automatic L-network tuner** for the
same Doublet, sharing the station's existing automation pattern
([LP-100A-Server](https://github.com/VU3ESV/LP-100A-Server)): one Go
process on a Raspberry Pi owns the hardware, fans out telemetry over
WebSocket, and accepts named control verbs from a browser / touchscreen
UI.

**Functional goals**

- Single-band tune in **< 4 s** on memory recall; **< 60 s** cold-start.
- **No mechanical motion under RF** — firmware refuses moves whenever
  forward power ≥ a configurable threshold (default 5 W). Belt + suspenders
  with the master-side check.
- **Memory keyed by frequency bucket within band** so the recall path
  is fast and stays accurate as the antenna environment drifts.
- **Two-controller architecture** — tuner-side at the enclosure
  (real-time motor + measurement); master-side in the shack (UI, CAT,
  memory). Ethernet between, WebSocket JSON.
- **Operator-confirmed memory writes** — no silent overwrites.

**Non-functional goals**

- LAN-only deployment, no internet exposure.
- Single static Go binary on the master, single firmware image on the
  Teensy 4.1 (Phase 1) or STM32H743 (Phase 2 fallback).
- One uniform shape across all station services so debug muscle memory
  transfers between LP-100A-Server and this project.

## Non-goals (Phase 1)

Phase 1 (M0 – M6, the committed delivery) intentionally does **not**
target:

- T-network or pi-network tuning. L-network only — see
  [CLAUDE.md](CLAUDE.md) "RF topology" for why.
- Balanced-line tuning without a balun. The 1:1 current balun on the
  output is part of the architecture, not an option.
- Cloud relay or NAT traversal. LAN-only, like LP-100A-Server.
- Long-term logging / charting of match data. A separate InfluxDB
  subscriber can do this if wanted, out of scope here.
- Replacing the LP-100A as the station reference meter. The tuner has
  its own SWR/Z chain for its own control loop; LP-100A remains
  authoritative for power and SWR display.

## Phase 2 extensions (documented, not yet scheduled)

The Phase 1 architecture is intentionally extensible. The following
extensions are designed but gated on Phase 1 commissioning being
clean — see [docs/EXTENSIONS.md](docs/EXTENSIONS.md) for the full
scope, architecture options, and evolution path:

- **Multiple antennas** — Doublet stays; HexBeam, Vertical, Yagi etc.
  added behind an `antenna_id` abstraction.
- **Multiple transceivers** — two or more rigs share the antenna
  farm, each with its own routing rules per `(radio, band)`.
- **SO2R** — Single Operator Two Radios contesting workflow, with
  the interlock + BPF requirements documented in EXTENSIONS.md §5.

The 4O3A TGXL / Antenna Genius family is the design reference for
Phase 2 SO2R behaviour.

## Design rationale

- **L-network over T-network** because L is theoretically efficient
  (only two reactive elements, both lossless in the limit) and matches
  the user's Doublet impedance ranges across all amateur bands when
  the C is switchable to either side via vacuum relays.
- **Two-controller split** because the safety-critical real-time path
  (motor + RF lockout + measurement at ~10 kSPS) belongs on a fast MCU
  next to the hardware, and the human-facing path (touchscreen GUI, CAT
  polling, memory store) belongs on a Pi with a real OS and disk.
- **WebSocket JSON over Ethernet** because it's the same shape as the
  station's existing LP-100A-Server, with the same client-fan-out
  semantics — one less protocol to debug. See
  [docs/PROTOCOL.md](docs/PROTOCOL.md) for the wire spec.
- **Incremental quadrature encoders + NVRAM anchor** as the position
  truth rather than absolute SSI: cost, wiring, and industrial track
  record all point to incremental. See
  [docs/ARCHITECTURE.md §5.2](docs/ARCHITECTURE.md) for the trade-off.
- **Sim HAL before real drivers** because the master ↔ controller
  protocol can be exercised end-to-end (verb dispatch, RF lockout,
  state fan-out, UI controls) without waiting on the hardware build —
  see [docs/ARCHITECTURE.md §5.1.3](docs/ARCHITECTURE.md) for the
  swap-in path.
- **Hybrid auto-tune algorithm (memory + analytic + hill-climb)** as
  the tuning strategy: fast on the common case, robust on the cold
  case, self-improving over weeks of use. See
  [docs/TUNING.md](docs/TUNING.md) for the four candidate strategies
  considered and the rationale for the choice.

## Status

Milestone-by-milestone state lives in [docs/PLAN.md](docs/PLAN.md).
Current state (2026-05-11):

- **M0** scaffolding: ✅
- **M1a** software scaffold: ✅
- **M1b.1** network bridge (Teensy → master → browser): ✅
- **M1b.2** software half (sim HAL + verb dispatch + Operate panel): ✅
- **M1b.2** real driver wiring, **M2** measurement chain, **M3** master
  core (CAT / ANO encoders / SQLite memory), **M4** auto-tune algorithm,
  **M5** RF commissioning, **M6** hardening: pending hardware build.
