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

Replace the manual tuner with an **automatic antenna tuner** for the
same Doublet — built around an **L-Match by default**, with **T-Match
and Pi-Match topologies also first-class supported** (selected once at
install time, declared to the firmware via configuration). The design
shares the station's existing automation pattern
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
- **Off-the-shelf carrier for Phase 1**: Phil Barrett's grblHAL-teensy
  4.x V2.09 board provides the Teensy-4.1 socket, 5 stepper-driver
  channels, opto-isolated digital inputs, relay drivers, and the PJRC
  Ethernet kit footprint. Avoids a custom-carrier PCB spin until the
  Phase-2 RF-commissioning decision. See
  [docs/HW-T41-CARRIER.md](docs/HW-T41-CARRIER.md).

## Non-goals (Phase 1)

Phase 1 (M0 – M6, the committed delivery) intentionally does **not**
target:

- T-Match / Pi-Match **auto-tune**. Phase 1 ships the L-Match search
  algorithm fully wired; T and Pi topologies are supported through the
  HAL and protocol layer for "drive each element to a commanded
  position", but the search/auto-tune work is deferred. T/Pi installs
  in Phase 1 are operator-driven via the encoders and *Save* button.
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

- **L-Match as default, T/Pi as opt-in** — L wins on efficiency
  (two reactive elements vs. three; lower insertion loss on near-50 Ω
  loads) and matches the operator's Doublet impedance ranges across
  all amateur bands when the C is switchable to either side via vacuum
  relays. T-Match and Pi-Match are supported in the firmware /
  protocol / HAL because the chosen carrier already has the stepper
  and relay channels to drive them, and a future build (or a different
  operator's antenna) may favour the wider impedance range of T or the
  band-edge behaviour of Pi. Selection is install-time, persisted on
  the controller.
- **Off-the-shelf Teensy-4.1 carrier (grblHAL-teensy-4.x V2.09)** for
  Phase 1 hardware. Spares a custom-PCB spin and brings the 5 stepper
  channels, 10 opto-isolated inputs, and 7 relay drivers we need
  (with headroom) on one board. The firmware-portability rule keeps
  the application layer carrier-agnostic, so the Phase-2 custom
  carrier (if commissioned) is a HAL swap. See
  [docs/HW-T41-CARRIER.md](docs/HW-T41-CARRIER.md).
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

## Bench-test learnings (to fold into design)

Notes captured during early hardware bring-up that the milestones (or
the HAL) need to absorb. Each one is currently a known-issue; none is
fixed in the eventual tuner-controller firmware yet.

- **Stepper pulses must be hardware-generated, not main-loop-polled.**
  Bench tests on an ESP32-C6 + TB6600 + NEMA 23 rig (firmware/esp32c6-
  stepper-test) used the standard AccelStepper `runSpeed()` pattern from
  the Arduino loop. As soon as the same loop also did `Preferences`
  writes (clean-shutdown position anchor, per CLAUDE.md invariant 3),
  WiFi event handling, or even ~2 ms of `Serial.println`, the pulse
  train hiccuped and the motor drifted visibly per revolution. Mitigation
  in the test sketch was throttling the NVS write interval to ~1 s and
  silencing the trace log; the proper fix — codified into the
  CLAUDE.md "Firmware portability rule" — is hardware pulse generation
  (FlexPWM on Teensy 4.1, TIM+DMA on STM32H743) with the main loop free
  to do persistence, networking, and UI work. Encoders catch the residual
  open-loop error per invariant 3, but step accuracy should still be
  achieved at the source.
- **TB6600 ENA polarity is inverted from the datasheet's plain reading.**
  Energising the ENA opto **disables** the driver — i.e. with common-
  cathode wiring (signal− to GND, signal+ to GPIO), `LOW` on ENA+ enables
  the motor and `HIGH` disables it. Documented in the test sketch but
  the HAL's relay-and-driver abstraction should make this explicit so
  the next person doesn't lose an hour to it.
- **TB6600 needs ≥ 5 µs pulse width.** AccelStepper's 1 µs default is
  too short for the TB6600 opto-coupled input. The HAL must expose a
  configurable minimum pulse width per driver; default safe values
  belong in `docs/HARDWARE.md` per driver family.
- **TB6600 holding-current chopper is audibly loud at high coil
  current.** At 3.5 A per phase the standstill PWM hiss is intrusive.
  The test sketch mitigates by auto-releasing ENA after a configurable
  idle timeout (3 s default, persisted across reboots), trading holding
  torque for silence — acceptable for shafts with mechanical detent /
  friction (roller inductor, vacuum cap), unsafe for back-drivable
  loads. This is a TB6600-specific symptom and not a production concern
  — CLAUDE.md hardware contract already specifies **TMC2209 in
  StealthChop** for the production tuner, which is essentially silent
  at standstill. The "release ENA when idle" behaviour is still worth
  porting to the production HAL as an opt-in low-power mode.
- **Open-network APs are unreliable on macOS clients.** During OTA
  bring-up the captive-portal setup AP needed a WPA2 password to be
  visible from a Mac; Windows 11 saw it open. Not load-bearing for the
  tuner (which is Ethernet, not WiFi) but worth remembering if any
  future bring-up boards use WiFi for first-time provisioning.
- **ESP32-C6 broadcasts 11ax (WiFi 6) by default**, which many phone /
  laptop clients filter out at scan time. If a WiFi-bring-up board is
  ever used again, force `WIFI_PROTOCOL_11B|G|N` on the AP interface.
  Captured for completeness even though tuner-controller is Ethernet.

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
