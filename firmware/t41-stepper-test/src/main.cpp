// X + Y axis stepper bench-test for Teensy 4.1 in the grblHAL-teensy-4.x V2.09
// carrier (Phil Barrett).  Tests the L-Match 2-axis path (X = roller
// inductor, Y = vacuum cap per docs/HW-T41-PINMAP.md §7).
//
// Drives the carrier's X- and Y-axis STEP / DIR / EN through external stepper
// drivers (TMC2209 / DM542 / equivalent) → motors, plus monitors the two
// opto-isolated end-stop inputs (X LIMIT / Y LIMIT).
//
// Interactive serial menu over USB (115200 baud).  An "active axis" selector
// (X / Y) decides which axis the motion commands act on.  Both axes run
// independently — you can leave X spinning in continuous mode and start a
// jog on Y while X keeps running.
//
//   X / Y    Switch active axis (motion commands target the active axis)
//   1 / 2    Active axis: rotate CW / CCW 1 revolution
//   3 / 4    Active axis: rotate CW / CCW N revolutions (prompted)
//   5 / 6    Active axis: jog CW / CCW 100 steps (blocking)
//   7 / 8    Active axis: continuous CW / CCW (toggle, prints rev count)
//   9        Active axis: stop (decelerate)
//   0        Active axis: emergency stop (immediate)
//   S / A    Active axis: set speed / acceleration
//   T        Active axis: toggle ENA pin (test driver polarity)
//   Z        Active axis: set current position = home (0), save to EEPROM
//   E / D    Enable / disable ALL axes
//   Q        Toggle idle auto-release (global, persisted in EEPROM)
//   ?        Print status (both axes + limits)
//   H        Print menu
//
// Persistence (Teensy 4.1 emulated EEPROM, 4 KB, schema v2):
//   - Per-axis step position is saved on every move-stop and throttled (1 s)
//     during continuous mode, so a power-cycle has a near-current anchor.
//   - Idle auto-release preference survives reboots.
//   - Schema upgrade automatic — old v1 EEPROM contents are recognised and
//     re-initialised to defaults (position = 0 per axis, release = ON).
//
// Boot behaviour:
//   - Read saved positions from EEPROM
//   - Drive each axis from its saved position → 0 (boot-time homing)
//   - Save 0, continue to menu
//
// Limit switches:
//   - X LIMIT (Teensy pin 20) and Y LIMIT (Teensy pin 21), opto-isolated
//     inputs on the carrier.  Configured as INPUT_PULLUP.
//   - Active-LOW (opto conducting = switch closed = limit asserted).
//   - State changes are debounced (~15 ms) and printed to serial.
//   - Current state is shown in '?' status output.
//   - POC level: no automatic motion-refusal logic — operator-visible only.

#include <Arduino.h>
#include <AccelStepper.h>
#include <EEPROM.h>

// Pin map for the V2.09 carrier lives with the production tuner-
// controller HAL so bench and production share one source of truth.
// Bench's platformio.ini adds an -I onto firmware/tuner-controller/
// src/hal/board/ to make this include resolve.
#include "t41_v209.h"

// ── Pin assignments — V2.09 carrier (from docs/HW-T41-PINMAP.md) ────────
namespace board = hal::board::t41_v209;

static constexpr uint8_t PIN_X_STEP  = board::AXIS_X.step;
static constexpr uint8_t PIN_X_DIR   = board::AXIS_X.dir;
static constexpr uint8_t PIN_X_EN    = board::AXIS_X.en;
static constexpr uint8_t PIN_X_LIMIT = board::AXIS_X.limit;

static constexpr uint8_t PIN_Y_STEP  = board::AXIS_Y.step;
static constexpr uint8_t PIN_Y_DIR   = board::AXIS_Y.dir;
static constexpr uint8_t PIN_Y_EN    = board::AXIS_Y.en;
static constexpr uint8_t PIN_Y_LIMIT = board::AXIS_Y.limit;

// ── Defaults (match your driver's micro-stepping setting) ───────────────
static const int   STEPS_PER_REV = 1600;   // 1/8 micro-stepping on a 1.8° motor

// ── EEPROM layout (schema v2) ───────────────────────────────────────────
//   0..3   uint32_t magic    (=NVS_MAGIC when our v2 schema is written)
//   4..7    int32_t X position
//   8..11   int32_t Y position
//   12      uint8_t  idleAutoRelease (0/1)
static const int      EE_ADDR_MAGIC    = 0;
static const int      EE_ADDR_POS_X    = 4;
static const int      EE_ADDR_POS_Y    = 8;
static const int      EE_ADDR_RELEASE  = 12;
static const uint32_t NVS_MAGIC_V2     = 0x41544132UL;  // 'ATA2' — recognises v2 schema

// ── Per-axis state ──────────────────────────────────────────────────────

struct Axis {
    const char*   name;
    uint8_t       pin_step;
    uint8_t       pin_dir;
    uint8_t       pin_en;
    uint8_t       pin_limit;
    int           ee_pos_addr;
    AccelStepper  stepper;

    // Position-persistence state
    long          lastSavedPos    = 0;
    unsigned long lastContSaveMs  = 0;

    // Idle / motion tracking
    bool          driverReleased  = false;
    unsigned long lastMotionMs    = 0;
    bool          wasRunning      = false;

    // Continuous-mode state
    bool          continuousMode  = false;
    int           continuousDir   = 1;
    long          lastReportedRev = 0;

    // Per-axis motion settings (applied to the AccelStepper)
    float         currentSpeed    = 800.0f;
    float         currentAccel    = 400.0f;

    // Limit-switch debounced state (HIGH = released, LOW = asserted)
    int           limitLastState  = HIGH;

    Axis(const char* n, uint8_t s, uint8_t d, uint8_t e, uint8_t l, int ee_addr)
        : name(n), pin_step(s), pin_dir(d), pin_en(e), pin_limit(l),
          ee_pos_addr(ee_addr),
          stepper(AccelStepper::DRIVER, s, d) {}
};

static Axis xAxis("X", PIN_X_STEP, PIN_X_DIR, PIN_X_EN, PIN_X_LIMIT, EE_ADDR_POS_X);
static Axis yAxis("Y", PIN_Y_STEP, PIN_Y_DIR, PIN_Y_EN, PIN_Y_LIMIT, EE_ADDR_POS_Y);
static Axis* const axes[] = { &xAxis, &yAxis };
static const int NUM_AXES = sizeof(axes) / sizeof(axes[0]);
static Axis* selected = &xAxis;

// ── Global state ────────────────────────────────────────────────────────
static bool motorEnabled   = true;
static bool idleAutoRelease = true;
static const unsigned long IDLE_RELEASE_MS         = 3000;
static const unsigned long REENGAGE_SETTLE_MS      = 5;
static const unsigned long CONT_SAVE_INTERVAL_MS   = 1000;
static const unsigned long LIMIT_POLL_INTERVAL_MS  = 15;  // sampled debounce
static unsigned long       lastLimitPollMs         = 0;

// ── EEPROM helpers ──────────────────────────────────────────────────────

static void nvsInit() {
    uint32_t magic = 0;
    EEPROM.get(EE_ADDR_MAGIC, magic);
    if (magic == NVS_MAGIC_V2) return;

    int32_t zero = 0;
    uint8_t rel  = 1;
    EEPROM.put(EE_ADDR_POS_X, zero);
    EEPROM.put(EE_ADDR_POS_Y, zero);
    EEPROM.put(EE_ADDR_RELEASE, rel);
    EEPROM.put(EE_ADDR_MAGIC, NVS_MAGIC_V2);
    Serial.println(F("EEPROM: initialised fresh v2 schema (positions=0, release=ON)."));
}

static long nvsLoadPosition(int ee_addr) {
    int32_t p = 0;
    EEPROM.get(ee_addr, p);
    return (long)p;
}

static void nvsSavePosition(int ee_addr, long p) {
    int32_t v = (int32_t)p;
    EEPROM.put(ee_addr, v);
}

static bool nvsLoadRelease() {
    uint8_t v = 1;
    EEPROM.get(EE_ADDR_RELEASE, v);
    return v != 0;
}

static void nvsSaveRelease(bool on) {
    uint8_t v = on ? 1 : 0;
    EEPROM.put(EE_ADDR_RELEASE, v);
}

// ── Per-axis helpers ────────────────────────────────────────────────────

// Most external stepper drivers (TMC2209 ENN, DM542 ENA-) are active-LOW:
// LOW = driver enabled / coils energised.  If your driver disagrees, flip
// the polarity in enableAxisDriver() below — use the 'T' command first to
// verify against the actual hardware.

static void setEnaPin(Axis& a, bool level) {
    digitalWrite(a.pin_en, level ? HIGH : LOW);
    Serial.print(F("[")); Serial.print(a.name); Serial.print(F("] ENA = "));
    Serial.println(level ? F("HIGH") : F("LOW"));
}

// EN polarity comes from the carrier-board pin map. With opto-isolated
// drivers (TMC2209 / DM542 / TB6600) EN_ACTIVE_LOW = true, so the
// "enable" level on the Teensy pin is LOW. Centralised here so a
// future driver family with the opposite polarity is a one-line flip.
static constexpr uint8_t EN_LEVEL_ENABLED  = board::EN_ACTIVE_LOW ? LOW  : HIGH;
static constexpr uint8_t EN_LEVEL_DISABLED = board::EN_ACTIVE_LOW ? HIGH : LOW;

static void enableAxisDriver(Axis& a, bool on) {
    digitalWrite(a.pin_en, on ? EN_LEVEL_ENABLED : EN_LEVEL_DISABLED);
    a.driverReleased = false;
}

static void releaseDriverForIdle(Axis& a) {
    digitalWrite(a.pin_en, EN_LEVEL_DISABLED);
    a.driverReleased = true;
    Serial.print(F("[")); Serial.print(a.name);
    Serial.println(F("] idle — driver released (silent)"));
}

static void ensureDriverReady(Axis& a) {
    if (!motorEnabled || !a.driverReleased) return;
    digitalWrite(a.pin_en, EN_LEVEL_ENABLED);
    a.driverReleased = false;
    delay(REENGAGE_SETTLE_MS);
    Serial.print(F("[")); Serial.print(a.name);
    Serial.println(F("] driver re-engaged"));
}

static void enableAll(bool on) {
    motorEnabled = on;
    for (int i = 0; i < NUM_AXES; i++) enableAxisDriver(*axes[i], on);
    Serial.print(F("Motors "));
    Serial.println(on ? F("ENABLED") : F("DISABLED"));
}

static void savePositionIfChanged(Axis& a, bool quiet = false) {
    long p = a.stepper.currentPosition();
    if (p == a.lastSavedPos) return;
    nvsSavePosition(a.ee_pos_addr, p);
    a.lastSavedPos = p;
    if (!quiet) {
        Serial.print(F("[")); Serial.print(a.name);
        Serial.print(F("] pos saved: ")); Serial.println(p);
    }
}

static void moveRevolutions(Axis& a, int n, int dir) {
    if (!motorEnabled) {
        Serial.println(F("Motors disabled — enable first (E)."));
        return;
    }
    ensureDriverReady(a);
    long target = a.stepper.currentPosition() + (long)dir * n * STEPS_PER_REV;
    Serial.print(F("[")); Serial.print(a.name);
    Serial.print(F("] moving to ")); Serial.print(target); Serial.println(F(" steps …"));
    a.stepper.moveTo(target);
}

static void homeAxisOnBoot(Axis& a) {
    long saved = a.stepper.currentPosition();
    if (saved == 0) {
        Serial.print(F("[")); Serial.print(a.name);
        Serial.println(F("] already at home (0)."));
        return;
    }
    Serial.print(F("[")); Serial.print(a.name);
    Serial.print(F("] homing: driving from "));
    Serial.print(saved); Serial.println(F(" steps → 0 …"));
    ensureDriverReady(a);
    a.stepper.moveTo(0);
    a.stepper.runToPosition();   // blocking
    savePositionIfChanged(a);
    Serial.print(F("[")); Serial.print(a.name);
    Serial.println(F("] home reached (position = 0)."));
}

// ── Limit-switch monitor ────────────────────────────────────────────────

static void pollLimits() {
    unsigned long now = millis();
    if (now - lastLimitPollMs < LIMIT_POLL_INTERVAL_MS) return;
    lastLimitPollMs = now;
    for (int i = 0; i < NUM_AXES; i++) {
        Axis& a = *axes[i];
        int cur = digitalRead(a.pin_limit);
        if (cur == a.limitLastState) continue;
        a.limitLastState = cur;
        Serial.print(F("[limit ")); Serial.print(a.name); Serial.print(F("] "));
        Serial.println(cur == LOW ? F("ASSERTED") : F("released"));
    }
}

// ── Per-axis service (call once per loop) ───────────────────────────────

static void serviceAxis(Axis& a) {
    if (a.continuousMode) {
        a.stepper.setSpeed(a.continuousDir > 0 ? a.currentSpeed : -a.currentSpeed);
        a.stepper.runSpeed();
        long rev = a.stepper.currentPosition() / STEPS_PER_REV;
        if (rev != a.lastReportedRev) {
            a.lastReportedRev = rev;
            Serial.print(F("[")); Serial.print(a.name);
            Serial.print(F("] Rev "));
            if (rev > 0) Serial.print('+');
            Serial.print(rev);
            Serial.print(F("  (")); Serial.print(a.stepper.currentPosition());
            Serial.println(F(" steps)"));
        }
        unsigned long now = millis();
        if (now - a.lastContSaveMs >= CONT_SAVE_INTERVAL_MS) {
            a.lastContSaveMs = now;
            savePositionIfChanged(a, true /*quiet*/);
        }
    } else {
        a.stepper.run();
    }

    bool nowRunning = a.continuousMode || a.stepper.distanceToGo() != 0;
    if (a.wasRunning && !nowRunning) {
        savePositionIfChanged(a);
        a.lastMotionMs = millis();
    }
    a.wasRunning = nowRunning;

    if (idleAutoRelease && !a.driverReleased && !nowRunning && motorEnabled
        && (millis() - a.lastMotionMs >= IDLE_RELEASE_MS)) {
        releaseDriverForIdle(a);
    }
}

// ── Status / menu ───────────────────────────────────────────────────────

static void printStatus() {
    Serial.println(F("──────────────────────────────────"));
    Serial.print(F("Active axis   : ")); Serial.println(selected->name);
    Serial.print(F("Steps/rev     : ")); Serial.println(STEPS_PER_REV);
    Serial.print(F("Motors enabled: ")); Serial.println(motorEnabled    ? F("YES") : F("NO"));
    Serial.print(F("Idle release  : ")); Serial.println(idleAutoRelease ? F("ON")  : F("OFF"));
    for (int i = 0; i < NUM_AXES; i++) {
        Axis& a = *axes[i];
        Serial.print(F("[")); Serial.print(a.name); Serial.println(F("]"));
        Serial.print(F("  position    : ")); Serial.print(a.stepper.currentPosition()); Serial.println(F(" steps"));
        Serial.print(F("  speed       : ")); Serial.print(a.currentSpeed, 0); Serial.println(F(" steps/s"));
        Serial.print(F("  accel       : ")); Serial.print(a.currentAccel, 0); Serial.println(F(" steps/s²"));
        Serial.print(F("  continuous  : "));
        if (a.continuousMode) Serial.println(a.continuousDir > 0 ? F("CW") : F("CCW"));
        else                  Serial.println(F("OFF"));
        Serial.print(F("  limit input : "));
        Serial.println(a.limitLastState == LOW ? F("ASSERTED (LOW)") : F("released (HIGH)"));
    }
    Serial.println(F("──────────────────────────────────"));
}

static void printMenu() {
    Serial.println();
    Serial.println(F("=== T41 X/Y Stepper Test Menu ==="));
    Serial.print  (F("(active axis: ")); Serial.print(selected->name); Serial.println(F(")"));
    Serial.println(F("  X / Y  Switch active axis"));
    Serial.println(F("  1 / 2  Rotate CW / CCW 1 revolution"));
    Serial.println(F("  3 / 4  Rotate CW / CCW N revolutions (prompted)"));
    Serial.println(F("  5 / 6  Jog CW / CCW 100 steps (blocking)"));
    Serial.println(F("  7 / 8  Continuous CW / CCW (toggle)"));
    Serial.println(F("  9      Stop (decelerate)"));
    Serial.println(F("  0      EMERGENCY STOP (immediate)"));
    Serial.println(F("  S / A  Set speed / acceleration"));
    Serial.println(F("  T      Toggle ENA pin (test driver polarity)"));
    Serial.println(F("  Z      Set position = home (0), save to EEPROM"));
    Serial.println(F("  E / D  Enable / disable all motors"));
    Serial.println(F("  Q      Toggle idle auto-release (global, persisted)"));
    Serial.println(F("  ?      Print status"));
    Serial.println(F("  H      Show this menu"));
    Serial.println();
}

static int readIntFromSerial() {
    Serial.print(F("Enter value: "));
    while (!Serial.available()) { /* wait */ }
    int val = Serial.parseInt();
    while (Serial.available()) Serial.read();
    Serial.println(val);
    return val;
}

static float readFloatFromSerial() {
    Serial.print(F("Enter value: "));
    while (!Serial.available()) { /* wait */ }
    float val = Serial.parseFloat();
    while (Serial.available()) Serial.read();
    Serial.println(val, 1);
    return val;
}

// ── setup / loop ────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);   // Teensy 4.1 USB CDC: let the host enumerate

    nvsInit();
    idleAutoRelease = nvsLoadRelease();

    for (int i = 0; i < NUM_AXES; i++) {
        Axis& a = *axes[i];
        pinMode(a.pin_en, OUTPUT);
        pinMode(a.pin_limit, INPUT_PULLUP);
        a.limitLastState = digitalRead(a.pin_limit);

        a.stepper.setMinPulseWidth(board::STEP_MIN_PULSE_US);
        a.stepper.setMaxSpeed(a.currentSpeed);
        a.stepper.setAcceleration(a.currentAccel);

        long savedPos = nvsLoadPosition(a.ee_pos_addr);
        a.stepper.setCurrentPosition(savedPos);
        a.lastSavedPos = savedPos;
    }
    enableAll(true);

    Serial.println();
    Serial.println(F("Teensy 4.1 / T41 V2.09 — X+Y stepper test"));
    Serial.print(F("X: STEP=")); Serial.print(PIN_X_STEP);
    Serial.print(F("  DIR="));   Serial.print(PIN_X_DIR);
    Serial.print(F("  EN="));    Serial.print(PIN_X_EN);
    Serial.print(F("  LIMIT=")); Serial.println(PIN_X_LIMIT);
    Serial.print(F("Y: STEP=")); Serial.print(PIN_Y_STEP);
    Serial.print(F("  DIR="));   Serial.print(PIN_Y_DIR);
    Serial.print(F("  EN="));    Serial.print(PIN_Y_EN);
    Serial.print(F("  LIMIT=")); Serial.println(PIN_Y_LIMIT);
    Serial.print(F("Steps/rev: ")); Serial.println(STEPS_PER_REV);
    Serial.print(F("Idle auto-release: "));
    Serial.println(idleAutoRelease ? F("ON  (silent at rest)") : F("OFF (motor holds with current)"));

    for (int i = 0; i < NUM_AXES; i++) homeAxisOnBoot(*axes[i]);

    printMenu();
}

void loop() {
    // Service both axes every loop iteration.
    for (int i = 0; i < NUM_AXES; i++) serviceAxis(*axes[i]);
    pollLimits();

    if (!Serial.available()) return;

    char cmd = Serial.read();
    delay(10);
    while (Serial.available() && (Serial.peek() == '\n' || Serial.peek() == '\r'))
        Serial.read();

    Axis& a = *selected;

    switch (cmd) {
    case 'X': case 'x':
        selected = &xAxis;
        Serial.println(F("Active axis: X"));
        break;
    case 'Y': case 'y':
        selected = &yAxis;
        Serial.println(F("Active axis: Y"));
        break;

    case '1': a.continuousMode = false; moveRevolutions(a, 1, +1); break;
    case '2': a.continuousMode = false; moveRevolutions(a, 1, -1); break;
    case '3': {
        a.continuousMode = false;
        int n = readIntFromSerial();
        if (n > 0) moveRevolutions(a, n, +1);
        break;
    }
    case '4': {
        a.continuousMode = false;
        int n = readIntFromSerial();
        if (n > 0) moveRevolutions(a, n, -1);
        break;
    }
    case '5':
        a.continuousMode = false;
        if (!motorEnabled) { Serial.println(F("Motors disabled — enable first (E).")); break; }
        ensureDriverReady(a);
        Serial.print(F("[")); Serial.print(a.name); Serial.println(F("] jog CW +100 steps"));
        a.stepper.move(100);
        a.stepper.runToPosition();
        savePositionIfChanged(a);
        a.lastMotionMs = millis();
        a.wasRunning = false;
        break;
    case '6':
        a.continuousMode = false;
        if (!motorEnabled) { Serial.println(F("Motors disabled — enable first (E).")); break; }
        ensureDriverReady(a);
        Serial.print(F("[")); Serial.print(a.name); Serial.println(F("] jog CCW -100 steps"));
        a.stepper.move(-100);
        a.stepper.runToPosition();
        savePositionIfChanged(a);
        a.lastMotionMs = millis();
        a.wasRunning = false;
        break;
    case '7':
        if (a.continuousMode && a.continuousDir > 0) {
            a.continuousMode = false;
            Serial.print(F("[")); Serial.print(a.name); Serial.println(F("] continuous CW stopped."));
        } else {
            ensureDriverReady(a);
            a.continuousMode = true;
            a.continuousDir = +1;
            a.lastReportedRev = a.stepper.currentPosition() / STEPS_PER_REV;
            a.lastContSaveMs  = millis();
            Serial.print(F("[")); Serial.print(a.name); Serial.println(F("] continuous CW started."));
        }
        break;
    case '8':
        if (a.continuousMode && a.continuousDir < 0) {
            a.continuousMode = false;
            Serial.print(F("[")); Serial.print(a.name); Serial.println(F("] continuous CCW stopped."));
        } else {
            ensureDriverReady(a);
            a.continuousMode = true;
            a.continuousDir = -1;
            a.lastReportedRev = a.stepper.currentPosition() / STEPS_PER_REV;
            a.lastContSaveMs  = millis();
            Serial.print(F("[")); Serial.print(a.name); Serial.println(F("] continuous CCW started."));
        }
        break;
    case '9':
        a.continuousMode = false;
        a.stepper.stop();
        Serial.print(F("[")); Serial.print(a.name); Serial.println(F("] STOP — decelerating."));
        break;
    case '0':
        a.continuousMode = false;
        a.stepper.setCurrentPosition(a.stepper.currentPosition());   // zero velocity
        Serial.print(F("[")); Serial.print(a.name); Serial.println(F("] EMERGENCY STOP."));
        break;

    case 'S': case 's': {
        float spd = readFloatFromSerial();
        if (spd > 0) {
            a.currentSpeed = spd;
            a.stepper.setMaxSpeed(a.currentSpeed);
            Serial.print(F("[")); Serial.print(a.name);
            Serial.print(F("] speed = ")); Serial.print(a.currentSpeed, 0); Serial.println(F(" steps/s"));
        }
        break;
    }
    case 'A': case 'a': {
        float acc = readFloatFromSerial();
        if (acc > 0) {
            a.currentAccel = acc;
            a.stepper.setAcceleration(a.currentAccel);
            Serial.print(F("[")); Serial.print(a.name);
            Serial.print(F("] accel = ")); Serial.print(a.currentAccel, 0); Serial.println(F(" steps/s²"));
        }
        break;
    }
    case 'E': case 'e': enableAll(true);  break;
    case 'D': case 'd': enableAll(false); break;
    case 'T': case 't': {
        int current = digitalRead(a.pin_en);
        setEnaPin(a, !current);
        Serial.println(F("(Use T to toggle, check if shaft locks/unlocks)"));
        break;
    }
    case 'Q': case 'q':
        idleAutoRelease = !idleAutoRelease;
        nvsSaveRelease(idleAutoRelease);
        Serial.print(F("Idle auto-release: "));
        Serial.println(idleAutoRelease ? F("ON (silent at rest, no holding torque)")
                                       : F("OFF (motors hold position with current)"));
        Serial.println(F("(setting saved — persists across reboots)"));
        if (!idleAutoRelease) {
            for (int i = 0; i < NUM_AXES; i++) ensureDriverReady(*axes[i]);
        }
        break;
    case 'Z': case 'z':
        a.stepper.setCurrentPosition(0);
        savePositionIfChanged(a);
        Serial.print(F("[")); Serial.print(a.name); Serial.println(F("] position zeroed (declared home)."));
        break;

    case '?':           printStatus(); break;
    case 'H': case 'h': printMenu();   break;
    default: break;
    }
}
