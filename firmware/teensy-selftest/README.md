# Teensy 4.1 + Ethernet selftest

Standalone bring-up project. **Run this first** on any new Teensy +
Ethernet kit, before flashing the production firmware in
[`../tuner-controller/`](../tuner-controller/). Once it passes, you
know the hardware platform is good; if it doesn't, you know the
problem is hardware, not firmware logic.

## What it checks

1. **Board + USB serial.** Banner appears on the serial console.
2. **LED.** On-board LED (pin 13) encodes progress at a glance:
   - **Solid** — booting, or PHY link not up yet.
   - **Slow blink (1 Hz)** — Ethernet link is up, but DHCP hasn't
     finished yet. If it stays here, you have an L2 connection but no
     L3 — DHCP is the suspect.
   - **Fast blink (5 Hz)** — DHCP lease acquired, server listening,
     fully operational. This is the "all green" state.
3. **PHY link.** Reports link speed (10/100 Mbps) and duplex.
4. **DHCP.** Acquires IP / mask / gateway / DNS within 15 s and prints
   them on serial.
5. **TCP server.** Listens on port 80, serves a small HTML status page
   to any LAN client. Each request bumps a counter and logs the
   remote IP to serial.
6. **Periodic heartbeat.** One-line status print every 10 s so you can
   leave it running and confirm the link stays up.

## What you need

- Teensy 4.1.
- PJRC Ethernet kit (DP83825 PHY + magjack), seated on the Teensy's
  Ethernet pads on the bottom of the board, or wired to the right
  pins per the kit instructions.
- Cat5e/Cat6 cable to a switch or router on a network with DHCP.
- USB cable from your dev machine to the Teensy.

## Ethernet library: dual-backend build

The selftest compiles against **either** of the two mainstream
Teensy 4.1 Ethernet libraries, selected by the PlatformIO environment:

| Env | Library | TCP/IP stack | When to pick |
|---|---|---|---|
| **`teensy41`** (default) | [QNEthernet](https://github.com/ssilverman/QNEthernet) | lwIP | Active maintenance, portable to STM32H7 (Phase 2). Default. |
| `teensy41_native` | [NativeEthernet](https://github.com/vjmuzik/NativeEthernet) | FNET | Matches Morconi / TeensyMaestro / FlexRigTeensy convention. Useful for A/B verification. |

Same `src/main.cpp` for both; the difference is one `#define` and one
`lib_deps` entry in [`platformio.ini`](platformio.ini). The abstraction
layer that makes this work is in `src/net_hal.{h,_qnethernet.cpp,_nativeethernet.cpp}`.
See [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §5.1.2
for the trade-off discussion.

## Flashing

From this directory:

```sh
# Default (QNEthernet):
pio run -e teensy41 -t upload

# Or NativeEthernet:
pio run -e teensy41_native -t upload
```

(First run downloads ~150 MB of Teensy toolchain into `~/.platformio/`,
plus the chosen library into `.pio/libdeps/`.)

The default `upload_protocol = teensy-gui` in `platformio.ini` uses
the **Teensy Loader app** (`teensy.app`) bundled with PlatformIO — a
small loader window pops up briefly while flashing. On the first run
the Teensy needs to be in *program mode*: press the small white button
on the board. After the first successful upload, the firmware's USB
stack auto-reboots for subsequent flashes; you won't need to press the
button again.

### Headless / CI alternative

If you don't want the GUI window (e.g., scripted bring-up, CI), install
[`teensy_loader_cli`](https://www.pjrc.com/teensy/loader_cli.html) and
switch the upload protocol:

```sh
brew install teensy_loader_cli
# then in platformio.ini:
#   upload_protocol = teensy-cli
```

## Watching it run

```sh
pio device monitor -e teensy41
```

Expected output:

```
===========================================
Teensy 4.1 + Ethernet selftest
Automatic Antenna Tuner / firmware bring-up
===========================================
[1/4] Booted; LED solid; serial up.
MAC:     04:E9:E5:0E:XX:XX
[2/4] Bringing up Ethernet (DHCP, 15 s timeout)...
       waiting for link..
       link up: 100 Mbps, full-duplex
[3/4] Waiting for DHCP lease...
IP:      192.168.1.50
Mask:    255.255.255.0
Gateway: 192.168.1.1
DNS:     192.168.1.1
[4/4] HTTP server listening on port 80
       open  http://192.168.1.50/  from another device on the LAN
===========================================
ALL GREEN. LED blinking @ 1 Hz; status heartbeat every 10 s.

[    10s] link=up ip=192.168.1.50 requests=0
[    20s] link=up ip=192.168.1.50 requests=0
```

Then from another machine on the LAN:

```sh
# Quick smoke
curl -i http://192.168.1.50/

# Or open it in a browser — you'll see a dark-themed status page with
# MAC / IP / uptime / requests served.
```

Each request bumps the `requests` counter in the serial heartbeat.

## Success criteria checklist

- [ ] Banner prints on serial within 5 s of plug-in.
- [ ] MAC line shows a valid Teensy MAC (PJRC OUI `04:E9:E5`).
- [ ] Link comes up; speed + duplex reported.
- [ ] DHCP returns a sensible IP on your subnet.
- [ ] LED visibly fast-blinks (5 Hz) once DHCP succeeds.
- [ ] `curl` / browser to `http://<ip>/` returns the HTML status page.
- [ ] Heartbeat line every 10 s in the serial monitor.

If all six pass, the hardware platform is good and you can move on to
the production firmware bring-up in [`../tuner-controller/`](../tuner-controller/).

## Troubleshooting

| Symptom                                          | Likely cause                                                                                |
|--------------------------------------------------|---------------------------------------------------------------------------------------------|
| No banner on serial                              | Wrong COM/tty device; check `pio device list`. Or VS Code is holding the port.              |
| `Ethernet.begin() returned false`                | PHY not detected. Re-seat the Ethernet kit; verify the magjack daughterboard solder joints. |
| `waiting for link...` never completes            | Cable / switch port issue. Try a known-good cable on a known-good port.                     |
| Link up but DHCP times out                       | No DHCP server on this VLAN, or DHCP is restricted to known MACs. Try a different network.  |
| LED solid forever, no blink                      | Link never came up. See above.                                                              |
| `curl` reaches Teensy but no response            | Almost always a firewall on the *client* side. Try from a wired LAN host.                   |
| Random reboots after ~30 s                       | USB power can be marginal. Try a powered USB hub or external 5 V to the Teensy.             |
| Serial shows garbage / wrong baud                | Set monitor speed to 115200 (default in `platformio.ini`).                                  |

## What this project intentionally is NOT

This is a *bring-up* project. It is **not** the production tuner
firmware. It has no protocol, no relay control, no motor control, and
no safety lockouts. Do not connect it to actual tuner hardware. Once
the selftest passes, delete this directory or leave it as a reference
— production work happens in [`../tuner-controller/`](../tuner-controller/).
