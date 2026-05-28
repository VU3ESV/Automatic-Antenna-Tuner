# TB6600 + NEMA 23 Stepper Motor Test — Arduino Mega 2560 R3

Standalone bench test for exercising a NEMA 23 stepper motor through a
TB6600 driver.  Interactive serial menu (115200 baud) — no re-flashing
needed to change speed, direction, or step count.

The TB6600 supports up to 4 A per phase, which is well-matched to typical
NEMA 23 motors (2–3 A rated).  Set the TB6600 current-limit DIP switches
(S4/S5/S6) to match your motor's rated phase current.

## Wiring

The TB6600 has **opto-isolated** inputs.  This project uses **common-cathode**
wiring: the `−` side of each signal pair goes to Arduino **GND** and the `+`
side to the digital GPIO pin (signals are **active-high**):

```
TB6600             Arduino Mega R3
─────────────────────────────────────────
ENA+   ──────────── D8    (DIGITAL header)
ENA-   ──────────── GND
DIR+   ──────────── D9    (DIGITAL header)
DIR-   ──────────── GND
PUL+   ──────────── D10   (DIGITAL header)
PUL-   ──────────── GND
VCC    ──────────── 24–48 VDC PSU (+)
GND    ──────────── 24–48 VDC PSU (−)
A+ / A-  ────────── NEMA 23 coil A
B+ / B-  ────────── NEMA 23 coil B
```

All three signal pins (D8, D9, D10) are on the Mega R3's **DIGITAL** header
(the PWM-capable row between D7 and D13, near the board edge opposite the
POWER header).  The three `−` pins share any GND on the Mega R3's POWER
header.

> **Note:** When the Arduino drives a signal+ pin HIGH, current flows
> through the TB6600's internal optocoupler LED to GND on the `−` pin,
> activating the signal.  This is the common-cathode connection method.

## TB6600 DIP-switch micro-stepping

| S1  | S2  | S3  | Mode       | Steps/rev (1.8° motor) |
|-----|-----|-----|------------|------------------------|
| ON  | ON  | OFF | Full step  | 200                    |
| ON  | OFF | ON  | 1/2 step   | 400                    |
| OFF | ON  | ON  | 1/2 step   | 400                    |
| ON  | OFF | OFF | 1/4 step   | 800                    |
| OFF | ON  | OFF | 1/8 step   | 1600                   |
| OFF | OFF | ON  | 1/16 step  | 3200                   |
| OFF | OFF | OFF | 1/32 step  | 6400                   |

The firmware defaults to **1/8 micro-stepping (1600 steps/rev)**.  If you
change the DIP switches, update `STEPS_PER_REV` in `src/main.cpp`.

## Build & upload

```bash
cd firmware/mega-stepper-test
pio run -t upload
pio device monitor        # 115200 baud
```

## Serial commands

| Key | Action                            |
|-----|-----------------------------------|
| `1` | Rotate CW 1 revolution            |
| `2` | Rotate CCW 1 revolution           |
| `3` | Rotate CW N revolutions (prompts) |
| `4` | Rotate CCW N revolutions (prompts)|
| `5` | Jog CW 100 steps                  |
| `6` | Jog CCW 100 steps                 |
| `7` | Toggle continuous CW rotation     |
| `8` | Toggle continuous CCW rotation    |
| `9` | Stop (decelerate to halt)         |
| `S` | Set speed (steps/s)               |
| `A` | Set acceleration (steps/s²)       |
| `E` | Toggle motor enable/disable       |
| `Z` | Zero the position counter         |
| `?` | Print current status              |
| `H` | Show menu                         |
