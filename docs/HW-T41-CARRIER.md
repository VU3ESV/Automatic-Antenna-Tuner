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
  terminals — covers the 2 axes a Balanced L-Network needs and the 3
  axes a Balanced Pi-Network needs, with axes to spare for future
  expansion.
- 10 opto-isolated digital inputs — limit switches per axis, probe,
  reset, feed-hold, cycle-start, safety-door. Repurposed for the
  lead-screw limit switches (home + max in series, one input per axis),
  the iHSS60 PED / ALM drive feedback, and the operator panic line.
- 7 relay-driver outputs (open-collector, 5 V/12 V coil-voltage jumper)
  — drives the Balanced-L selector relays (K1/K2) and the bypass (K3),
  each two-pole from one output, plus any bandswitch / antenna-select /
  fault-output we add later.
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
| Opto-isolated inputs | 10 (EL357N family on V2.09; EL3H7 from V2.20). Electrically: +5 V → 330 Ω → LED → Sig; assert by sinking ≈ 11 mA to Gnd; Teensy reads LOW. See [HW-T41-PINMAP.md](HW-T41-PINMAP.md) §2.1 |
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

### Balanced L-Network (2 axes, default)

| Function                | Carrier resource                          | Notes                                                |
| ----------------------- | ----------------------------------------- | ---------------------------------------------------- |
| `L` inductor-pair stepper | Axis X (STEP/DIR/EN)                    | One JMC iHSS60 integrated closed-loop stepper coupled directly to the first roller inductor; the second coil is belted 1:1 off the same shaft (CLAUDE.md hardware contract) |
| `C` vacuum-cap stepper  | Axis Y (STEP/DIR/EN)                      | Same drive, coupled directly to the capacitor shaft  |
| `L` limit switches      | Opto input — limit X (pin 20)             | Home + max switches beside the axis's 3:1 lead screw, NC in series: open = at a limit or a cable fault |
| `C` limit switches      | Opto input — limit Y (pin 21)             | Same                                                 |
| Drive PED, motors X / Y | Opto inputs — limit A / B (pins 23 / 28)  | Arrival confirmation per move; opt-in ([DRIVE-FEEDBACK.md](DRIVE-FEEDBACK.md)) |
| Drive ALM, all drives   | Opto input — safety door (pin 29)         | The drives' ALM outputs in parallel; opt-in          |
| Hi-Z relay K1           | Relay driver — pin 12                     | Two-pole (both line legs); external 26 V vacuum-relay coils |
| Lo-Z relay K2           | Relay driver — pin 11                     | Same                                                 |
| Bypass relay K3         | Relay driver — pin 19                     | Two-pole changeover; de-energised = bypass, latched at power-up per invariant 2 |
| TX-key panic input      | Opto input — feed-hold (pin 16)           | Hardware kill while PTT'd                            |
| RF detector chain       | Direct to Teensy ADC pins (not via opto)  | AD8302 + AD8307×2; analog, bypasses the carrier inputs |

### Balanced Pi-Network (3 axes)

| Function                | Carrier resource                          | Notes                                                |
| ----------------------- | ----------------------------------------- | ---------------------------------------------------- |
| `C1` stepper            | Axis X                                    | Capacitor across the line, transceiver side          |
| `L` inductor-pair stepper | Axis Y                                  | Synchronized pair in series, as in Balanced L        |
| `C2` stepper            | Axis Z                                    | Capacitor across the line, antenna side              |
| Limit switches          | Opto inputs — limits X / Y / Z (20 / 21 / 22) | Home + max per axis, NC in series                |
| Drive PED, motor Z      | Opto input — probe (pin 15)               | In addition to PED X / Y above                       |
| Bypass relay K3         | Relay driver — pin 19                     | K1 / K2 are absent: the Pi needs no side selector    |

The axis columns are the firmware defaults (`hal/board/t41_v209.h`).
Element-to-axis mapping is **per-install configuration**, not
hard-coded: the HAL exposes axes 0/1/2 as opaque `hal::Axis` handles,
and the `set_topology` block (served by the master, persisted on the
controller) names each element, gives its type (L / C), binds it to an
axis — the operator's motor-to-component choice — marks the inductor
pair, and carries its calibration and limits.

## Topology selection (firmware)

The chosen topology is determined at runtime, not compile-time:

- The controller boots with the topology it last persisted; a fresh
  controller starts on the default Balanced L map (`L` → axis X,
  `C` → axis Y).
- `set_topology` (see [docs/PROTOCOL.md](PROTOCOL.md)) carries
  `{ kind: "balanced_l" | "balanced_pi", elements: [...] }`. The
  firmware checks that the element set matches the kind
  (`bad_elements`), that every axis exists (`bad_axis`) and that no two
  elements share one (`duplicate_axis`), and marks `L` as the inductor
  pair.
- The verb is accepted only with bypass engaged and no axis moving
  (`not_bypassed`, `moving`); the controller then commits the topology
  to NVRAM and applies it at once. `set_side` is refused with
  `wrong_topology` on a Balanced Pi.
- The verb only declares what is wired — physically changing the
  network is a hardware change. [CLAUDE.md](../CLAUDE.md) "RF topology"
  calls for a power cycle after a change; the firmware does not enforce
  one today.

The HAL sizes its axis arrays at compile time to `hal::kMaxAxes = 3`; a
Balanced L leaves the third channel unbound and unused.

## Firmware architecture impact

- **No new MCU port** — the Teensy 4.1 target already exists. Carrier
  boards live one layer below the MCU port and surface as a
  `hal/board/` file selected by a PlatformIO env.

```
firmware/tuner-controller/
├── src/
│   ├── hal/
│   │   ├── hal.h               # motor, encoder, limits, feedback, relay, safety, nvs, sdcard, firmware, led
│   │   ├── *_teensy41.cpp      # real drivers on the V2.09 carrier (TARGET_TEENSY41)
│   │   ├── *_sim.cpp           # simulation backends for the native unit tests
│   │   └── board/
│   │       ├── t41_v209.h      # Teensy-4.1 + grblHAL-T41-V2.09 carrier
│   │       └── t41_v209.cpp    #   pin map only; no logic
│   ├── app/                    # topology-aware motion, config, settings, OTA, protocol
│   ├── tuner_server.cpp        # master link (line-JSON over TCP, port 8089)
│   └── http_server.cpp         # bring-up page + firmware update (port 80)
├── test/                       # native unit tests (env:native)
└── platformio.ini              # one env per (MCU, Ethernet backend), plus *_ota upload variants
```

- The board file is **pin map + relay enable polarity + opto polarity
  + jumper-driven defaults**. Pin numbers are derived from the upstream
  V2.07 schematic / `T41U5XBB_map.h` in grblHAL's iMXRT1062 source
  tree; we re-typeset them in our board file (factual data, not
  copyrightable). The full per-pin allocation, including which
  carrier output drives K1/K2/K3 and which inputs serve as end-stops
  and as the TX-key panic line, is documented in
  [HW-T41-PINMAP.md](HW-T41-PINMAP.md).
- The STM32H743 Phase-2 carrier (if/when built) gets its own
  `hal/board/stm32h743_tuner.{h,cpp}`. The application layer never
  notices the swap, per the firmware-portability rule.

## Bring-up plan (delta from [docs/PLAN.md](PLAN.md))

This board changes M1b.2 onwards. M0 / M1a / M1b.1 are unaffected.

| Step  | Goal                                                                                           | Status (2026-09-14) |
| ----- | ---------------------------------------------------------------------------------------------- | ------------------- |
| H1    | **Buy / assemble** one T41E5XBB V2.09 board (or open-box T41U5XBB if Ethernet not yet needed). Verify against upstream BOM. | ✅ bench board in service |
| H2    | **Wire up the iHSS60 integrated closed-loop steppers** — STEP/DIR/EN from channels X / Y, plus Z for a Balanced Pi (TB6600 / DM542 acceptable as bench substitutes) — and each axis's lead-screw home + max limit switches, NC in series into its limit input. | ◐ drives on the bench; lead-screw limit mechanism pending |
| H2b   | **Closed-loop driver feedback.** Wire PED per motor and the ALMs in parallel per [HW-T41-PINMAP.md](HW-T41-PINMAP.md) §2.2, verify ≈ 10 mA LED current through the drive's opto transistor and a clean LOW at the Teensy, then enable supervision from the browser page ([DRIVE-FEEDBACK.md](DRIVE-FEEDBACK.md)). P10 stays at the drive default 0; P10 = 1 needs the HISU tool and series ALM wiring. | ◐ firmware ✅ 2026-09-13; wiring pending |
| H3    | **Wire three relay outputs** (pins 12 / 11 / 19) to a test board representing K1/K2/K3, each two-pole. | ◐ firmware ✅ 2026-09-06; wiring pending |
| H4    | **Wire the Ethernet jack** (PJRC kit, T41E5XBB variant only) and prove the existing tuner-controller's network HAL still pings the master. | ✅ |
| H5    | **Re-validate firmware-portability rule:** the existing tuner-controller firmware must compile against the new `hal/board/t41_v209.*` with no application-layer change. | ✅ 2026-09-06 |
| H6    | **Topology negotiation:** implement `set_topology` verb and the topology-aware state machine; verify Balanced L end-to-end via simulated RF, then on the real Doublet at low power. | ◐ verb ✅ 2026-09-06; RF verification with M2 / M5 |
| H7    | ~~**T-Match dry-run**~~ — dropped 2026-09-05 with T-Match.                                     | —                   |
| H8    | **Balanced Pi dry-run** on the bench (no antenna, fixed dummy load) to prove the 3-axis path. Initial support is "drive each element to a commanded position", not auto-tune. | ◐ bench board declared `balanced_pi` on three motor channels |
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
| User manual, all versions V2.07–V2.21 (BOM, errata, input/relay wiring) | <https://github.com/phil-barrett/grblHAL-teensy-4.x/blob/master/T41U5XBB%20User%20Manual.pdf>                                          |
| grblHAL web-builder T41U5XBB pin map (JSON in zip)    | <https://github.com/phil-barrett/grblHAL-teensy-4.x/blob/master/iMXRT1062_T41U5XBB-files.zip>                                                        |
| Maintainer's layout-redistribution policy (Issue #134) | <https://github.com/phil-barrett/grblHAL-teensy-4.x/issues/134>                                                                                      |
| grblHAL iMXRT1062 source (for `T41U5XBB_map.h`)        | <https://github.com/grblHAL/iMXRT1062>                                                                                                                |
