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
- **[docs/RF-DESIGN.md](docs/RF-DESIGN.md)** — L-network theory, component
  sizing, detector math. *(scaffold during M0)*
- **[docs/DEV-SETUP.md](docs/DEV-SETUP.md)** — VS Code dev environment
  for both firmware (PlatformIO/Teensy) and master (Go/Pi).
- **[firmware/teensy-selftest/](firmware/teensy-selftest/)** —
  standalone bring-up project. Run this first on a new Teensy + Ethernet
  kit to verify the hardware platform before flashing production firmware.

## Status

**M0 done (master side), M1 software scaffold done, M1 firmware pending
hardware.** The Go master compiles for macOS and `linux/arm64`, serves
an embedded web UI that subscribes to `/ws` and renders live state +
telemetry from a `fakecontroller` source. The firmware skeleton builds
all three PlatformIO envs (`teensy41`, `nucleo_h743zi`, `native`); real
Ethernet + WS bring-up happens when a Teensy is in hand. See
[docs/PLAN.md](docs/PLAN.md) for the milestone-by-milestone state.

## Deploy the master to a Raspberry Pi

Targets Pi 4 / 5 running 64-bit Raspberry Pi OS (Debian 12+ derivative).
Pure-Go module — no cross-compile toolchain needed; any Go install on
the dev machine can target `linux/arm64`. Layout follows the same
LP-100A-Server shape so station services stay uniform.

### First-time install

On the dev machine:

```sh
cd master/tuner-master
./deploy/build-pi.sh                              # → dist/tuner-master-linux-arm64
scp -r dist deploy pi@<pi-host>:/tmp/tuner-master-deploy
```

On the Pi:

```sh
cd /tmp/tuner-master-deploy/deploy
sudo ./install.sh
sudo nano /etc/tuner-master/config.toml           # set [tuner].host to the controller's IP
sudo systemctl restart tuner-master.service
```

`install.sh` is idempotent — re-run it for upgrades. It creates:

| Path                                          | Purpose                                |
|-----------------------------------------------|----------------------------------------|
| `/opt/tuner-master/tuner-master`              | the binary                             |
| `/etc/tuner-master/config.toml`               | config (never overwritten on re-install) |
| `/var/lib/tuner-master/`                      | SQLite memory DB + runtime state       |
| `/etc/systemd/system/tuner-master.service`    | systemd unit                           |
| user/group `tuner`                            | system account, no shell               |

Verify:

```sh
systemctl status tuner-master.service
journalctl -u tuner-master.service -f
curl http://<pi-host>:8088/healthz
```

### Fast iteration during dev

Once installed, redeploy without re-running the whole install path:

```sh
# Export once in your shell rc:
export PI_HOST=tuner-pi.local   # or 192.168.1.42
export PI_USER=pi

./deploy/redeploy.sh
```

That cross-compiles, scp's the new binary, atomically swaps it via
`install -m 755`, restarts the service, and hits `/healthz` to
confirm. The PI user needs passwordless sudo (the usual Pi default).

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
