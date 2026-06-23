#include "flexpwm_stepper.h"

// ─── ISR dispatch ──────────────────────────────────────────────────────
// One static instance pointer per FlexPWM submodule we support, plus a
// thin C-callable trampoline that hands the interrupt back to the right
// instance. Bench uses two submodules (X→FlexPWM4.2, Y→FlexPWM2.0); add
// more slots here if Z/M3/M4 are wired up later. Hard-coded rather than
// a generic array because attachInterruptVector takes a real function
// pointer, not a thunk-capturing lambda.

static FlexPwmStepper *s_isrInstance_FP4_2 = nullptr;
static FlexPwmStepper *s_isrInstance_FP2_0 = nullptr;

static void isr_flexpwm4_2() {
    if (s_isrInstance_FP4_2) s_isrInstance_FP4_2->handleIsr();
}
static void isr_flexpwm2_0() {
    if (s_isrInstance_FP2_0) s_isrInstance_FP2_0->handleIsr();
}

// ─── FlexPwmStepper ────────────────────────────────────────────────────

FlexPwmStepper::FlexPwmStepper(int stepPin, int dirPin,
                               IMXRT_FLEXPWM_t *pwm, uint8_t submodule,
                               IRQ_NUMBER_t irq)
    : _stepPin(stepPin), _dirPin(dirPin),
      _pwm(pwm), _sm(submodule), _irq(irq),
      _position(0), _remaining(0), _running(false),
      _direction(1), _speedHz(800) {}

void FlexPwmStepper::init() {
    pinMode(_dirPin, OUTPUT);
    digitalWrite(_dirPin, LOW);
    pinMode(_stepPin, OUTPUT);
    digitalWrite(_stepPin, LOW);

    // Wire the right static trampoline to this instance.
    if (_pwm == &IMXRT_FLEXPWM4 && _sm == 2) {
        s_isrInstance_FP4_2 = this;
        attachInterruptVector(_irq, isr_flexpwm4_2);
    } else if (_pwm == &IMXRT_FLEXPWM2 && _sm == 0) {
        s_isrInstance_FP2_0 = this;
        attachInterruptVector(_irq, isr_flexpwm2_0);
    } else {
        // Caller passed an (pwm, submodule) pair without an ISR slot.
        // Add it above and to flexpwm_stepper.cpp before using it.
        // Without dispatch, step counting silently breaks — fail loud.
        Serial.print(F("[FlexPwmStepper] no ISR slot for submodule on pin "));
        Serial.println(_stepPin);
    }
    // Slightly elevate priority over Ethernet stack timers so step
    // counts don't get starved under heavy network load. Lower number
    // = higher priority (M7 NVIC default is 128).
    NVIC_SET_PRIORITY(_irq, 32);
}

void FlexPwmStepper::setSpeed(uint32_t hz) {
    if (hz < 1)      hz = 1;
    if (hz > 200000) hz = 200000;
    _speedHz = hz;
    // analogWriteFrequency is callable mid-run; it reloads the submodule
    // period at the next boundary.
    if (_running) analogWriteFrequency(_stepPin, hz);
}

long FlexPwmStepper::position() const {
    noInterrupts();
    long p = _position;
    interrupts();
    return p;
}

void FlexPwmStepper::setPosition(long p) {
    noInterrupts();
    _position = p;
    interrupts();
}

long FlexPwmStepper::distanceToGo() const {
    noInterrupts();
    long r = (long)_remaining * (long)_direction;
    interrupts();
    return r;
}

void FlexPwmStepper::moveSteps(long n) {
    if (n == 0) return;
    stop();
    _direction = (n > 0) ? 1 : -1;
    digitalWrite(_dirPin, _direction > 0 ? HIGH : LOW);
    delayMicroseconds(2);  // DIR-to-STEP setup time (typ. ≥ 1 µs on
                           // TB6600 / TMC2209 / DM542; 2 µs is safe)
    noInterrupts();
    _remaining = (n > 0) ? n : -n;
    interrupts();
    _startPulses();
}

void FlexPwmStepper::moveTo(long target) {
    moveSteps(target - position());
}

void FlexPwmStepper::runContinuous(int dir) {
    stop();
    _direction = (dir > 0) ? 1 : -1;
    digitalWrite(_dirPin, _direction > 0 ? HIGH : LOW);
    delayMicroseconds(2);
    noInterrupts();
    _remaining = 0;  // 0 == infinite (ISR doesn't decrement)
    interrupts();
    _startPulses();
}

void FlexPwmStepper::stop() {
    if (!_running) return;
    _stopPulses();
    noInterrupts();
    _remaining = 0;
    interrupts();
}

// ─── Private ───────────────────────────────────────────────────────────

void FlexPwmStepper::_startPulses() {
    // Teensyduino's analogWrite configures the FlexPWM submodule from
    // scratch on first call (pin mux, clock, VAL0..VAL3, OUTEN). Setting
    // duty to 128 (50%) gives us a clean square-wave step train.
    analogWriteFrequency(_stepPin, _speedHz);
    analogWrite(_stepPin, 128);

    // Clear any stale reload flag, then enable the reload IRQ so we
    // get one interrupt per pulse for step counting.
    _pwm->SM[_sm].STS = FLEXPWM_SMSTS_RF;
    _pwm->SM[_sm].INTEN |= FLEXPWM_SMINTEN_RIE;
    NVIC_ENABLE_IRQ(_irq);
    _running = true;
}

void FlexPwmStepper::_stopPulses() {
    // Disable NVIC dispatch first so a late-firing IRQ doesn't see
    // _running == false and update position.
    NVIC_DISABLE_IRQ(_irq);
    _pwm->SM[_sm].INTEN &= ~FLEXPWM_SMINTEN_RIE;

    // Gate the pin output off via the master OUTEN register (ISR-safe;
    // it's a single register write, no library locking). The submodule
    // counter keeps running internally — cheap, doesn't matter, and
    // avoids the risk of analogWrite() locking from ISR context.
    _pwm->OUTEN &= ~FLEXPWM_OUTEN_PWMA_EN(1 << _sm);
    digitalWrite(_stepPin, LOW);
    _running = false;
}

// ─── ISR body ──────────────────────────────────────────────────────────
// Runs in interrupt context. Keep it tight.

void FlexPwmStepper::handleIsr() {
    // W1C: writing 1 clears the reload flag.
    _pwm->SM[_sm].STS = FLEXPWM_SMSTS_RF;

    _position += _direction;

    if (_remaining > 0) {
        if (--_remaining == 0) {
            // End of bounded N-step burst — kill the pulse train
            // right here so the next period doesn't fire.
            _pwm->OUTEN &= ~FLEXPWM_OUTEN_PWMA_EN(1 << _sm);
            _pwm->SM[_sm].INTEN &= ~FLEXPWM_SMINTEN_RIE;
            NVIC_DISABLE_IRQ(_irq);
            _running = false;
        }
    }
    // _remaining == 0 from the start means infinite (continuous) mode —
    // ISR just counts steps; pulse train runs until stop() is called.
}
