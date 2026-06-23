# Hardware port — grblHAL Teensy 4.x carrier board (V2.09)

Plan for using **Phil Barrett's grblHAL-teensy-4.x V2.09 board** as the
tuner-controller carrier, instead of a bespoke Teensy 4.1 carrier PCB.

Upstream repository:
[github.com/phil-barrett/grblHAL-teensy-4.x](https://github.com/phil-barrett/grblHAL-teensy-4.x).

## Why this board

The board is sold as a 5-axis CNC controller, but the underlying silicon
is just a **Teensy 4.1 breakout** with the peripherals an antenna tuner
also needs:

- Teensy 4.1 socket — matches the [CLAUDE.md](../CLAUDE.md) Phase-1
  MCU choice exactly. The firmware-portability rule (Teensy → STM32H743
  must remain a small port) is unaffected: the application layer never
  touches the carrier's GRBL/CNC heritage.
- 5 independent stepper channels (STEP / DIR / EN per axis) on screw
  terminals — covers the 2 axes an L-Match needs and the 3 axes a T-
  or Pi-Match needs, with axes to spare for future expansion.
- 10 opto-isolated digital inputs — limit switches per axis, probe,
  reset, feed-hold, cycle-start, safety-door. Repurposable for our
  end-stops, the operator panic line, and any auxiliary inputs.
- 7 relay-driver outputs (open-collector, 5 V/12 V coil-voltage jumper)
  — drives the L-network selector relays (K1/K2/K3) and any
  bandswitch / antenna-select / fault-output we add later.
- 4 EMI-protected, Schmitt-triggered digital inputs (15.9 kHz LPF) —
  spare for tach / encoder index / external trigger.
- 0–10 V analog spindle output — **not used** by the tuner, but
  available if we ever need an analog control signal.
- Optional **Ethernet via PJRC Ethernet Kit** on the T41E5XBB SKU
  (functionally V2.09's E-variant) — meets CLAUDE.md's Ethernet-only
  network requirement without any extra PCB work.
- microSD via the Teensy's on-board slot.

The financial trade-off (a board purchase vs. a one-off PCB spin) is
favourable for Phase 1 development. Whether to migrate to a custom
carrier at Phase-2 RF commissioning is still a separate decision
([CLAUDE.md](../CLAUDE.md) "MCU selection" §Phase 2).

## What we are **not** taking from upstream

- **grblHAL firmware** is not used. The firmware in this repo is a
  fresh tuner-controller, written against the existing
  [firmware/tuner-controller/](../firmware/tuner-controller/) skeleton
  and our own HAL. We use Phil Barrett's project as a **hardware
  reference** only.
- **Gerbers / Eagle layout** are not mirrored. The upstream maintainer
  has stated (Issue #134) he does not redistribute layout files for
  newer revisions; V2.09 happens to have its Eagle source published in
  the repo, but the repo has no LICENSE file (default *all rights
  reserved*), so even those files are not ours to redistribute.
- **Schematic PDFs and board photos** likewise stay upstream. This doc
  links to them; it does not mirror them. If you ever need a copy in
  this repo, ask Phil Barrett for written permission first.

## V2.09 board summary

| Subsystem            | V2.09 specifics                                                                                  |
| -------------------- | ------------------------------------------------------------------------------------------------ |
| MCU                  | Teensy 4.1 socket (T41 prefix in the board family name)                                          |
| Stepper outputs      | 5 axes, screw terminals, STEP/DIR/EN per axis (external drivers — Geckodrive, DM542, TMC2208 carrier, etc.) |
| Opto-isolated inputs | 10 (EL357N family on V2.09; EL3H7 from V2.20)                                                   |
| Aux digital inputs   | 4, EMI-filtered, Schmitt-trigger                                                                 |
| Relay drivers        | 7, open-collector, coil-voltage jumper 5 V/12 V                                                  |
| Analog output        | 1 × 0–10 V (op-amp), unused in this project                                                      |
| Ethernet             | PJRC Ethernet Kit, populated only on the T41E5XBB variant                                        |
| microSD              | Via Teensy 4.1 on-board slot                                                                     |
| Power                | 5 V and 12 V both required (separate LEDs)                                                       |
| Connectors           | Screw terminals for I/O; pin headers for I2C and Serial daughterboards; USB-B via Teensy        |
| Form factor          | 85 × 96 mm, 2-layer FR4, 1.6 mm                                                                  |
| Versus V2.07/V2.08   | Cosmetic + extra mounting holes; **electrically identical** to V2.07. V2.07 schematic PDF is the canonical pin reference until a V2.09-specific PDF is published. |

The V2.09 release was the first revision after Ethernet support was
declared solid (Sept 2020) and is the **last revision in the upstream
repo with Eagle source available** — newer revs (V2.10+) ship as PDF
only and have migrated to KiCad.

## Mapping to tuner needs

### L-Match (2 axes, current baseline)

| Function                | Carrier resource                          | Notes                                                |
| ----------------------- | ----------------------------------------- | ---------------------------------------------------- |
| Roller-inductor stepper | Axis 0 (STEP/DIR/EN)                      | External stepper driver — TMC2209 per CLAUDE.md hardware table |
| Vacuum-cap stepper      | Axis 1 (STEP/DIR/EN)                      | Same driver family                                   |
| L axis end-stop         | Opto input — limit X                      | Mechanical microswitch at mechanical home            |
| C axis end-stop         | Opto input — limit Y                      | Same                                                 |
| Hi-Z relay K1           | Relay driver 1                            | Drives external 26 V vacuum-relay coil               |
| Lo-Z relay K2           | Relay driver 2                            | Same                                                 |
| Bypass relay K3         | Relay driver 3                            | Same; latched on at power-up per invariant 2         |
| TX-key panic input      | Opto input — feed-hold                    | Hardware kill while PTT'd                            |
| RF detector chain       | Direct to Teensy ADC pins (not via opto)  | AD8302 + AD8307×2; analog, bypasses the carrier inputs |

### T-Match / Pi-Match (3 axes, added scope)

| Function                | Carrier resource                          | Notes                                                |
| ----------------------- | ----------------------------------------- | ---------------------------------------------------- |
| Element-1 stepper       | Axis 0                                    | T: series-C₁; Pi: shunt-C₁                           |
| Element-2 stepper       | Axis 1                                    | T: series-C₂; Pi: series-L                           |
| Element-3 stepper       | Axis 2                                    | T: shunt-L; Pi: shunt-C₂                             |
| End-stops               | Opto inputs — limits X / Y / Z            | One per axis                                         |
| Selector/bypass relays  | Relay drivers 1–3 (or more)               | T/Pi may not need the L-Match Hi-Z/Lo-Z selector, but reuse K3 as bypass |

Element-to-axis mapping is **per-topology configuration**, not hard-
coded. The HAL exposes axes 0/1/2 as opaque `step_axis_t` handles; the
application layer reads a TOML/JSON topology block (served by the
master, persisted on the controller) that names each element, gives its
type (L / C), connects it to a HAL axis, and stores its
calibration and limits.

## Topology selection (firmware)

The chosen topology is determined at runtime, not compile-time:

- The controller boots with **topology = unknown**, refuses every
  control verb except `set_topology` and `home`.
- The master sends a `set_topology` frame (new verb, see
  [docs/PROTOCOL.md](PROTOCOL.md)) carrying `{ kind: "L" | "T" | "Pi",
  elements: [...] }` either at first connect or from the saved
  station config.
- Once accepted, the controller commits the topology to NVRAM and
  starts accepting motion verbs.
- Switching topology requires a power cycle (intentional — physically
  changing the network is a hardware change, the verb just declares
  what's wired).

The HAL's `stepper_t` array sizes itself at compile time to
`MAX_TOPOLOGY_AXES = 3`; an L-Match leaves the third element unbound
and unused.

## Firmware architecture impact

- **No new MCU port** — the Teensy 4.1 target already exists. Carrier
  boards live one layer below the MCU port and surface as a
  `hal/board/` file selected by a PlatformIO env.

```
firmware/tuner-controller/
├── hal/
│   ├── stepper.h           # generic axis interface
│   ├── encoder.h
│   ├── adc.h
│   ├── relay.h
│   └── board/
│       ├── t41_v209.h      # Teensy-4.1 + grblHAL-T41-V2.09 carrier
│       ├── t41_v209.cpp    #   pin map only; no logic
│       └── bench.h         #   bare Teensy-4.1 dev board (current)
├── app/                    # topology-aware control loop, protocol, …
└── platformio.ini          # one env per (MCU, carrier) pair
```

- The board file is **pin map + relay enable polarity + opto polarity
  + jumper-driven defaults**. Pin numbers are derived from the upstream
  V2.07 schematic / `T41U5XBB_map.h` in grblHAL's iMXRT1062 source
  tree; we re-typeset them in our board file (factual data, not
  copyrightable).
- The STM32H743 Phase-2 carrier (if/when built) gets its own
  `hal/board/stm32h743_tuner.{h,cpp}`. The application layer never
  notices the swap, per the firmware-portability rule.

## Bring-up plan (delta from [docs/PLAN.md](PLAN.md))

This board changes M1b.2 onwards. M0 / M1a / M1b.1 are unaffected.

| Step  | Goal                                                                                           |
| ----- | ---------------------------------------------------------------------------------------------- |
| H1    | **Buy / assemble** one T41E5XBB V2.09 board (or open-box T41U5XBB if Ethernet not yet needed). Verify against upstream BOM. |
| H2    | **Wire up two TMC2209 stepper drivers** (or DM542 substitutes) to axes 0/1, motor + opto-input limit-switch on each axis. |
| H3    | **Wire three relay outputs** (driver 1/2/3) to a 3-relay test board representing K1/K2/K3.    |
| H4    | **Wire the Ethernet jack** (PJRC kit, T41E5XBB variant only) and prove the existing tuner-controller's network HAL still pings the master. |
| H5    | **Re-validate firmware-portability rule:** the existing tuner-controller firmware must compile against the new `hal/board/t41_v209.*` with no application-layer change. |
| H6    | **Topology negotiation:** implement `set_topology` verb and the topology-aware state machine; verify L-Match end-to-end via simulated RF, then on the real Doublet at low power. |
| H7    | **T-Match dry-run** on the bench (no antenna, fixed dummy load) to prove the 3-axis path. Tuning algorithm changes deferred — initial T-Match support is "drive each element to a commanded position", not auto-tune. |
| H8    | **Pi-Match dry-run** — same scope as H7.                                                       |
| H9    | **Phase-2 decision** — same content as the existing PLAN.md M5 RF-commissioning go/no-go. The decision now also includes "does the off-the-shelf carrier's ground plane and opto isolation hold up at full-legal-limit RF, or do we still need a custom carrier?"  |

## Licensing and IP

- Upstream repo has **no LICENSE file** — default copyright applies.
- This plan **links** to upstream artefacts; it does **not** mirror
  PDFs, photos, or schematic source.
- Pin-mapping tables we author in `hal/board/t41_v209.{h,cpp}` are
  factual data (Teensy pin number ↔ board net name) and are not
  copyrightable expression.
- If we ever need to mirror Phil Barrett's schematic PDF or board
  photos into this repo (e.g., for offline build-instruction packs),
  **email him for written permission first** and include the permission
  text in `docs/LICENSES.md` alongside the file.

## References (upstream, link-only)

| Resource                                              | URL                                                                                                                                                  |
| ----------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| Upstream repository                                   | <https://github.com/phil-barrett/grblHAL-teensy-4.x>                                                                                                  |
| V2.07 schematic PDF (canonical for V2.07–V2.09)        | <https://github.com/phil-barrett/grblHAL-teensy-4.x/blob/master/v2.07%20schematic.pdf>                                                              |
| V2.09 Eagle source (`.sch` + `.brd`) zip               | <https://github.com/phil-barrett/grblHAL-teensy-4.x/blob/master/teensy%204.1x209.zip>                                                                |
| V2.09 mechanical STEP model                            | <https://github.com/phil-barrett/grblHAL-teensy-4.x/blob/master/teensy%204.1x209%20STEP.zip>                                                         |
| V2.09 "Unkit PCB" photo (referenced in upstream README) | <https://github.com/phil-barrett/grblHAL-teensy-4.x/blob/master/RA159231_DxO_2048.jpg>                                                              |
| Closest user manual (V2.07)                           | <https://github.com/phil-barrett/grblHAL-teensy-4.x/blob/master/T41U5XBB%20v207.pdf>                                                                 |
| grblHAL web-builder T41U5XBB pin map (JSON in zip)    | <https://github.com/phil-barrett/grblHAL-teensy-4.x/blob/master/iMXRT1062_T41U5XBB-files.zip>                                                        |
| Maintainer's layout-redistribution policy (Issue #134) | <https://github.com/phil-barrett/grblHAL-teensy-4.x/issues/134>                                                                                      |
| grblHAL iMXRT1062 source (for `T41U5XBB_map.h`)        | <https://github.com/grblHAL/iMXRT1062>                                                                                                                |
