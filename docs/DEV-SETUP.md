# Development environment

VS Code on macOS, targeting two builds: the **tuner-controller firmware**
(Teensy 4.1 — C/C++ via PlatformIO) and the **tuner-master service**
(Go, deployed to a Raspberry Pi). One multi-root workspace, two
toolchains, one git repo.

Phase 2 of the MCU plan (STM32H743 — see [`../CLAUDE.md`](../CLAUDE.md)
"MCU selection") uses the same PlatformIO workflow with a different
`env:` block, so nothing in this setup is throwaway.

## 0. Prerequisites (one-time, dev machine)

These should already be present on a typical dev Mac — verify, install
the gaps:

```sh
# Toolchains
brew install go                  # 1.22+
brew install --cask visualstudio  # if not already installed
brew install git python pipx     # pipx hosts PlatformIO Core cleanly

# PlatformIO Core (the CLI underneath the PlatformIO VS Code extension).
# Installing via pipx keeps it out of system Python; the VS Code
# extension picks it up automatically.
pipx install platformio
pio --version

# Go tooling used by gopls + Go extension (Go installs the rest on demand)
go install golang.org/x/tools/gopls@latest
go install github.com/go-delve/delve/cmd/dlv@latest
go install honnef.co/go/tools/cmd/staticcheck@latest

# Optional but worth it
brew install golangci-lint       # lint pre-commit
brew install jq                  # WS payload inspection
```

For Pi-side deployment later (M6), no extra toolchain is needed — Go
cross-compiles `linux/arm64` from macOS with zero CGO dependencies,
exactly like LP-100A-Server's `deploy/build-pi.sh`.

## 1. VS Code extensions (curated list)

Install all of these in one shot — they are the minimum for both builds:

| Extension                              | Why                                                                       |
|----------------------------------------|---------------------------------------------------------------------------|
| **PlatformIO IDE** (`platformio.platformio-ide`) | Teensy 4.1 build / upload / monitor; STM32H7 later. Pulls in C/C++ tools. |
| **C/C++** (`ms-vscode.cpptools`)       | IntelliSense for firmware. Auto-installed by PlatformIO IDE.              |
| **Cortex-Debug** (`marus25.cortex-debug`) | SWD debugging for Teensy/STM32 when a J-Link/Black Magic Probe is connected. |
| **Serial Monitor** (`ms-vscode.vscode-serial-monitor`) | Teensy USB-serial console; multi-tab capable.                  |
| **Go** (`golang.go`)                   | Master service: gopls IntelliSense, delve debug, test runner, gofmt.      |
| **Remote – SSH** (`ms-vscode-remote.remote-ssh`) | Edit/run on the Pi as if local. Used during M5 commissioning.   |
| **Better TOML** (`tamasfe.even-better-toml`) | All configs are TOML; gives schema-aware editing.                     |
| **YAML** (`redhat.vscode-yaml`)        | GitHub Actions, PlatformIO `platformio.ini` lint helper.                  |
| **Markdown All in One** (`yzhang.markdown-all-in-one`) | These docs are markdown-heavy.                                |
| **GitLens** (`eamodio.gitlens`)        | Optional; blame + history inline. Skip if you find it noisy.              |
| **EditorConfig** (`editorconfig.editorconfig`) | Honors `.editorconfig` for consistent indent across firmware/master.  |

One-liner install once the workspace is open:

```sh
code --install-extension platformio.platformio-ide \
     --install-extension marus25.cortex-debug \
     --install-extension ms-vscode.vscode-serial-monitor \
     --install-extension golang.go \
     --install-extension ms-vscode-remote.remote-ssh \
     --install-extension tamasfe.even-better-toml \
     --install-extension redhat.vscode-yaml \
     --install-extension yzhang.markdown-all-in-one \
     --install-extension editorconfig.editorconfig
```

## 2. Workspace layout

One multi-root workspace, so a single VS Code window covers both
builds. Save as `Automatic-Antenna-Tuner.code-workspace` at the repo
root:

```json
{
  "folders": [
    { "path": ".", "name": "tuner (root)" },
    { "path": "firmware/tuner-controller", "name": "firmware (Teensy/STM32)" },
    { "path": "master/tuner-master",       "name": "master (Go on Pi)" }
  ],
  "settings": {
    "files.exclude": { ".pio": true, "dist": true, "**/.DS_Store": true },
    "go.lintTool": "golangci-lint",
    "go.useLanguageServer": true,
    "[go]":   { "editor.formatOnSave": true, "editor.codeActionsOnSave": { "source.organizeImports": "explicit" } },
    "[c]":    { "editor.formatOnSave": true },
    "[cpp]":  { "editor.formatOnSave": true },
    "platformio-ide.useBuiltinPIOCore": false,
    "platformio-ide.activateProjectOnTextEditorChange": true
  },
  "extensions": {
    "recommendations": [
      "platformio.platformio-ide", "marus25.cortex-debug",
      "ms-vscode.vscode-serial-monitor", "golang.go",
      "ms-vscode-remote.remote-ssh", "tamasfe.even-better-toml"
    ]
  }
}
```

`useBuiltinPIOCore: false` tells the PlatformIO extension to use the
`pipx`-installed `pio` from §0 — keeps one PlatformIO on the system
instead of two, and makes CI behaviour match local.

A repo-level `.editorconfig`:

```ini
root = true
[*]
indent_style = space
indent_size = 4
end_of_line = lf
charset = utf-8
trim_trailing_whitespace = true
insert_final_newline = true
[*.go]
indent_style = tab
[*.{md,yaml,yml,toml,json}]
indent_size = 2
```

## 3. Firmware workflow (Teensy 4.1 via PlatformIO)

### 3.1 Initial `platformio.ini`

To live at `firmware/tuner-controller/platformio.ini`. Both targets are
declared from day one so the portability rule from
[`../CLAUDE.md`](../CLAUDE.md) "MCU selection" is enforced by the build:

```ini
[platformio]
default_envs = teensy41

[env]
framework        = arduino
build_unflags    = -std=gnu++11
build_flags      = -std=gnu++17 -Wall -Wextra -Wno-unused-parameter
lib_deps         =
    ssilverman/QNEthernet           ; Teensy 4.1 native Ethernet (lwIP)
test_framework   = unity

[env:teensy41]
platform         = teensy
board            = teensy41
upload_protocol  = teensy-cli
monitor_speed    = 115200
build_flags      = ${env.build_flags} -DTARGET_TEENSY41

; Phase-2 fallback target — kept building from M0 so the HAL split stays honest.
[env:nucleo_h743zi]
platform         = ststm32
board            = nucleo_h743zi
upload_protocol  = stlink
debug_tool       = stlink
monitor_speed    = 115200
build_flags      = ${env.build_flags} -DTARGET_STM32H743

; Native (host) target — for HAL-independent unit tests.
[env:native]
platform         = native
build_flags      = -std=gnu++17 -DUNIT_TEST -DTARGET_NATIVE
```

The `native` env compiles HAL-agnostic logic on the dev Mac for unit
tests — no MCU needed.

### 3.2 Day-to-day commands

| Command                                    | What it does                              |
|--------------------------------------------|-------------------------------------------|
| `pio run -e teensy41`                      | Compile firmware for Teensy.              |
| `pio run -e teensy41 -t upload`            | Compile + flash over USB (Teensy Loader). |
| `pio device monitor -e teensy41`           | USB serial console.                       |
| `pio run -e nucleo_h743zi`                 | Verify STM32 portability builds clean.    |
| `pio test -e native`                       | Run HAL-independent unit tests on host.   |
| `pio run -t clean`                         | Wipe build artefacts.                     |

PlatformIO VS Code sidebar gives buttons for each of these — but
remember the CLI commands; they're what CI runs.

### 3.3 Flashing the Teensy

USB-attached Teensy 4.1: hit the on-module program button, then `pio
run -t upload`. PlatformIO will use `teensy_loader_cli` (installed
automatically into `~/.platformio/`).

For OTA-flashing once Ethernet is up, use the `TeensyOTA` library or
the `RemoteFlash` library; M0 sticks to USB-only flashing — OTA is an
M6 nice-to-have.

### 3.4 Debugging

Teensy 4.1 doesn't expose SWD by default — there are six SWD pads on
the back of the board you have to solder to. Once those are wired to a
J-Link or Black Magic Probe, Cortex-Debug picks it up via this
`launch.json` snippet:

```json
{
  "name": "Teensy 4.1 SWD",
  "cwd": "${workspaceFolder}/firmware/tuner-controller",
  "type": "cortex-debug",
  "request": "launch",
  "servertype": "jlink",
  "device": "MIMXRT1062DVL6A",
  "interface": "swd",
  "executable": ".pio/build/teensy41/firmware.elf"
}
```

For most M0–M2 work `Serial.print` debugging + the on-board LEDs +
test fixtures are enough. SWD becomes important when chasing motor-ISR
timing or lwIP stalls.

### 3.5 Project skeleton (created at M0)

```
firmware/tuner-controller/
├── platformio.ini
├── include/                    # shared headers
├── src/
│   ├── main.cpp                # task setup, super-loop
│   ├── app/                    # MCU-agnostic logic (telemetry diff, state, protocol)
│   │   ├── state.{h,cpp}
│   │   ├── protocol.{h,cpp}
│   │   └── tuning.{h,cpp}
│   └── hal/                    # one file per peripheral, target-gated
│       ├── motor_teensy41.cpp
│       ├── motor_stm32h7.cpp
│       ├── encoder_teensy41.cpp
│       ├── encoder_stm32h7.cpp
│       ├── adc_teensy41.cpp
│       ├── adc_stm32h7.cpp
│       ├── relay.{h,cpp}        # GPIO-only; one file fits both
│       └── net_lwip.{h,cpp}     # lwIP is portable; PHY init is target-gated
└── test/
    └── test_state_native/      # runs on PC via env:native
```

`#ifdef TARGET_TEENSY41` / `#ifdef TARGET_STM32H743` macros come from
the `build_flags` in §3.1.

## 4. Master workflow (Go service for the Pi)

### 4.1 Initial module

```sh
cd master/tuner-master
go mod init github.com/VU3ESV/Automatic-Antenna-Tuner/master/tuner-master
go mod tidy
```

Initial dep set, matching LP-100A-Server where possible so the station
stays uniform:

```sh
go get github.com/gorilla/websocket
go get github.com/BurntSushi/toml
go get modernc.org/sqlite           # pure-Go SQLite, no CGO
```

### 4.2 Day-to-day commands

| Command                                          | What it does                                  |
|--------------------------------------------------|-----------------------------------------------|
| `go run . -config deploy/config.example.toml`    | Run the master locally on the Mac.            |
| `go test ./... -race`                            | Unit tests with race detector (CI gate).      |
| `go vet ./...`                                   | Quick static check.                           |
| `golangci-lint run`                              | Full lint.                                    |
| `GOOS=linux GOARCH=arm64 go build -o dist/tuner-master-linux-arm64 .` | Cross-compile for Pi 4/5 (64-bit). |
| `dlv debug . -- -config deploy/config.example.toml` | Interactive debug.                         |

The cross-compile is byte-for-byte the same shape as LP-100A-Server's
`deploy/build-pi.sh`; that script will be copied across at M0.

### 4.3 Hot-reload during dev

Optional, but pays off as soon as the embedded web UI exists:

```sh
go install github.com/air-verse/air@latest
cd master/tuner-master && air      # restarts on save
```

### 4.4 Debugging in VS Code

`.vscode/launch.json` (committed):

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "master: run",
      "type": "go",
      "request": "launch",
      "mode": "auto",
      "program": "${workspaceFolder}/master/tuner-master",
      "args": ["-config", "${workspaceFolder}/master/tuner-master/deploy/config.example.toml"]
    },
    {
      "name": "master: test (current package)",
      "type": "go",
      "request": "launch",
      "mode": "test",
      "program": "${fileDirname}"
    }
  ]
}
```

### 4.5 Working on the Pi directly

Two modes, pick by intent:

- **Cross-compile + scp** (default — fast iteration, no Pi-side toolchain
  required). Same as LP-100A-Server's `deploy/redeploy.sh`.
- **Remote-SSH** (for commissioning at M5): `code --remote
  ssh-remote+pi-tuner /opt/tuner-master/` — VS Code runs locally,
  filesystem and extensions run on the Pi. Useful when you need to
  reproduce something tied to real Pi hardware (encoder GPIO, USB CAT
  cable).

## 5. Combined dev loop (firmware + master + browser)

Once both halves have their M0 hello-world:

```
┌─ Terminal 1 ─────────────────────────────────┐
│  pio run -e teensy41 -t upload               │  ← every firmware change
│  pio device monitor -e teensy41              │  ← Teensy USB serial logs
└──────────────────────────────────────────────┘
┌─ Terminal 2 ─────────────────────────────────┐
│  cd master/tuner-master && air               │  ← master auto-restarts
└──────────────────────────────────────────────┘
┌─ Browser ────────────────────────────────────┐
│  http://localhost:8088/                      │  ← embedded UI
└──────────────────────────────────────────────┘
┌─ Optional: WS sniff ─────────────────────────┐
│  websocat ws://<teensy-ip>:80/ws | jq        │
│  websocat ws://localhost:8088/ws  | jq       │
└──────────────────────────────────────────────┘
```

(`brew install websocat` if you don't have it — invaluable for poking
the WS endpoints without the UI in the way.)

## 6. CI parity (so local matches CI)

GitHub Actions, two workflows (mirroring LP-100A-Server):

- **`firmware-ci.yml`** — `pio run -e teensy41`, `pio run -e nucleo_h743zi`,
  `pio test -e native`. Runs on every push touching `firmware/**`.
- **`master-ci.yml`** — `go vet`, `go test -race`, `go build`,
  `golangci-lint`. Runs on every push touching `master/**`.

Locally, before pushing:

```sh
( cd firmware/tuner-controller && pio run -e teensy41 && pio run -e nucleo_h743zi && pio test -e native )
( cd master/tuner-master       && go vet ./... && go test ./... -race && golangci-lint run )
```

A repo-root `Makefile` with `make ci-local` running both is M0 work.

## 7. What this setup does NOT include yet

- **Linux/Windows dev parity.** macOS-only as written. Adding Linux is
  trivial (same `brew` packages exist as `apt`); document on demand.
- **Docker / devcontainer.** Possible later if onboarding gets messier,
  but PlatformIO + Go are both happy native; a container adds friction
  for USB/serial passthrough.
- **OTA firmware flashing.** USB-only at M0; OTA is an M6 nice-to-have.
- **STM32H743 hardware in hand.** The `env:nucleo_h743zi` builds clean
  from M0 even without a board attached; flashing it is Phase 2 work.

## 8. First-day checklist

Run top to bottom on a fresh dev Mac:

1. `brew install go git python pipx websocat`
2. `pipx install platformio && pio --version`
3. `go install golang.org/x/tools/gopls@latest github.com/go-delve/delve/cmd/dlv@latest honnef.co/go/tools/cmd/staticcheck@latest`
4. `git clone <repo> && cd Automatic-Antenna-Tuner`
5. `code Automatic-Antenna-Tuner.code-workspace` (created in §2 once the workspace file is in the repo — M0)
6. Install the recommended extensions when VS Code prompts.
7. Attach a Teensy 4.1, run `pio run -e teensy41 -t upload` from the
   firmware folder. LED should blink — that's M0 hello-world.
8. From the master folder, `go run . -config deploy/config.example.toml`,
   open `http://localhost:8088/` — should show the "controller
   connected" pill green once the Teensy is on the LAN with M0
   firmware.

When that loop closes, the dev environment is done.
