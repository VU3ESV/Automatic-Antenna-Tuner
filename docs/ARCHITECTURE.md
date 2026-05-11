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
   │   │  Tuner Controller  (Teensy 4.1 + W5500 Ethernet)            │    │
   │   │   ┌────────────┐  ┌────────────┐  ┌───────────────┐         │    │
   │   │   │ Motor loop │  │ ADC sampler│  │ WS/JSON server│◄── LAN ─┼────┘
   │   │   │ (steppers, │  │ (Fwd, Rev, │  │ (lwIP, ~5 cli)│         │
   │   │   │  encoders, │  │  AD8302 I/Q│  │               │         │
   │   │   │  StallGd)  │  │  log dets) │  │               │         │
   │   │   └─────┬──────┘  └─────┬──────┘  └───────┬───────┘         │
   │   │         │               │                 │                 │
   │   │         ▼               ▼                 ▼                 │
   │   │   ┌────────────┐  ┌────────────┐  ┌───────────────┐         │
   │   │   │ TMC2209 ×2 │  │ Detector   │  │ Relay drivers │         │
   │   │   │ Stepper drv│  │  bias supply│ │ (K1/K2/K3 HV) │         │
   │   │   └─────┬──────┘  └────┬───────┘  └──────┬────────┘         │
   │   └─────────┼───────────────┼─────────────────┼──────────────────┘
   │             ▼               ▼                 ▼
   │   ┌────────────────┐ ┌──────────────────┐  ┌─────────────────┐
   │   │ Roller L       │ │ Directional      │  │ Vacuum relays   │
   │   │ (NEMA17 + enc) │ │ coupler (Tandem) │  │ K1: load-side C │
   │   │                │ │  → Fwd/Rev to    │  │ K2: src-side C  │
   │   │ Vac variable C │ │  AD8307 ×2,      │  │ K3: bypass      │
   │   │ (NEMA17 + enc) │ │  AD8302 phase    │  └─────────────────┘
   │   └────────────────┘ └──────────────────┘
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

Three strategies, escalating:

### 4.1 Recall (fast path)

On any `recall(freq_hz)` (driven by either CAT polling or user input):

1. Compute `band` from `freq_hz`, then `bucket = round(freq_hz / bucket_size)`.
2. Look up `(l_steps, c_steps, side)` from SQLite.
3. If found and `swr_at_save ≤ swr_recall_threshold` (default 1.5):
   - Engage bypass (K3).
   - Drive both axes to target positions in parallel.
   - Switch K1/K2 to `side`.
   - Disengage bypass.
   - Operator may key TX; measured SWR is reported but no further motion.
4. If not found or stale: fall to nearest neighbour in the same band, then
   to band default, then to auto-tune.

Typical recall time: **< 4 s** including bypass make/break (steppers
microstep-limited, not RF-limited).

### 4.2 Auto-tune (slow path)

`auto_tune(freq_hz, power_w)` — operator keys a low-power tune carrier
(≤ 10 W typical, configurable). The controller:

1. Engages bypass; reports current `r`, `x`, swr.
2. **Coarse step:** uses `r`, `x` at the network input (with bypass engaged
   — i.e., the load impedance as seen through the balun, presented to the
   tuner port) to compute the analytic L-network solution:
   - If `r > 50`: choose **Hi-Z**; `Q = √(r/50 − 1)`; `Xl = Q·50 − x`;
     `Xc = r/Q · 50/(r − 50/(1+Q²))` (textbook closed form, see
     [`RF-DESIGN.md`](RF-DESIGN.md) §3).
   - If `r < 50`: choose **Lo-Z**; mirror form.
   - Convert `Xl → L` and `Xc → C` for the operating frequency, map to
     stepper positions via the per-axis calibration curve.
3. Drives to the analytic target; disengages bypass.
4. **Fine step:** hill-climbs in (L, C) on SWR with the operator keying the
   carrier. Step size shrinks geometrically. Stops when:
   - `swr ≤ swr_done_threshold` (default 1.10), or
   - improvement < 0.5 % over the last 8 iterations, or
   - operator cancels.
5. On success: emits a `memory` frame with the proposed slot; master GUI
   prompts the operator to *Save*. Auto-save only if `auto_save = true` in
   config (off by default — invariant #4 in `CLAUDE.md`).

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
   mechanical home stop. End detection is one of:
   - **TMC2209 StallGuard** (sensorless): the driver flags a stall when
     load torque crosses a tunable threshold. No mechanical limit
     switch needed.
   - **Limit switch** (optional belt-and-suspenders): mechanical switch
     wired to a GPIO with hardware debounce. Used as a redundant
     check, not the primary detection.
   At the detected end, zero the encoder counter and the step counter.
   Then move to the persisted target if any, else stay at home. Cost:
   ~30 – 60 s per axis at safe speed.

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
   the stall-only homing in (2).

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

### 5.3 Master controller (Go, runs on Pi)

One process, four goroutines, one shared state — same shape as
LP-100A-Server:

| Goroutine          | Responsibility                                                                                |
|--------------------|------------------------------------------------------------------------------------------------|
| `tuner-client`     | Maintains the WS connection to the tuner controller. Auto-reconnect with 1 s → 30 s backoff.  |
| `cat-poller`       | Polls the transceiver for QRG every `cat.poll_ms` (default 200 ms). Emits `qrg` events.       |
| `encoder-reader`   | Reads the two ANO encoders via GPIO; debounces; emits nudge/save events.                       |
| `hub`              | WS server at `/ws` for the embedded web UI; fans out unified state to all browser/touch clients.|

Plus the embedded HTTP server (`/`, `/ws`, `/healthz`, `/api/config`,
`/api/log-level`) serving the same kind of static UI bundle as
LP-100A-Server.

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

## 6. Failure modes

| Failure                                  | Behavior                                                                                  |
|------------------------------------------|-------------------------------------------------------------------------------------------|
| Tuner controller WS drops                | Master shows red pill, reconnects 1 s → 30 s. Operator sees stale state with timestamp.   |
| RF detected during commanded motion      | Controller halts steppers within 1 ms, latches K3 bypass, emits `status:warn`. Master shows lockout banner; recall/auto-tune disabled until Fwd ≤ threshold for 3 s. |
| Encoder count diverges from step counter | Controller treats encoder as truth (§5.2.4), re-syncs step counter, emits `status:info enc_resync`. ≥3 events in 10 min → `status:warn`. |
| Stepper stall (StallGuard)               | During homing: normal end-of-travel signal (§5.2.2). Outside homing: controller halts, emits `status:warn stall`. Operator runs `home`. |
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
