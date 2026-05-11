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
| M0 | Scaffolding                 | Repo skeleton, CI, two binaries that build and exchange a heartbeat.                                   | **Master side ✅ · firmware side pending HW**    |
| M1 | Motion + position           | Steppers move under encoder closed-loop; bypass + relay state machine; faked RF.                       | **Master software ✅ · firmware pending HW**     |
| M2 | Measurement                 | AD8302 + dual AD8307 chains live; SWR, R, X reported in `telemetry`.                                   | Pending hardware                                  |
| M3 | Master core + GUI           | Go master with embedded web UI, CAT polling, ANO encoders, memory store.                               | Partial — UI + WS hub done; CAT / encoders / memory pending |
| M4 | Auto-tune algorithm         | Recall + analytic L-network solve + hill-climb fine-tune, validated on a dummy load network.           | Pending M2/M3                                     |
| M5 | RF commissioning            | Real Doublet on-air, per-band calibration of memory, soak test.                                        | Pending hardware                                  |
| M6 | Hardening                   | Reconnect, lockouts, log-level API, systemd unit, cross-compile to Pi, udev rules, release pipeline.   | Cross-compile verified; rest pending              |

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
- [ ] Cross-compile to Pi: `deploy/build-pi.sh`. Cross-compile *command*
      verified manually (`GOOS=linux GOARCH=arm64 go build`); the
      packaged script remains to be written.

**Exit criteria:** open `http://<pi-ip>:8088/` in a browser, see the
embedded UI showing a green "controller connected" pill when the Teensy is
on the LAN, red when it isn't. **Currently met against the
`fakecontroller` source; gated on real firmware Ethernet for the live
Teensy case.**

---

## M1 — Motion + position (≈ 2 weeks)

**Goal:** real steppers move real shafts under closed-loop encoder
feedback. No RF involved; safety lockouts gated on a fake `fwd_w` value
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

- [ ] Add `hal::encoder`, `hal::motor`, `hal::relay`, `hal::safety`
      interfaces to the firmware HAL. Implementations for `TARGET_TEENSY41`
      gate stepper / QEI / GPIO calls; `TARGET_NATIVE` provides stubs
      so the host-side test suite keeps building.
- [ ] Wire two TMC2209 drivers to the Teensy (UART config). Pick microstep
      resolution (default 1/16) and current limit per motor spec.
- [ ] Implement `motor` task with trapezoidal accel/decel; verify motion
      profile on scope.
- [ ] Wire quadrature encoders into the Teensy's hardware QEI peripherals.
      Verify count direction matches motor direction; document polarity.
      See [`ARCHITECTURE.md`](ARCHITECTURE.md) §5.2 for the encoder
      strategy (incremental + homing or NVRAM anchor; absolute as
      alternative).
- [ ] Implement homing routine using StallGuard (TMC2209). Persist
      post-homing position to NVRAM on every clean move complete so the
      next boot can skip homing when the last shutdown was clean.
- [x] ~~Bring up Ethernet (QNEthernet on Teensy 4.1) and a minimal WS
      server that emits `state` + `heartbeat` and accepts `resync`.~~
      Replaced by the M1b.1 TCP line-JSON transport (PROTOCOL.md §1.0).
      WebSocket framing upgrade is deferred to M6 hardening; the JSON
      payload format is unchanged.
- [ ] Implement `set_steps` and `get_state` verbs end-to-end. Verb set for
      M1: `move_l`, `move_c`, `home`, `set_side`, `set_bypass`, `resync`.
- [ ] Wire vacuum relays through optoisolated MOSFET drivers + HV bias;
      implement K1/K2 mutual-exclusion + K3 override in firmware.
- [ ] `safety` task: refuse `move_l`/`move_c`/`set_side` if fake
      `fwd_w > tx_lockout_w` (master can set the fake via a debug verb so
      the lockout path is testable without a transmitter).
- [x] ~~Replace `internal/fakecontroller` with a real
      `internal/tunerclient` package: dial `cfg.Tuner.URL`, 1 s → 30 s
      reconnect backoff, decode inbound frames into the same
      `state.Core` interface (drop-in).~~ Done as part of M1b.1.
      `fakecontroller` retained behind `--fake-controller` flag for
      hardware-less dev.
- [ ] Master GUI: add an "Operate" page stub with two big number panels
      (L_steps / C_steps), bypass toggle, side toggle, and step nudge
      buttons. Wire the two ANO encoders to nudge L and C.
- [ ] Bench validation: drive each axis full-range, confirm encoder counts
      match expected step counts within ±1 LSB after each move; latch
      bypass on every state change.

**Exit criteria:** with no RF on the system, the operator can drive both
axes from end to end via the GUI and via the ANO encoders, see live
position update in two browsers simultaneously, and any attempt to move
during a faked TX is refused with a visible lockout banner.

---

## M2 — Measurement chain (≈ 2 weeks)

**Goal:** real SWR, R, X, |Z|, ∠Z in the `telemetry` stream, calibrated
against bench instruments.

- [ ] Build the Tandem-match coupler; verify Fwd/Rev directivity ≥ 25 dB
      across 1.8 – 54 MHz on the network analyzer.
- [ ] Wire two AD8307s to Fwd/Rev coupler ports; calibrate slope/intercept
      against a signal generator and known attenuator at 1, 10, 30 MHz
      (multi-point cal table in NVRAM).
- [ ] Wire AD8302 with V (capacitive tap on tuner input port) and I (small
      Stockton CT) inputs at matched electrical length; calibrate `|Z|`
      and `∠Z` against known resistive and reactive loads
      (50 Ω, 100 Ω, 25 Ω, 50 − j50, 50 + j50).
- [ ] Firmware `adc` task: 4-channel DMA at 100 µs, IIR-smoothed values
      published to `telemetry` at ~30 Hz, raw values exposed via debug
      verb for auto-tune use.
- [ ] Master GUI: live SWR meter (analog-style sweep), |Z| / ∠Z polar dot,
      R / X readouts. Same widgets the LP-100A-Server "Vector" view uses
      where possible — reuse, don't reinvent.
- [ ] Document the cal procedure in `docs/HARDWARE.md` so a future rebuild
      of the coupler/detector chain is reproducible.

**Exit criteria:** with a calibrated load (50 Ω, 100 Ω, 25 Ω, complex
loads via a stub-tuner test fixture), the GUI reads R, X, SWR within 3 %
and ±2° phase across 1.8 – 54 MHz.

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

- [ ] Analytic L-network solver in master Go code, fed by the latest
      `r`, `x` from `telemetry` while bypass is engaged at low power.
- [ ] Per-axis calibration curves (`L(steps)`, `C(steps)`) derived from a
      one-time sweep at install; stored in TOML on the master, served
      to the controller on connect.
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
- [ ] Ladder line check: confirm common-mode currents are tolerable with
      the chosen balun ratio (1:1 vs 4:1 decision happens *here*, based
      on measured impedance ranges, not earlier).
- [ ] Build the memory table: walk every band in 25 / 50 / 100 kHz
      buckets per [`ARCHITECTURE.md`](ARCHITECTURE.md) §5.4, save a slot
      at each.
- [ ] Power ramp: QRP → 100 W → legal limit, monitor heating and SWR
      stability. Document max continuous power per band.
- [ ] 48 h on-air soak: leave the master + tuner running, exercise from
      multiple bands and stations, verify no drift, no spurious lockouts.
- [ ] **MCU Phase 1 / Phase 2 go/no-go.** During the power ramp and the
      48 h soak, log every event that looks like RF immunity trouble:
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

## Open decisions to make before / during M0

These are not blockers but should be decided early. None of them change
the architecture, only the BoM:

1. **Tuner-side MCU.** Phase 1 is **Teensy 4.1**; Phase 2 fallback is
   **STM32H743** on a custom board. See [`../CLAUDE.md`](../CLAUDE.md)
   "MCU selection" for the portability rules. Decision point is M5.
2. **Ethernet library.** **QNEthernet** (lwIP) is the default; the
   build also supports **NativeEthernet** (FNET) via a separate PIO env
   for A/B testing and to match the Morconi / TeensyMaestro convention.
   The abstraction lives in `src/net_hal.{h,*.cpp}`; the choice is a
   one-line change in `platformio.ini`. See
   [`ARCHITECTURE.md`](ARCHITECTURE.md) §5.1.2.
3. **Balun ratio (1:1 vs 4:1).** Resolved during M5 when actual ladder-line
   impedances are measured. Carry both on the BoM until then.
4. **Power rating target.** Default to **400 W continuous, 1.5 kW peak**
   in BoM sizing (matches VU3ESV regional legal limit + headroom). If the
   user wants legal-limit US (1.5 kW continuous), upsize the cap voltage
   rating (Jennings UCSL-1500 → CMV1-1500, 7.5 kV) and the relay rating.
5. **Stepper-driver microstep resolution.** Default 1/16; lower means
   smoother but slower. Confirm during M1 against actual roller-inductor
   mechanics.
6. **Encoder CPR and type.** Default: **2000 CPR incremental
   quadrature** (resolves ~0.1 % of end-to-end travel per count).
   Higher CPR is fine. **Absolute encoders** (SSI / BiSS) are supported
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

These should be answered in conversation before scaffolding decisions
that depend on them — most realistically, during M0.
