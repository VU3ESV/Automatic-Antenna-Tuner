# Automatic Antenna Tuner

A two-controller automatic antenna tuner for a **Doublet fed with 460 Ω /
600 Ω open-wire ladder line, 160 m – 6 m**. Supports **L-Match (default),
T-Match, and Pi-Match** network topologies; the chosen topology is declared
to the firmware at install time and persisted on the controller. A **tuner
controller** sits at the tuner enclosure (drives 2 or 3 stepper-actuated
reactive elements — roller inductors, vacuum-variable capacitors, depending
on topology; switches **vacuum relays** for topology-specific path
selection; samples the RF detector chain). A **master controller** lives
in the shack: a Raspberry Pi with a touchscreen GUI, two Adafruit ANO
directional encoders, a CAT link to the transceiver, and a SQLite memory of
optimal element-positions per (topology, band, frequency). The two
controllers talk over **Ethernet using a WebSocket JSON protocol**,
following the [LP-100A-Server](https://github.com/VU3ESV/LP-100A-Server)
pattern (one process owns the hardware, fans out telemetry, accepts named
control verbs).

This `CLAUDE.md` is the **contract reference** — the hardware truths, the
network protocol, and the invariants a future change must not break. Design
rationale and milestones live in [PROPOSAL.md](PROPOSAL.md) and
[docs/PLAN.md](docs/PLAN.md); the architectural deep-dive is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Repository layout

```
Automatic-Antenna-Tuner/
├── CLAUDE.md            # this file — protocol + invariants
├── README.md            # build / install / operate
├── PROPOSAL.md          # problem, goals, design, milestones
├── docs/
│   ├── ARCHITECTURE.md  # system + RF + software architecture
│   ├── PLAN.md          # milestone-based implementation plan
│   ├── HARDWARE.md      # BoM, schematic notes, wiring
│   ├── PROTOCOL.md      # full WebSocket JSON protocol spec
│   ├── RF-DESIGN.md     # network-topology theory, component sizing, detector chain
│   └── HW-T41-CARRIER.md # plan: grblHAL-teensy 4.x V2.09 board as the carrier
├── firmware/
│   └── tuner-controller/  # MCU firmware (C/C++ on Teensy 4.1 or STM32H7)
└── master/
    └── tuner-master/      # Go server + embedded web UI, runs on the Pi
```

## RF topology (configurable)

This codebase supports three network topologies. The L-Match is the
**default and most-developed** path; T-Match and Pi-Match are first-
class supported but built second. The chosen topology is declared
to the firmware once at install time via the `set_topology`
configuration verb and committed to NVRAM; changing topology is
treated as a hardware change and requires a power cycle.

A 1:1 or 4:1 **current balun** on the output couples the unbalanced
network to the balanced ladder line; the balun is fixed (not switched)
and is independent of topology choice.

### L-Match (2 reactive elements, default)

Reconfigurable single L + single C, with a vacuum-relay-driven selector
that places the capacitor on either the **source side** (for Zload < 50 Ω)
or the **load side** (for Zload > 50 Ω).

```
                          ┌──── relay K1 (HI-Z position) ────┐
                          │                                  │
TX ── SWR/Z ── series L ──┼─────────────────────────── BALUN ──── ladder line
                          │                                  │
                          └── shunt C ── relay K2 (LO-Z pos) ┘
```

- **HI-Z mode (Zload > 50 Ω):** K1 closed, K2 open. C is in shunt **across
  the load side** (after L).
- **LO-Z mode (Zload < 50 Ω):** K1 open, K2 closed. C is in shunt **across
  the source side** (before L).
- **BYPASS:** a third relay (K3) hard-shorts the network so the
  transceiver feeds the balun directly. Used during startup and any time
  positions are being changed.

### T-Match (3 reactive elements)

Two series capacitors with a shunt inductor between them. Wide
impedance range; higher loss than L on near-50 Ω loads.

```
TX ── SWR/Z ── C₁ ──┬── C₂ ────── BALUN ── ladder line
                    │
                    L (shunt to GND)
```

All three elements are stepper-driven (typically two vacuum-variable
capacitors and one roller inductor). No source/load-side selector
relay is required — the T topology is symmetric — but the **BYPASS
relay (K3)** is retained per invariant 2.

### Pi-Match (3 reactive elements)

Series inductor with two shunt capacitors. Common in higher-power
applications; behaves well at the band edges.

```
TX ── SWR/Z ──┬── L ──┬────── BALUN ── ladder line
              │       │
              C₁      C₂
              │       │
             GND     GND
```

Same actuator and relay set as T-Match; only the wiring differs.

### Topology vs firmware

The HAL exposes axes 0/1/2 as opaque handles; the application layer
reads a topology block (served by the master, persisted on the
controller) that names each element, gives its type (L / C), connects
it to a HAL axis, and stores its calibration and limits. The control
loop, tuning algorithm, and memory schema are topology-aware; pin
maps and motor counts live in `firmware/tuner-controller/hal/board/`
and are decoupled from the topology choice — see
[docs/HW-T41-CARRIER.md](docs/HW-T41-CARRIER.md) for the planned
mapping on the grblHAL Teensy 4.x V2.09 carrier.

T/Pi-Match support is in scope but **L-Match is the only topology
exercised end-to-end through M6**; T/Pi auto-tune development is a
Phase-2 deliverable (see [docs/PLAN.md](docs/PLAN.md)).

## Invariants (do not violate)

1. **No mechanical motion while RF is present.** The tuner controller MUST
   refuse any stepper-move command if forward power exceeds
   `safety.tx_lockout_w` (default 5 W). The master MUST also refuse to send
   move commands while CAT reports the rig PTT-keyed. Belt + suspenders.
2. **Bypass on power-up.** K3 latches the network out of circuit on every
   boot until the controller has read both encoders, confirmed plausible
   positions, and received an explicit "engage" command.
3. **Encoders are the position truth, anchored to a known reference.**
   Stepper step counts are open-loop and drift (stalls, end-stop hits,
   power-cycle while moving); on boot and after every move the controller
   reads the encoder counts and treats those as the actual position, with
   the step counter reset to match. An incremental encoder's count is
   anchored to either (a) the last `home` routine within this power
   cycle, or (b) a clean position record persisted to NVRAM at last
   `bypass=true` shutdown; if neither is valid the controller refuses
   motion verbs except `home` and reports `homed:false` in `state`. An
   absolute SSI encoder, if fitted to an axis, is self-anchoring and
   skips this dance. Both pathways live behind the same HAL —
   application code is encoder-kind-agnostic. See
   [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §5.2.
4. **Memory writes are explicit.** The master never silently overwrites a
   memory slot. A user adjustment with the encoders updates a working
   position; persisting it to the slot for `(band, freq_bucket)` requires
   the operator to press *Save* (or to opt-in via an `auto_save` setting
   that is off by default).
5. **One client owns control at a time.** The tuner controller accepts
   commands from any connected client, but every accepted command broadcasts
   a `state` frame to every client so the GUI cannot silently diverge. If a
   second client is connected, the master GUI shows a "shared control"
   indicator.
6. **Frequency drives recall, not band.** Memory is keyed by frequency
   bucket within band (default 25 kHz on 160/80/60/40 m, 50 kHz on
   30/20/17/15/12 m, 100 kHz on 10/6 m). Falling back to band-only
   defaults is a last resort.

## WebSocket protocol (summary; full spec in [docs/PROTOCOL.md](docs/PROTOCOL.md))

Same shape as LP-100A-Server: JSON over a single `/ws` endpoint, `type`
discriminator, monotonic `seq` on server→client frames, named verbs only
(no raw bytes exposed to clients).

**Server → client**

| `type`       | Purpose                                                                            |
|--------------|------------------------------------------------------------------------------------|
| `telemetry`  | Live measurements: `fwd_w`, `rev_w`, `swr`, `z_mag`, `z_phase`, `r`, `x`, `mode`.  |
| `state`      | Mechanical/relay state: `topology`, `axes[]` (each with `name`, `steps`, `enc`), `side` (L-Match only), `bypass`, `homed`, `last_move`. |
| `memory`     | Result of memory lookup/save: `topology`, `band`, `freq_hz`, `slot`, `positions[]`, `side` (L-Match only), `swr_at_save`. |
| `status`     | Free-form `{level, msg}` for warnings, errors, lockouts, reconnects.               |
| `heartbeat`  | Sent every `heartbeat_ms` when no other frame would be sent.                       |
| `ack`        | Reply to a client command (`ref`, `ok`, optional `err`).                           |

**Client → server (control verbs only)**

| `action`         | Args                              | Effect                                                |
|------------------|-----------------------------------|-------------------------------------------------------|
| `set_topology`   | `{kind, elements[]}`              | Declare wired topology (L / T / Pi) and element map. Persisted to NVRAM; required before any motion verb on first install. |
| `move_axis`      | `axis`, `delta_steps` *or* `target_steps` | Drive a named axis. Refused if RF present.    |
| `move_l`         | `delta_steps` *or* `target_steps` | Alias for axis "L" (L-Match only). Refused if RF present. |
| `move_c`         | `delta_steps` *or* `target_steps` | Alias for axis "C" (L-Match only). Refused if RF present. |
| `set_side`       | `"hi_z" \| "lo_z"`                | Switch K1/K2 (L-Match only). Refused if RF present.    |
| `set_bypass`     | `true \| false`                   | K3. The only relay verb safe to issue with RF on.     |
| `recall`         | `freq_hz`                         | Look up slot, move to stored element-positions, then engage. Tuple shape is per declared topology — see PROTOCOL.md. |
| `save`           | `freq_hz`, optional `label`       | Persist current element-positions for `(topology, band, bucket)`. |
| `auto_tune`      | `freq_hz`, `power_w`              | Run the search algorithm at low power; updates slot.  |
| `home`           | —                                 | Drive every active axis to mechanical home; recalibrate encs. |
| `resync`         | —                                 | Re-emit current `state` + `telemetry`.                |

Frame examples and error semantics: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Hardware contract (summary; full notes in [docs/HARDWARE.md](docs/HARDWARE.md))

| Subsystem              | Choice (default)                             | Notes                                                                  |
|------------------------|----------------------------------------------|------------------------------------------------------------------------|
| Tuner-side MCU         | **Teensy 4.1** (NXP i.MX RT1062, 600 MHz) — **Phase 1** | Hardware QEI, 16-bit ADC, native Ethernet PHY (PJRC kit), microSD on-board. PSRAM + 2nd QSPI flash footprints for expansion. **Phase 2 migration target: STM32H743 on a custom board** if RF immunity testing at M5 surfaces problems (see "MCU selection" below). |
| Tuner-side carrier     | **grblHAL-teensy-4.x V2.09** (Phil Barrett, T41E5XBB SKU for Ethernet) — **Phase 1** | Off-the-shelf Teensy 4.1 carrier with 5 stepper-driver channels, 10 opto-isolated inputs, 7 relay drivers, and the PJRC Ethernet Kit footprint. Avoids a custom carrier PCB for Phase 1; revisited at the M5 Phase-2 decision. Full mapping in [docs/HW-T41-CARRIER.md](docs/HW-T41-CARRIER.md). |
| Reactive-element axes  | 2 (L-Match) or 3 (T-Match / Pi-Match)        | Each axis: stepper + driver + encoder + end-stop. Axis count is a function of declared topology, not a build-time choice. |
| Element actuators      | NEMA 17 (or NEMA 23 for high-Q roller inductors) stepper + planetary gearbox → roller inductor / vacuum-variable capacitor shaft | Driver: **TMC2209** in StealthChop; sensorless homing via StallGuard. Cap example: Jennings UCSL-1500 (10–1500 pF, 5 kV). |
| Position encoder       | **Incremental quadrature optical**, 2000+ CPR (default per axis) | Direct-coupled to shaft (post-gearbox). Drives the MCU's hardware QEI. Z-phase / index pulse used opportunistically. Anchored by homing (StallGuard) and a clean-shutdown NVRAM record — see docs/ARCHITECTURE.md §5.2. Absolute SSI is a per-axis drop-in alternative via the same HAL (useful if the L axis's homing time becomes intolerable). |
| Vacuum relays          | Gigavac G2/G81 or Kilovac H-series, SPDT      | 26 V coil, HV bias supply. Hot-switch protection in firmware (TX lock).|
| Balun                  | 1:1 or 4:1 current balun, ferrite (Fair-Rite 43 / 31) | Fixed, on the output. Spec: ≥3 kW dissipation safety margin.      |
| RF detector            | **AD8302** (gain/phase) + dual log detector (AD8307 ×2 on Fwd/Rev tap) | AD8302 for complex Z; AD8307 pair for SWR/return loss. |
| Directional coupler    | Stockton or Tandem-match, 50 Ω, ~50 dB coupling | Sized for full legal-limit power.                                   |
| Master MCU             | **Raspberry Pi 4 / 5**                        | 7" or 10" capacitive touchscreen. 64-bit Raspberry Pi OS.              |
| Master input           | 2 × Adafruit ANO directional encoder (5735); a 3rd added for T/Pi-Match installs | Connect via GPIO; A/B + 5 directional + push. One encoder per active reactive-element axis. |
| Transceiver link       | USB serial (CAT)                              | CI-V, Yaesu CAT, Kenwood, K3/K4 — abstracted behind one driver iface.  |

## MCU selection (Phase 1 / Phase 2)

The tuner-side MCU is chosen in two phases:

- **Phase 1 (M0 – M4, bench development): Teensy 4.1.** Fast development,
  generous memory headroom (1 MB RAM on-chip, 8 MB flash, footprints for
  +16 MB PSRAM and +16 MB QSPI flash), on-board microSD and Ethernet
  magnetics via the PJRC Ethernet Kit. Productive Arduino/PlatformIO
  toolchain.
- **Phase 2 decision at M5 (RF commissioning):** if and only if RF
  immunity testing on the live Doublet at the target power level shows
  problems we can't resolve via enclosure/feed-through/decoupling work,
  **migrate the firmware to an STM32H743 on a custom carrier board**. The
  H743 is the same M7 class, slightly slower (480 MHz), industrially
  proven, and a custom board lets us control the ground plane, the power
  tree, and the shielding partition. The migration must remain small
  (low single-digit days of work), which constrains the firmware coding
  style — see the rule below.
- All Phase-2 hardware risks are captured in [docs/PLAN.md](docs/PLAN.md)
  M5 as an explicit go/no-go decision; the architecture is identical
  either way.

**Firmware portability rule (applies from M0).** Tuner-controller firmware
MUST be written so that an STM32H743 port is a small, mechanical change.
Concretely:

- **No PJRC-only APIs** in the core control / measurement / protocol
  code. Stick to Arduino-core idioms (`pinMode`, `digitalWrite`,
  `Serial`, `analogRead`, etc.) and to lwIP at the network layer — both
  are available on STM32 cores (`stm32duino`, `STM32Cube` + lwIP).
- **Hardware-specific code is isolated** behind a `hal/` directory:
  one file per peripheral (stepper, encoder, adc, relay, net), with the
  port-specific implementation selectable at compile time. Application
  logic links only against the HAL interface.
- **No reliance on the i.MX RT1062's specific peripheral mix** (e.g.,
  XBAR or FlexPWM-specific timing tricks) without a documented STM32
  equivalent. If a Teensy-only feature is irresistible, gate it behind
  an `#ifdef TEENSY41` and provide an STM32 fallback in the same PR.
- **No CGO-equivalent native dependencies** that would block re-tooling.
  PlatformIO is used for both targets; the build matrix in CI tests
  both even while only the Teensy is the deployed target.
- **Step pulses MUST be hardware-generated, not software-timed in the
  main loop.** AccelStepper-style `runSpeed()` polling in `loop()` is
  forbidden in the tuner-controller firmware. Any blocking operation in
  the main loop — NVS / SD writes, Serial flushes, lwIP socket activity,
  WiFi events — silently pauses the pulse train and causes cumulative
  step loss visible as per-revolution drift. The HAL must wrap a hardware
  pulse generator: **FlexPWM on Teensy 4.1**, **TIM + DMA on STM32H743**,
  **LEDC + PCNT on ESP32-class targets** if any are used for bring-up.
  Position-counter writes to NVRAM (the clean-shutdown anchor required
  by invariant 3) and any other persistent state MUST run independently
  of pulse timing. Surfaced during ESP32-C6 + TB6600 bench testing,
  2026-06; see PROPOSAL.md "Bench-test learnings".

This rule does not apply to the master controller (Pi / Go) — that side
is platform-stable.

## Stack (locked in)

Following the LP-100A-Server precedent so the station's services stay uniform:

- **Master (`master/tuner-master/`):** Go, single static binary. Embedded web
  UI via `go:embed`. `gorilla/websocket`, `BurntSushi/toml`,
  `modernc.org/sqlite` (pure-Go SQLite — keeps cross-compile easy), `log/slog`.
  Cross-compile to `linux/arm64` for the Pi via the same `deploy/build-pi.sh`
  pattern as LP-100A-Server.
- **Tuner controller (`firmware/tuner-controller/`):** C/C++, **PlatformIO**.
  Phase 1 target: Teensyduino core on Teensy 4.1. Phase 2 target:
  `stm32duino` (or Cube+FreeRTOS) on STM32H743. Real-time motor control
  loop, ADC sampling at ~10 kSPS, **WebSocket server with a swappable
  Ethernet backend** — `QNEthernet` (lwIP, default; portable to STM32)
  or `NativeEthernet` (FNET; ham-radio-Teensy convention used by Morconi
  / TeensyMaestro / IW7DMH `FlexRigTeensy`). The choice is a `platformio.ini`
  env (one build flag + `lib_deps` swap); both backends are built in CI
  to keep the `net_hal/` abstraction honest. See
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §5.1.2. Portability
  constraints are spelled out under "MCU selection" above.
- **Config:** TOML on the master; a minimal JSON config served by the master
  to the controller on first connection (so the controller is stateless
  across reboots aside from encoder calibration).
- **Auth:** none. LAN-only. Documented loudly. If WAN access is ever wanted,
  front with Tailscale/WireGuard, do not bake it in.

## What this codebase intentionally does **not** do

**Phase 1 scope (M0 – M6, the committed delivery):**

- T-Match / Pi-Match auto-tune (search algorithm). The topologies are
  supported through M6 to the level of "drive each declared element to a
  commanded position", but the auto-tune search algorithm is L-Match-only
  in Phase 1; T/Pi auto-tune is a Phase-2 deliverable.
- Balanced-line tuning without a balun. The balun is part of the architecture.
- Hot-switching the L or C under RF. The TX lockout is enforced in firmware
  *and* the master GUI; both must agree before motion is permitted.
- Multiple antennas, multiple transceivers, SO2R. **Phase 1 is single
  Doublet, single rig.** Multi-antenna / multi-rig / SO2R is documented
  as a Phase 2 extension in [docs/EXTENSIONS.md](docs/EXTENSIONS.md);
  not built until Phase 1 commissioning is clean.
- Cloud relay / NAT traversal. LAN deployment, like LP-100A-Server.
- Logging/charting of long-term match data. A separate subscriber can write
  to InfluxDB if wanted — out of scope here.
- Replacing the LP-100A. The tuner has its own SWR/Z chain; the LP-100A
  remains the station reference meter. The master MAY optionally subscribe
  to an LP-100A-Server instance for a second opinion (`[lp100a]` block in
  the TOML), but it is not required.
