// TB6600 + NEMA 23 stepper-motor test bench for Arduino Mega 2560 R3.
//
// Interactive serial menu (115200 baud) lets you exercise direction,
// speed, revolutions, acceleration, jog, and enable/disable without
// re-flashing.
//
// TB6600 wiring (common-cathode: signal− to GND, signal+ to GPIO):
//
//   TB6600 pin  │  Arduino Mega R3 pin  │  Notes
//   ────────────┼───────────────────────┼───────────────────────────
//   ENA+        │  D8  (DIGITAL)        │  HIGH = enabled
//   ENA-        │  GND                  │
//   DIR+        │  D9  (DIGITAL)        │  HIGH/LOW = CW/CCW
//   DIR-        │  GND                  │
//   PUL+        │  D10 (DIGITAL)        │  Step pulse
//   PUL-        │  GND                  │
//   VCC / GND   │  24–48 VDC PSU        │  Match motor voltage rating
//   A+/A-/B+/B- │  NEMA 23 coils        │
//
// TB6600 DIP-switch micro-stepping reference:
//   S1   S2   S3   │  Micro-step
//   ON   ON   OFF  │  Full step   (200 steps/rev for 1.8° motor)
//   ON   OFF  ON   │  1/2  step   (400)
//   OFF  ON   ON   │  1/2  step   (400)
//   ON   OFF  OFF  │  1/4  step   (800)
//   OFF  ON   OFF  │  1/8  step   (1600)  ← default below
//   OFF  OFF  ON   │  1/16 step   (3200)
//   OFF  OFF  OFF  │  1/32 step   (6400)

#include <Arduino.h>
#include <AccelStepper.h>

// ── Pin assignments (Mega R3 DIGITAL header, PWM-capable) ───────────
static const uint8_t PIN_ENA  = 8;   // D8
static const uint8_t PIN_DIR  = 9;   // D9
static const uint8_t PIN_STEP = 10;  // D10

// ── Defaults (match your DIP-switch setting) ────────────────────────
static const int STEPS_PER_REV   = 1600;   // 1/8 micro-stepping
static const float DEFAULT_SPEED = 800.0;  // steps/sec (≈ 0.5 rev/s)
static const float DEFAULT_ACCEL = 400.0;  // steps/sec²

// ── AccelStepper (driver interface = pulse+dir, type 1) ─────────────
AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);

// ── State ───────────────────────────────────────────────────────────
static float currentSpeed = DEFAULT_SPEED;
static float currentAccel = DEFAULT_ACCEL;
static bool  motorEnabled = true;
static bool  continuousMode = false;
static int   continuousDir  = 1;   // +1 CW, -1 CCW

// ── Helpers ─────────────────────────────────────────────────────────

static void setEnaPin(bool level) {
    digitalWrite(PIN_ENA, level ? HIGH : LOW);
    Serial.print(F("ENA+ pin = "));
    Serial.println(level ? F("HIGH") : F("LOW"));
}

static void enableMotor(bool on) {
    motorEnabled = on;
    setEnaPin(!on); // LOW = driver enabled, HIGH = driver disabled
    Serial.print(F("Motor "));
    Serial.println(on ? F("ENABLED") : F("DISABLED"));
}

static void printStatus() {
    Serial.println(F("──────────────────────────────────"));
    Serial.print(F("Position      : ")); Serial.print(stepper.currentPosition());
    Serial.println(F(" steps"));
    Serial.print(F("Speed setting : ")); Serial.print(currentSpeed, 0);
    Serial.println(F(" steps/s"));
    Serial.print(F("Accel setting : ")); Serial.print(currentAccel, 0);
    Serial.println(F(" steps/s²"));
    Serial.print(F("Steps/rev     : ")); Serial.println(STEPS_PER_REV);
    Serial.print(F("Motor enabled : ")); Serial.println(motorEnabled ? F("YES") : F("NO"));
    Serial.print(F("Continuous    : "));
    if (continuousMode)
        Serial.println(continuousDir > 0 ? F("CW") : F("CCW"));
    else
        Serial.println(F("OFF"));
    Serial.println(F("──────────────────────────────────"));
}

static void printMenu() {
    Serial.println();
    Serial.println(F("=== TB6600 Stepper Test Menu ==="));
    Serial.println(F("  1  Rotate CW  1 revolution"));
    Serial.println(F("  2  Rotate CCW 1 revolution"));
    Serial.println(F("  3  Rotate CW  N revolutions  (prompts for N)"));
    Serial.println(F("  4  Rotate CCW N revolutions  (prompts for N)"));
    Serial.println(F("  5  Jog CW  100 steps"));
    Serial.println(F("  6  Jog CCW 100 steps"));
    Serial.println(F("  7  Continuous CW  (toggle)"));
    Serial.println(F("  8  Continuous CCW (toggle)"));
    Serial.println(F("  9  Stop (decelerate to halt)"));
    Serial.println(F("  0  EMERGENCY STOP (immediate, no decel)"));
    Serial.println(F("  S  Set speed   (steps/s)"));
    Serial.println(F("  A  Set accel   (steps/s²)"));
    Serial.println(F("  E  Enable motor  (ENA+ LOW)"));
    Serial.println(F("  D  Disable motor (ENA+ HIGH)"));
    Serial.println(F("  T  Toggle ENA+ pin (test polarity)"));
    Serial.println(F("  Z  Zero position counter"));
    Serial.println(F("  ?  Print current status"));
    Serial.println(F("  H  Show this menu"));
    Serial.println();
}

static int readIntFromSerial() {
    Serial.print(F("Enter value: "));
    while (!Serial.available()) { /* wait */ }
    int val = Serial.parseInt();
    // Drain any trailing newline/carriage-return
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
        Serial.println(F("Motor is disabled — enable first (E)."));
        return;
    }
    long target = stepper.currentPosition() + (long)dir * n * STEPS_PER_REV;
    Serial.print(F("Moving to "));
    Serial.print(target);
    Serial.println(F(" steps …"));
    stepper.moveTo(target);
}

// ── setup / loop ────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    while (!Serial) { /* wait for USB serial on Mega */ }

    pinMode(PIN_ENA, OUTPUT);
    enableMotor(true);

    stepper.setMinPulseWidth(5);
    stepper.setMaxSpeed(currentSpeed);
    stepper.setAcceleration(currentAccel);
    stepper.setCurrentPosition(0);

    Serial.println();
    Serial.println(F("TB6600 + NEMA 23 Stepper Test — Arduino Mega 2560 R3"));
    Serial.println(F("Pins: ENA+=D8  DIR+=D9  PUL+=D10  (−  to GND)"));
    Serial.print(F("Steps/rev: ")); Serial.println(STEPS_PER_REV);
    printMenu();
}

void loop() {
    // ── Service stepper (non-blocking) ──────────────────────────────
    if (continuousMode) {
        stepper.setSpeed(continuousDir > 0 ? currentSpeed : -currentSpeed);
        stepper.runSpeed();
    } else {
        stepper.run();
    }

    // ── Check serial input ──────────────────────────────────────────
    if (!Serial.available()) return;

    char cmd = Serial.read();
    // Drain leftover CR/LF
    delay(10);
    while (Serial.available() && (Serial.peek() == '\n' || Serial.peek() == '\r'))
        Serial.read();

    switch (cmd) {
    case '1':
        continuousMode = false;
        moveRevolutions(1, +1);
        break;
    case '2':
        continuousMode = false;
        moveRevolutions(1, -1);
        break;
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
        stepper.move(100);
        Serial.println(F("Jog CW +100 steps"));
        break;
    case '6':
        continuousMode = false;
        stepper.move(-100);
        Serial.println(F("Jog CCW -100 steps"));
        break;
    case '7':
        if (continuousMode && continuousDir > 0) {
            continuousMode = false;
            Serial.println(F("Continuous CW stopped."));
        } else {
            continuousMode = true;
            continuousDir = +1;
            Serial.println(F("Continuous CW started."));
        }
        break;
    case '8':
        if (continuousMode && continuousDir < 0) {
            continuousMode = false;
            Serial.println(F("Continuous CCW stopped."));
        } else {
            continuousMode = true;
            continuousDir = -1;
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
        stepper.setCurrentPosition(stepper.currentPosition());
        Serial.println(F("EMERGENCY STOP — immediate halt."));
        break;
    case 'S': case 's': {
        float spd = readFloatFromSerial();
        if (spd > 0) {
            currentSpeed = spd;
            stepper.setMaxSpeed(currentSpeed);
            Serial.print(F("Speed set to "));
            Serial.print(currentSpeed, 0);
            Serial.println(F(" steps/s"));
        }
        break;
    }
    case 'A': case 'a': {
        float acc = readFloatFromSerial();
        if (acc > 0) {
            currentAccel = acc;
            stepper.setAcceleration(currentAccel);
            Serial.print(F("Accel set to "));
            Serial.print(currentAccel, 0);
            Serial.println(F(" steps/s²"));
        }
        break;
    }
    case 'E': case 'e':
        enableMotor(true);
        break;
    case 'D': case 'd':
        enableMotor(false);
        break;
    case 'T': case 't': {
        bool current = digitalRead(PIN_ENA);
        setEnaPin(!current);
        Serial.println(F("(Use T to toggle, check if motor shaft locks/unlocks)"));
        break;
    }
    case 'Z': case 'z':
        stepper.setCurrentPosition(0);
        Serial.println(F("Position zeroed."));
        break;
    case '?':
        printStatus();
        break;
    case 'H': case 'h':
        printMenu();
        break;
    default:
        break;
    }
}
