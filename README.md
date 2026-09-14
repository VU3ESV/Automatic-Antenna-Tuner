# Automatic Antenna Tuner

An automatic **balanced** antenna tuner for a **Doublet on 460/600 Ω
open-wire ladder line, 160 m – 6 m** — a **Balanced L-Network** by
default or a **Balanced Pi-Network**, fed through a fixed 1:1 current
balun on the transceiver side. Two controllers:

- A **tuner controller** at the tuner enclosure — a Teensy 4.1 on the
  grblHAL-teensy-4.x V2.09 carrier driving two (Balanced L) or three
  (Balanced Pi) JMC iHSS60 closed-loop steppers coupled directly to the
  elements (a synchronized roller-inductor pair on one motor, one or two
  vacuum-variable capacitors), each axis homed by limit switches on a
  belt-driven lead screw; switching two-pole vacuum relays (Hi-Z / Lo-Z
  selector on Balanced L, bypass on both); and sampling an AD8302 + dual
  AD8307 detector chain for SWR, R, X.
- A **master controller** in the shack — a Raspberry Pi running a Go
  service with an embedded touchscreen web UI, one Adafruit ANO
  directional encoder per element axis for manual nudges, a CAT link to
  the transceiver for QRG, and a SQLite memory of element positions per
  (topology, band, frequency).

The two controllers talk over Ethernet with a JSON protocol following
the [LP-100A-Server](https://github.com/VU3ESV/LP-100A-Server) pattern:
one process owns the hardware, many clients subscribe, named verbs only.
Browsers reach the master over WebSocket; the master reaches the
controller over line-framed TCP carrying the same JSON payloads.

## Documents

- **[CLAUDE.md](CLAUDE.md)** — project contract: invariants, protocol
  summary, hardware contract. Read this first.
- **[PROPOSAL.md](PROPOSAL.md)** — problem, goals, non-goals, design
  rationale. Matches LP-100A-Server's PROPOSAL.md shape.
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — system view, RF
  topology, measurement chain, tuning algorithm, software architecture,
  failure modes.
- **[docs/PLAN.md](docs/PLAN.md)** — milestone-based implementation
  plan (M0 scaffolding → M6 release).
- **[docs/HARDWARE.md](docs/HARDWARE.md)** — BoM, schematics, wiring,
  calibration procedures. Short-form BoM today; long-form fills in
  M1b.2 → M5.
- **[docs/HW-T41-CARRIER.md](docs/HW-T41-CARRIER.md)** — why the
  off-the-shelf grblHAL-teensy-4.x V2.09 board is the Phase 1 carrier,
  and how its stepper channels, inputs and relay drivers map to each
  topology.
- **[docs/HW-T41-PINMAP.md](docs/HW-T41-PINMAP.md)** — Teensy 4.1 pin →
  carrier net reference, including the PED / ALM inputs and the
  per-topology allocation.
- **[docs/DRIVE-FEEDBACK.md](docs/DRIVE-FEEDBACK.md)** — iHSS60 PED / ALM
  drive feedback: why it exists, wiring to the V2.09 carrier,
  commissioning, fault recovery, troubleshooting and limits. Off by
  default until wired.
- **[docs/PROTOCOL.md](docs/PROTOCOL.md)** — full WebSocket JSON protocol
  spec, frame by frame.
- **[docs/TUNING.md](docs/TUNING.md)** — tuning-algorithm strategy:
  universal safety-gated tune protocol, per-band starting conditions,
  four candidate algorithms (memory-first / pure analytic / coarse-grid
  / hybrid) with pros + cons, and the chosen project decision (Proposal
  D — hybrid).
- **[docs/RF-DESIGN.md](docs/RF-DESIGN.md)** — balanced-network theory,
  component sizing, detector math. §3 closed-form L-network solution
  (with `L = 2 × L_leg` for the inductor pair) is the math reference for
  the auto-tune algorithm; §4 (detector chain) and §5 (balun) fill in
  during M2 / M5.
- **[docs/EXTENSIONS.md](docs/EXTENSIONS.md)** — Phase 2 scope: multi-
  antenna (Doublet + HexBeam + …), multi-transceiver, and SO2R
  workflow modelled on the 4O3A TGXL / Antenna Genius family.
  Architecture options + trade-offs + evolution path M7 – M9. *(design
  proposal, gated on Phase 1 commissioning.)*
- **[docs/DEV-SETUP.md](docs/DEV-SETUP.md)** — VS Code dev environment
  for both firmware (PlatformIO/Teensy) and master (Go/Pi).
- **[firmware/test/](firmware/test/)** — standalone bench and bring-up
  projects (Teensy + Ethernet selftest, stepper rigs). Run the selftest
  first on a new Teensy + Ethernet kit to verify the hardware platform
  before flashing the production firmware in `firmware/tuner-controller/`.

## Status

**M1b.2 — production firmware on the real carrier (since 2026-09-06).**
The tuner controller runs on a Teensy 4.1 in the V2.09 carrier on the
bench, with real drivers behind the HAL:

- **Motion:** FlexPWM hardware step generation for up to three iHSS60
  axes, trapezoidal ramps, per-axis travel windows, and clean-shutdown
  position anchors in EEPROM (invariant 3). `set_topology` declares
  Balanced L or Balanced Pi with the element→motor map; `move_axis`,
  `run`, `stop` and a latched `estop` operate it.
- **Relays and inputs:** K1 / K2 / K3 on carrier relay outputs, with
  bypass as the de-energised state latched first at boot; limit-switch
  inputs reported; **iHSS60 PED / ALM supervision** in firmware since
  2026-09-13, off until the lines are wired
  ([docs/DRIVE-FEEDBACK.md](docs/DRIVE-FEEDBACK.md)).
- **Control surfaces:** the master link (line-JSON over TCP, port 8089)
  and a browser page on the controller's own HTTP port 80 calling the
  same verbs — the operating UI until the Pi master is deployed.
- **Firmware update over Ethernet** (FlasherX,
  `pio run -e teensy41_native_ota -t upload`); USB remains the
  first-install and recovery path.
- **Master:** Go service on the Pi under systemd, embedded web UI on
  `:8088`, WebSocket fan-out to every browser, auto-reconnect to the
  controller, Operate panel.

Still simulated or pending: forward power is injected with the debug
`set_fwd_w` verb until the AD8307 chain lands (M2), so the RF-lockout
path is exercised but not yet measured; the lead-screw limit mechanism
and its direction-latched trip handling; vacuum-relay and PED / ALM
wiring; CAT, ANO encoders and SQLite memory (M3); auto-tune (M4); RF
commissioning (M5). See [docs/PLAN.md](docs/PLAN.md) for the
milestone-by-milestone state and
[docs/ARCHITECTURE.md §5.1.3](docs/ARCHITECTURE.md) for the HAL
backends.

## Deploy the master to a Raspberry Pi

Targets Pi 4 / 5 running 64-bit Raspberry Pi OS (Debian 12+ derivative).
Pure-Go module — no cross-compile toolchain needed; any Go install on
the dev machine can target `linux/arm64`. Layout follows the same
LP-100A-Server shape so station services stay uniform.

### One script handles both first install and updates

`redeploy.sh` detects whether `tuner-master.service` is already
installed on the Pi and chooses the right path automatically:

| Situation                       | What `redeploy.sh` does                                                                                  |
|---------------------------------|----------------------------------------------------------------------------------------------------------|
| Service **not** installed       | Cross-compiles, stages `deploy/` + the binary on the Pi, runs `install.sh` end-to-end (creates user, FHS dirs, registers + starts systemd unit). |
| Service **installed**           | Cross-compiles, scp's the new binary, atomically swaps via `install -m 755`, restarts the service.       |

In both cases it finishes with a `/healthz` check. Target the Pi
either as a positional argument or via env vars:

```sh
# Quickest: pass the SSH spec inline.
./deploy/redeploy.sh pi@tuner-pi.local
./deploy/redeploy.sh vinod@192.168.1.42:2222     # custom user + non-standard port

# Or set the env vars once (e.g. in your shell rc) and just run the script:
export PI_HOST=tuner-pi.local
export PI_USER=pi
./deploy/redeploy.sh
```

Defaults: `pi@tuner-pi.local:22`. The positional arg overrides the env
vars; the env vars override the defaults. The Pi user needs
passwordless sudo (the usual Pi default).

### Config is preserved by default

`redeploy.sh` **never overwrites** the Pi-side `[tuner]`, `[cat]`, or
any other config section across redeploys. Once you've edited
`/etc/tuner-master/config.toml` on the Pi (to set the real
controller IP, CAT device, etc.), those edits survive every
subsequent `redeploy.sh` run. The script logs `Config: preserved on
the Pi` at start so the intent is obvious.

If you *do* need to push a new config from the dev machine — e.g.,
after adding a new top-level section — use the opt-in flag:

```sh
./deploy/redeploy.sh --push-config ./my-tuner.toml pi@tuner-pi.local
```

That scp's the file, **backs up the existing config to
`/etc/tuner-master/config.toml.bak`** on the Pi, installs the new
one as `/etc/tuner-master/config.toml`, and restarts the service.
Pass any path — typically you'd keep a non-committed
`master/tuner-master/deploy/config.tuner-pi.toml` (or similar) with
your live station values.

> First-time use note: after the install-mode path completes, edit
> `/etc/tuner-master/config.toml` on the Pi to point at your tuner
> controller's IP, then `sudo systemctl restart tuner-master.service`.
> Subsequent `redeploy.sh` runs preserve your config and take the
> fast-update path.

### What gets installed on the Pi

| Path                                          | Purpose                                  |
|-----------------------------------------------|------------------------------------------|
| `/opt/tuner-master/tuner-master`              | the binary                               |
| `/etc/tuner-master/config.toml`               | config (never overwritten on re-install) |
| `/var/lib/tuner-master/`                      | SQLite memory DB + runtime state         |
| `/etc/systemd/system/tuner-master.service`    | systemd unit                             |
| user/group `tuner`                            | system account, no shell                 |

Verify on the Pi:

```sh
systemctl status tuner-master.service
journalctl -u tuner-master.service -f
curl http://<pi-host>:8088/healthz
```

### Manual install (without redeploy.sh)

If you'd rather drive the install yourself — for example, when the dev
machine can't reach the Pi directly and you need to sneakernet a
binary on a USB stick:

```sh
# On the dev machine:
cd master/tuner-master
./deploy/build-pi.sh
scp -r dist deploy pi@<pi-host>:/tmp/tuner-master-deploy

# On the Pi:
cd /tmp/tuner-master-deploy/deploy
sudo ./install.sh
```

`install.sh` is idempotent — re-run it for upgrades.

### Build for 32-bit Raspberry Pi OS / Pi Zero

```sh
ARCH=arm ./deploy/build-pi.sh    # → dist/tuner-master-linux-armv7
```

Same install.sh + systemd unit; just pass the armv7 binary to
`install.sh`.

## Stack at a glance

| Layer              | Choice                                                            |
|--------------------|-------------------------------------------------------------------|
| Network topology   | **Balanced L-Network** (default, 2 axes) or **Balanced Pi-Network** (3 axes), declared at install with `set_topology` |
| Balun              | 1:1 Guanella current balun, fixed, on the transceiver side (Fair-Rite 43 / 31) |
| Tuner-side MCU     | Teensy 4.1 (Phase 1); STM32H743 on a custom board only if M5 RF testing demands it |
| Tuner-side carrier | grblHAL-teensy-4.x V2.09 (Phil Barrett; T41E5XBB with the PJRC Ethernet kit) |
| Firmware           | C/C++, PlatformIO + Teensyduino; QNEthernet or NativeEthernet; ArduinoJson |
| Element actuators  | JMC iHSS60 integrated closed-loop stepper (NEMA 24) ×2 / ×3, coupled directly to the element, 6400 steps per element turn |
| Inductor           | Two matched roller inductors, one per line leg, on one motor (1:1 GT2 belt) — one axis, `L = 2 × L_leg` |
| Capacitor          | Vacuum variable, e.g. Jennings UCSL-1500 (10–1500 pF, 5 kV) — ×1 (Balanced L) / ×2 (Balanced Pi) |
| Step pulses        | FlexPWM hardware generation, pulses counted in the reload ISR (`firmware/lib/flexpwm_stepper`) |
| Position truth     | Pulse counter anchored by homing or a clean-shutdown record; iHSS60 ALM / PED opto outputs as drive feedback (no external encoder) |
| Limit switches     | Home + max per axis on a 3:1 belt-driven lead screw, NC in series into one carrier opto input |
| Vacuum relays      | Gigavac G2/G81 or Kilovac H, two-pole per switch: K1 Hi-Z, K2 Lo-Z, K3 bypass (Balanced L); K3 only (Balanced Pi) |
| RF detection       | AD8302 + AD8307 ×2 behind a Stockton / Tandem-match coupler       |
| Controller link    | Line-JSON over TCP, port 8089 (master); HTTP bring-up page and firmware update, port 80 |
| Master MCU         | Raspberry Pi 4/5 with 7" / 10" capacitive touchscreen             |
| Master service     | Go, single static binary, embedded web UI via `go:embed` on port 8088 |
| Master deps        | `gorilla/websocket`, `BurntSushi/toml`, `modernc.org/sqlite`      |
| Operator input     | Adafruit ANO directional encoder (p/n 5735), one per element axis: 2 / 3 |
| Transceiver link   | USB serial CAT (CI-V / Yaesu / Kenwood / K3/K4)                   |

## Why this shape (briefly)

It mirrors the station's existing automation pattern from
LP-100A-Server: one Go binary on the Pi, embedded UI, WebSocket fan-out,
TOML config, systemd deployment. New device, same shape — uniform to
operate, uniform to debug.

The network is **balanced** because the Doublet is fed with ladder line.
With the 1:1 current balun on the transceiver side, the balun always
works at its 50 Ω design impedance and the ladder line is fed straight
from the network. **Balanced L** is the default: it has the fewest
elements, and vacuum relays put the capacitor on either side of the
inductor pair. **Balanced Pi** is supported for installs that want to
cover both impedance ranges without a selector, at the cost of a third
axis. See [CLAUDE.md](CLAUDE.md) §"RF topology" for the contract.
