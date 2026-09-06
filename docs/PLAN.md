# Implementation Plan

Milestone-based plan. Each milestone produces something runnable on the
bench — no "implement everything then debug" steps. Order is chosen so the
RF chain is the *last* dependency: we prove out the controller, the
network, and the GUI against fakes/stubs first, then commission the real RF
hardware once.

For the architecture this plan implements, see
[`ARCHITECTURE.md`](ARCHITECTURE.md); for invariants, see
[`../CLAUDE.md`](../CLAUDE.md).

## Milestone overview

| M  | Name                        | Deliverable                                                                                            | Status (2026-05-11)                              |
|----|-----------------------------|--------------------------------------------------------------------------------------------------------|--------------------------------------------------|
| M0 | Scaffolding                 | Repo skeleton, CI, two binaries that build and exchange a heartbeat.                                   | **✅** (master + firmware skeleton + CI)         |
| M1 | Motion + position           | iHSS60 axes drive the elements directly, homed by belt-driven lead-screw limit switches; bypass + relay state machine; faked RF. | **M1a ✅ · M1b.1 ✅ · M1b.2 real drivers on the T41 carrier ✅ (2026-09-06) · lead-screw limit mechanism + ALM/PED pending HW** |
| M2 | Measurement                 | AD8302 + dual AD8307 chains live; SWR, R, X reported in `telemetry`.                                   | Pending hardware                                  |
| M3 | Master core + GUI           | Go master with embedded web UI, CAT polling, ANO encoders, memory store.                               | Partial — UI + WS hub + Operate panel + command forwarding done; CAT / ANO encoders / SQLite memory pending |
| M4 | Auto-tune algorithm         | Recall + analytic L-network solve + hill-climb fine-tune, validated on a dummy load network.           | Pending M2/M3                                     |
| M5 | RF commissioning            | Real Doublet on-air, per-band calibration of memory, soak test.                                        | Pending hardware                                  |
| M6 | Hardening                   | Reconnect, lockouts, log-level API, systemd unit, cross-compile to Pi, udev rules, release pipeline.   | **Pi deploy pipeline ✅** (cross-compile + install.sh + systemd + redeploy.sh); reconnect ✅ (tunerclient 1s→30s backoff); log-level / udev / release pipeline pending |
| M7 | Multi-antenna (Phase 2)     | Antenna abstraction, routing rules, memory schema with `antenna_id`, selector hardware.                | **Documented** in [`EXTENSIONS.md`](EXTENSIONS.md); gated on M5.   |
| M8 | Multi-transceiver (Phase 2) | Second CAT + radio input matrix + per-radio routing, sequential TX (no SO2R yet).                      | **Documented**; gated on M7.                                       |
| M9 | SO2R (Phase 2)              | Simultaneous TX on different antennas with BPF isolation + hardware interlock; ref 4O3A TGXL family.   | **Documented**; gated on M8.                                       |

### Scope change 2026-09-05 — balanced networks, lead-screw limit switches

Adopted after the iHSS60 bench work; contract in
[`../CLAUDE.md`](../CLAUDE.md) "RF topology" / "Hardware contract",
mechanics in [`HARDWARE.md`](HARDWARE.md) "Drive train":

- Topologies are now **Balanced L-Network (default)** and **Balanced
  Pi-Network**; the unbalanced L / T / Pi set with an output balun is
  superseded and T-Match is dropped. The **1:1 current balun moves to
  the transceiver side**, which closes the balun-ratio decision (#3
  below).
- The two roller inductors are a **synchronized pair on one motor** —
  one firmware axis (`L`). Axis counts stay 2 (L) / 3 (Pi).
- Every axis: **iHSS60 coupled directly to the element** (6400 steps
  per element turn); a small GT2 pulley on the motor shaft belts 3:1 to
  a parallel **lead screw** whose traveling nut block trips the **home
  and max limit switches**, wired NC-in-series to one carrier opto
  input.
- `set_topology` carries an **operator-chosen element→motor map**; the
  master gets a topology settings page (M3).

Milestones touched: M1b.2 (limit mechanism, relays, topology verb),
M3 (settings page), M4 (`L = 2 × L_leg`), M5 (leg-current balance,
drive-train checks), Phase 2 (Balanced Pi auto-tune, optional fixed-cap
bank).

- [ ] **Reconcile the remaining docs** that still describe the
      unbalanced design: `ARCHITECTURE.md` (§5.1.3 done 2026-09-06;
      §2 / §4 still unbalanced), `RF-DESIGN.md`, `HW-T41-CARRIER.md`,
      `HW-T41-PINMAP.md` (two-switch axes, opto-input budget, relay
      pairs), `TUNING.md`, `../README.md`, `../PROPOSAL.md`.
      Done 2026-09-06: `PROTOCOL.md` (`set_topology`, `move_axis`,
      per-axis `state` fields) and the axis-mapping comment in
      `firmware/tuner-controller/src/hal/board/t41_v209.h`.

Numbers below assume one operator and an existing bench (scope, signal
generator, dummy load + reactive simulator, RF wattmeter). Calendar weeks
are *very* rough — adjust to the user's hours.

---

## M0 — Scaffolding (≈ 1 week)

**Goal:** the two binaries exist, talk to each other over loopback, and CI
is green on every push.

- [x] `.gitignore`, `.editorconfig`, multi-root VS Code workspace,
      `.vscode/launch.json`. License **deferred** — pick before first
      tag (CC0 / MIT / dual).
- [ ] `git init` and first commit. Not yet run; the repo is a working
      directory only.
- [x] `master/tuner-master/` — Go module, `main.go`, TOML config loader,
      embedded `static/index.html`. (WS *server* live with fan-out;
      real WS *client* to the controller is the M1 firmware-side task.)
- [x] `firmware/tuner-controller/` — PlatformIO project with three envs
      (teensy41 / nucleo_h743zi / native), HAL split, `app::Snapshot`
      with native Unity tests. Ethernet + lwIP + WS server: **deferred
      to M1 firmware bring-up** (no Teensy in hand yet).
- [x] `docs/PROTOCOL.md` — written end-to-end at v1 (not just v0.1).
      Telemetry, state, memory, qrg, status, heartbeat, ack, all command
      verbs.
- [ ] CI: GitHub Actions running `go vet`, `go test -race`, `go build` on
      the master; `pio run -e teensy41 -e nucleo_h743zi` + `pio test
      -e native` on the firmware.
- [x] Cross-compile to Pi: `deploy/build-pi.sh` produces a stripped
      5.9 MB `linux/arm64` binary. Plus `deploy/install.sh`,
      `deploy/redeploy.sh`, `deploy/tuner-master.service` (systemd
      unit) — full Pi deploy pipeline. See repo README "Deploy the
      master to a Raspberry Pi".

**Exit criteria:** open `http://<pi-ip>:8088/` in a browser, see the
embedded UI showing a green "controller connected" pill when the Teensy is
on the LAN, red when it isn't. **Currently met against the
`fakecontroller` source; gated on real firmware Ethernet for the live
Teensy case.**

---

## M1 — Motion + position (≈ 2 weeks)

**Goal:** real iHSS60 axes drive real element shafts directly, with the
drive's internal loop closed and the belt-driven lead-screw limit
switches homing each axis. No RF involved; safety lockouts gated on a fake `fwd_w` value
the master can inject.

### M1a — Software scaffold (no hardware) ✅

Completed 2026-05-11 as the software-only portion of this milestone.

- [x] `docs/PROTOCOL.md` v1: full server→client and client→server frame
      set, error semantics, sequence numbering, reserved binary-frame
      lane.
- [x] Master `internal/protocol` — Go types for every frame, ack helpers,
      6 unit tests covering parse / round-trip / SWR-null / id-preserve.
- [x] Master `internal/state` — last-known `Snapshot`, pub/sub Event
      channel, drop-on-slow-subscriber, 6 race-tested unit tests.
- [x] Master `internal/hub` — `/ws` upgrader, fan-out goroutine per
      client, per-hop monotonic `seq` (wraps to 1), heartbeat ticker,
      command dispatcher seam.
- [x] Master `internal/fakecontroller` — synthetic source so the UI is
      exercised end-to-end before the real controller link exists.
- [x] Web UI subscribes to `/ws`, renders SWR / |Z| / ∠Z / R / X / L /
      C / side / bypass / QRG at ~10 Hz, auto-reconnects on disconnect.
- [x] Smoke-tested end-to-end: 4 s WS capture showed 1 state + 41
      telemetry + 2 qrg + 2 heartbeat, seq strictly monotonic.

### M1b — Firmware bring-up (requires Teensy 4.1 hardware)

#### M1b.1 — Network bridge ✅ (2026-05-11)

End-to-end: Teensy → master → browser shows real controller state.

- [x] Teensy + Ethernet kit hardware verified (selftest passes).
- [x] `net_hal` abstraction supports **QNEthernet** and **NativeEthernet**
      via build flag; both Teensy envs build clean. See
      [`ARCHITECTURE.md`](ARCHITECTURE.md) §5.1.2.
- [x] Firmware `app::protocol` — JSON serializers for `state`,
      `heartbeat`, `status`, `ack` (ArduinoJson v7 underneath).
- [x] Firmware `tuner_server` — TCP server on port 8089, line-JSON
      framing (PROTOCOL.md §1.0), warm-start `state` on connect,
      heartbeat every 2 s, up to 4 concurrent clients, dispatches
      `noop` / `resync` and acks unimplemented motion verbs with
      `unknown_action` (transparent stub state).
- [x] Master `internal/tunerclient` — TCP dial with 1 s → 30 s
      exponential reconnect backoff, line-delimited JSON decoder
      pushes inbound `state` / `telemetry` / `status` into
      `state.Core`. 3 unit tests cover decode, reconnect, outbound
      Send.
- [x] Master `main.go` — wires real `tunerclient` by default;
      `--fake-controller` flag retained for development without
      hardware.
- [x] End-to-end smoke verified: fake controller emits a state frame
      → tunerclient → state.Core → hub → browser `/ws` receives
      identical payload.

#### M1b.2 — Hardware integration (motion / measurement / safety)

- [x] **Firmware update over Ethernet (2026-09-06).** `POST /api/firmware`
      streams PlatformIO's hex into the controller, which stages it in
      free flash (FlasherX flash layer, `hal::firmware`), reports record
      count + CRC-32, and reboots into it on `firmware_apply` once the
      uploader has compared the CRC — gated on bypass, no motion, no RF.
      `pio run -e teensy41_native_ota -t upload` or the browser panel.
      EEPROM emulation region reserved; USB remains first-install and
      recovery. Rules in CLAUDE.md "Firmware update over Ethernet".
- [x] **Production controller running on the T41 carrier (2026-09-06).**
      `firmware/tuner-controller` now has real Teensy 4.1 backends behind
      the HAL — `motor_teensy41.cpp` (three FlexPWM axes via the shared
      `firmware/lib/flexpwm_stepper/` library, trapezoidal ramps, drives
      held enabled), `relay_teensy41.cpp` (K1/K2/K3 on carrier outputs
      12/11/19; bypass is the de-energised state and is latched before
      anything else at boot), `limits_teensy41.cpp` (end-stop opto
      inputs, reported), `nvs_teensy41.cpp` (emulated EEPROM with update
      semantics) — with sim backends kept for the native tests. The app
      layer gained `app/config` (Balanced L / Balanced Pi topology with
      the operator's element→axis map, per-axis element kind + rated
      travel + speed, clean-shutdown position anchors) and the bench's
      travel-window logic moved into `app::motion` where it is unit-
      tested (29 native tests). Invariant 3 is enforced as *anchoring*:
      an axis moves only when its home is declared and the last
      power-down was clean, except bounded setup moves in bypass. Two
      control surfaces share one verb layer: the master TCP link
      (`move_axis`, `run`, `set_home`, `set_element`, `set_speed`,
      `set_topology`, … — PROTOCOL.md) and an HTTP server + embedded
      page on port 80 (`http_server.h`), which is the operating UI until
      the Pi master is deployed. Flashed to the bench board the same day;
      motion on real elements not yet exercised through this build.
      Same day: latched per-motor / all-motor **E-stop** with industrial
      beacon + toggle mushroom buttons in the page, and **settings on
      the Teensy 4.1 microSD** (`/tuner/config.json`, human-readable;
      card wins for settings at boot, EEPROM stays authoritative for
      position anchors and mirrors everything — `app/settings.h`).
- [x] Add `hal::encoder`, `hal::motor`, `hal::relay`, `hal::safety`
      interfaces to the firmware HAL. Implementations for `TARGET_TEENSY41`
      gate stepper / QEI / GPIO calls; `TARGET_NATIVE` provides stubs
      so the host-side test suite keeps building.
      *(Done 2026-05-11 with a portable **sim** backend — `motor_sim`,
      `encoder_sim`, `relay_sim`, `safety_sim` — that compiles on every
      target. Real driver implementations replace the sim files when the
      hardware arrives.)*
- [x] `app::motion` task: drives the motor sim each loop iteration,
      rebuilds the published snapshot from the HAL, and gates every
      motion / side / home verb on the safety lockout. 8 native unit
      tests cover the verb-accept matrix end-to-end against the sim
      HAL.
- [x] Verb dispatch wired in `tuner_server`: `move_l`, `move_c`,
      `set_side`, `set_bypass`, `home`, plus a debug `set_fwd_w` so the
      lockout path is testable without a transmitter.
- [x] Master `hub` → `tunerclient.Send` forwarding handler — every
      well-formed browser command lands on the controller's wire.
- [x] Web UI "Operate" panel: ±10/±100/±1000 step nudges per axis,
      Hi-Z / Lo-Z toggle (auto-highlighted from `state.side`), bypass
      engage/release, re-home button, fake-Fwd-W injector, last-ack
      readout. Two-client smoke deferred to bench validation.
- [x] **Bench software travel window** (`firmware/test/t41-stepper-test`,
      2026-09-06): each bench axis declares the element it drives —
      roller inductor, vacuum-variable capacitor, variable capacitor
      with or without end stops, variometer — and, for elements with
      stops, its rated travel in revolutions. Once home is declared
      (O / "Set current pos as home") the firmware clamps every motion
      verb — jog, single step, ±N steps, ±N rev, run CW/CCW — to
      `[0, rated_rev × 6400]`, stops exactly on the bound, refuses a
      move that would leave the window, and the web UI shows a
      per-motor "STOPPED AT HOME / MAX LIMIT" badge plus a travel bar
      and turn counter. Single-step buttons (±1 / ±10 / ±100 / ±N)
      added for fine tuning; the ISR-counted position is updated for
      every pulse. Kind, home flag and rated travel persist in EEPROM.
      This is the bench prototype of the invariant-7 software soft
      limits; production adds `SAFE_MARGIN` inside the lead-screw
      switches and PED confirmation. Same day: third bench axis `Z` on
      the carrier's Z channel (FlexPWM2.2, own EEPROM block), so the
      rig now covers a Balanced Pi motor count.
- [ ] **Carrier-board bring-up** — assemble / verify Phil Barrett's
      grblHAL-teensy-4.x V2.09 board (T41E5XBB SKU for Ethernet) and
      author `firmware/tuner-controller/hal/board/t41_v209.{h,cpp}`
      with the pin map, relay-polarity, and opto-input-polarity for
      this carrier. Bare-Teensy dev wiring keeps working via a
      `hal/board/bench.h` env. Plan + reference URLs:
      [`HW-T41-CARRIER.md`](HW-T41-CARRIER.md).
- [ ] Wire two iHSS60 integrated closed-loop steppers (`L`-pair, `C`)
      via the carrier's axis-0/axis-1 STEP/DIR/EN screw terminals (a
      third, axis 2, for a Balanced Pi build). DIP = 6400 p/r; P8/P9
      currents and P16 following-error limit set via HISU for the
      belt-driven load. Each axis's two lead-screw limit switches wire
      NC-in-series to one carrier opto input (limit-X / limit-Y /
      limit-Z). Vacuum relays K1/K2/K3 — each two-pole, switching both
      line legs — driven from the carrier's relay-driver outputs 1–3
      (12 V coil jumper).
- [ ] **Build the limit mechanism** per [`HARDWARE.md`](HARDWARE.md)
      "Drive train": motor coupled directly to the element shaft; small
      GT2 pulley on the motor shaft, 3:1 belt to the large pulley on a
      parallel lead screw; guided traveling nut block / wheel; home and
      max switches positioned so they trip when the element is
      `SAFE_MARGIN` inside each mechanical stop. L axis: 1:1 belt off
      the motor shaft to the second roller inductor. Verify 6400 steps
      per element revolution and one lead-screw turn per three element
      turns on the bench; measure homing repeatability in steps and
      pick lead-screw lead / switch type from the HARDWARE.md table.
- [ ] Confirm each element's rated maximum shaft speed (vacuum-cap
      bellows, roller-inductor contact) and set the production speed
      limit from it — with direct drive, 6400 pps is 1 rev/s and the
      bench-saved 25 600 pps is 4 rev/s.
- [ ] **Limit-mechanism mitigations** (risk register in
      [`HARDWARE.md`](HARDWARE.md) "Risks of the belt-driven lead-screw
      limit mechanism"; rules in CLAUDE.md invariant 7): latched limit
      events by direction, homing travel watchdog, home→max span check
      against a stored reference, immediate pulse cut on trip, bounded
      pull-off when a switch is active at power-up, `SAFE_MARGIN` ≥
      stopping distance at homing speed, cam-actuated switches. Provoke
      each on the bench — belt removed with the motor running toward a
      switch, a switch held closed, power-up with the block on a switch,
      a deliberate one-tooth belt shift — and confirm the firmware
      response.
- [ ] **Unpowered creep test (C axis):** mark the shaft, power the
      drive off for 24 h with the capacitor at mid-travel and again at
      each end, re-power and re-home; record the drift in steps. Decides
      whether the NVRAM anchor can ever be trusted across a cold start
      without confirmation (CLAUDE.md invariant 3 requires confirmation
      until proven otherwise). See [`HARDWARE.md`](HARDWARE.md) "Memory
      reliability with direct coupling".
- [ ] **Inductor-pair tracking:** measure the inductance of each leg at
      ≥ 5 positions across travel with the pair coupled; record the
      mismatch curve. Acceptance target ≤ 2 % (revisit at M5 against
      measured leg currents). Decide and document how the two coils are
      phased so both rollers travel the same way.
- [ ] Implement `motor` task with trapezoidal accel/decel; verify motion
      profile on scope.
- [ ] *(Phase-2 fallback only — a non-integrated motor; the iHSS60 has
      no external encoder.)* Wire quadrature encoders into the Teensy's
      hardware QEI peripherals.
      Verify count direction matches motor direction; document polarity.
      See [`ARCHITECTURE.md`](ARCHITECTURE.md) §5.2 for the encoder
      strategy (incremental + homing or NVRAM anchor; absolute as
      alternative).
- [ ] Implement homing routine against the per-axis lead-screw limit
      switches (the iHSS60 has no StallGuard / sensorless homing):
      approach the low switch from one direction at low speed, back off,
      re-approach; the series-NC pair reads "at limit" at either end, so
      use the last commanded direction to tell which. Persist
      post-homing position to NVRAM on every clean move complete so the
      next boot can skip homing when the last shutdown was clean.
- [x] ~~Bring up Ethernet (QNEthernet on Teensy 4.1) and a minimal WS
      server that emits `state` + `heartbeat` and accepts `resync`.~~
      Replaced by the M1b.1 TCP line-JSON transport (PROTOCOL.md §1.0).
      WebSocket framing upgrade is deferred to M6 hardening; the JSON
      payload format is unchanged.
- [x] Verb set for M1: `move_l`, `move_c`, `home`, `set_side`,
      `set_bypass`, `resync`. All wired end-to-end against the sim HAL.
- [ ] **Topology declaration**: add `set_topology` verb (kind:
      `balanced_l`, `balanced_pi`; element map with operator-chosen
      element→axis bindings and the `pair` flag on `L`, per
      [`../CLAUDE.md`](../CLAUDE.md) "RF topology" +
      [`HW-T41-CARRIER.md`](HW-T41-CARRIER.md) §"Topology selection").
      Reject duplicate axis bindings. Persist the chosen topology to
      NVRAM; refuse every motion verb until topology is declared. M1
      only exercises the Balanced L path end-to-end; Balanced Pi lands
      as an inert switch arm (motion HAL works for any declared axis
      count, but the Balanced-L-specific `set_side` is rejected with
      `wrong_topology` on a Pi install).
- [ ] Wire vacuum relays through optoisolated MOSFET drivers + HV bias;
      each logical relay is two-pole (both line legs; paralleled coils
      on one driver, or a DPST/DPDT unit). Implement K1/K2
      mutual-exclusion + K3 override in firmware.
- [x] `safety` task: refuses `move_l`/`move_c`/`set_side`/`home` when
      `fwd_w >= tx_lockout_w` (5 W default). Master can drive the
      fake reading via the `set_fwd_w` debug verb so the lockout path
      is testable without a transmitter.
- [x] ~~Replace `internal/fakecontroller` with a real
      `internal/tunerclient` package: dial `cfg.Tuner.URL`, 1 s → 30 s
      reconnect backoff, decode inbound frames into the same
      `state.Core` interface (drop-in).~~ Done as part of M1b.1.
      `fakecontroller` retained behind `--fake-controller` flag for
      hardware-less dev.
- [x] Master GUI: "Operate" panel with L/C step nudges, side toggle,
      bypass engage, home, fake-Fwd-W injector. ANO-encoder wiring
      moves to M3 alongside the rest of the master-input work.
- [ ] Bench validation: drive each axis full-range, confirm PED arrival
      and no ALM after every move, confirm the limit switches trip at the
      commissioned step counts at both ends, and that the L-pair coils
      stay in step across the travel; latch bypass on every state change.

**Exit criteria:** with no RF on the system, the operator can drive both
axes from end to end via the GUI and via the ANO encoders, see live
position update in two browsers simultaneously, and any attempt to move
during a faked TX is refused with a visible lockout banner.

**State (2026-05-11):**
- GUI drive ✅ — Operate panel ships ±10/±100/±1000 step nudges per
  axis, Hi-Z/Lo-Z toggle, bypass engage/release, and a "Re-home both
  axes" button. Verified against the sim HAL on a real Teensy 4.1.
- Two-browser sync ✅ — `state` frames fan out through `hub`; opening a
  second tab shows the same position update within one frame.
- Faked-TX lockout ✅ — set fake Fwd W ≥ 5 in the Operate panel and
  every motion verb comes back as `ack ok:false code:rf_lockout`. The
  ack readout shows the refusal inline; the lockout *banner* is M3
  polish.
- ANO encoders ⏳ — wired in M3 alongside CAT and memory.

---

## M2 — Measurement chain (≈ 2 weeks)

**Goal:** real SWR, R, X, |Z|, ∠Z in the `telemetry` stream, calibrated
against bench instruments.

Engineering detail and "what 'live' looks like" checklist:
[`RF-DESIGN.md`](RF-DESIGN.md) §4. The list below is the milestone
task breakdown; the *spec* lives in §4.

- [ ] Build the Tandem-match coupler; verify Fwd/Rev directivity ≥ 25 dB
      across 1.8 – 54 MHz on the network analyzer.
      *(spec: RF-DESIGN.md §4.2; build notes: HARDWARE.md §5.)*
- [ ] Wire two AD8307s to Fwd/Rev coupler ports with input pad, RC LPF,
      and op-amp buffer per chip; calibrate slope/intercept per band.
      *(spec: RF-DESIGN.md §4.3; cal procedure: §4.9 step 1, HARDWARE.md §6.)*
- [ ] Wire AD8302 with V (capacitive tap) and I (small Stockton CT)
      inputs at matched electrical length; bring PFLT pin out to a
      GPIO for sign(X) disambiguation.
      *(spec: RF-DESIGN.md §4.2 + §4.4; cal procedure: §4.9 steps 2–4.)*
- [ ] Firmware `hal::adc` task: 4-channel DMA at 100 µs, 500 ms
      averaging window, IIR-smoothed values published to `telemetry`
      at ~30 Hz, raw samples exposed via `get_raw_adc` debug verb.
      *(spec: RF-DESIGN.md §4.5.)*
- [ ] Firmware decode path: V_fwd / V_rev / V_mag / V_phs → R, X, SWR,
      P_fwd via the cal tables. PFLT-based sign(X) wired as primary.
      *(spec: RF-DESIGN.md §4.6.)*
- [ ] Firmware sanity gates: boot baseline in NVRAM, saturation
      detection, path-consistency cross-check. Trip → `status warn:
      cal_missing`; algorithm falls through to the manual escape hatch
      (TUNING.md §4.1) when any gate trips.
      *(spec: RF-DESIGN.md §4.10.)*
- [ ] Detector-board power + grounding: clean +5 V LDO, star ground,
      per-chip decoupling, separate rail from motor / relay drivers.
      Inner shield can + feed-through caps + ferrites + common-mode
      chokes on V/I lines per RF-DESIGN.md §4.7 / §4.8.
- [ ] Calibration data storage: per-band slope / intercept (AD8307)
      and offsets (AD8302) in TOML on master; pushed to controller on
      connect.
- [ ] Master GUI: live SWR meter (analog-style sweep), |Z| / ∠Z polar
      dot, R / X readouts. Same widgets the LP-100A-Server "Vector"
      view uses where possible — reuse, don't reinvent.
- [ ] Document the cal procedure in `docs/HARDWARE.md` §6 (already
      cross-referenced; build the actual step-by-step bench checklist
      against measured values during commissioning).

**Exit criteria:** with a calibrated load (50 Ω, 100 Ω, 25 Ω, complex
loads via a stub-tuner test fixture), the GUI reads R, X, SWR within
3 % and ±2° phase across 1.8 – 54 MHz, **and** every row in the
RF-DESIGN.md §4.11 "chain is live" checklist is ticked.

---

## M3 — Master core + memory + CAT (≈ 2 weeks)

**Goal:** the master is feature-complete *except* for auto-tune. Recall
from memory works; manual operation via encoders is polished.

- [ ] SQLite memory schema (`docs/ARCHITECTURE.md` §5.4) with CRUD over
      `save` / `recall` / `memory` verbs.
- [ ] CAT-rig abstraction: one interface, drivers for Icom CI-V, Yaesu
      CAT, Kenwood, Elecraft K3/K4. Default to whatever the user's rig is;
      others stubbed.
- [ ] Master `cat-poller` goroutine; QRG events update the "current QRG"
      banner and offer one-touch *Recall*.
- [ ] Memory page in the web UI: table, edit/delete, export to JSON for
      backup.
- [ ] **Topology settings page:** choose Balanced L / Balanced Pi,
      assign a motor (HAL axis) to each element, save to the TOML
      `[topology]` table; the master sends `set_topology` on every
      connect and shows the declared map (and a "shared control" style
      warning if the controller reports a different persisted topology).
- [ ] Auto-recall mode toggle (off by default). When on, QRG changes
      trigger `recall` automatically, with a 2 s debounce.
- [ ] ANO encoders: rotate = nudge, push = save current as memory for
      current QRG, dpad up/down = coarse step, dpad left/right = side
      switch.
- [ ] Two-client fan-out test: GUI in two browser tabs + the encoders +
      CAT all converging on the same state without flicker.

**Exit criteria:** operator presses recall on a memory slot, the tuner
moves to it within 4 s; rotating the encoders adjusts L/C live; pushing
the encoder centre saves the slot; opening the page in a second tab shows
identical state immediately.

---

## M4 — Auto-tune (≈ 2 weeks)

**Goal:** `auto_tune` verb produces a sub-1.2:1 SWR on any sane load
across all bands, from a cold start (no memory).

Strategy and trade-offs: see [`TUNING.md`](TUNING.md). M4 implements
**Proposal D** (hybrid memory-first + analytic-seed-on-miss +
hill-climb refine).

- [ ] Analytic L-network solver in master Go code, fed by the latest
      `r`, `x` from `telemetry` while bypass is engaged at low power.
      Balanced form: the series element is `L = 2 × L_leg` and `C` sits
      across the line; the solver is otherwise the unbalanced one.
      Balanced Pi search is Phase 2.
- [ ] Per-axis calibration curves (`L_leg(steps)` for the pair,
      `C(steps)` per capacitor) derived from a one-time sweep at
      install; stored in TOML on the master, served to the controller
      on connect.
- [ ] Hill-climb fine tuner: search in `(l_steps, c_steps)` with adaptive
      step size, watchdog (max iterations), and operator cancel.
- [ ] Algorithm validation on a *reactive-load simulator* (R/L/C network
      box) at every band centre and band edges; record convergence time
      and final SWR.
- [ ] GUI: "Tune" button kicks off auto-tune with a low-power carrier
      prompt; progress bar; on success, prompt to save.
- [ ] Add `swr_recall_threshold`, `swr_done_threshold`, and `auto_save`
      to the TOML.

**Exit criteria:** with no memory entries, pressing "Tune" on any band
within 5 minutes produces a stored slot with SWR ≤ 1.2:1 measured by both
the tuner's own chain and the LP-100A as a cross-check.

---

## M5 — RF commissioning (≈ 2 weeks, weather-dependent)

**Goal:** put the tuner on the actual Doublet, build out memory, soak.

- [ ] Mount the tuner enclosure at the planned location (shack edge or
      antenna base). Run weatherproofed Ethernet + 230 V mains.
- [ ] Initial bring-up at QRP (≤ 5 W) on every band; ensure auto-tune
      converges; log measured R, X for the band's centre.
- [ ] Ladder line check: confirm common-mode current is tolerable with
      the 1:1 transceiver-side balun, and measure the RF current in each
      line leg (clamp-on RF ammeter) at several settings per band — leg
      balance is the balanced network's reason to exist, and any
      inductor-pair tracking error shows up here first.
- [ ] **Limit-mechanism commissioning:** re-check limit-switch trip points
      (steps) at both ends of every axis after the first full-power
      thermal cycle; confirm belt tension and that homing repeatability
      is within the M1b.2 target; record lead-screw lead, pulley teeth,
      switch positions and the home→max span reference in
      `docs/HARDWARE.md` §2 / §4, and set the §10 inspection interval
      for belts and switches.
- [ ] Build the memory table: walk every band in 25 / 50 / 100 kHz
      buckets per [`ARCHITECTURE.md`](ARCHITECTURE.md) §5.4, save a slot
      at each.
- [ ] Recall accuracy after a re-home on 10 m and 6 m: `home`, then
      recall each slot and record the SWR before the hill-climb runs.
      This is the real-world measure of the lead-screw anchor
      ([`HARDWARE.md`](HARDWARE.md) "Memory reliability with direct
      coupling") and feeds open decision 11.
- [ ] Power ramp: QRP → 100 W → legal limit, monitor heating and SWR
      stability. Document max continuous power per band.
- [ ] 48 h on-air soak: leave the master + tuner running, exercise from
      multiple bands and stations, verify no drift, no spurious lockouts.
- [ ] **Closed-loop driver feedback — final-build verification.** The
      iHSS60 integrated closed-loop drive was adopted 2026-09-05
      (CLAUDE.md hardware contract). Verify on the built tuner before
      the soak:
      ALM/PED wired to the carrier's spare opto inputs per
      [`HW-T41-PINMAP.md`](HW-T41-PINMAP.md) §2.2 with ≈ 10 mA LED
      current and a clean LOW at the Teensy; P10 = 1 fail-safe
      polarity (cable-open reads as fault); P16 following-error limit
      set for the geared load; firmware stops pulses and drops
      `homed` on ALM, and waits for PED before persisting position.
      Record the per-axis drive parameters (P8/P9/P10/P14/P16, DIP
      setting) in `docs/HARDWARE.md` §4a.
- [ ] **MCU + carrier-board Phase 1 / Phase 2 go/no-go.** During the
      power ramp and the 48 h soak, evaluate **both** the Teensy 4.1
      MCU choice and the grblHAL-teensy-4.x V2.09 off-the-shelf
      carrier. The two decisions are coupled: a "stay on Teensy 4.1"
      result can still require a custom carrier if the V2.09 PCB's
      ground plane, opto isolation, or relay-driver trace routing
      can't be tamed by enclosure-level fixes. Log every event that
      looks like RF immunity trouble:
      ADC noise spikes during TX, MCU resets, Ethernet link drops on
      key-up, encoder count glitches, spurious safety lockouts. Score
      each by reproducibility and by whether enclosure-level fixes
      (additional ferrites, better bonding, VDD_SOC LDO, shield-can
      addition) resolved it. **Go/no-go criteria for staying on Teensy
      4.1:**
      - Zero unexplained MCU resets in 48 h on-air.
      - ADC noise floor with TX keyed at full power within 2× of the
        TX-unkeyed noise floor across all bands.
      - No spurious safety lockouts attributable to RF (vs. real RF
        leakage into the control loop).
      - Ethernet link stable through full-power TX on every band.
      If any of these fail and can't be fixed by enclosure work alone,
      open the Phase 2 migration to STM32H743 (see
      [`../CLAUDE.md`](../CLAUDE.md) "MCU selection"). The firmware
      portability rule means the migration is a HAL swap, not a rewrite.

**Exit criteria:** every authorised band has at least one memory slot
yielding SWR ≤ 1.2:1; full memory table built; no firmware-level lockout
events in the 48 h soak; MCU Phase 1/2 decision recorded with evidence.

---

## M6 — Hardening + release (≈ 1 week)

**Goal:** treat this like LP-100A-Server: install scripts, runtime knobs,
release pipeline.

- [ ] Auto-reconnect on the master ↔ controller WS link, 1 s → 30 s
      backoff (same shape as LP-100A-Server's serial reconnect).
- [ ] `/healthz`, `/api/log-level`, `/api/config` on the master.
- [ ] `deploy/install.sh`, `deploy/redeploy.sh`, systemd unit, udev rules
      for the ANO encoders (stable device names).
- [ ] GitHub Actions release workflow — cross-compile linux/{amd64,arm64,
      armv7}, darwin/{amd64,arm64}, windows/amd64; firmware `.hex`
      attached; `SHA256SUMS`.
- [ ] README: build / install / operate, mirroring LP-100A-Server's
      README structure.
- [ ] CLAUDE.md updated with anything we learned during commissioning
      (especially anything that surprised us — protocol gotchas,
      calibration tricks, hardware revisions).

**Exit criteria:** `./deploy/build-pi.sh && ./deploy/install.sh` on a
fresh Pi brings the master up under systemd in one shot; the firmware
flashes from a release artifact; a tagged release on GitHub publishes
binaries automatically.

---

## Phase 2 — Multi-antenna, multi-transceiver, SO2R (M7+)

The Phase 1 plan (M0 – M6) targets a single Doublet + single rig.
Phase 2 extends to multiple antennas, multiple transceivers, and
optional SO2R contesting mode. **Scope, architecture options, and
trade-offs are in [`EXTENSIONS.md`](EXTENSIONS.md)** — read that
first; the milestones below assume the evolution path in
EXTENSIONS.md §9.

Phase 2 is **documented but not scheduled.** Phase 2 milestones are
gated on M5 (Phase 1 RF commissioning) being clean; commissioning
findings may reshape the M7 scope.

Two further Phase-2 items fall out of the 2026-09-05 topology change
and are independent of the multi-antenna work: **Balanced Pi
auto-tune** (the three-element search algorithm) and an optional
**switched fixed-capacitor bank** in parallel with the transceiver-side
capacitor for range extension on 160 m.

### M7 — Multi-antenna (single radio)

**Goal:** operator switches between Doublet, HexBeam, and any other
configured antenna in the UI; memory is keyed by `(antenna_id, band,
bucket)`; routing rules drive automatic selection on QRG change.

- [ ] Antenna abstraction (EXTENSIONS.md §2): TOML config, type
      catalogue, per-type tuning behaviour.
- [ ] Routing rules (EXTENSIONS.md §4): `(radio, band)` → antenna
      table, operator override.
- [ ] Memory schema migration (EXTENSIONS.md §6): add `antenna_id`
      column to `slot`; default existing rows to the Phase 1 antenna.
- [ ] Protocol v2: `set_antenna`, `set_routing`, `query_routing`,
      `query_antennas` verbs (EXTENSIONS.md §7).
- [ ] Hardware: M × N antenna-selector relay matrix (M = 1 radio in
      M7, scales in M8).
- [ ] Web UI: antenna picker; per-antenna live state; bypass path
      logic for `needs_tuner = false` antennas.

**Exit criteria:** with at least two antennas configured (e.g.
Doublet + HexBeam), the operator can swap between them in the UI;
each antenna has its own memory slots; CAT QRG changes auto-route per
the configured rules.

### M8 — Multi-transceiver (sequential)

**Goal:** two or more transceivers share the antenna farm, each with
its own CAT link and routing rules. SO2R remains *off* by default —
only one radio TX at a time.

- [ ] Second `[[radio]]` config block + second CAT-poller goroutine
      tagged with `radio_id`.
- [ ] Per-radio UI context (top-level radio selector; two-browser
      workflow tested).
- [ ] Interlock invariant #7 (EXTENSIONS.md §5.3): refuse routing of
      a second radio to an in-use antenna.
- [ ] Radio input matrix hardware.
- [ ] `query_routing` returns resolved per-radio routes including
      lockout flags.

**Exit criteria:** with K3 + IC-7300 (or equivalent), each radio
holds its own routing rules and memory; switching the active radio in
the UI updates the routing display; the in-use antenna is correctly
locked out from the second radio.

### M9 — SO2R mode

**Goal:** both radios may TX simultaneously on different antennas
with band-pass filter isolation; the SO2R-capable contest workflow.

- [ ] BPF declaration in TOML per radio path per band.
- [ ] Interlock invariants #8 and #9 (EXTENSIONS.md §5.3): SO2R mode
      gated on BPF coverage; per-tuner motion lockout when its
      antenna is in use by a radio.
- [ ] `set_so2r_mode` verb; mode toggle in UI.
- [ ] Optional: migrate any always-needs-tuner antenna that wants
      simultaneous-on-two-radios coverage to a tuner-bank
      architecture (EXTENSIONS.md §8 Option B applied per-antenna).
- [ ] Hardware-level antenna interlock (relay matrix layout makes
      "two radios on one antenna" physically impossible, not just
      firmware-refused).

**Exit criteria:** with `mode = "so2r"` in config and BPFs in place,
both radios can key simultaneously on different antennas without
inter-radio desense, and the master refuses any routing that would
violate the SO2R interlock.

---

## Open decisions to make before / during M0

These are not blockers but should be decided early. None of them change
the architecture, only the BoM:

1. **Tuner-side MCU.** Phase 1 is **Teensy 4.1**; Phase 2 fallback is
   **STM32H743** on a custom board. See [`../CLAUDE.md`](../CLAUDE.md)
   "MCU selection" for the portability rules. Decision point is M5.
1a. **Tuner-side carrier board** — **decided: grblHAL-teensy-4.x V2.09**
   (Phil Barrett, T41E5XBB SKU for Ethernet). Off-the-shelf board with
   5 stepper channels, 10 opto-isolated digital inputs, 7 relay
   drivers, and the PJRC Ethernet Kit footprint. Avoids a custom-PCB
   spin for Phase 1; revisited as part of the Phase-1/Phase-2 go/no-go
   in M5. Plan + reference URLs: [`HW-T41-CARRIER.md`](HW-T41-CARRIER.md).
1b. **Tuner topology** — *decided 2026-09-05:* **Balanced L-Network**
   is the Phase 1 default and the only topology with auto-tune in
   scope through M6. **Balanced Pi-Network** is first-class supported
   through the HAL / protocol / "drive each element to a commanded
   position" path; its auto-tune is a Phase-2 deliverable. The
   unbalanced L / T / Pi set is superseded and T-Match dropped. See
   [`../CLAUDE.md`](../CLAUDE.md) §"RF topology" for the topology
   selection mechanism and the element→motor map.
2. **Ethernet library.** **QNEthernet** (lwIP) is the default; the
   build also supports **NativeEthernet** (FNET) via a separate PIO env
   for A/B testing and to match the Morconi / TeensyMaestro convention.
   The abstraction lives in `src/net_hal.{h,*.cpp}`; the choice is a
   one-line change in `platformio.ini`. See
   [`ARCHITECTURE.md`](ARCHITECTURE.md) §5.1.2.
3. **Balun ratio.** *Resolved 2026-09-05 by the topology change:* 1:1
   Guanella current balun on the transceiver side, working at 50 Ω. M5
   verifies leg balance and common-mode current instead of choosing a
   ratio.
4. **Power rating target.** Default to **400 W continuous, 1.5 kW peak**
   in BoM sizing (matches VU3ESV regional legal limit + headroom). If the
   user wants legal-limit US (1.5 kW continuous), upsize the cap voltage
   rating (Jennings UCSL-1500 → CMV1-1500, 7.5 kV) and the relay rating.
5. **Step resolution and limit mechanism.** *Resolved 2026-09-05:*
   iHSS60 DIP 6400 p/r, direct drive = 6400 steps per element turn; the
   3:1 GT2 belt drives only the limit-switch lead screw. Remaining:
   lead-screw lead per axis (block travel = turns / 3 × lead must fit
   the enclosure, and home repeatability in steps improves with a
   larger lead), pulley tooth counts, switch type, and each element's
   max shaft speed — decide in M1b.2. See [`HARDWARE.md`](HARDWARE.md)
   "Drive train".
6. **Encoder CPR and type.** *Superseded 2026-09-05:* the iHSS60's
   internal encoder closes the loop and the controller sees ALM / PED
   only. The rest of this item applies only to a Phase-2 non-integrated
   motor. Original default: **2000 CPR incremental quadrature**
   (resolves ~0.1 % of end-to-end travel per count). Higher CPR is fine. **Absolute encoders** (SSI / BiSS) are supported
   as a per-axis alternative via the HAL; the L axis is the natural
   candidate if its mechanical home is too slow to reach on every
   power-up. See [`ARCHITECTURE.md`](ARCHITECTURE.md) §5.2 for the
   trade-off and the homing / NVRAM anchor strategy that lets
   incremental encoders satisfy the position-truth invariant.
7. **CAT rig and protocol.** What's the user's primary rig? Drives which
   CAT driver is built first in M3.
8. **Optional LP-100A integration.** The architecture already supports
   subscribing to LP-100A-Server as a second-opinion source. Build it if
   the user wants a cross-check display; skip otherwise.
9. **Vacuum-relay pole arrangement.** Two SPST/SPDT relays with
   paralleled coils per logical switch vs one DPST/DPDT unit. Affects
   BoM count (6 contacts for Balanced L, 2 for Balanced Pi), HV-bias
   supply loading, and relay-driver current. Decide at M1b.2 relay
   wiring.
10. **Fixed-capacitor bank.** Whether the 10–1500 pF vacuum capacitor
   covers 160 m without switched fixed caps in parallel; decide from
   M5 measured impedances. Phase 2 if needed.
11. **Absolute position sensing for the anchor.** Options: a linear
   potentiometer or magnetic linear sensor along the lead-screw block
   (coarse absolute position at power-up, no motion needed), and a
   multi-turn absolute encoder on a free roller-inductor shaft end.
   Decide after the M1b.2 repeatability and creep measurements and the
   M5 re-homed recall results on 10 m / 6 m. Phase 2 unless M5 shows
   re-homed recall is not good enough on the high bands.

These should be answered in conversation before scaffolding decisions
that depend on them — most realistically, during M0.
