# Architecture

This document is the full architectural picture: how the boxes split, what
runs in each, the RF chain, the measurement chain, the tuning algorithm, and
the network protocol shape. For the *invariants* a future change must not
break, see [`CLAUDE.md`](../CLAUDE.md). For *what to build first*, see
[`PLAN.md`](PLAN.md).

## 1. System view

```
   ┌─────────────────────────────────────────────────────────────────────┐
   │                          SHACK                                      │
   │                                                                     │
   │   Transceiver ──CAT (USB)──► Master Controller ──HDMI──► Touchscreen│
   │       │                          (Raspberry Pi 4/5)                 │
   │       │  coax (50 Ω)              │                                 │
   │       │                           ├── 2× Adafruit ANO encoders (GPIO)│
   │       │                           ├── Ethernet (LAN)                │
   │       │                           └── (optional) LP-100A-Server WS  │
   │       ▼                                                             │
   │   ┌───────┐         coax                                            │
   │   │  TX/RX│◄──── 50 Ω ─────┐                                        │
   │   └───────┘                 │                                       │
   │                             │  (run as short as practical)          │
   └─────────────────────────────┼───────────────────────────────────────┘
                                 │
   ┌─────────────────────────────▼───────────────────────────────────────┐
   │                       TUNER ENCLOSURE (outdoors / shack-edge)       │
   │                                                                     │
   │   ┌────────────────────────────────────────────────────────────┐    │
   │   │  Tuner Controller  (Teensy 4.1 on grblHAL V2.09 carrier)    │    │
   │   │   ┌────────────┐  ┌────────────┐  ┌───────────────┐         │    │
   │   │   │ Motor loop │  │ ADC sampler│  │ WS/JSON server│◄── LAN ─┼────┘
   │   │   │ (FlexPWM   │  │ (Fwd, Rev, │  │ (lwIP, ~5 cli)│         │
   │   │   │  steppers, │  │  AD8302 I/Q│  │               │         │
   │   │   │  optional  │  │  log dets) │  │               │         │
   │   │   │  encoders) │  │            │  │               │         │
   │   │   └─────┬──────┘  └─────┬──────┘  └───────┬───────┘         │
   │   │         │               │                 │                 │
   │   │         ▼               ▼                 ▼                 │
   │   │   ┌────────────┐  ┌────────────┐  ┌───────────────┐         │
   │   │   │ TB6600 ×2  │  │ Detector   │  │ Relay drivers │         │
   │   │   │ Stepper drv│  │  bias supply│ │ (K1/K2/K3 HV) │         │
   │   │   └─────┬──────┘  └────┬───────┘  └──────┬────────┘         │
   │   └─────────┼───────────────┼─────────────────┼──────────────────┘
   │             ▼               ▼                 ▼
   │   ┌──────────────────────┐ ┌──────────────────┐  ┌─────────────────┐
   │   │ Roller L             │ │ Directional      │  │ Vacuum relays   │
   │   │ NEMA 23 (dual-shaft) │ │ coupler (Tandem) │  │ K1: load-side C │
   │   │ + gearbox            │ │  → Fwd/Rev to    │  │ K2: src-side C  │
   │   │                      │ │  AD8307 ×2,      │  │ K3: bypass      │
   │   │ Vac variable C       │ │  AD8302 phase    │  └─────────────────┘
   │   │ NEMA 23 (dual-shaft) │ │                  │
   │   │ + gearbox            │ │  Rear shaft on   │
   │   │                      │ │  each NEMA 23 →  │
   │   │                      │ │  optional encoder│
   │   └──────────────────────┘ └──────────────────┘
   │                                       ▲
   │                                       │
   │      RF path:  TX ──► coupler ──► L-network (with relays) ──► BALUN
   │                                                              │
   └──────────────────────────────────────────────────────────────┼──────┘
                                                                  ▼
                                                          Open-wire 460/600 Ω
                                                          ladder line → Doublet
```

## 2. RF / L-network design

See [`RF-DESIGN.md`](RF-DESIGN.md) for component sizing and detector math;
this section is the topology and operating mode summary.

### 2.1 Topology

The network is the standard reconfigurable **two-element L** with the shunt
capacitor electrically movable to either side of the series inductor by a
pair of vacuum relays (K1, K2). A third vacuum relay (K3) shorts the
network out of circuit ("bypass") for safe boot and for moving the L/C
during retune.

```
        K3 (bypass)
   ┌──────/ ──────┐
   │              │
TX ─┴── Lvar ─────┴── BALUN ── ladder line
        │   │
        │   └── K1 (load-side shunt) ─── Cvar ──┐
        │                                       │
        └── K2 (source-side shunt) ──── Cvar ───┘
                       ▲
                       └── single shared variable cap, switched onto whichever rail K1/K2 selects
```

Only **one of K1 / K2** is closed at any time. K3 is independent and
overrides the rest (bypass = "RF passes straight through to balun, network
sees no current").

### 2.2 Operating modes

| Mode      | K1   | K2   | K3   | When chosen                                           |
|-----------|------|------|------|-------------------------------------------------------|
| Bypass    | open | open | clsd | Boot, retuning, manual user adjustment, fault.        |
| Hi-Z      | clsd | open | open | Measured Zload Re part > 50 Ω at the tuner input.     |
| Lo-Z      | open | clsd | open | Measured Zload Re part < 50 Ω at the tuner input.     |
| Forbidden | clsd | clsd | —    | Never. Firmware enforces mutual exclusion.            |

### 2.3 Coverage targets

| Band  | f (MHz)      | Expected L range | Expected C range |
|-------|--------------|------------------|------------------|
| 160 m | 1.80 – 2.00  | 8 – 30 µH        | 200 – 1500 pF    |
| 80 m  | 3.50 – 4.00  | 4 – 18 µH        | 100 – 1200 pF    |
| 60 m  | 5.30 – 5.40  | 3 – 12 µH        | 80 – 1000 pF     |
| 40 m  | 7.00 – 7.30  | 2 – 8 µH         | 50 – 800 pF      |
| 30 m  | 10.1 – 10.15 | 1.5 – 6 µH       | 40 – 600 pF      |
| 20 m  | 14.0 – 14.35 | 1 – 4 µH         | 30 – 400 pF      |
| 17 m  | 18.07 – 18.17| 0.8 – 3 µH       | 25 – 300 pF      |
| 15 m  | 21.0 – 21.45 | 0.7 – 2.5 µH     | 20 – 250 pF      |
| 12 m  | 24.9 – 25.0  | 0.5 – 2 µH       | 18 – 200 pF      |
| 10 m  | 28.0 – 29.7  | 0.4 – 1.8 µH     | 15 – 180 pF      |
| 6 m   | 50.0 – 54.0  | 0.1 – 0.7 µH     | 10 – 120 pF      |

These are *first-cut* envelope estimates for a Doublet on 600 Ω ladder line
of typical length. The exact range is calibrated empirically at install —
the memory table is the operational ground truth, not these numbers.

## 3. Measurement chain

Three measurements, all sampled by the tuner-side MCU's 16-bit ADC:

1. **Forward power** — log detector (AD8307) on the forward-port tap of the
   Tandem-match directional coupler. Slope-and-intercept calibration table
   stored in NVRAM at factory; recalibratable via a `cal_fwd` admin verb.
2. **Reverse power** — second AD8307 on the reverse-port tap.
   - `swr = (1 + √(rev/fwd)) / (1 − √(rev/fwd))`
   - `return_loss_dB = 10·log10(fwd/rev)`
3. **Complex impedance** — **AD8302 gain/phase detector** fed by samples of
   `V` (load voltage) and `I` (load current) taken from a small Stockton
   current transformer and a capacitive voltage tap, both calibrated to a
   common reference plane at the **tuner input port** (before L, before
   K1/K2 split).
   - AD8302 `Vmag` output → ratio `|V|/|I|·k` → `|Z|`.
   - AD8302 `Vphs` output → angle of `V/I` → `∠Z`.
   - Then `R = |Z|·cos(∠Z)`, `X = |Z|·sin(∠Z)`.

The AD8302 is needed because SWR alone cannot tell us *which side* of 50 Ω
we're on — a 25 Ω load and a 100 Ω load both read SWR=2. The phase/magnitude
chain breaks that ambiguity, which is what picks Hi-Z vs Lo-Z (K1 vs K2).

ADC sampling runs at **~10 kSPS per channel**, IIR-smoothed for telemetry
(~30 Hz update rate to clients), and **raw** for the auto-tune algorithm so
hill-climbing sees the instantaneous match.

## 4. Tuning algorithm

The master is the brain; the controller is the manipulator. The master
decides "what L, C, side to be at"; the controller executes.

The full strategy comparison — including the universal per-tune
safety protocol (forced by invariant #1), the per-band starting-condition
table, four candidate algorithms (A–D) with pros / cons, and the chosen
project strategy with thresholds — is in [`TUNING.md`](TUNING.md). What
follows is the one-paragraph summary; consult `TUNING.md` for the
trade-offs and `RF-DESIGN.md` §3 for the closed-form L-network math.

**Project decision: Proposal D — hybrid memory-first with analytic
fallback.** On any `recall(freq_hz)` (driven by either CAT polling or
operator action), the master looks up `(l_steps, c_steps, side)` from
SQLite by `(band, bucket = round(freq_hz / bucket_size))`. On a hit
within `swr_recall_threshold` (default 1.5), it engages bypass, moves
both axes in parallel, restores `side`, disengages bypass, and reports.
On a miss or a stale slot it measures `R, jX` in bypass mode, computes
the analytic L-network solution to seed a starting point, moves there,
and then hill-climbs on SWR until `≤ swr_done_threshold` (default 1.10).
Successful tunes prompt the operator to save (auto-save disabled by
default per invariant #4). Typical recall time **< 4 s**; cold-start
auto-tune 15 – 60 s.

### 4.3 Operator nudge

Outside of recall/auto-tune, the operator can rotate either Adafruit ANO
encoder to manually nudge L or C in single microsteps (or coarse steps if
the encoder is pressed-and-rotated — push acts as a coarse/fine modifier).
The 5-way directional pad on each encoder:

- **Up/Down** = ±10× the rotary step (coarse).
- **Left/Right** = switch side (Lo-Z ↔ Hi-Z), with bypass auto-engaged.
- **Centre push** = save current as memory for current QRG.

Encoder events go to the master, the master sends `move_l`/`move_c`/
`set_side` verbs, the controller broadcasts `state` after each move so the
GUI live-tracks reality.

## 5. Software architecture

### 5.1 Tuner controller (firmware, C/C++)

**MCU choice and migration path.** Phase 1 target is the **Teensy 4.1**
(NXP i.MX RT1062, Cortex-M7 @ 600 MHz). Chosen for development velocity
and headroom — 1 MB on-chip RAM (512 KB DTCM + 512 KB OCRAM), 8 MB QSPI
flash, on-board microSD, Ethernet via the PJRC magjack kit (with
transformer isolation), and footprints to solder in 16 MB PSRAM + 16 MB
extra QSPI flash if anything ever needs them.

The known concern is **RF susceptibility** when the module sits inside a
tuner enclosure carrying kilowatt-class RF. The mitigations are the
*same* for any modern fast MCU (M7 or otherwise) and live at the
enclosure level, not the silicon level — see §5.1.1. The Teensy-specific
worry is the RT1062's internal DCDC, which can show up on the ADC; the
mitigation is feeding `VDD_SOC` from an external clean LDO per PJRC's
documented procedure, and is part of the M2 detector-chain bring-up.

**Phase 2 fallback** is an **STM32H743 on a custom carrier board** —
same M7 class, 480 MHz, industrially proven, and a custom PCB lets us
own the ground plane, the power tree, and the shielding partition. The
go/no-go on the migration happens at M5 commissioning (see
[`PLAN.md`](PLAN.md) M5).

To keep the migration cheap, firmware follows the portability rule from
[`../CLAUDE.md`](../CLAUDE.md) "MCU selection": no PJRC-only APIs in the
core code, hardware specifics behind a `hal/` directory with one file
per peripheral, both targets built in CI even while only Teensy ships.

The task model below is target-agnostic — it describes the *control loop
shape*, not the i.MX peripherals. Five RTOS-ish tasks (cooperative with
hardware timer ISRs; no full RTOS required for this load):

| Task          | Period / trigger     | Responsibility                                                       |
|---------------|----------------------|----------------------------------------------------------------------|
| `motor`       | 50 µs (ISR)          | Step pulse generation, microstep timing, accel/decel ramps.          |
| `encoder`     | hardware QEI         | Quadrature decode in hardware; firmware reads on each move-complete. |
| `adc`         | 100 µs DMA           | Free-running ADC of 4 channels (Fwd, Rev, AD8302 Vmag, Vphs).        |
| `safety`      | 1 ms                 | Watches Fwd power; on RF detect during motion → emergency stop + K3. |
| `net`         | event-driven         | lwIP stack; WebSocket frames in/out; telemetry diffing.              |

Telemetry diffing matches the LP-100A-Server pattern: a `Snapshot` struct
holds the last-sent state; a 30 Hz timer emits `telemetry` frames only when
any field changed beyond its deadband (e.g., `swr` ±0.01, `fwd_w` ±0.5 W).

#### 5.1.1 RF immunity practices (enclosure-level, MCU-agnostic)

These are the practices the firmware *assumes* the enclosure provides. If
RF gets through anyway, fix the enclosure before blaming the MCU:

- **Faraday enclosure.** Tuner electronics sealed in a continuous metal
  box; every seam bonded. The MCU sub-board sits inside its own inner
  shield can if proximity to the L/C demands it.
- **Bulkhead feed-through on every wire leaving the box.** Power: pi
  filter (feed-through cap → series ferrite → feed-through cap). Logic:
  optoisolators or RS-485-style differential with common-mode chokes.
  No bare DC or signal lines crossing the boundary.
- **Ethernet through transformer-isolated magnetics** (built into the
  PJRC kit's RJ45; same for any STM32 carrier we'd build in Phase 2)
  with a shielded twisted-pair cable, shield bonded only at the
  enclosure, plus a clamp-on ferrite at the exit.
- **Coax shield current managed at the input** with a clamp-on choke at
  the enclosure, and at the output with the architecture's current
  balun.
- **High-impedance ADC inputs kept short and inside the can.** The
  AD8302 / AD8307 detector boards sit immediately adjacent to the MCU
  ADC pins; the long runs are at *RF*, not at *baseband*.
- **External clean LDO** feeding the RT1062's `VDD_SOC` (Phase 1
  Teensy-specific). On STM32H7 the equivalent is bypassing the SMPS
  regulator and feeding `VCAP` via an external LDO — same idea.
- **All cable shields bonded** to the enclosure at the bulkhead, never
  carried to the PCB ground.

These mitigations are part of M2 (detector chain bring-up) and M5 (full
power commissioning); see [`PLAN.md`](PLAN.md).

#### 5.1.2 Ethernet library choice (QNEthernet / NativeEthernet)

The Teensy 4.1 has two mainstream Ethernet libraries; this project
**supports both**, selectable at compile time via a build flag, and
the firmware is structured so the choice is a one-line change.

##### The two contenders

| Aspect                         | **QNEthernet** ([ssilverman/QNEthernet](https://github.com/ssilverman/QNEthernet)) | **NativeEthernet** ([vjmuzik/NativeEthernet](https://github.com/vjmuzik/NativeEthernet)) |
|--------------------------------|----------------------|----------------------|
| Underlying TCP/IP stack        | **lwIP** (industry-standard, RFC-tracking) | **FNET** (vjmuzik's fork of FNET) |
| Author / maintenance           | Shawn Silverman, actively developed (0.35.0 Mar 2026) | Vincent Muzik, low activity since 2023 |
| API style                      | Arduino-compat + modern extensions (mDNS, IPv6, async callbacks, link-state callbacks, raw-frame access, direct lwIP API access) | Arduino-compat — close to drop-in for Arduino Ethernet examples |
| Namespace                      | `qindesign::network` (must `using namespace` or qualify) | Global |
| Link-state API                 | `Ethernet.linkState()`, `waitForLink(ms)`, `linkInfo().{speed,fullNotHalfDuplex}` | `Ethernet.linkStatus()` (enum), poll manually |
| DHCP wait                      | `Ethernet.waitForLocalIP(ms)` separate from `begin()` | DHCP blocks inside `begin(mac)` |
| Binary footprint (selftest, measured 2026-05) | 180 KB flash · 28 KB RAM1 vars · 56 KB RAM2 vars | 110 KB flash · 15 KB RAM1 vars · 12 KB RAM2 vars |
| Portability to STM32H743       | **Direct** — lwIP is the standard embedded TCP/IP stack on STM32 (CubeMX, `stm32duino`) | **None** — FNET is Teensy-specific |
| Ham-radio-Teensy ecosystem use | Newer / less common in this niche | **Dominant** — Morconi, TeensyMaestro, IW7DMH's `FlexRigTeensy` library all use NativeEthernet |
| WS/MQTT/HTTPS ecosystem fit    | Pairs cleanly with lwIP-based libs (Mongoose, etc.) | Self-contained ecosystem; cross-library integration thinner |

##### Why we support both

The default choice for *this* project is **QNEthernet**, because:

1. **Phase-2 portability.** The MCU fallback target is STM32H743 (see
   "MCU selection" in [`../CLAUDE.md`](../CLAUDE.md)). STM32 uses lwIP
   via CubeMX. With QNEthernet's lwIP base, the net-stack code is
   structurally identical on both targets — only the PHY init layer
   under `hal/` changes. Switching to NativeEthernet now and then
   migrating to STM32 later would mean rewriting the net layer.

2. **WebSocket server.** Our wire protocol ([`PROTOCOL.md`](PROTOCOL.md))
   needs an embedded WS server on the controller. Most embedded WS
   implementations target lwIP directly, so the WS framer can sit on
   top of QNEthernet's exposed lwIP API with minimal adaptation.

3. **Active maintenance.** Shawn Silverman responds to GitHub issues in
   days; recent releases land regularly. NativeEthernet works but is
   maintenance-mode at this point.

But **NativeEthernet is supported as a first-class alternative**
because:

1. **It's the de-facto ham-radio convention on Teensy 4.1.** The
   reference projects Morconi (remote CW interface for FlexRadio) and
   TeensyMaestro (Flex 6000 controller) both use NativeEthernet via
   IW7DMH's `FlexRigTeensy` library. Forum threads, troubleshooting,
   and code-sharing on the PJRC forum are easier with NativeEthernet.
2. **It's worth being able to A/B test.** If a future RF-immunity
   problem on the live Doublet manifests at the network layer (e.g.,
   the stack stalls under heavy EMI), having both libraries
   build-ready lets us swap one line in `platformio.ini` and observe.
3. **Independent verification.** Having a second backend that compiles
   the same source code is a strong sanity check on the abstraction —
   if the `net_hal` layer was too thin or too coupled, the second
   backend wouldn't compile.

##### Where the choice lives in code

A small abstraction in `firmware/.../src/net_hal.h` exposes the *only*
Ethernet operations the firmware uses — `begin`, `wait_link`,
`wait_dhcp`, `link_state`, `link_speed_mbps`, `link_full_duplex`,
`hw_mac`, `lib_name` — plus a typedef so callers see
`EthernetClient` / `EthernetServer` regardless of backend namespace.
Two implementation files under `#ifdef` guards:

```
firmware/teensy-selftest/src/
├── net_hal.h                       # interface; backend selected by build flag
├── net_hal_qnethernet.cpp          # active iff TUNER_NET_QNETHERNET
├── net_hal_nativeethernet.cpp      # active iff TUNER_NET_NATIVEETHERNET
└── main.cpp                        # calls net_hal::*; backend-agnostic
```

`platformio.ini` selects via build flag + `lib_deps`. The CI build
matrix should build *both* environments so a code change that breaks
either backend fails before merge:

```
pio run -e teensy41          # default: QNEthernet
pio run -e teensy41_native   # alternative: NativeEthernet
```

Both produce identical operational behaviour on the wire — they just
have different libraries underneath. The choice is the operator's, and
it can be revisited without source-code changes.

#### 5.1.3 HAL backend strategy (sim today, drivers next)

The same `hal/` directory holds two parallel sets of backend
implementations: a **simulation** backend that compiles on every target
and runs on the bench without any wired peripherals, and the
target-specific driver backends that land at M1b.2 hardware
integration. Application code (`src/app/`) and the protocol layer
(`tuner_server.cpp`) link only against the namespaces declared in
[`hal/hal.h`](../firmware/tuner-controller/src/hal/hal.h) and have no
knowledge of which backend is wired underneath.

| Namespace        | Sim today (M1b.2 software half)           | Real driver next (M1b.2 hardware half)                |
|------------------|-------------------------------------------|--------------------------------------------------------|
| `hal::motor`     | `motor_sim.cpp` — virtual stepper, advances kStepsPerTick toward target each `motor::tick()`. | TB6600 driver behind `TARGET_TEENSY41`; step pulses generated by **FlexPWM** with reload-IRQ step counting (per-axis). The bench reference impl in [`firmware/t41-stepper-test/src/flexpwm_stepper.h`](../firmware/t41-stepper-test/src/flexpwm_stepper.h) is the prototype — copy that pattern. Step / DIR / EN polarities + ≥ 5 µs pulse width per the bench findings in PROPOSAL.md. |
| `hal::encoder`   | `encoder_sim.cpp` — couples count to motor position 1:1. | **Optional** per axis. When fitted: hardware QEI (Teensy XBAR + TMR; STM32 TIM encoder mode) reads the optical encoder on the NEMA 23's rear extended shaft. When omitted: same interface returns the open-loop FlexPWM step counter (no drift detection, but invariant 3's anchoring still works via the limit-switch home). |
| `hal::relay`     | `relay_sim.cpp` — pure state.             | GPIO → opto-isolated MOSFET → HV bias.                |
| `hal::safety`    | `safety_sim.cpp` — operator-injected Fwd W. | AD8307 fwd channel ADC sample compared to threshold. |

The swap-in path at M1b.2 hardware integration is:

1. Add `motor_teensy41.cpp` under `TARGET_TEENSY41` gating; remove the
   sim source from the build (or keep both behind a compile-time
   `TUNER_HW_BACKEND` flag for A/B testing).
2. Repeat for `encoder`, `relay`, `safety`.
3. Native test target keeps using the sim files — host-side unit tests
   in `test/test_motion_native/` continue to validate the verb-accept
   matrix and the RF lockout path on every CI run.

The portability rule still holds: real driver files live under their
own `#ifdef TARGET_*` block, and the STM32H743 fallback at Phase 2 is
a parallel `motor_stm32h7.cpp` etc. — no application changes required.

**End-to-end against the sim today:**

```
browser ── /ws WS frame ──► master/hub ──► forwarding handler
                                              │
                                              ▼
                                      tunerclient.Send()
                                              │
                                          line-JSON :8089
                                              ▼
            Teensy 4.1 ── tuner_server::dispatch ──► app::motion
                                                          │
                                                          ▼
                                              hal::motor / encoder /
                                              relay / safety (sim)
                                                          │
                          state.differs() ◄──── app::motion::tick(now, snapshot)
                                  │
                                  ▼
                       tuner_server::publish → broadcast on every
                       client TCP connection (master is one of them)
                                              │
                                              ▼
                   tunerclient.handleFrame() → state.Core.UpdateState
                                              │
                                              ▼
                          hub fan-out ──► every connected browser
```

### 5.2 Encoder strategy (incremental default, absolute optional)

The position-feedback contract from [`../CLAUDE.md`](../CLAUDE.md)
invariant #3 — *encoders are the position truth, anchored to a known
reference* — is satisfied by **either** an incremental quadrature
encoder + anchor strategy **or** an absolute encoder. The HAL exposes
the same `encoder::l_count() / c_count()` interface in both cases;
application code never knows which is wired underneath.

#### 5.2.1 Why incremental is the default

- **Cost:** a 2000 CPR optical quadrature encoder runs $20–$50; a
  comparable single-turn absolute SSI encoder is $150–$300, multi-turn
  more.
- **Wiring:** 4 wires (A, B, +V, GND), optionally +Z for index; vs SSI's
  clock + data + power + shield, requiring careful routing and a CRC'd
  protocol.
- **Latency:** hardware quadrature decoders (Teensy 4.1 XBAR + TMR /
  STM32 TIM encoder mode) update the count on every edge in silicon —
  zero CPU cost, zero protocol overhead. SSI requires a polled clocked
  read.
- **Industrial track record:** every commercial roller-inductor /
  vacuum-cap tuner the author is aware of (Elecraft KAT-series,
  MFJ-998, SteppIR, Palstar) uses incremental quadrature with homing
  on boot.

#### 5.2.2 Anchoring an incremental count

An incremental encoder's count is meaningful only relative to a
reference. The controller establishes the reference one of three ways,
in priority order:

1. **NVRAM persistence (warm-boot fast path).** On every clean move
   completion, write `(l_steps, c_steps, l_enc, c_enc, side, bypass=true,
   gen++)` to a small NVRAM region (Teensy 4.1: on-board EEPROM or
   QSPI flash sector; STM32H743: option byte area or external FRAM).
   On boot, read the record. If `bypass=true` at last write and the
   record CRCs clean, **trust the persisted count** — set the encoder
   counter to `l_enc/c_enc` and the step counter to match. Boot to
   operational state in under a second. This is the expected path 99 %
   of the time.

2. **Homing (cold-boot fallback).** If NVRAM is invalid, corrupt, or
   indicates the last shutdown was unclean (`bypass=false` at last
   write, or motors mid-move), drive each axis slowly toward its
   mechanical home stop. End detection is:
   - **Mechanical limit switch** — per-axis opto-isolated input on the
     V2.09 carrier (X→pin 20, Y→pin 21, Z→pin 22; see
     [docs/HW-T41-PINMAP.md](HW-T41-PINMAP.md) §2). The chosen TB6600
     driver does not expose a StallGuard-equivalent sensorless-stall
     signal, so the limit switch is the only primary detection mechanism.
   At the detected switch transition, zero the encoder counter (if
   fitted) and the step counter. Then move to the persisted target if
   any, else stay at home. Cost: ~30 – 60 s per axis at safe speed.

3. **Optional: index pulse (Z phase).** Encoders with an index output
   produce one pulse per shaft revolution at a fixed mechanical angle.
   The controller uses this only as a *crosscheck* in normal operation
   (flag drift if an index pulse arrives at an unexpected count), and
   as a fast re-anchor after a partial-blind homing — drive past one
   revolution beyond the stop, capture the Z transition, and you have
   sub-degree absolute reference inside the post-gearbox shaft.

   Index support is **encoder-dependent**. The HAL exposes a
   `hal::encoder::index_seen(axis)` query; if the encoder has no Z
   phase it returns false forever, and the controller falls back to
   the limit-switch homing in (2).

#### 5.2.3 What changes if you choose absolute SSI

If the L axis has a long, slow mechanical travel where homing 30–60 s
on every cold boot is intolerable, swap the L encoder for an absolute
SSI unit. Concrete changes:

- **HAL:** drop `encoder_l_quad_teensy41.cpp`, add `encoder_l_ssi_teensy41.cpp`
  implementing the same `hal::encoder::l_count()` interface but reading
  via SPI/SSI instead of the hardware quadrature decoder. C axis can
  remain incremental.
- **Anchor strategy:** the boot fast-path in §5.2.2 (1) is unnecessary
  for that axis — the encoder reports its real position immediately,
  every boot. NVRAM persistence is still useful as a sanity check
  (detect ±N count drift from last shutdown).
- **No protocol changes.** The wire format already treats `l_enc` as
  an `int32` — same field for either encoder kind.
- **No application-logic changes.** The tuning algorithm, the memory
  schema, and the GUI never see the difference.

The decision is **per-axis** and reversible — both implementations live
in the codebase, and the choice is a build-flag (or even a runtime
config knob if the hardware variants share connector pinout).

#### 5.2.4 Drift detection (applies to both encoder kinds)

On every move completion, the controller compares the *step delta*
(commanded microsteps) with the *encoder delta* over the same window.
If they diverge by more than a per-axis tolerance (default 5 counts on
a 2000 CPR encoder, ~0.25 % of one revolution), the controller:

1. Treats the encoder reading as truth (invariant #3 spirit) and
   rewrites the step counter.
2. Emits `status code:enc_resync` with the observed and expected counts
   so the operator sees what happened.
3. If three resync events happen in a 10 minute window, escalates to
   `level:warn` — likely indicates mechanical slip, encoder noise, or
   a step-pulse generation problem worth investigating.

#### 5.2.5 Mechanical safety stack (protecting the vacuum cap)

TB6600 is open-loop and exposes no stall-feedback signal (no
StallGuard-equivalent), so the firmware cannot trust the step
counter to mean "the rotor actually moved that far". A vacuum-
variable capacitor is unforgiving — past its mechanical stop the
bellows or the lead-screw can fracture. Cap protection is by
concentric envelopes, not by trusting any single mechanism. Listed
outermost to innermost, all are enforced from M1b.2 onward unless
noted:

1. **Driver current dip-switch set to motor-rated, not max** (M1b.2
   commissioning step). The motor stalls before it can build enough
   torque to break the gearbox or the cap.

2. **No motion under RF** (invariant 1) — eliminates the highest-risk
   EMI-induced step-loss window (TX keyed, driver opto under noise)
   by refusing motion at all in that state.

3. **`homed:false` refuses motion** (invariant 3). Until `home` has
   run once this power cycle or a clean-shutdown NVRAM record proves
   anchoring, all motion verbs other than `home` are refused. This
   prevents a corrupt or stale step counter from issuing an
   out-of-envelope command.

4. **Per-axis software soft limits** derived from `home`. Once the
   homing routine has established the limit-switch positions at both
   ends of travel, the HAL refuses any commanded move outside
   `[home_low + SAFE_MARGIN, home_high − SAFE_MARGIN]` (default
   margin: 100 steps inside the switch position). The soft limits
   sit inside the limit switches, which sit inside the mechanical
   stops — three concentric envelopes.

5. **Per-axis mechanical limit switch** on the V2.09 carrier opto
   inputs (X→20, Y→21, Z→22 — see
   [docs/HW-T41-PINMAP.md](HW-T41-PINMAP.md) §2). Hit inside the
   cap's own mechanical hard stop, latched in firmware, the move
   halts immediately. This is the only hardware fallback the
   firmware has against a runaway pulse train if the soft limit is
   wrong (mis-calibration) or the step counter has drifted past it
   undetected.

6. **Optional rear-shaft encoder** (per-axis,
   [HARDWARE.md](HARDWARE.md) §"BoM"). When fitted, post-move
   step-vs-encoder reconciliation in §5.2.4 detects stalls the
   open-loop counter cannot see; ≥3 resync events in 10 min
   escalates to `level:warn` so a mechanical problem doesn't
   quietly destroy a cap over hours. Phase 1 ships without
   encoders — the M5 RF commissioning go/no-go decision in
   [PLAN.md](PLAN.md) determines whether stalls observed at full
   power justify adding them per axis.

The first install-time commissioning **must map each limit-switch
position to a safe number of steps inside the cap's mechanical
stop, both ends**. That mapping is per-build and not derivable from
the schematic — it is an M1b.2 calibration step recorded in NVRAM
alongside the rest of the per-axis configuration. The soft-limit
constants (`home_low`, `home_high`, `SAFE_MARGIN`) live with the
per-axis topology block, not in the HAL board header.

### 5.3 Master controller (Go, runs on Pi)

One process, four goroutines, one shared state — same shape as
LP-100A-Server:

| Goroutine          | Status   | Responsibility                                                                                |
|--------------------|----------|------------------------------------------------------------------------------------------------|
| `tuner-client`     | **✅ M1b.1** | Maintains the TCP line-JSON connection to the tuner controller. Auto-reconnect with 1 s → 30 s backoff. Decodes inbound frames; `Send(Command)` for outbound verbs. |
| `cat-poller`       | M3       | Polls the transceiver for QRG every `cat.poll_ms` (default 200 ms). Emits `qrg` events.       |
| `encoder-reader`   | M3       | Reads the two ANO encoders via GPIO; debounces; emits nudge/save events.                       |
| `hub`              | **✅ M1a + M1b.2** | WS server at `/ws` for the embedded web UI; fans out unified state to all browser/touch clients. Browser commands are forwarded to the controller via `tunerclient.Send()` through the `forwardingHandler` constructed in `main.go`. |

Plus the embedded HTTP server (`/`, `/ws`, `/healthz`, `/api/config`,
`/api/log-level`) serving the same kind of static UI bundle as
LP-100A-Server. `/api/log-level` is M6 hardening; `/healthz` and
`/api/config` are live now.

**Browser warm-start.** When a new WS client connects, the hub
immediately writes the latest `state`, `telemetry`, *and* a synthesised
`status` frame carrying the current controller-link state. Without the
status frame, a browser that opens *after* the controller is already
connected would never receive a link-state transition event and its
pill would stay red.

```
   ┌── Pi GPIO ──┐    ┌── USB ───────────┐   ┌── Ethernet ──────┐
   │  ANO enc ×2 │    │  CAT (CI-V/etc.) │   │  Tuner controller │
   └──────┬──────┘    └────────┬─────────┘   └────────┬─────────┘
          │ events             │ qrg                  │ telemetry + state
          ▼                    ▼                      ▼
   ┌────────────────────────────────────────────────────────────┐
   │                tuner-master (Go)                            │
   │   encoder-reader   cat-poller    tuner-client   hub         │
   │          \             |               /          \         │
   │           ▼            ▼              ▼            ▼        │
   │                  ┌──────────────┐                           │
   │                  │ state core   │  ◄── SQLite memory ────┐  │
   │                  │ (last-known  │                        │  │
   │                  │  snapshot)   │                        │  │
   │                  └──────┬───────┘                        │  │
   │                         │                                │  │
   │                         ▼                                │  │
   │                 embedded web UI + /ws ── browser/touch ──┘  │
   └─────────────────────────────────────────────────────────────┘
```

### 5.4 Memory schema (SQLite, on the master)

```sql
CREATE TABLE slot (
  band         TEXT    NOT NULL,    -- '160m', '80m', ..., '6m'
  freq_hz      INTEGER NOT NULL,    -- centre of bucket
  bucket_hz    INTEGER NOT NULL,    -- 25_000 / 50_000 / 100_000
  l_steps      INTEGER NOT NULL,
  c_steps      INTEGER NOT NULL,
  side         TEXT    NOT NULL CHECK(side IN ('hi_z','lo_z')),
  swr_at_save  REAL    NOT NULL,
  saved_at     TEXT    NOT NULL,    -- ISO8601
  label        TEXT,
  PRIMARY KEY (band, freq_hz)
);

CREATE INDEX slot_band_freq ON slot(band, freq_hz);
```

Recall picks the slot whose `freq_hz` is nearest to the rig's reported QRG
within the same band; if none within `2·bucket_hz`, fall back to band
default; if no default, fall to auto-tune.

### 5.5 Web UI (embedded in master binary)

Plain HTML/JS, served from `go:embed`, no SPA framework. Three views:

- **Operate** (default): big SWR meter, |Z|/∠Z readout, current band/QRG,
  current memory slot, L/C bar gauges, "Tune", "Save", "Bypass" buttons,
  and a connection-state pill. Mirrors the *intent* of the Alpha RF tuner
  GUI (clear primary readouts + one-touch tune) without copying its
  T-network controls.
- **Memory**: tabular view of all saved slots per band, with `swr_at_save`,
  last-tuned timestamp, delete/edit.
- **Setup**: log-level picker (like LP-100A-Server), CAT-rig selector,
  tuner-controller IP, encoder calibration helpers, optional LP-100A-Server
  URL for second-opinion telemetry.

The Adafruit ANO encoders drive the *same* state the web UI mutates; both
are equal first-class inputs.

**Status (2026-05-11):** Operate is a single-page layout today,
combining the live SWR/|Z|/∠Z/R/X panels with a control strip
(±10/±100/±1000 step nudges per axis, Hi-Z/Lo-Z, bypass engage/release,
re-home, fake-Fwd-W injector, last-ack readout). The "Tune" / "Save"
buttons and the dedicated Memory and Setup tabs ship in M3 alongside
the CAT poller and SQLite memory store.

## 6. Failure modes

| Failure                                  | Behavior                                                                                  |
|------------------------------------------|-------------------------------------------------------------------------------------------|
| Tuner controller WS drops                | Master shows red pill, reconnects 1 s → 30 s. Operator sees stale state with timestamp.   |
| RF detected during commanded motion      | Controller halts steppers within 1 ms, latches K3 bypass, emits `status:warn`. Master shows lockout banner; recall/auto-tune disabled until Fwd ≤ threshold for 3 s. |
| Encoder count diverges from step counter | Controller treats encoder as truth (§5.2.4), re-syncs step counter, emits `status:info enc_resync`. ≥3 events in 10 min → `status:warn`. |
| Stepper stall detected                   | TB6600 has no stall signal — detection is by post-move step-vs-encoder reconciliation (§5.2.4), only if a rear-shaft encoder is fitted on that axis. Phase 1 has no encoders, so silent stalls are caught only when they reach the limit switch (§5.2.5 layer 5) or when the operator notices SWR diverging from the saved memory. |
| Limit switch hit outside homing          | A move ran into the mechanical limit switch (§5.2.5 layer 5). Controller halts the axis immediately, latches the axis as `homed:false`, emits `status:warn limit_hit`. Operator runs `home` and investigates — soft limit was wrong or step counter drifted. |
| Cold boot with unclean NVRAM             | Controller refuses motion verbs except `home`; emits `status:warn` and runs auto-home on next `home` command. `homed:false` in state until complete (§5.2.2). |
| Cold boot with absolute encoder mismatch | (If absolute SSI used) controller compares the SSI reading against the last persisted count; >0.5 % drift emits `status:warn` and forces a homing crosscheck. |
| K1+K2 both reading closed                | Hardware fault. Controller drops K1+K2, latches K3, refuses motion, emits `status:error relay_fault`.|
| CAT link drops                           | Master continues with last-known QRG, badges it stale after 5 s. Auto-recall disabled.   |
| LP-100A second-opinion disagrees         | Cosmetic warning only; tuner's own measurement remains authoritative for control.        |
| Pi loses power mid-tune                  | Controller times out client cmds after 10 s with no heartbeat → halts motion, K3 bypass. NVRAM marked `bypass=true` only after the halt completes, so next boot homes rather than trusting stale persisted position. |

## 7. Out of scope (intentionally)

These exclusions are part of the architecture and should be enforced in
code review:

- **T-network or pi-network tuning** — separate project.
- **Balanced-tuner topology** — the balun is the boundary.
- **Hot-switching the L or C** under RF — interlocked in firmware and GUI.
- **Cloud relay / WAN access** — LAN + VPN if needed.
- **Long-term match logging / charting** — a separate WS subscriber.
- **Multi-tuner support** — one tuner per master process. Run multiple
  master instances on different ports if multi-tuner ever appears.
