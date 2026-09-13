// hal::motor on the Teensy 4.1 / grblHAL V2.09 carrier.
//
// One FlexPwmStepper per carrier channel (X→FlexPWM4.2, Y→FlexPWM2.0,
// Z→FlexPWM2.2 — docs/HW-T41-PINMAP.md §1): the pulse train is generated
// by the FlexPWM submodule and counted in its reload ISR, so it is
// immune to anything that blocks the main loop (HTTP flush, EEPROM
// writes, Serial). This is the production realisation of the CLAUDE.md
// "Firmware portability rule" (no loop-polled stepping); the STM32H7
// port swaps in TIM + DMA behind the same hal::motor interface.
//
// Motion profile: every move is a trapezoid — pulses start at
// kRampMinSpeed, accelerate at the axis' accel setting to its speed
// setting, and decelerate so the last pulses land near kRampMinSpeed.
// serviceRamp() re-programs the FlexPWM period from tick(); the end
// position is still exact because the ISR stops the train on the
// counted step. stop() cuts pulses immediately (limit-trip path).
//
// Retargeting a running move (a second "+N rev" before the first ends, a
// goto while moving): a target far enough ahead on the current heading
// changes the pulse count on the fly — the train never stops, so the ramp
// carries on at its current speed. A target behind it, or closer than the
// stopping distance, first decelerates to the nearest stop point (never
// past the old target), then runs to the new one. busy() and target() cover that hand-over so the app never sees the
// axis idle — or records a clean anchor — halfway through a reversal.
//
// Drivers (iHSS60) are enabled at init and stay enabled — invariant 3:
// a direct-coupled vacuum capacitor must never be free to back-drive.
// set_enabled(false) is a deliberate operator action for setup only.
// iHSS60 timing (DIR setup/hold, ENA lead) is honoured inside
// FlexPwmStepper and by the settle delay in set_enabled().

#ifdef TARGET_TEENSY41

#include "hal/hal.h"

#include <Arduino.h>

#include "flexpwm_stepper.h"
#include "hal/board/t41_v209.h"

namespace hal::motor {

namespace {

namespace board = hal::board::t41_v209;

constexpr float    kRampMinSpeed = 400.0f;   // steps/s at the start / end of a ramp
constexpr uint32_t kRampTickUs   = 1000;
constexpr uint32_t kEnaSettleMs  = 5;        // iHSS60: ENA leads DIR/PUL by ≥ 5 µs; be generous

constexpr uint8_t EN_ON  = board::EN_ACTIVE_LOW ? LOW  : HIGH;
constexpr uint8_t EN_OFF = board::EN_ACTIVE_LOW ? HIGH : LOW;

struct AxisDrv {
    FlexPwmStepper stepper;
    uint8_t        pin_en;
    uint32_t       speed      = kDefaultSpeed;
    uint32_t       accel      = kDefaultAccel;
    bool           enabled    = true;
    bool           rampActive = false;
    float          rampSpeed  = 0.0f;
    uint32_t       rampLastUs = 0;
    bool           pending    = false;   // reversal: decelerating, then run to pendingTarget
    int32_t        pendingTarget = 0;

    AxisDrv(const board::axis_pins_t &p, IMXRT_FLEXPWM_t *pwm, uint8_t sm, IRQ_NUMBER_t irq)
        : stepper(p.step, p.dir, pwm, sm, irq), pin_en(p.en) {}
};

AxisDrv drv[kMaxAxes] = {
    AxisDrv(board::AXIS_X, &IMXRT_FLEXPWM4, 2, IRQ_FLEXPWM4_2),
    AxisDrv(board::AXIS_Y, &IMXRT_FLEXPWM2, 0, IRQ_FLEXPWM2_0),
    AxisDrv(board::AXIS_Z, &IMXRT_FLEXPWM2, 2, IRQ_FLEXPWM2_2),
};

AxisDrv &D(Axis a) { return drv[a < kMaxAxes ? a : 0]; }

void startRamp(AxisDrv &d, int32_t target) {
    const float vmin = (static_cast<float>(d.speed) < kRampMinSpeed) ? static_cast<float>(d.speed) : kRampMinSpeed;
    d.rampSpeed  = vmin;
    d.rampLastUs = micros();
    d.rampActive = true;
    d.stepper.setSpeed(static_cast<uint32_t>(vmin));
    d.stepper.moveTo(target);
}

// Steps needed to ramp from the current rate down to the ramp floor.
int32_t stopDistance(const AxisDrv &d) {
    const float vtarget = static_cast<float>(d.speed);
    const float vmin    = (vtarget < kRampMinSpeed) ? vtarget : kRampMinSpeed;
    const float v       = d.rampActive ? d.rampSpeed : vmin;
    const float acc     = (d.accel > 1) ? static_cast<float>(d.accel) : 1.0f;
    const float dist    = (v * v - vmin * vmin) / (2.0f * acc);
    return dist > 0.0f ? static_cast<int32_t>(dist) : 0;
}

// Decel starts when the remaining distance equals the stopping distance
// from the current rate (plus one tick of travel so a late tick errs
// towards decelerating early, never late).
void serviceRamp(AxisDrv &d) {
    if (!d.rampActive) return;
    if (!d.stepper.isRunning()) {
        if (d.pending) {                     // reversal: stopped — now run to the new target
            d.pending = false;
            if (d.pendingTarget != static_cast<int32_t>(d.stepper.position())) {
                startRamp(d, d.pendingTarget);
                return;
            }
        }
        d.rampActive = false;
        d.stepper.setSpeed(d.speed);
        return;
    }
    const uint32_t now = micros();
    const uint32_t el  = now - d.rampLastUs;
    if (el < kRampTickUs) return;
    d.rampLastUs = now;

    const float dt      = static_cast<float>(el) * 1e-6f;
    const float acc     = (d.accel > 1) ? static_cast<float>(d.accel) : 1.0f;
    const float vtarget = static_cast<float>(d.speed);
    const float vmin    = (vtarget < kRampMinSpeed) ? vtarget : kRampMinSpeed;
    const float remain  = static_cast<float>(labs(d.stepper.distanceToGo()));
    float v = d.rampSpeed;

    const float stopDist = (v * v - vmin * vmin) / (2.0f * acc);
    if (remain <= stopDist + v * dt) {
        v -= acc * dt; if (v < vmin) v = vmin;
    } else if (v < vtarget) {
        v += acc * dt; if (v > vtarget) v = vtarget;
    } else if (v > vtarget) {
        v = vtarget;
    }
    if (static_cast<uint32_t>(v) != static_cast<uint32_t>(d.rampSpeed)) d.stepper.setSpeed(static_cast<uint32_t>(v));
    d.rampSpeed = v;
}

} // namespace

void init() {
    for (auto &d : drv) {
        pinMode(d.pin_en, OUTPUT);
        digitalWrite(d.pin_en, EN_ON);      // enabled from the first instant
        d.enabled = true;
        d.stepper.init();
        d.stepper.setSpeed(d.speed);
        d.rampActive = false;
    }
    delay(kEnaSettleMs);
}

void move_to(Axis a, int32_t target_steps) {
    AxisDrv &d = D(a);
    if (!d.enabled) return;
    d.pending = false;
    // Running: retarget without stopping the train. Re-read and retry if a
    // pulse lands between the read and the lock (retarget() refuses a
    // target that is no longer ahead); bounded, then fall through.
    for (int attempt = 0; attempt < 8 && d.stepper.isRunning(); ++attempt) {
        const int32_t pos  = static_cast<int32_t>(d.stepper.position());
        const int32_t togo = static_cast<int32_t>(d.stepper.distanceToGo());
        if (togo == 0) break;                                   // burst just ended
        const int dir = togo > 0 ? +1 : -1;                     // current heading
        const int64_t ahead = static_cast<int64_t>(target_steps - pos) * dir;
        const int32_t stop_dist = stopDistance(d);
        if (ahead > stop_dist) {
            // Far enough ahead on this heading to stop on it: new end point at
            // the current speed; serviceRamp() re-plans the deceleration.
            if (d.stepper.retarget(target_steps)) { d.rampActive = true; return; }
            continue;
        }
        // Behind, exactly here, or too close ahead to stop in time: decelerate
        // to the nearest stop point on this heading — never past the old
        // target — then come back. Ending the train at speed instead would be
        // an instant stop (following-error alarm or lost steps).
        int32_t stop_steps = stop_dist + 1;
        if (stop_steps > togo * dir) stop_steps = togo * dir;
        if (d.stepper.retarget(pos + dir * stop_steps)) {
            d.pendingTarget = target_steps;
            d.pending       = true;
            d.rampActive    = true;
            return;
        }
    }
    // At rest (or the running burst ended while we looked): start from rest.
    if (target_steps == static_cast<int32_t>(d.stepper.position())) { d.rampActive = false; return; }
    startRamp(d, target_steps);
}

// Relative to where the axis is heading, like app::motion::move_axis().
void move_by(Axis a, int32_t delta_steps) { move_to(a, target(a) + delta_steps); }

void stop(Axis a) {
    AxisDrv &d = D(a);
    d.pending = false;
    d.stepper.stop();
    d.rampActive = false;
    d.stepper.setSpeed(d.speed);
}

int32_t position(Axis a) { return static_cast<int32_t>(D(a).stepper.position()); }

int32_t target(Axis a) {
    const AxisDrv &d = D(a);
    if (d.pending) return d.pendingTarget;
    return static_cast<int32_t>(d.stepper.position() + d.stepper.distanceToGo());
}

bool busy(Axis a) { return D(a).stepper.isRunning() || D(a).pending; }

void set_position(Axis a, int32_t value) {
    AxisDrv &d = D(a);
    d.pending = false;
    d.stepper.stop();
    d.rampActive = false;
    d.stepper.setPosition(value);
}

void set_speed(Axis a, uint32_t v) {
    AxisDrv &d = D(a);
    if (v < 1) v = 1;
    if (v > kMaxSpeed) v = kMaxSpeed;
    d.speed = v;
    if (!d.rampActive) d.stepper.setSpeed(v);   // mid-ramp: serviceRamp converges instead
}

void set_accel(Axis a, uint32_t v) { D(a).accel = v < 1 ? 1 : v; }
uint32_t speed(Axis a)             { return D(a).speed; }
uint32_t accel(Axis a)             { return D(a).accel; }

void set_enabled(Axis a, bool on) {
    AxisDrv &d = D(a);
    if (!on) stop(a);
    digitalWrite(d.pin_en, on ? EN_ON : EN_OFF);
    d.enabled = on;
    if (on) delay(kEnaSettleMs);
}

bool enabled(Axis a) { return D(a).enabled; }

void tick() {
    for (auto &d : drv) serviceRamp(d);
}

} // namespace hal::motor

#endif // TARGET_TEENSY41
