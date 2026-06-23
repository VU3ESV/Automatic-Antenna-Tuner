// X-axis stepper bench-test for Teensy 4.1 in the grblHAL-teensy-4.x V2.09
// carrier (Phil Barrett).
//
// Drives the carrier's X-axis STEP / DIR / EN through an external stepper
// driver (TMC2209 / DM542 / equivalent) → motor.  Feature parity with the
// Arduino-Mega and ESP32-C6 bench-tests minus the WiFi-only features
// (captive portal, web-OTA) — Teensy uploads via Teensy Loader over USB.
//
// Interactive serial menu over USB (115200 baud):
//   1/2  Rotate CW/CCW 1 revolution
//   3/4  Rotate CW/CCW N revolutions (prompted)
//   5/6  Jog CW/CCW 100 steps (blocking)
//   7/8  Continuous CW/CCW (toggle, prints rev count, saves position)
//   9    Stop (decelerate to halt)
//   0    Emergency stop (immediate, no decel)
//   S/A  Set speed / acceleration (steps/s, steps/s²)
//   E/D  Enable / disable motor
//   T    Toggle ENA pin (test driver polarity)
//   Q    Toggle idle auto-release (persisted in EEPROM)
//   Z    Set current position = home (0), save to EEPROM
//   ?    Print status
//   H    Print menu
//
// Persistence (Teensy 4.1 emulated EEPROM, 4 KB):
//   - Step position is saved on every move-stop and throttled (1 s) during
//     continuous mode, so a power-cycle has a near-current anchor to home
//     back to on the next boot.
//   - Idle auto-release preference survives reboots.
//
// Boot behaviour:
//   - Read saved position from EEPROM
//   - Drive motor from saved position → 0 (boot-time homing, home = 0)
//   - Save 0, continue to menu
//
// PIN-MAP TODO ────────────────────────────────────────────────────────────
// Fill PIN_X_STEP / PIN_X_DIR / PIN_X_EN below with the Teensy 4.1 pin
// numbers wired to the X-axis stepper outputs on the V2.09 carrier.
// Source: V2.07 schematic PDF in the upstream repo (link in
// docs/HW-T41-CARRIER.md References) — V2.07 pin map applies to V2.09.
// A build-time guard refuses to compile until non-zero values are set.

#include <Arduino.h>
#include <AccelStepper.h>
#include <EEPROM.h>

// ── X-axis pin assignments (from upstream T41U5XBB_map.h, factual) ──────
// Cross-checked against grblHAL/iMXRT1062 master, file
// grblHAL_Teensy4/src/boards/T41U5XBB_map.h.  Pin numbers are facts about
// the board net-to-Teensy-pin mapping and apply to V2.07 / V2.08 / V2.09
// (all electrically equivalent per the upstream README).
static const uint8_t PIN_X_STEP = 2;   // Teensy 4.1 pin 2  → X STEP
static const uint8_t PIN_X_DIR  = 3;   // Teensy 4.1 pin 3  → X DIR
static const uint8_t PIN_X_EN   = 10;  // Teensy 4.1 pin 10 → X ENABLE (active-LOW)

// ── Defaults (match your driver's micro-stepping DIP/UART setting) ──────
static const int   STEPS_PER_REV   = 1600;   // 1/8 micro-stepping on a 1.8° motor
static const float DEFAULT_SPEED   = 800.0;  // steps/sec
static const float DEFAULT_ACCEL   = 400.0;  // steps/sec²

// ── EEPROM layout (Teensy 4.1 has 4 KB emulated EEPROM) ─────────────────
//   0..3  uint32_t magic   (=NVS_MAGIC when our schema is written)
//   4..7   int32_t position (last-known step count)
//   8..8     uint8_t  idleAutoRelease (0/1)
static const int EE_ADDR_MAGIC   = 0;
static const int EE_ADDR_POS     = 4;
static const int EE_ADDR_RELEASE = 8;
static const uint32_t NVS_MAGIC  = 0x41545554UL;  // 'TUTA' — recognises our schema

// ── AccelStepper (driver interface = pulse+dir, type 1) ─────────────────
AccelStepper stepper(AccelStepper::DRIVER, PIN_X_STEP, PIN_X_DIR);

// ── State ───────────────────────────────────────────────────────────────
static float currentSpeed    = DEFAULT_SPEED;
static float currentAccel    = DEFAULT_ACCEL;
static bool  motorEnabled    = true;
static bool  continuousMode  = false;
static int   continuousDir   = 1;          // +1 CW, -1 CCW
static long  lastSavedPos    = 0;
static bool  wasRunning      = false;
static long  lastReportedRev = 0;
static unsigned long lastContSaveMs       = 0;
static const unsigned long CONT_SAVE_INTERVAL_MS = 1000;

// Idle auto-release: after this much idle time, raise ENA to silence the
// driver chopper PWM.  Persisted across reboots in EEPROM.
static bool          idleAutoRelease   = true;
static bool          driverReleased    = false;
static unsigned long lastMotionMs      = 0;
static const unsigned long IDLE_RELEASE_MS    = 3000;
static const unsigned long REENGAGE_SETTLE_MS = 5;

// ── EEPROM helpers ──────────────────────────────────────────────────────

static void nvsInit() {
    uint32_t magic = 0;
    EEPROM.get(EE_ADDR_MAGIC, magic);
    if (magic == NVS_MAGIC) return;

    // Fresh chip / different schema — write defaults.
    int32_t pos = 0;
    uint8_t rel = 1;
    EEPROM.put(EE_ADDR_POS, pos);
    EEPROM.put(EE_ADDR_RELEASE, rel);
    EEPROM.put(EE_ADDR_MAGIC, NVS_MAGIC);
    Serial.println(F("EEPROM: initialised fresh schema (position=0, release=ON)."));
}

static long nvsLoadPosition() {
    int32_t p = 0;
    EEPROM.get(EE_ADDR_POS, p);
    return (long)p;
}

static void nvsSavePosition(long p) {
    int32_t v = (int32_t)p;
    EEPROM.put(EE_ADDR_POS, v);
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

// ── Helpers ─────────────────────────────────────────────────────────────

static void setEnaPin(bool level) {
    digitalWrite(PIN_X_EN, level ? HIGH : LOW);
    Serial.print(F("ENA pin = "));
    Serial.println(level ? F("HIGH") : F("LOW"));
}

// Most external stepper drivers (TMC2209 ENN, DM542 ENA-) are active-LOW:
// LOW = driver enabled / coils energised.  If your driver disagrees, flip
// the polarity of setEnaPin() below — use the 'T' command first to verify.
static void enableMotor(bool on) {
    motorEnabled = on;
    setEnaPin(!on);            // active-LOW
    driverReleased = false;    // explicit user command overrides idle release
    Serial.print(F("Motor "));
    Serial.println(on ? F("ENABLED") : F("DISABLED"));
}

static void releaseDriverForIdle() {
    digitalWrite(PIN_X_EN, HIGH);   // disable driver
    driverReleased = true;
    Serial.println(F("[idle] driver released (silent)"));
}

static void ensureDriverReady() {
    if (!motorEnabled || !driverReleased) return;
    digitalWrite(PIN_X_EN, LOW);
    driverReleased = false;
    delay(REENGAGE_SETTLE_MS);
    Serial.println(F("[idle] driver re-engaged"));
}

static void savePositionIfChanged(bool quiet = false) {
    long p = stepper.currentPosition();
    if (p == lastSavedPos) return;
    nvsSavePosition(p);
    lastSavedPos = p;
    if (!quiet) {
        Serial.print(F("[pos] saved: "));
        Serial.println(p);
    }
}

static void homeOnBoot() {
    long saved = stepper.currentPosition();
    if (saved == 0) {
        Serial.println(F("Already at home (0).  No homing motion needed."));
        return;
    }
    Serial.print(F("Homing: driving from "));
    Serial.print(saved);
    Serial.println(F(" steps → 0 …"));
    ensureDriverReady();
    stepper.moveTo(0);
    stepper.runToPosition();    // blocking
    savePositionIfChanged();
    Serial.println(F("Home reached (position = 0)."));
}

static void printStatus() {
    Serial.println(F("──────────────────────────────────"));
    Serial.print(F("Position      : ")); Serial.print(stepper.currentPosition()); Serial.println(F(" steps"));
    Serial.print(F("Speed setting : ")); Serial.print(currentSpeed, 0);          Serial.println(F(" steps/s"));
    Serial.print(F("Accel setting : ")); Serial.print(currentAccel, 0);          Serial.println(F(" steps/s²"));
    Serial.print(F("Steps/rev     : ")); Serial.println(STEPS_PER_REV);
    Serial.print(F("Motor enabled : ")); Serial.println(motorEnabled    ? F("YES") : F("NO"));
    Serial.print(F("Idle release  : ")); Serial.println(idleAutoRelease ? F("ON")  : F("OFF"));
    Serial.print(F("Continuous    : "));
    if (continuousMode) Serial.println(continuousDir > 0 ? F("CW") : F("CCW"));
    else                Serial.println(F("OFF"));
    Serial.println(F("──────────────────────────────────"));
}

static void printMenu() {
    Serial.println();
    Serial.println(F("=== T41 X-axis Stepper Test Menu ==="));
    Serial.println(F("  1  Rotate CW  1 revolution"));
    Serial.println(F("  2  Rotate CCW 1 revolution"));
    Serial.println(F("  3  Rotate CW  N revolutions (prompts)"));
    Serial.println(F("  4  Rotate CCW N revolutions (prompts)"));
    Serial.println(F("  5  Jog CW  +100 steps (blocking)"));
    Serial.println(F("  6  Jog CCW -100 steps (blocking)"));
    Serial.println(F("  7  Continuous CW  (toggle)"));
    Serial.println(F("  8  Continuous CCW (toggle)"));
    Serial.println(F("  9  Stop (decelerate)"));
    Serial.println(F("  0  EMERGENCY STOP (immediate)"));
    Serial.println(F("  S  Set speed (steps/s)"));
    Serial.println(F("  A  Set accel (steps/s²)"));
    Serial.println(F("  E  Enable motor"));
    Serial.println(F("  D  Disable motor"));
    Serial.println(F("  T  Toggle ENA pin (test driver polarity)"));
    Serial.println(F("  Q  Toggle idle auto-release (persisted)"));
    Serial.println(F("  Z  Set current position = home (0), save to EEPROM"));
    Serial.println(F("  ?  Print status"));
    Serial.println(F("  H  Show this menu"));
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

static void moveRevolutions(int n, int dir) {
    if (!motorEnabled) {
        Serial.println(F("Motor disabled — enable first (E)."));
        return;
    }
    ensureDriverReady();
    long target = stepper.currentPosition() + (long)dir * n * STEPS_PER_REV;
    Serial.print(F("Moving to "));
    Serial.print(target);
    Serial.println(F(" steps …"));
    stepper.moveTo(target);
}

// ── setup / loop ────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    // Teensy 4.1 USB CDC: brief settle so the first prints aren't lost if
    // a monitor was already attached; don't block on !Serial — that hangs
    // until a host opens the port.
    delay(500);

    pinMode(PIN_X_EN, OUTPUT);
    enableMotor(true);

    stepper.setMinPulseWidth(5);
    stepper.setMaxSpeed(currentSpeed);
    stepper.setAcceleration(currentAccel);

    nvsInit();
    long savedPos = nvsLoadPosition();
    stepper.setCurrentPosition(savedPos);
    lastSavedPos = savedPos;
    idleAutoRelease = nvsLoadRelease();

    Serial.println();
    Serial.println(F("Teensy 4.1 / T41 V2.09 — X-axis stepper test"));
    Serial.print(F("Pins: STEP=")); Serial.print(PIN_X_STEP);
    Serial.print(F("  DIR="));      Serial.print(PIN_X_DIR);
    Serial.print(F("  EN="));       Serial.println(PIN_X_EN);
    Serial.print(F("Steps/rev: "));        Serial.println(STEPS_PER_REV);
    Serial.print(F("Restored position: ")); Serial.print(savedPos); Serial.println(F(" steps (home = 0)"));
    Serial.print(F("Idle auto-release: "));
    Serial.println(idleAutoRelease ? F("ON  (silent at rest)") : F("OFF (motor holds with current)"));

    homeOnBoot();
    printMenu();
}

void loop() {
    if (continuousMode) {
        stepper.setSpeed(continuousDir > 0 ? currentSpeed : -currentSpeed);
        stepper.runSpeed();
        // Print revolution boundary
        long rev = stepper.currentPosition() / STEPS_PER_REV;
        if (rev != lastReportedRev) {
            lastReportedRev = rev;
            Serial.print(F("[Rev] "));
            if (rev > 0) Serial.print('+');
            Serial.print(rev);
            Serial.print(F("  ("));
            Serial.print(stepper.currentPosition());
            Serial.println(F(" steps)"));
        }
        // Throttled position save during continuous mode
        unsigned long now = millis();
        if (now - lastContSaveMs >= CONT_SAVE_INTERVAL_MS) {
            lastContSaveMs = now;
            savePositionIfChanged(true /*quiet*/);
        }
    } else {
        stepper.run();
    }

    // Detect running → idle edge
    bool nowRunning = continuousMode || stepper.distanceToGo() != 0;
    if (wasRunning && !nowRunning) {
        savePositionIfChanged();
        lastMotionMs = millis();
    }
    wasRunning = nowRunning;

    // Idle auto-release
    if (idleAutoRelease && !driverReleased && !nowRunning && motorEnabled
        && (millis() - lastMotionMs >= IDLE_RELEASE_MS)) {
        releaseDriverForIdle();
    }

    if (!Serial.available()) return;

    char cmd = Serial.read();
    delay(10);
    while (Serial.available() && (Serial.peek() == '\n' || Serial.peek() == '\r'))
        Serial.read();

    switch (cmd) {
    case '1': continuousMode = false; moveRevolutions(1, +1); break;
    case '2': continuousMode = false; moveRevolutions(1, -1); break;
    case '3': {
        continuousMode = false;
        int n = readIntFromSerial();
        if (n > 0) moveRevolutions(n, +1);
        break;
    }
    case '4': {
        continuousMode = false;
        int n = readIntFromSerial();
        if (n > 0) moveRevolutions(n, -1);
        break;
    }
    case '5':
        continuousMode = false;
        if (!motorEnabled) { Serial.println(F("Motor disabled — enable first (E).")); break; }
        ensureDriverReady();
        Serial.println(F("Jog CW +100 steps"));
        stepper.move(100);
        stepper.runToPosition();
        savePositionIfChanged();
        lastMotionMs = millis();
        wasRunning = false;
        break;
    case '6':
        continuousMode = false;
        if (!motorEnabled) { Serial.println(F("Motor disabled — enable first (E).")); break; }
        ensureDriverReady();
        Serial.println(F("Jog CCW -100 steps"));
        stepper.move(-100);
        stepper.runToPosition();
        savePositionIfChanged();
        lastMotionMs = millis();
        wasRunning = false;
        break;
    case '7':
        if (continuousMode && continuousDir > 0) {
            continuousMode = false;
            Serial.println(F("Continuous CW stopped."));
        } else {
            ensureDriverReady();
            continuousMode = true;
            continuousDir = +1;
            lastReportedRev = stepper.currentPosition() / STEPS_PER_REV;
            lastContSaveMs  = millis();
            Serial.println(F("Continuous CW started."));
        }
        break;
    case '8':
        if (continuousMode && continuousDir < 0) {
            continuousMode = false;
            Serial.println(F("Continuous CCW stopped."));
        } else {
            ensureDriverReady();
            continuousMode = true;
            continuousDir = -1;
            lastReportedRev = stepper.currentPosition() / STEPS_PER_REV;
            lastContSaveMs  = millis();
            Serial.println(F("Continuous CCW started."));
        }
        break;
    case '9':
        continuousMode = false;
        stepper.stop();
        Serial.println(F("STOP — decelerating to halt."));
        break;
    case '0':
        continuousMode = false;
        stepper.setCurrentPosition(stepper.currentPosition());  // zero velocity
        Serial.println(F("EMERGENCY STOP — immediate halt."));
        break;
    case 'S': case 's': {
        float spd = readFloatFromSerial();
        if (spd > 0) {
            currentSpeed = spd;
            stepper.setMaxSpeed(currentSpeed);
            Serial.print(F("Speed set to ")); Serial.print(currentSpeed, 0); Serial.println(F(" steps/s"));
        }
        break;
    }
    case 'A': case 'a': {
        float acc = readFloatFromSerial();
        if (acc > 0) {
            currentAccel = acc;
            stepper.setAcceleration(currentAccel);
            Serial.print(F("Accel set to ")); Serial.print(currentAccel, 0); Serial.println(F(" steps/s²"));
        }
        break;
    }
    case 'E': case 'e': enableMotor(true);  break;
    case 'D': case 'd': enableMotor(false); break;
    case 'T': case 't': {
        bool current = digitalRead(PIN_X_EN);
        setEnaPin(!current);
        Serial.println(F("(Use T to toggle, check if shaft locks/unlocks)"));
        break;
    }
    case 'Q': case 'q':
        idleAutoRelease = !idleAutoRelease;
        nvsSaveRelease(idleAutoRelease);
        Serial.print(F("Idle auto-release: "));
        Serial.println(idleAutoRelease ? F("ON (silent at rest, no holding torque)")
                                       : F("OFF (motor holds position with current)"));
        Serial.println(F("(setting saved — persists across reboots)"));
        if (!idleAutoRelease && driverReleased) ensureDriverReady();
        break;
    case 'Z': case 'z':
        stepper.setCurrentPosition(0);
        savePositionIfChanged();
        Serial.println(F("Position zeroed (declared home)."));
        break;
    case '?':           printStatus(); break;
    case 'H': case 'h': printMenu();   break;
    default: break;
    }
}
