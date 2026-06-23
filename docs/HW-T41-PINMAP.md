# T41 V2.09 carrier — Teensy 4.1 pin map

Reference pin map for the grblHAL Teensy 4.x V2.09 carrier as used by
this antenna-tuner project. Pin numbers are **facts about the board's
net-to-Teensy-pin wiring**, cross-checked against upstream sources:

- `grblHAL/iMXRT1062 :: grblHAL_Teensy4/src/boards/T41U5XBB_map.h`
  (master branch) — the authoritative grblHAL board map for this carrier.
- The upstream V2.07 schematic PDF in
  [phil-barrett/grblHAL-teensy-4.x](https://github.com/phil-barrett/grblHAL-teensy-4.x)
  (V2.07 / V2.08 / V2.09 are electrically identical per the upstream
  README; the V2.07 PDF applies to V2.09).

Each table is grouped by function. The **Tuner use** column records the
antenna-tuner application's intended use per pin — that part is our
design, not upstream.

## 1 · Stepper outputs (5 axes)

Each axis drives an external stepper driver (TMC2209 / DM542 /
equivalent) via STEP / DIR / EN screw terminals on the carrier.

| Axis | STEP | DIR  | EN   | Tuner use (L-Match)        | Tuner use (T-Match) | Tuner use (Pi-Match) |
| ---- | ---- | ---- | ---- | -------------------------- | ------------------- | -------------------- |
| X    | 2    | 3    | 10   | Roller inductor (L)        | Series C₁           | Shunt C₁             |
| Y    | 4    | 5    | 40   | Vacuum-variable cap (C)    | Series C₂           | Series L             |
| Z    | 6    | 7    | 39   | *(unused)*                 | Shunt L             | Shunt C₂             |
| M3   | 8    | 9    | 38   | *(spare — bandswitch, etc.)* | spare             | spare                |
| M4   | 26   | 27   | 37   | *(spare)*                  | spare               | spare                |

**ENA polarity:** active-LOW on most external drivers (LOW = driver
enabled, coils energised). Confirm with the `T` command in
`firmware/t41-stepper-test/` before flipping a high-current axis.

## 2 · Axis end-stop inputs (opto-isolated)

| Pin  | Board net    | Tuner use                                  |
| ---- | ------------ | ------------------------------------------ |
| 20   | X LIMIT      | X-axis mechanical home microswitch         |
| 21   | Y LIMIT      | Y-axis mechanical home microswitch         |
| 22   | Z LIMIT      | Z-axis mechanical home microswitch (T/Pi)  |
| 23   | A LIMIT (M3) | Spare — fault input from external interlock |
| 28   | B LIMIT (M4) | Spare — shared with AUXINPUT5 (MPG mode)   |

## 3 · GRBL control inputs (opto-isolated) — repurposed for tuner

These five inputs are factory-labelled for CNC use; the tuner-controller
reuses them for its own operational signals.

| Pin  | grblHAL function | Board net  | Tuner use                                         |
| ---- | ---------------- | ---------- | ------------------------------------------------- |
| 14   | RESET            | RESET      | Operator panic — drive everything to BYPASS + halt |
| 15   | PROBE            | PROBE      | Spare — RF-presence sense from external detector  |
| 16   | FEED_HOLD        | FEED HOLD  | **TX-key panic** — hardware lockout while PTT'd    |
| 17   | CYCLE_START      | CYCLE START | Engage-from-bypass momentary input                |
| 29   | SAFETY_DOOR      | SAFETY DOOR | Enclosure interlock — refuse motion if open       |

## 4 · Auxiliary digital inputs (4× EMI-filtered, Schmitt-trigger)

| Pin  | grblHAL alias | Tuner use                                              |
| ---- | ------------- | ------------------------------------------------------ |
| 30   | AUXINPUT1 / QEI_A | X-axis encoder A (or motor-warning input)          |
| 34   | AUXINPUT2 / QEI_B | X-axis encoder B                                   |
| 35   | AUXINPUT3 / QEI_SELECT | spare encoder index / Z-pulse                 |
| 36   | AUXINPUT0     | Motor-fault aggregate input from stepper drivers      |
| 41   | AUXINPUT4 / I2C strobe | spare (I²C bus if a daughterboard is fitted) |

The board multiplexes the QEI A/B/SELECT pins with AUXINPUT1/2/3 — they
are the same physical pins. Use them as quadrature-encoder inputs for
the X (and optionally Y) axis per [CLAUDE.md](../CLAUDE.md) invariant 3.

## 5 · Relay-driver outputs (open-collector, 5 V/12 V coil jumper)

The carrier exposes seven driver outputs intended for relay coils; we
use them to drive the external vacuum-relay coils via opto-isolated
MOSFET stages (HV bias side is independent).

| Pin  | grblHAL alias    | Board net      | Tuner use                                   |
| ---- | ---------------- | -------------- | ------------------------------------------- |
| 11   | AUXOUTPUT4 / Spindle DIR  | SPINDLE DIR    | **K2 — Lo-Z selector** (L-Match)            |
| 12   | AUXOUTPUT3 / Spindle EN   | SPINDLE EN     | **K1 — Hi-Z selector** (L-Match)            |
| 13   | AUXOUTPUT5 / Spindle PWM  | SPINDLE PWM    | Unused (0–10 V analog, irrelevant to tuner) |
| 18   | AUXOUTPUT7 / Coolant MIST | COOLANT MIST   | Spare — bandswitch / antenna-select         |
| 19   | AUXOUTPUT6 / Coolant FLOOD| COOLANT FLOOD  | **K3 — bypass relay** (latched at power-up) |
| 31   | AUXOUTPUT0       | (aux relay 1)  | Spare — fault-output indicator              |
| 32   | AUXOUTPUT1       | (aux relay 2)  | Spare                                       |
| 33   | AUXOUTPUT2 / SPINDLE1 PWM | (aux relay 3 / shared) | Spare                              |

Note: pin 33 is dual-purposed in the grblHAL source (AUXOUTPUT2 *and*
SPINDLE1_PWM). The factory-default carrier wiring is the relay-driver
role; treat the PWM alias as unused for our application.

## 6 · Miscellaneous

| Pin  | Function       | Tuner use                            |
| ---- | -------------- | ------------------------------------ |
| 24   | NeoPixel UART  | Spare — front-panel status LED chain |
| 37   | M4 ENABLE      | (see §1 — M4-axis enable)            |
| 38   | M3 ENABLE      | (see §1 — M3-axis enable)            |
| 39   | Z ENABLE       | (see §1 — Z-axis enable)             |
| 40   | Y ENABLE       | (see §1 — Y-axis enable)             |

## 7 · Tuner-specific allocation summary

The pins our firmware actually drives, in topology order:

### L-Match (2 stepper axes)

| Function          | Teensy pin | Carrier net   |
| ----------------- | ---------- | ------------- |
| Roller-inductor STEP | 2       | X STEP        |
| Roller-inductor DIR  | 3       | X DIR         |
| Roller-inductor EN   | 10      | X ENABLE      |
| Vacuum-cap STEP   | 4          | Y STEP        |
| Vacuum-cap DIR    | 5          | Y DIR         |
| Vacuum-cap EN     | 40         | Y ENABLE      |
| L-axis end-stop   | 20         | X LIMIT       |
| C-axis end-stop   | 21         | Y LIMIT       |
| K1 (Hi-Z)         | 12         | SPINDLE EN    |
| K2 (Lo-Z)         | 11         | SPINDLE DIR   |
| K3 (Bypass)       | 19         | COOLANT FLOOD |
| TX-key panic      | 16         | FEED HOLD     |

### T-Match / Pi-Match (3 stepper axes)

Adds a third stepper on the Z axis:

| Function       | Teensy pin | Carrier net |
| -------------- | ---------- | ----------- |
| 3rd-element STEP | 6        | Z STEP      |
| 3rd-element DIR  | 7        | Z DIR       |
| 3rd-element EN   | 39       | Z ENABLE    |
| 3rd-axis end-stop | 22      | Z LIMIT     |

K1 / K2 are L-Match-only (the selector relay is unused in symmetric
T/Pi topologies). K3 (bypass) is retained per
[CLAUDE.md](../CLAUDE.md) invariant 2.

## 8 · Sourcing & licensing note

Pin numbers in this document are factual data about the V2.09 board's
Teensy-pin-to-net wiring, cross-checked against the grblHAL upstream
header file cited at the top. No copyrighted schematic excerpts or
artwork are reproduced; the tables, grouping, and Tuner-use annotations
are this project's own work. See [HW-T41-CARRIER.md](HW-T41-CARRIER.md)
§"Licensing and IP" for the carrier-board redistribution policy.
