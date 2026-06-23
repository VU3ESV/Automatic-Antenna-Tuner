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

| Axis | STEP | DIR  | EN   | STEP-pin PWM source       | Tuner use (L-Match)        | Tuner use (T-Match) | Tuner use (Pi-Match) |
| ---- | ---- | ---- | ---- | ------------------------- | -------------------------- | ------------------- | -------------------- |
| X    | 2    | 3    | 10   | **FlexPWM4.2 A**          | Roller inductor (L)        | Series C₁           | Shunt C₁             |
| Y    | 4    | 5    | 40   | **FlexPWM2.0 A**          | Vacuum-variable cap (C)    | Series C₂           | Series L             |
| Z    | 6    | 7    | 39   | **FlexPWM2.2 A**          | *(unused)*                 | Shunt L             | Shunt C₂             |
| M3   | 8    | 9    | 38   | **FlexPWM1.3 A**          | *(spare — bandswitch, etc.)* | spare             | spare                |
| M4   | 26   | 27   | 37   | **none** (PIT-ISR only)   | *(spare)*                  | spare               | spare                |

**ENA polarity:** active-LOW on most external drivers (LOW = driver
enabled, coils energised). Confirm with the `T` command in
`firmware/t41-stepper-test/` before flipping a high-current axis.

**STEP-pin PWM column:** verified against `framework-arduinoteensy/
cores/teensy4/pwm.c`. Pins 2 / 4 / 6 / 8 each map to a distinct
FlexPWM submodule (4.2 / 2.0 / 2.2 / 1.3) — submodule-independent,
so all four can run concurrent autonomous step trains. Pin 26 has
**no FlexPWM or QuadTimer mux** on it; software `analogWrite()`
emulation only, which is useless for stepper pulse trains.

This matters when picking the hardware-pulse-generation strategy
required by [CLAUDE.md](../CLAUDE.md) "Firmware portability rule"
(no `runSpeed()` polling in `loop()`). Two viable architectures:

1. **FlexPWM-direct** — peripheral toggles the STEP pin
   autonomously; acceleration ramps written into submodule
   registers; **zero CPU per pulse**. Limited to FlexPWM-capable
   pins. Natural fit for our 3-axis tuner (X / Y / Z all qualify
   on distinct submodules — no contention). DIR pins (3 / 5 / 7
   / 9) don't need PWM; pin 3 happens to share submodule 4.2 with
   X STEP, which is fine because DIR is a static GPIO write.
2. **PIT + ISR + fast GPIO** — `PIT_LDVAL` schedules the next
   pulse time; the ISR sets the STEP pin via
   `IMXRT_GPIO*.DR_SET/DR_CLEAR`. **Light CPU per pulse but at
   interrupt priority** — immune to `loop()` blocking. Works on
   any GPIO including pin 26. This is what upstream grblHAL uses
   (see `iMXRT1062/grblHAL_Teensy4/src/driver.c`).

Both honour the portability rule. The tuner application is
**axis-independent** (each reactive element moves to its commanded
position; we never coordinate multi-axis Bresenham motion), so
FlexPWM-direct on X / Y / Z is the natural fit and pin 26 not
having a hardware-PWM mux costs us nothing — M4 is spare across
every supported topology. If a 4th-axis use ever appears, the
PIT-ISR path is the fallback and is fully compatible with the
same `hal/motor_teensy41.cpp` interface.

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

## 4 · Auxiliary digital inputs (5× EMI-filtered, Schmitt-trigger)

These five pins are the carrier's only inputs fast enough for quadrature
encoders — they feed the Teensy through an EMI-filter + Schmitt-trigger
front end, not through the slow optocoupler chain used by the limit and
GRBL-control inputs in §2 / §3. Call this the **tier-1 input bank**.

| Pin  | grblHAL alias            | Tuner use                                         |
| ---- | ------------------------ | ------------------------------------------------- |
| 30   | AUXINPUT1 / QEI_A        | Axis-1 encoder A (Phase 2)                        |
| 34   | AUXINPUT2 / QEI_B        | Axis-1 encoder B (Phase 2)                        |
| 35   | AUXINPUT3 / QEI_SELECT   | Axis-1 encoder Z / index (Phase 2)                |
| 36   | AUXINPUT0                | Motor-fault aggregate (default); axis-2 encoder A if repurposed |
| 41   | AUXINPUT4 / I²C strobe   | spare (I²C bus if a daughterboard is fitted); axis-2 encoder B if repurposed |

grblHAL's stock board map defines exactly one hardware-decoded
quadrature slot — A=30, B=34, optional index=35 — intended as the
single CNC MPG (manual-pulse-generator) jog wheel, not per-axis
position feedback. The tuner-controller HAL provisions axis 1 the
same way and allocates additional axes per the budget below.

### 4.1 · Encoder pin budget (Phase 2)

[CLAUDE.md](../CLAUDE.md) invariant #3 makes encoders the position
truth when fitted. Per-axis encoders are an M5+ Phase-2 deliverable
(see [HARDWARE.md](HARDWARE.md) BoM — "Position encoder"). The
tier-1 input bank constrains how many axes the carrier can host
directly:

- **2 axes:** fits — A=30, B=34, index=35 on axis 1; A=36, B=41 on
  axis 2 (no index). Costs the `MOTOR_FAULT` aggregate input (pin
  36) and the I²C-strobe option (pin 41). Motor-fault aggregation
  can be re-implemented as an external OR / wired-OR feeding a
  spare opto input.
- **3 axes** (full T/Pi-Match per-axis encoders): **does not fit**
  on the tier-1 bank — short by at least one A/B pair. Pick one:

  1. **External SPI quadrature counter** (LS7366R or HCTL-2032
     class) on a small daughterboard, one chip per axis, daisy
     chained on SPI. Counter handles edge counting at MHz rates;
     differential RS-422 receivers on the daughterboard tolerate
     long encoder cable runs. Carrier-side cost: SPI bus + one CS
     per axis from the AUXOUTPUT bank (§5). **Recommended for
     3-axis Phase 2.**
  2. **Direct Teensy-header tap for the 3rd axis.** The carrier
     exposes the Teensy 4.1 pin header; an unused GPIO can be
     pulled off there with an external Schmitt-trigger / EMI
     filter mounted near the connector. Cheaper than (1) but
     adds risk and bypasses the carrier's RF-hardening intent.
  3. **Equip only the most stall-prone axis** (typically the
     roller inductor on a stiff gearbox), leave other axes
     open-loop. Reduces the value of invariant #3's drift
     detection but is the cheapest path.

Note: the tier-2 opto-isolated inputs (limits §2, GRBL control §3)
have ~50–100 µs propagation through their 6N137-class optocoupler
+ RC filter — max edge rate around 10 kHz. A 2000 CPR encoder
loses edges past about 1 shaft-rev/s on those pins, so they are
**not viable** as encoder inputs no matter how attractive the
spare-pin count looks.

The Teensy 4.1's hardware QuadTimer (TMR1–4, 4 channels each) plus
the XBAR crossbar can route any of the tier-1 pins to a hardware
quadrature decoder — no pin-mux constraint at the MCU layer. The
constraint is the carrier's tier-1 pin count, not Teensy-side
decode capacity.

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
