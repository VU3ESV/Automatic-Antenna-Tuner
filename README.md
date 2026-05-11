# Automatic Antenna Tuner

An automatic L-network antenna tuner for a **Doublet on 460/600 Ω
open-wire ladder line, 160 m – 6 m**. Two controllers:

- A **tuner controller** at the tuner enclosure — Teensy 4.1 driving two
  stepper motors (roller inductor + vacuum-variable cap), reading two
  quadrature encoders, switching three vacuum relays (Hi-Z / Lo-Z / bypass),
  and sampling an AD8302 + dual AD8307 detector chain for SWR, R, X.
- A **master controller** in the shack — a Raspberry Pi running a Go
  service with an embedded touchscreen web UI, two Adafruit ANO directional
  encoders for manual L/C nudges, a CAT link to the transceiver for QRG, and
  a SQLite memory of optimal (L, C, side) per band/frequency.

The two controllers talk over Ethernet with a WebSocket JSON protocol,
following the [LP-100A-Server](https://github.com/VU3ESV/LP-100A-Server)
pattern: one process owns the hardware, many clients subscribe, named
verbs only.

## Documents

- **[CLAUDE.md](CLAUDE.md)** — project contract: invariants, protocol
  summary, hardware contract. Read this first.
- **[PROPOSAL.md](PROPOSAL.md)** — problem, goals, design rationale.
  *(scaffold next; matches LP-100A-Server's PROPOSAL.md shape)*
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — system view, RF
  topology, measurement chain, tuning algorithm, software architecture,
  failure modes.
- **[docs/PLAN.md](docs/PLAN.md)** — milestone-based implementation
  plan (M0 scaffolding → M6 release).
- **[docs/HARDWARE.md](docs/HARDWARE.md)** — BoM, schematics, wiring,
  calibration procedures. *(scaffold during M0)*
- **[docs/PROTOCOL.md](docs/PROTOCOL.md)** — full WebSocket JSON protocol
  spec, frame by frame. *(scaffold during M0)*
- **[docs/TUNING.md](docs/TUNING.md)** — tuning-algorithm strategy:
  universal safety-gated tune protocol, per-band starting conditions,
  four candidate algorithms (memory-first / pure analytic / coarse-grid
  / hybrid) with pros + cons, and the chosen project decision (Proposal
  D — hybrid).
- **[docs/RF-DESIGN.md](docs/RF-DESIGN.md)** — L-network theory, component
  sizing, detector math. *(scaffold during M0)*
- **[docs/DEV-SETUP.md](docs/DEV-SETUP.md)** — VS Code dev environment
  for both firmware (PlatformIO/Teensy) and master (Go/Pi).
- **[firmware/teensy-selftest/](firmware/teensy-selftest/)** —
  standalone bring-up project. Run this first on a new Teensy + Ethernet
  kit to verify the hardware platform before flashing production firmware.

## Status

**M1b live on the bench (simulated HAL).** End-to-end is up:

- The Go master runs on a Raspberry Pi under systemd, serving the
  embedded web UI on `:8088`.
- A Teensy 4.1 dials in over TCP (PROTOCOL.md §1.0) and publishes
  real `state` + `heartbeat` frames; the master fans them out to
  every connected browser.
- The web UI's **Operate** panel sends `move_l` / `move_c` /
  `set_side` / `set_bypass` / `home` verbs — they hit the controller,
  drive the simulated stepper / encoder / relay / safety HAL, and the
  resulting state lands back on every browser within a frame.
- The RF-lockout path is exercisable from the browser via the debug
  `set_fwd_w` verb — set it above 5 W and motion verbs come back with
  `ack ok:false code:rf_lockout`.

Real TMC2209 drivers, hardware encoders, and vacuum relays plug in
behind the same HAL interfaces under M1b.2 hardware integration —
application code does not change. See [docs/PLAN.md](docs/PLAN.md)
for the milestone-by-milestone state and
[docs/ARCHITECTURE.md §5.1.3](docs/ARCHITECTURE.md) for the HAL backend
swap-in path.

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
| Tuner-side MCU     | Teensy 4.1 (PlatformIO, C/C++)                                    |
| Stepper drivers    | TMC2209 ×2                                                        |
| Position feedback  | Quadrature optical encoders ≥ 2000 CPR ×2                         |
| Vacuum relays      | Gigavac G2/G81 ×3 (Hi-Z, Lo-Z, bypass)                            |
| RF detection       | AD8302 + AD8307 ×2 behind a Tandem-match coupler                  |
| Master MCU         | Raspberry Pi 4/5 with HDMI touchscreen                            |
| Master service     | Go, single static binary, embedded web UI via `go:embed`          |
| Master deps        | `gorilla/websocket`, `BurntSushi/toml`, `modernc.org/sqlite`      |
| Operator input     | 2 × Adafruit ANO directional encoder (Adafruit p/n 5735)          |
| Transceiver link   | USB serial CAT (CI-V / Yaesu / Kenwood / K3/K4)                   |

## Why this shape (briefly)

It mirrors the station's existing automation pattern from
LP-100A-Server: one Go binary on the Pi, embedded UI, WebSocket fan-out,
TOML config, systemd deployment. New device, same shape — uniform to
operate, uniform to debug.

The L-network topology is what the user specified: a single L and a single
C with the C electrically movable to either side via vacuum relays. Not a
T-network, not a pi-network, not a balanced tuner. See
[CLAUDE.md](CLAUDE.md) §"RF topology" for why that's locked in.
