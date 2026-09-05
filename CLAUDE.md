# Automatic Antenna Tuner

A two-controller automatic antenna tuner for a **Doublet fed with 460 Ω /
600 Ω open-wire ladder line, 160 m – 6 m**. Supports two **balanced**
network topologies — **Balanced L-Network (default)** and **Balanced
Pi-Network** — fed through a 1:1 current balun on the transceiver side;
the chosen topology and its element-to-motor map are declared to the
firmware at install time and persisted on the controller. A **tuner
controller** sits at the tuner enclosure (drives 2 or 3 stepper-actuated
reactive elements — a synchronized pair of roller inductors on one
motor, one or two vacuum-variable capacitors, depending on topology;
switches **vacuum relays** for topology-specific path selection; samples
the RF detector chain). A **master controller** lives
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
│   ├── HW-T41-CARRIER.md # plan: grblHAL-teensy 4.x V2.09 board as the carrier
│   └── HW-T41-PINMAP.md  # Teensy 4.1 pin → V2.09 carrier net mapping (reference)
├── firmware/
│   └── tuner-controller/  # MCU firmware (C/C++ on Teensy 4.1 or STM32H7)
└── master/
    └── tuner-master/      # Go server + embedded web UI, runs on the Pi
```

## RF topology (configurable)

This codebase supports two **balanced** network topologies. The
**Balanced L-Network is the default and most-developed path**; the
**Balanced Pi-Network** is first-class supported but built second. Both
were adopted 2026-09-05, superseding the unbalanced L / T / Pi networks
with an output balun of the 2026-05 design (T-Match is no longer
supported). The chosen topology, together with the map of **which motor
drives which element**, is declared to the firmware once at install time
via the `set_topology` configuration verb and committed to NVRAM;
changing topology is treated as a hardware change and requires a power
cycle.

A **1:1 Guanella current balun on the transceiver side** converts the
rig's 50 Ω unbalanced output to 50 Ω balanced *before* the network, so
the balun always works at its design impedance. Everything after it —
inductors, capacitors, relays — is balanced, and the ladder line is fed
directly from the network output. The balun is fixed (not switched) and
independent of topology choice.

### Synchronized inductor pair (both topologies)

The series inductance is split equally between the two line legs as
**two matched roller inductors driven by one stepper motor** — the
first coil sits on the motor shaft, the second is belted 1:1 off the
same shaft with GT2 pulleys — so they turn in lock-step and the legs
stay balanced at every setting. Firmware treats the pair as **one axis**
(`L`). Electrically the network is the familiar unbalanced network with
`L = 2 × L_leg`, so the analytic L-network solver needs only that
factor. Mechanical detail (direct motor-to-element coupling; the 3:1
belt-driven lead screw that carries the home and max limit switches) is
in the "Element actuators" row below and in
[docs/HARDWARE.md](docs/HARDWARE.md) "Drive train".

### Balanced L-Network (2 axes: L-pair, C — default)

Series inductor pair with one variable capacitor **across the line**,
placed by vacuum relays on either the **antenna side** of the inductors
(for Zload > 50 Ω) or the **transceiver side** (for Zload < 50 Ω).

```
HI-Z (Zload > 50 Ω): K1 closed, K2 open — C across the antenna side

TX ── SWR/Z ── 1:1 ──┬── L_leg ───────┬──── ladder
              BALUN  │   (sync'd)     ═ C        line
                     └── L_leg ───────┴────

LO-Z (Zload < 50 Ω): K1 open, K2 closed — C across the transceiver side

TX ── SWR/Z ── 1:1 ──┬────┬── L_leg ────── ladder
              BALUN  │    ═ C  (sync'd)     line
                     └────┴── L_leg ──────
```

- **HI-Z mode (Zload > 50 Ω):** K1 closed, K2 open. C is across the
  line on the **antenna side** (after L).
- **LO-Z mode (Zload < 50 Ω):** K1 open, K2 closed. C is across the
  line on the **transceiver side** (before L).
- **BYPASS:** K3 is a two-pole changeover that connects the balun
  output straight to the ladder line, isolating the whole network. Used
  during startup and any time positions are being changed.
- Because the network is balanced, **every switch is two-pole**: K1, K2
  and K3 each comprise two vacuum-relay contacts (one per line leg,
  coils in parallel on one carrier relay driver) or a single DPST /
  DPDT unit. Firmware semantics (K1/K2 mutual exclusion, K3 override)
  are unchanged.
- Optional, Phase 2: switched fixed capacitors in parallel with C to
  extend the range on 160 m (the reference design does this).

### Balanced Pi-Network (3 axes: C1, L-pair, C2)

Symmetrical balanced version of the Collins Pi: capacitor C1 across the
line on the transceiver side, the synchronized inductor pair in series,
capacitor C2 across the line on the antenna side.

```
TX ── SWR/Z ── 1:1 ──┬────┬── L_leg ──┬──── ladder
              BALUN  │    ═ C1 (sync'd)═ C2       line
                     └────┴── L_leg ──┴────
```

- No source/load selector: the Pi covers both impedance ranges by
  itself, so K1/K2 are absent. The **BYPASS relay (K3)** is retained per
  invariant 2.
- Simpler switching than Balanced L at the cost of a third axis (second
  vacuum capacitor, third motor, third ANO encoder).
- Optional, Phase 2: switched fixed capacitors in parallel with C1 on
  the transceiver side, as in the reference design.

### Topology vs firmware

The HAL exposes axes 0/1/2 as opaque handles; the application layer
reads a topology block (served by the master from its TOML `[topology]`
table, persisted on the controller) that names each element, gives its
type (L / C), **binds it to a HAL axis — this is the operator-chosen
motor-to-component assignment**, marks the inductor pair, and stores its
calibration and limits:

```json
{"action": "set_topology", "kind": "balanced_l",
 "elements": [
   {"name": "L", "type": "L", "axis": 0, "pair": true},
   {"name": "C", "type": "C", "axis": 1}
 ]}
```

Balanced Pi declares three elements, default `C1` → axis 0, `L` → axis 1
(`pair: true`), `C2` → axis 2. Any element may be bound to any axis; the
firmware rejects duplicate bindings, and rejects `set_side` with
`wrong_topology` on a Balanced Pi install. The control loop, tuning
algorithm, and memory schema are topology-aware; pin maps and motor
counts live in `firmware/tuner-controller/hal/board/` and are decoupled
from the topology choice — see
[docs/HW-T41-CARRIER.md](docs/HW-T41-CARRIER.md) for the planned mapping
on the grblHAL Teensy 4.x V2.09 carrier.

Balanced Pi support is in scope but **Balanced L is the only topology
exercised end-to-end through M6**; Balanced Pi auto-tune development is
a Phase-2 deliverable (see [docs/PLAN.md](docs/PLAN.md)).

## Invariants (do not violate)

1. **No mechanical motion while RF is present.** The tuner controller MUST
   refuse any stepper-move command if forward power exceeds
   `safety.tx_lockout_w` (default 5 W). The master MUST also refuse to send
   move commands while CAT reports the rig PTT-keyed. Belt + suspenders.
2. **Bypass on power-up.** K3 latches the network out of circuit on every
   boot until the controller has read both encoders, confirmed plausible
   positions, and received an explicit "engage" command.
3. **Position truth is anchored to a known reference — never a bare
   pulse count.** With the iHSS60 integrated closed-loop drives the
   position loop closes inside the drive, so the controller's pulse
   counter equals the shaft position *only while* the drive's ALM line
   is clear and PED confirms arrival after each bounded move. An ALM
   trip (following error, over-current, over-voltage) means shaft and
   counter have diverged: the controller MUST stop pulses on that axis
   and report `homed:false`. The counter is anchored to either (a) the
   last `home` routine (mechanical limit switch) within this power
   cycle, or (b) a clean position record persisted to NVRAM at last
   `bypass=true` shutdown; if neither is valid the controller refuses
   motion verbs except `home` and reports `homed:false` in `state`. The
   NVRAM record assumes the shaft did not move while unpowered; a
   direct-coupled vacuum capacitor can be back-driven by its bellows, so
   the controller MUST keep the drives enabled whenever it is powered
   and, after a cold start, MUST confirm the record against the
   lead-screw home switch (or an absolute position sensor, if fitted)
   before recall on that axis — see [docs/HARDWARE.md](docs/HARDWARE.md)
   "Memory reliability with direct coupling". If
   an external encoder is fitted to an axis (non-integrated motor), its
   counts replace the pulse counter as the position source under the
   same anchoring rules; an absolute SSI encoder is self-anchoring. All
   pathways live behind the same HAL — application code is
   feedback-kind-agnostic. See
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
7. **Vacuum-variable cap is protected by a defence-in-depth stack, not
   by trusting the pulse train.** The iHSS60 closes its loop internally
   but reports only pass/fail. The firmware and commissioning MUST
   enforce, in order: drive currents (P8/P9) and following-error limit
   (P16) set for the geared load at install time via the HISU tool;
   ALM wired to a carrier opto input (fail-safe polarity P10 = 1
   preferred) with any trip stopping pulses and setting `homed:false`;
   refusal of all motion verbs except `home` while `homed:false`;
   per-axis software soft limits sitting `SAFE_MARGIN` steps inside the
   limit-switch position (default 100); per-axis mechanical limit switches at
   both ends of travel, tripped by the traveling block on the axis lead
   screw, as the hardware fallback; PED required before a bounded move is
   recorded as complete. The first install-time commissioning must set
   each limit switch along the lead screw so it trips a safe number of
   steps inside the element's mechanical stop, both ends, record those
   positions in steps, and persist them to NVRAM with the per-axis
   topology block. Because the switches ride a belt-driven lead screw
   rather than the element itself, the firmware MUST also: latch limit
   events by travel direction while moving (never level-sample); abort
   a homing move that exceeds the commissioned travel plus margin and
   report `homed:false`; re-measure the home→max span on every full
   home and refuse recall if it has drifted; cut pulses immediately on
   a limit trip (no deceleration ramp); size `SAFE_MARGIN` to at least
   the stopping distance at homing speed; and pull off with a bounded
   move when a switch is already active at power-up. Risk register:
   [docs/HARDWARE.md](docs/HARDWARE.md) "Risks of the belt-driven
   lead-screw limit mechanism". See also
   [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §5.2.5 and
   [docs/HW-T41-PINMAP.md](docs/HW-T41-PINMAP.md) §2.2.

## WebSocket protocol (summary; full spec in [docs/PROTOCOL.md](docs/PROTOCOL.md))

Same shape as LP-100A-Server: JSON over a single `/ws` endpoint, `type`
discriminator, monotonic `seq` on server→client frames, named verbs only
(no raw bytes exposed to clients).

**Server → client**

| `type`       | Purpose                                                                            |
|--------------|------------------------------------------------------------------------------------|
| `telemetry`  | Live measurements: `fwd_w`, `rev_w`, `swr`, `z_mag`, `z_phase`, `r`, `x`, `mode`.  |
| `state`      | Mechanical/relay state: `topology`, `axes[]` (each with `name`, `steps`, `enc`), `side` (Balanced L only), `bypass`, `homed`, `last_move`. |
| `memory`     | Result of memory lookup/save: `topology`, `band`, `freq_hz`, `slot`, `positions[]`, `side` (Balanced L only), `swr_at_save`. |
| `status`     | Free-form `{level, msg}` for warnings, errors, lockouts, reconnects.               |
| `heartbeat`  | Sent every `heartbeat_ms` when no other frame would be sent.                       |
| `ack`        | Reply to a client command (`ref`, `ok`, optional `err`).                           |

**Client → server (control verbs only)**

| `action`         | Args                              | Effect                                                |
|------------------|-----------------------------------|-------------------------------------------------------|
| `set_topology`   | `{kind, elements[]}`              | Declare wired topology (`balanced_l` / `balanced_pi`) and the element→axis (motor) map. Persisted to NVRAM; required before any motion verb on first install. |
| `move_axis`      | `axis`, `delta_steps` *or* `target_steps` | Drive a named axis. Refused if RF present.    |
| `move_l`         | `delta_steps` *or* `target_steps` | Alias for element `L` — the synchronized inductor pair (Balanced L only). Refused if RF present. |
| `move_c`         | `delta_steps` *or* `target_steps` | Alias for element `C` (Balanced L only). Refused if RF present. |
| `set_side`       | `"hi_z" \| "lo_z"`                | Switch K1/K2 (Balanced L only): C across the antenna side (`hi_z`) or the transceiver side (`lo_z`) of the inductor pair. Refused if RF present.    |
| `set_bypass`     | `true \| false`                   | K3. The only relay verb safe to issue with RF on.     |
| `recall`         | `freq_hz`                         | Look up slot, move to stored element-positions, then engage. Tuple shape is per declared topology — see PROTOCOL.md. |
| `save`           | `freq_hz`, optional `label`       | Persist current element-positions for `(topology, band, bucket)`. |
| `auto_tune`      | `freq_hz`, `power_w`              | Run the search algorithm at low power; updates slot.  |
| `home`           | —                                 | Drive every active axis to its lead-screw home switch; re-anchor the position counter. |
| `resync`         | —                                 | Re-emit current `state` + `telemetry`.                |

Frame examples and error semantics: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Hardware contract (summary; full notes in [docs/HARDWARE.md](docs/HARDWARE.md))

| Subsystem              | Choice (default)                             | Notes                                                                  |
|------------------------|----------------------------------------------|------------------------------------------------------------------------|
| Tuner-side MCU         | **Teensy 4.1** (NXP i.MX RT1062, 600 MHz) — **Phase 1** | Hardware QEI, 16-bit ADC, native Ethernet PHY (PJRC kit), microSD on-board. PSRAM + 2nd QSPI flash footprints for expansion. **Phase 2 migration target: STM32H743 on a custom board** if RF immunity testing at M5 surfaces problems (see "MCU selection" below). |
| Tuner-side carrier     | **grblHAL-teensy-4.x V2.09** (Phil Barrett, T41E5XBB SKU for Ethernet) — **Phase 1** | Off-the-shelf Teensy 4.1 carrier with 5 stepper-driver channels, 10 opto-isolated inputs, 7 relay drivers, and the PJRC Ethernet Kit footprint. Avoids a custom carrier PCB for Phase 1; revisited at the M5 Phase-2 decision. Full mapping in [docs/HW-T41-CARRIER.md](docs/HW-T41-CARRIER.md). |
| Reactive-element axes  | 2 (Balanced L: `L`-pair, `C`) or 3 (Balanced Pi: `C1`, `L`-pair, `C2`) | Each axis: one iHSS60 coupled **directly** to the element shaft; a small GT2 pulley on the motor shaft belts 3:1 to a large pulley on a parallel lead screw whose traveling nut block trips the **home** and **max** limit switches (NC-in-series to one carrier opto input); the drive's ALM / PED feedback lines into carrier opto inputs. The two roller inductors are **one** axis (belted together). Axis count is a function of declared topology, not a build-time choice. |
| Element actuators      | **JMC iHSS60 integrated closed-loop stepper** (NEMA 24 / 60 mm frame; bench unit iHSS60-36-30, 118 mm long) coupled **directly** to the roller-inductor / vacuum-variable-capacitor shaft (**6400 steps per element revolution**, full motor torque at the element). A small GT2 pulley on the motor shaft drives a large pulley on a parallel **lead screw** at 3:1; the screw is not in the torque path — its traveling nut block is a turns counter that trips the **home** and **max** limit switches, so the switches, not the element, take the contact (arithmetic, home-repeatability table and open mechanical items in [docs/HARDWARE.md](docs/HARDWARE.md) "Drive train"). The servo driver is built onto the motor's rear, so there is **no free rear shaft**; position feedback is the drive's internal optical encoder (50 µs sampling), visible to the controller only as **ALM** (fault) and **PED** (arrived) opto outputs. Adopted 2026-09-05, replacing TB6600 + NEMA 23. | Drive spec (iHSS60 manual V1.1, rocketronics.de): 24–50 VDC (36 V typ.), 4.5 A, 200 kHz max pulse, PUL/DIR/ENA opto inputs 5 V or 24 V compatible, over-current 8 A, over-voltage 80 V. **Pulses/rev set by DIP to 6400** (SW1 on, SW2 on, SW3 off, SW4 on); SW5 = active edge, SW6 = direction; P1–P20 (currents, PID, P10 alarm polarity, P14 arrival polarity, P16 following-error limit, P19 smoothing, P20 user p/r) only via the RS-232 HISU port. **Timing the HAL MUST honour (manual §5.5):** ENA ≥ 5 µs before DIR; DIR stable ≥ 6 µs before the first PUL edge and unchanged ≥ 5 µs after the last; PUL high and low each ≥ 2.5 µs, so a 50 %-duty train is at spec only up to 200 kHz — treat ≈ 100 kHz as the practical ceiling. Step pulses are hardware-generated via FlexPWM, not loop-polled — bench reference impl in [firmware/t41-stepper-test/src/flexpwm_stepper.h](firmware/t41-stepper-test/src/flexpwm_stepper.h) (10 µs DIR setup/hold, `dsb` in the reload ISR). Homing: per-axis home and max limit switches beside the lead screw, tripped by the traveling block, wired NC-in-series to one carrier opto input (fail-safe: open = at a limit or cable fault; the firmware knows the travel direction, so it knows which end); home is approached from one direction at low speed so belt / nut backlash cannot move the reference. Opto-input allocation in [docs/HW-T41-PINMAP.md](docs/HW-T41-PINMAP.md) §2 (to be redone for two-switch axes); ALM/PED wiring and open final-build checks in §2.2. TB6600's ENA-polarity and 5 µs quirks stay documented in PROPOSAL.md "Bench-test learnings" for anyone re-using the bench rigs. Cap example: Jennings UCSL-1500 (10–1500 pF, 5 kV). |
| Position encoder       | **Inside the drive.** The iHSS60's optical encoder closes the position loop within the drive; the controller never sees counts, only ALM (fault / following error beyond P16 × 10 counts) and PED (arrived within tolerance). No external rear-shaft encoder is possible on the integrated unit. | The controller's pulse counter (exact — counted in the FlexPWM reload ISR) is the position source, trustworthy while ALM is clear; PED confirms each bounded move landed before the position is persisted. External incremental / absolute-SSI encoders remain a per-axis option behind the same HAL only for a non-integrated motor (Phase-2 fallback). Anchoring rules (limit-switch home, clean-shutdown NVRAM record) — see docs/ARCHITECTURE.md §5.2. |
| Vacuum relays          | Gigavac G2/G81 or Kilovac H-series — **two-pole per switch** (2 × SPST/SPDT with paralleled coils, or one DPST/DPDT) | 26 V coil, HV bias supply. The network is balanced, so K1 and K2 (Balanced L) and the K3 bypass changeover (both topologies) each switch both line legs from one carrier relay driver: 6 contacts for Balanced L, 2 for Balanced Pi. Hot-switch protection in firmware (TX lock). |
| Balun                  | **1:1 Guanella current balun**, ferrite (Fair-Rite 43 / 31) | Fixed, on the **transceiver side** ahead of the network (50 Ω unbalanced → 50 Ω balanced), so it always runs at its design impedance. Spec: ≥3 kW dissipation safety margin. Replaced the 1:1 / 4:1 output balun 2026-09-05; the ratio decision is closed. |
| RF detector            | **AD8302** (gain/phase) + dual log detector (AD8307 ×2 on Fwd/Rev tap) | AD8302 for complex Z; AD8307 pair for SWR/return loss. |
| Directional coupler    | Stockton or Tandem-match, 50 Ω, ~50 dB coupling | Sized for full legal-limit power.                                   |
| Master MCU             | **Raspberry Pi 4 / 5**                        | 7" or 10" capacitive touchscreen. 64-bit Raspberry Pi OS.              |
| Master input           | 2 × Adafruit ANO directional encoder (5735); a 3rd added for Balanced Pi installs | Connect via GPIO; A/B + 5 directional + push. One encoder per active reactive-element axis. |
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

- Balanced Pi-Network auto-tune (search algorithm). The topology is
  supported through M6 to the level of "drive each declared element to a
  commanded position", but the auto-tune search algorithm is Balanced-L-
  only in Phase 1; Balanced Pi auto-tune is a Phase-2 deliverable.
- Unbalanced L / T / Pi networks with an output balun (the 2026-05
  design). Superseded 2026-09-05 by the balanced networks; T-Match is not
  a supported topology any more.
- Tuning without a balun. The 1:1 current balun on the transceiver side
  is part of the architecture.
- Switched fixed-capacitor banks in parallel with the variable
  capacitor(s). Documented as an optional Phase-2 range extension only.
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

## Notes for Claude (token-budget defaults)

Bench-iteration sessions on this codebase tend to run for hours and
accumulate large context (firmware + docs + protocol). To keep token
spend in check:

- **Direct work over subagents.** This repo is small enough to navigate
  from the main loop. Spawn an Agent only for genuine multi-file
  exploration; prefer the cheap `Explore` agent when you do. Don't
  delegate three Read calls' worth of work.
- **Read narrowly.** Big files (this CLAUDE.md, `docs/ARCHITECTURE.md`,
  `firmware/*/src/main.cpp`) run 600+ lines. Use `grep` to locate the
  section first, then `Read` with `offset` / `limit`. Never re-read a
  file you just edited — the tool harness tracks state.
- **Compact at PR-merge breakpoints when context crosses ~150k.**
  Suggest `/compact` (or call it) after a PR merges and before the
  next feature, not mid-task.
- **Suggest `/clear` on topic pivots.** Stepper-bench debugging and
  protocol-spec design share no useful context — say so when the user
  pivots ("let's design the auto-tune algorithm now") rather than
  letting the prior context ride.
