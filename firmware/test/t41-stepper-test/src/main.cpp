// X + Y + Z axis stepper bench-test for Teensy 4.1 in the grblHAL-teensy-4.x
// V2.09 carrier (Phil Barrett).  Three axes cover the Balanced Pi path
// (C1, L-pair, C2) and, with Z left idle, the Balanced L path (L-pair, C)
// — see docs/HW-T41-PINMAP.md §7 and CLAUDE.md "RF topology".
//
// Drives the carrier's X- / Y- / Z-axis STEP / DIR / EN through external
// stepper drivers (iHSS60 closed-loop, or TMC2209 / DM542 / equivalent) →
// motors, plus monitors the three opto-isolated end-stop inputs (X / Y / Z
// LIMIT).
//
// Interactive serial menu over USB (115200 baud).  An "active axis" selector
// (X / Y / Z) decides which axis the motion commands act on.  All axes run
// independently — you can leave X spinning in continuous mode and start a
// jog on Y while X keeps running.
//
//   X / Y / Z  Switch active axis (motion commands target the active axis)
//   1 / 2    Active axis: rotate CW / CCW 1 revolution
//   3 / 4    Active axis: rotate CW / CCW N revolutions (prompted)
//   5 / 6    Active axis: jog CW / CCW 100 steps
//   + / -    Active axis: single step CW / CCW (fine tuning)
//   7 / 8    Active axis: continuous CW / CCW (toggle, prints rev count)
//   9        Active axis: stop (decelerate)
//   0        Active axis: emergency stop (immediate)
//   S / A    Active axis: set speed / acceleration
//   T        Active axis: toggle ENA pin (test driver polarity)
//   O        Active axis: set current position = home / origin (0), save
//            to EEPROM — activates the software travel window (see below)
//            ('O' because 'Z' now selects the Z axis)
//   U        Active axis: unset home (travel window inactive until O again)
//   K        Active axis: set element kind (0 unset, 1 roller inductor,
//            2 vacuum-variable cap, 3 variable cap with stops, 4 variable
//            cap free rotation, 5 variometer) + rated travel in revolutions
//   M        Active axis: set the element's rated travel (max revolutions)
//   W        Active axis: save current speed + accel to EEPROM (restored at boot)
//   E / D    Enable / disable ALL axes
//   Q        Toggle idle auto-release (global, persisted in EEPROM)
//   N        Print network status (Ethernet backend, link, IP)
//   ?        Print status (all axes + limits)
//   H        Print menu
//
// Ethernet:
//   - PJRC Ethernet kit on the V2.09 carrier socket.
//   - Backend selectable at build time via the platformio.ini env:
//       env:teensy41        — QNEthernet (lwIP, default)
//       env:teensy41_native — NativeEthernet (FNET)
//   - DHCP attempt runs once during setup() with short timeouts so a
//     missing cable doesn't hold up motor testing. Inspect with 'N'
//     later (the underlying library keeps the lease renewed in the
//     background).
//
// HTTP control surface (when DHCP succeeds):
//   - http://<dhcp-ip>/  serves a one-page web UI mirroring the Serial
//     menu (jog ±1/±10/±100/±N steps, ±1/N revs, run CW/CCW, stop,
//     e-stop, speed/accel + save-speed, enable/disable, idle-release
//     toggle, element kind + rated travel, set-current-position-as-home,
//     unset home).
//   - JSON status at /api/status (polled by the UI every 1000 ms).
//     Each axis carries rev_job {revs,eff,dir,done,left,state} so the UI
//     can show rotate-N progress ("12.3 / 40 revs") until the next
//     command, and element {kind,limited,max_rev,max_steps,home_set,
//     travel,last_clamp,turns} for the travel window (see below).
//   - GET-only verb endpoints under /api/* — same code paths as the
//     Serial handlers (jogSteps / moveRevolutions / startContinuous /
//     stopAxis / zeroAxis / setElement), so HTTP and Serial can never
//     disagree.
//   - LAN-only, no auth (same posture as LP-100A-Server).
//   - All motion is non-blocking: FlexPWM emits the pulses, the reload
//     ISR counts them, serviceAxis() does the bookkeeping.
//
// Travel window (software hard limits — protects inductors / vacuum caps):
//   - Each axis declares what it drives (K key / "Set element" in the UI):
//       0 unset · 1 roller inductor · 2 vacuum-variable capacitor ·
//       3 variable capacitor with end stops · 4 variable capacitor, free
//       rotation · 5 variometer, free rotation
//     plus, for kinds 1–3, the element's rated travel in revolutions.
//   - Kinds 1–3 have mechanical stops that a mis-step would destroy, so
//     once the operator has declared home (O / "Set current pos as home")
//     the firmware keeps a window [0, rated_rev × STEPS_PER_REV] and
//     clamps every motion verb to it: jog / single step / ±N rev /
//     continuous ("run to end") all stop exactly on the bound, latch
//     which bound was hit, and the web UI shows a per-motor
//     "STOPPED AT HOME / MAX LIMIT" badge. A move that would start at a
//     bound and head outward is refused (HTTP 409). The position counter
//     is updated by the ISR for every pulse, single steps included.
//   - Until home is declared the window is inactive (UI: "HOME NOT SET")
//     so the operator can jog the element onto its real home stop first;
//     'U' / "Unset home" deactivates it again for recovery.
//   - Moves back *into* the window from outside are always allowed.
//   - Kinds 4–5 and "unset" have no window (position still counts).
//   - Kind, home-set flag and rated travel persist in EEPROM per axis.
//     Changing the kind clears home-set (a new device is on the shaft).
//   - Home is bound 0 exactly: declare it a few steps INSIDE the
//     physical stop. The lead-screw limit switches (CLAUDE.md
//     invariant 7) remain the hardware fallback once fitted; this is the
//     bench prototype of the invariant-7 software soft limits.
//
// Persistence (Teensy 4.1 emulated EEPROM, 4 KB, schema v2):
//   - Per-axis step position is saved on every move-stop and throttled (1 s)
//     during continuous mode, so a power-cycle has a near-current anchor.
//   - Idle auto-release preference survives reboots.
//   - Per-axis speed and accel are saved only on explicit request (W key /
//     "Save spd+acc" button) and restored at boot; defaults 800 steps/s,
//     25600 steps/s².
//   - Per-axis element kind, home-set flag and rated travel are saved
//     whenever they change (K / M / O / U keys, /api/element, /api/zero,
//     /api/unhome) and restored at boot.
//   - The Z axis has its own EEPROM block (own magic) appended after the
//     X/Y records, so boards already in the field keep their saved X/Y
//     positions, speeds and element settings when this build lands.
//
// Motion profile:
//   - Rotate-N and boot-time homing use a trapezoidal ramp: start at
//     RAMP_MIN_SPEED, accelerate at the axis' accel setting to its speed
//     setting, decelerate so the last pulses land near RAMP_MIN_SPEED.
//     The ramp is stepped from the main loop (serviceRamp, 1 ms tick) by
//     re-programming the FlexPWM period; the end position is still exact
//     because the reload ISR stops the train on the counted step.
//   - Jog (100 steps) and continuous mode remain constant-velocity.
//   - Schema upgrade automatic — old v1 EEPROM contents are recognised and
//     re-initialised to defaults (position = 0 per axis, release = ON).
//
// Boot behaviour:
//   - Read saved positions from EEPROM
//   - Drive each axis from its saved position → 0 (boot-time homing)
//   - Save 0, continue to menu
//
// Limit switches:
//   - X LIMIT (Teensy pin 20), Y LIMIT (pin 21) and Z LIMIT (pin 22),
//     opto-isolated inputs on the carrier.  Configured as INPUT_PULLUP.
//   - Active-LOW (opto conducting = switch closed = limit asserted).
//   - State changes are debounced (~15 ms) and printed to serial.
//   - Current state is shown in '?' status output.
//   - POC level: no automatic motion-refusal logic — operator-visible only.

#include <Arduino.h>
#include <EEPROM.h>

// Pin map for the V2.09 carrier lives with the production tuner-
// controller HAL so bench and production share one source of truth.
// Bench's platformio.ini adds an -I onto firmware/tuner-controller/
// src/hal/board/ to make this include resolve.
#include "t41_v209.h"

// Ethernet HAL shim — pick QNEthernet (lwIP) or NativeEthernet (FNET)
// at compile time via the env's build_flags. Same source compiles
// against either backend. See firmware/lib/net_hal/.
#include "net_hal.h"

// Hardware-timed stepper driver (shared library firmware/lib/
// flexpwm_stepper/, also used by the production tuner-controller HAL).
// Replaces AccelStepper's loop()-polled runSpeed() pattern with
// FlexPWM-generated pulse trains + ISR step counting so motion is
// immune to main-loop blocking (HTTP flush/stop, Serial prints, EEPROM
// writes).
#include "flexpwm_stepper.h"

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

static constexpr uint8_t PIN_Z_STEP  = board::AXIS_Z.step;
static constexpr uint8_t PIN_Z_DIR   = board::AXIS_Z.dir;
static constexpr uint8_t PIN_Z_EN    = board::AXIS_Z.en;
static constexpr uint8_t PIN_Z_LIMIT = board::AXIS_Z.limit;

// ── Defaults (match your driver's micro-stepping setting) ───────────────
static const int   STEPS_PER_REV = 6400;   // closed-loop NEMA 24 driver configured for 6400 pulses/rev (1.8° motor, 1/32 equivalent)

// ── Element kinds ───────────────────────────────────────────────────────
// What is bolted to each motor decides whether the axis has hard
// mechanical stops. Roller inductors, vacuum-variable capacitors and
// stop-limited air variables are destroyed by over-travel, so for those
// the firmware keeps a software travel window [0, maxSteps] anchored at
// the operator-declared home and clamps every motion verb to it (see
// clampTarget / beginBoundedMove). Free-rotating air variables and
// variometers have no stops; no window is enforced. Ids are persisted in
// EEPROM — append new kinds, never renumber.
enum ElementKind : uint8_t {
    EK_UNSET          = 0,   // nothing declared — no window, UI warns
    EK_INDUCTOR       = 1,   // roller inductor: stops at both ends
    EK_VACUUM_CAP     = 2,   // vacuum-variable capacitor: stops, fragile bellows
    EK_VARCAP_LIMITED = 3,   // air variable with end stops (e.g. 180°)
    EK_VARCAP_FREE    = 4,   // air variable that rotates freely (no stops)
    EK_VARIOMETER     = 5,   // variometer, rotates freely
    EK_COUNT
};
static const char* const ELEMENT_KIND_NAMES[EK_COUNT] = {
    "unset", "inductor", "vacuum_cap", "varcap_limited", "varcap_free", "variometer"
};
static bool kindHasStops(uint8_t k) {
    return k == EK_INDUCTOR || k == EK_VACUUM_CAP || k == EK_VARCAP_LIMITED;
}
static const char* kindName(uint8_t k) { return k < EK_COUNT ? ELEMENT_KIND_NAMES[k] : "?"; }
// Accepts a kind id ("2") or name ("vacuum_cap"); -1 if unrecognised.
static int parseKind(const char* str) {
    if (!str || !*str) return -1;
    if (str[0] >= '0' && str[0] <= '9' && str[1] == '\0') {
        const int k = str[0] - '0';
        return k < EK_COUNT ? k : -1;
    }
    for (int k = 0; k < EK_COUNT; k++)
        if (strcmp(str, ELEMENT_KIND_NAMES[k]) == 0) return k;
    return -1;
}
// Rated-travel sanity bound (revolutions). 100 000 turns is far beyond
// any tuner element; it only guards against garbage in EEPROM / a typo.
static const float MAX_RATED_REV = 100000.0f;

// ── EEPROM layout (schema v2) ───────────────────────────────────────────
//   0..3   uint32_t magic    (=NVS_MAGIC when our v2 schema is written)
//   4..7    int32_t X position
//   8..11   int32_t Y position
//   12      uint8_t  idleAutoRelease (0/1)
//   16..19  uint32_t motion-block magic ('ATSP' = speeds only, 'ATS2' = +accels)
//   20..23  uint32_t X saved speed (steps/s)
//   24..27  uint32_t Y saved speed (steps/s)
//   28..31  uint32_t X saved accel (steps/s²)
//   32..35  uint32_t Y saved accel (steps/s²)
//   36..39  uint32_t element-block magic ('ATEL')
//   40      uint8_t  X element kind (ElementKind)
//   41      uint8_t  X home-set flag (0/1)
//   44..47  float    X rated travel, revolutions
//   48      uint8_t  Y element kind
//   49      uint8_t  Y home-set flag
//   52..55  float    Y rated travel, revolutions
//   56..59  uint32_t Z-axis block magic ('ATZ1')
//   60..63   int32_t Z position
//   64..67  uint32_t Z saved speed (steps/s)
//   68..71  uint32_t Z saved accel (steps/s²)
//   72      uint8_t  Z element kind
//   73      uint8_t  Z home-set flag
//   76..79  float    Z rated travel, revolutions
// The motion block carries its own magic so adding it did not invalidate
// the v2 position records already stored on boards in the field; an
// 'ATSP' block is upgraded in place (accels defaulted, speeds kept). The
// element block likewise has its own magic and defaults to "unset". The
// Z axis arrived later still and lives in one self-contained block of
// its own (position + motion + element) behind the 'ATZ1' magic.
static const int      EE_ADDR_MAGIC       = 0;
static const int      EE_ADDR_POS_X       = 4;
static const int      EE_ADDR_POS_Y       = 8;
static const int      EE_ADDR_RELEASE     = 12;
static const int      EE_ADDR_SPEED_MAGIC = 16;
static const int      EE_ADDR_SPEED_X     = 20;
static const int      EE_ADDR_SPEED_Y     = 24;
static const int      EE_ADDR_ACCEL_X     = 28;
static const int      EE_ADDR_ACCEL_Y     = 32;
static const int      EE_ADDR_ELEM_MAGIC  = 36;
static const int      EE_ADDR_ELEM_X      = 40;   // 8-byte element record per axis
static const int      EE_ADDR_ELEM_Y      = 48;
static const int      ELEM_OFF_KIND       = 0;    // offsets inside an element record
static const int      ELEM_OFF_HOMESET    = 1;
static const int      ELEM_OFF_MAXREV     = 4;
static const int      EE_ADDR_Z_MAGIC     = 56;   // Z-axis block: pos + speed + accel + element
static const int      EE_ADDR_POS_Z       = 60;
static const int      EE_ADDR_SPEED_Z     = 64;
static const int      EE_ADDR_ACCEL_Z     = 68;
static const int      EE_ADDR_ELEM_Z      = 72;
static const uint32_t NVS_MAGIC_V2        = 0x41544132UL;  // 'ATA2' — recognises v2 schema
static const uint32_t NVS_SPEED_MAGIC     = 0x41545350UL;  // 'ATSP' — motion block v1 (speeds)
static const uint32_t NVS_MOTION_MAGIC    = 0x41545332UL;  // 'ATS2' — motion block v2 (+accels)
static const uint32_t NVS_ELEM_MAGIC      = 0x4154454CUL;  // 'ATEL' — element block v1
static const uint32_t NVS_Z_MAGIC         = 0x41545A31UL;  // 'ATZ1' — Z-axis block v1
static const uint32_t DEFAULT_SPEED_HZ    = 800;           // when nothing has been saved
static const uint32_t DEFAULT_ACCEL_HZ2   = 25600;         // steps/s² — 0→25600 steps/s in 1 s

// ── Trapezoidal ramp (rotate-N / homing) ────────────────────────────────
static const float         RAMP_MIN_SPEED = 400.0f;   // steps/s: start / end rate of a ramped move
static const unsigned long RAMP_TICK_US   = 1000;     // ramp update period (main-loop paced)

// ── Per-axis state ──────────────────────────────────────────────────────

struct Axis {
    const char*       name;
    uint8_t           pin_step;
    uint8_t           pin_dir;
    uint8_t           pin_en;
    uint8_t           pin_limit;
    int               ee_pos_addr;
    int               ee_speed_addr;
    int               ee_accel_addr;
    int               ee_elem_addr;
    FlexPwmStepper    stepper;

    // Position-persistence state
    long              lastSavedPos    = 0;
    unsigned long     lastContSaveMs  = 0;

    // Idle / motion tracking
    bool              driverReleased  = false;
    unsigned long     lastMotionMs    = 0;
    bool              wasRunning      = false;

    // Continuous-mode state
    bool              continuousMode  = false;
    int               continuousDir   = 1;
    long              lastReportedRev = 0;

    // Per-axis motion settings (both replaced at boot by the EEPROM values).
    // currentSpeed is the cruise rate; currentAccel is the ramp rate used
    // by rotate-N / homing moves (jog + continuous ignore it).
    float             currentSpeed    = 800.0f;
    float             currentAccel    = 25600.0f;
    float             savedSpeed      = 0.0f;     // last values persisted via W / "Save spd+acc"
    float             savedAccel      = 0.0f;

    // Trapezoidal ramp state for the bounded move in flight (see serviceRamp).
    bool              rampActive      = false;
    float             rampSpeed       = 0.0f;     // rate currently programmed into FlexPWM
    unsigned long     rampLastUs      = 0;

    // Limit-switch debounced state (HIGH = released, LOW = asserted)
    int               limitLastState  = HIGH;

    // Boot-time homing: set true when startHomeOnBoot kicks off a move
    // toward 0, cleared when the axis actually reaches 0 (or when the
    // operator overrides with any other motion command).
    bool              homing          = false;

    // Rotate-N-revolutions job tracking (UI progress). Set by
    // moveRevolutions(); cleared by any other motion verb (jog,
    // continuous, homing, zero). Deliberately NOT cleared by stop /
    // e-stop so the UI keeps showing "stopped at 12.3 / 40".
    int               revJobRevs      = 0;   // requested revs, 0 = none
    int               revJobDir       = 0;   // +1 CW / -1 CCW
    long              revJobStartPos  = 0;
    long              revJobTargetPos = 0;

    // Element / travel-window configuration (persisted — see EEPROM layout).
    // The window [0, maxSteps] is enforced only while kindHasStops(kind)
    // && homeSet (see limitsActive). lastClamp latches which bound the
    // most recent motion verb ran into (-1 home / +1 max / 0 none) so the
    // UI can show "STOPPED AT MAX" until the next command replaces it.
    uint8_t           kind            = EK_UNSET;
    bool              homeSet         = false;
    float             maxRev          = 0.0f;     // rated travel, element revolutions
    long              maxSteps        = 0;        // derived: lroundf(maxRev × STEPS_PER_REV)
    int8_t            lastClamp       = 0;

    Axis(const char* n, uint8_t s, uint8_t d, uint8_t e, uint8_t l,
         int ee_addr, int ee_spd_addr, int ee_acc_addr, int ee_el_addr,
         IMXRT_FLEXPWM_t* pwm, uint8_t submodule, IRQ_NUMBER_t irq)
        : name(n), pin_step(s), pin_dir(d), pin_en(e), pin_limit(l),
          ee_pos_addr(ee_addr), ee_speed_addr(ee_spd_addr), ee_accel_addr(ee_acc_addr),
          ee_elem_addr(ee_el_addr),
          stepper(s, d, pwm, submodule, irq) {}
};

// FlexPWM submodule assignment per axis — see docs/HW-T41-PINMAP.md §1.
// Pin 2 (X STEP) is FlexPWM4 submodule 2; pin 4 (Y STEP) is FlexPWM2
// submodule 0; pin 6 (Z STEP) is FlexPWM2 submodule 2. Each submodule
// has its own counter and IRQ line → independent step trains, no
// contention. If you ever wire M3/M4 STEP pins, add the right
// (pwm, submodule, irq) triple here AND a dispatch slot in
// flexpwm_stepper.cpp.
static Axis xAxis("X", PIN_X_STEP, PIN_X_DIR, PIN_X_EN, PIN_X_LIMIT, EE_ADDR_POS_X, EE_ADDR_SPEED_X, EE_ADDR_ACCEL_X,
                  EE_ADDR_ELEM_X, &IMXRT_FLEXPWM4, 2, IRQ_FLEXPWM4_2);
static Axis yAxis("Y", PIN_Y_STEP, PIN_Y_DIR, PIN_Y_EN, PIN_Y_LIMIT, EE_ADDR_POS_Y, EE_ADDR_SPEED_Y, EE_ADDR_ACCEL_Y,
                  EE_ADDR_ELEM_Y, &IMXRT_FLEXPWM2, 0, IRQ_FLEXPWM2_0);
static Axis zAxis("Z", PIN_Z_STEP, PIN_Z_DIR, PIN_Z_EN, PIN_Z_LIMIT, EE_ADDR_POS_Z, EE_ADDR_SPEED_Z, EE_ADDR_ACCEL_Z,
                  EE_ADDR_ELEM_Z, &IMXRT_FLEXPWM2, 2, IRQ_FLEXPWM2_2);
static Axis* const axes[] = { &xAxis, &yAxis, &zAxis };
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

// ── Ethernet bring-up ──────────────────────────────────────────────────
// Short, non-blocking-friendly timeouts: this is a bench tool, so a
// missing cable shouldn't keep the operator waiting. If link/DHCP
// don't come up, log it and continue — the motor menu still works.
static const uint32_t ETH_LINK_TIMEOUT_MS = 3000;
static const uint32_t ETH_DHCP_TIMEOUT_MS = 8000;

static uint8_t netMac[6]  = {0};
static bool    netLinkUp  = false;
static bool    netDhcpOK  = false;

// ── HTTP control server ────────────────────────────────────────────────
// Tiny GET-only API mirroring the Serial menu. LAN-only, no auth — same
// posture as LP-100A-Server. Served only when DHCP succeeds.
static EthernetServer httpServer(80);
static bool           httpReady = false;

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

// ── Motion block: saved speed + accel (own magic — see EEPROM layout) ────
static void nvsInitSpeed() {
    uint32_t magic = 0;
    EEPROM.get(EE_ADDR_SPEED_MAGIC, magic);
    if (magic == NVS_MOTION_MAGIC) return;
    if (magic == NVS_SPEED_MAGIC) {
        // v1 block: keep the saved speeds, add default accels.
        EEPROM.put(EE_ADDR_ACCEL_X, DEFAULT_ACCEL_HZ2);
        EEPROM.put(EE_ADDR_ACCEL_Y, DEFAULT_ACCEL_HZ2);
        EEPROM.put(EE_ADDR_SPEED_MAGIC, NVS_MOTION_MAGIC);
        Serial.println(F("EEPROM: motion block upgraded (speeds kept, accel = 25600 steps/s²)."));
        return;
    }
    EEPROM.put(EE_ADDR_SPEED_X, DEFAULT_SPEED_HZ);
    EEPROM.put(EE_ADDR_SPEED_Y, DEFAULT_SPEED_HZ);
    EEPROM.put(EE_ADDR_ACCEL_X, DEFAULT_ACCEL_HZ2);
    EEPROM.put(EE_ADDR_ACCEL_Y, DEFAULT_ACCEL_HZ2);
    EEPROM.put(EE_ADDR_SPEED_MAGIC, NVS_MOTION_MAGIC);
    Serial.println(F("EEPROM: initialised motion block (800 steps/s, 25600 steps/s² per axis)."));
}

static uint32_t nvsLoadSpeed(int ee_addr) {
    uint32_t v = DEFAULT_SPEED_HZ;
    EEPROM.get(ee_addr, v);
    if (v < 1 || v > 200000) v = DEFAULT_SPEED_HZ;   // FlexPwmStepper::setSpeed range
    return v;
}

static uint32_t nvsLoadAccel(int ee_addr) {
    uint32_t v = DEFAULT_ACCEL_HZ2;
    EEPROM.get(ee_addr, v);
    if (v < 1 || v > 10000000UL) v = DEFAULT_ACCEL_HZ2;
    return v;
}

// Persist the axis' current speed + accel so they become the boot-time
// defaults. Shared by the 'W' serial key and GET /api/save_speed.
static void saveSpeed(Axis& a) {
    const uint32_t v = (uint32_t)a.currentSpeed;
    const uint32_t acc = (uint32_t)a.currentAccel;
    EEPROM.put(a.ee_speed_addr, v);
    EEPROM.put(a.ee_accel_addr, acc);
    a.savedSpeed = (float)v;
    a.savedAccel = (float)acc;
    Serial.print(F("[")); Serial.print(a.name);
    Serial.print(F("] saved to EEPROM: speed ")); Serial.print(v);
    Serial.print(F(" steps/s, accel ")); Serial.print(acc);
    Serial.println(F(" steps/s² (restored at boot)"));
}

// ── Element block: kind + home-set + rated travel (own magic) ────────────
static void nvsInitElement() {
    uint32_t magic = 0;
    EEPROM.get(EE_ADDR_ELEM_MAGIC, magic);
    if (magic == NVS_ELEM_MAGIC) return;
    const uint8_t z8 = 0;
    const float   z32 = 0.0f;
    const int bases[] = { EE_ADDR_ELEM_X, EE_ADDR_ELEM_Y };
    for (int base : bases) {
        EEPROM.put(base + ELEM_OFF_KIND,    z8);
        EEPROM.put(base + ELEM_OFF_HOMESET, z8);
        EEPROM.put(base + ELEM_OFF_MAXREV,  z32);
    }
    EEPROM.put(EE_ADDR_ELEM_MAGIC, NVS_ELEM_MAGIC);
    Serial.println(F("EEPROM: initialised element block (kind = unset, no travel window)."));
}

// maxSteps is derived, never stored: recompute after any change to
// kind / maxRev so the two can't drift apart.
static void applyMaxSteps(Axis& a) {
    a.maxSteps = (kindHasStops(a.kind) && a.maxRev > 0.0f)
               ? lroundf(a.maxRev * (float)STEPS_PER_REV) : 0;
}

static void nvsLoadElement(Axis& a) {
    uint8_t k = 0, h = 0;
    float   mr = 0.0f;
    EEPROM.get(a.ee_elem_addr + ELEM_OFF_KIND,    k);
    EEPROM.get(a.ee_elem_addr + ELEM_OFF_HOMESET, h);
    EEPROM.get(a.ee_elem_addr + ELEM_OFF_MAXREV,  mr);
    a.kind    = (k < EK_COUNT) ? k : (uint8_t)EK_UNSET;
    a.homeSet = h != 0;
    a.maxRev  = (mr > 0.0f && mr < MAX_RATED_REV) ? mr : 0.0f;   // NaN / garbage → 0
    applyMaxSteps(a);
}

static void nvsSaveElement(const Axis& a) {
    const uint8_t h = a.homeSet ? 1 : 0;
    EEPROM.put(a.ee_elem_addr + ELEM_OFF_KIND,    a.kind);
    EEPROM.put(a.ee_elem_addr + ELEM_OFF_HOMESET, h);
    EEPROM.put(a.ee_elem_addr + ELEM_OFF_MAXREV,  a.maxRev);
}

// ── Z-axis block: position + speed + accel + element, one magic ──────────
// Added after the X/Y layout was already in the field; a self-contained
// block means an old board picks up Z with clean defaults and keeps all
// of its X/Y records untouched.
static void nvsInitAxisZ() {
    uint32_t magic = 0;
    EEPROM.get(EE_ADDR_Z_MAGIC, magic);
    if (magic == NVS_Z_MAGIC) return;
    const int32_t zero = 0;
    const uint8_t z8   = 0;
    const float   z32  = 0.0f;
    EEPROM.put(EE_ADDR_POS_Z,   zero);
    EEPROM.put(EE_ADDR_SPEED_Z, DEFAULT_SPEED_HZ);
    EEPROM.put(EE_ADDR_ACCEL_Z, DEFAULT_ACCEL_HZ2);
    EEPROM.put(EE_ADDR_ELEM_Z + ELEM_OFF_KIND,    z8);
    EEPROM.put(EE_ADDR_ELEM_Z + ELEM_OFF_HOMESET, z8);
    EEPROM.put(EE_ADDR_ELEM_Z + ELEM_OFF_MAXREV,  z32);
    EEPROM.put(EE_ADDR_Z_MAGIC, NVS_Z_MAGIC);
    Serial.println(F("EEPROM: initialised Z-axis block (pos 0, 800 steps/s, 25600 steps/s², element unset)."));
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
    long p = a.stepper.position();
    if (p == a.lastSavedPos) return;
    nvsSavePosition(a.ee_pos_addr, p);
    a.lastSavedPos = p;
    if (!quiet) {
        Serial.print(F("[")); Serial.print(a.name);
        Serial.print(F("] pos saved: ")); Serial.println(p);
    }
}

// Forward decl — body lives with the other homing helpers below.
static void cancelHoming(Axis& a);
// Forward decl — rotate-N job progress, defined next to the JSON writer.
static void revJobProgress(const Axis &a, float &done, float &left,
                           const char *&dir, const char *&state);

// Forget the current rotate-N job (UI progress). Called by every motion
// verb that is not itself a rotate-N, so a stale job never shows against
// unrelated motion.
static void clearRevJob(Axis& a) {
    a.revJobRevs = 0;
    a.rampActive = false;   // a different motion is about to start — drop any ramp in flight
}

// Start a bounded move with a trapezoidal speed profile. Pulses begin at
// RAMP_MIN_SPEED (or the speed setting if lower); serviceRamp() then walks
// the FlexPWM rate up at currentAccel steps/s² to currentSpeed and back
// down so the final pulses land near RAMP_MIN_SPEED. The end position is
// unaffected: the reload ISR still stops the train on the counted step.
static void startRampedMove(Axis& a, long target) {
    const float vmin = (a.currentSpeed < RAMP_MIN_SPEED) ? a.currentSpeed : RAMP_MIN_SPEED;
    a.rampSpeed  = vmin;
    a.rampLastUs = micros();
    a.rampActive = true;
    a.stepper.setSpeed((uint32_t)vmin);
    a.stepper.moveTo(target);
}

// Main-loop paced (RAMP_TICK_US). Decel starts when the remaining distance
// equals the stopping distance from the current rate (plus one tick of
// travel so a late tick errs towards decelerating early, never late).
static void serviceRamp(Axis& a) {
    if (!a.rampActive) return;
    if (!a.stepper.isRunning()) {
        a.rampActive = false;
        a.stepper.setSpeed((uint32_t)a.currentSpeed);   // leave the cruise rate armed for jog/continuous
        return;
    }
    const unsigned long now = micros();
    const unsigned long el  = now - a.rampLastUs;
    if (el < RAMP_TICK_US) return;
    a.rampLastUs = now;

    const float dt      = (float)el * 1e-6f;
    const float acc     = (a.currentAccel > 1.0f) ? a.currentAccel : 1.0f;
    const float vtarget = a.currentSpeed;
    const float vmin    = (vtarget < RAMP_MIN_SPEED) ? vtarget : RAMP_MIN_SPEED;
    const float remain  = (float)labs(a.stepper.distanceToGo());
    float v = a.rampSpeed;

    const float stopDist = (v * v - vmin * vmin) / (2.0f * acc);
    if (remain <= stopDist + v * dt) {
        v -= acc * dt; if (v < vmin) v = vmin;               // decelerate
    } else if (v < vtarget) {
        v += acc * dt; if (v > vtarget) v = vtarget;         // accelerate
    } else if (v > vtarget) {
        v = vtarget;                                         // operator lowered speed mid-move
    }
    if ((uint32_t)v != (uint32_t)a.rampSpeed) a.stepper.setSpeed((uint32_t)v);
    a.rampSpeed = v;
}

// ── Software travel window ──────────────────────────────────────────────

// The window is enforced only when the element has stops AND the
// operator has declared home since the kind was set. Before that the
// axis is free so the operator can jog it onto the real home stop.
static bool limitsActive(const Axis& a) {
    return kindHasStops(a.kind) && a.homeSet && a.maxSteps > 0;
}

// Clamp a requested absolute target into [0, maxSteps]. `hit` reports
// which bound trimmed it (-1 home, +1 max, 0 none). A move that heads
// back *into* the window from outside is always allowed, so an axis
// whose window was tightened after the fact can still be recovered.
static long clampTarget(const Axis& a, long target, int& hit) {
    hit = 0;
    if (!limitsActive(a)) return target;
    const long pos = a.stepper.position();
    if (target > a.maxSteps && target > pos) { hit = +1; return pos > a.maxSteps ? pos : a.maxSteps; }
    if (target < 0          && target < pos) { hit = -1; return pos < 0 ? pos : 0; }
    return target;
}

// Where the axis sits relative to its window (status JSON / UI).
static const char* travelState(const Axis& a) {
    if (!kindHasStops(a.kind) || a.maxSteps <= 0) return "unlimited";
    if (!a.homeSet) return "unhomed";
    const long p = a.stepper.position();
    if (p < 0)           return "below_home";
    if (p == 0)          return "home";
    if (p < a.maxSteps)  return "in_range";
    if (p == a.maxSteps) return "max";
    return "above_max";
}

static const char* clampName(int hit) { return hit > 0 ? "max" : (hit < 0 ? "home" : "none"); }

// A target far outside any plausible window; clampTarget turns it into
// "the bound in that direction" for run-to-end moves. `long` is 32-bit
// on Cortex-M, so stay well inside it.
static const long FAR_AWAY_STEPS = 1000000000L;

// ── Motion verbs (shared by Serial + HTTP) ──────────────────────────────

enum MoveResult {
    MOVE_STARTED,    // running to the requested target
    MOVE_CLAMPED,    // running, but the target was trimmed to a bound
    MOVE_AT_LIMIT,   // refused: already on the bound the move points at
    MOVE_DISABLED,   // refused: motors disabled
    MOVE_NOOP        // nothing to do (target == position)
};

// Every bounded move funnels through here so the travel window is
// enforced in exactly one place. `out_target` receives the target
// actually programmed. Trapezoidal ramp for every size of move; a
// single step is one pulse at RAMP_MIN_SPEED (or slower if the axis
// speed is set lower).
static MoveResult beginBoundedMove(Axis& a, long target, long& out_target) {
    out_target = a.stepper.position();
    if (!motorEnabled) {
        Serial.println(F("Motors disabled — enable first (E)."));
        return MOVE_DISABLED;
    }
    cancelHoming(a);
    a.continuousMode = false;
    int hit = 0;
    const long tgt = clampTarget(a, target, hit);
    a.lastClamp = (int8_t)hit;
    out_target  = tgt;
    if (tgt == a.stepper.position()) {
        if (hit) {
            Serial.print(F("[")); Serial.print(a.name);
            Serial.print(F("] REFUSED: already at ")); Serial.print(clampName(hit));
            Serial.println(F(" limit."));
            return MOVE_AT_LIMIT;
        }
        return MOVE_NOOP;
    }
    ensureDriverReady(a);
    Serial.print(F("[")); Serial.print(a.name);
    Serial.print(F("] moving to ")); Serial.print(tgt); Serial.print(F(" steps (ramped)"));
    if (hit) {
        Serial.print(F(" — requested ")); Serial.print(target);
        Serial.print(F(", clamped at ")); Serial.print(clampName(hit)); Serial.print(F(" limit"));
    }
    Serial.println();
    startRampedMove(a, tgt);
    a.lastMotionMs = millis();
    a.wasRunning   = true;
    return hit ? MOVE_CLAMPED : MOVE_STARTED;
}

// Relative jog of n signed steps: ±1 for fine tuning, ±100 classic jog,
// or any operator-entered count. Clamped to the travel window.
static MoveResult jogSteps(Axis& a, long n) {
    clearRevJob(a);
    long tgt;
    return beginBoundedMove(a, a.stepper.position() + n, tgt);
}

// Rotate n revolutions in `dir`. The rev-job progress records both the
// requested count and the target actually reachable inside the window.
static MoveResult moveRevolutions(Axis& a, int n, int dir) {
    const long start = a.stepper.position();
    long tgt;
    const MoveResult r = beginBoundedMove(a, start + (long)dir * n * STEPS_PER_REV, tgt);
    if (r == MOVE_STARTED || r == MOVE_CLAMPED) {
        a.revJobRevs      = n;
        a.revJobDir       = dir > 0 ? +1 : -1;
        a.revJobStartPos  = start;
        a.revJobTargetPos = tgt;
    }
    return r;
}

// Continuous run. On an axis with an active travel window this is a
// bounded run to the end of the window in that direction — the motor
// stops on the bound and the UI shows which one. Otherwise the classic
// unbounded pulse train.
static MoveResult startContinuous(Axis& a, int dir) {
    if (limitsActive(a)) {
        clearRevJob(a);
        long tgt;
        return beginBoundedMove(a, dir > 0 ? FAR_AWAY_STEPS : -FAR_AWAY_STEPS, tgt);
    }
    if (!motorEnabled) {
        Serial.println(F("Motors disabled — enable first (E)."));
        return MOVE_DISABLED;
    }
    cancelHoming(a);
    clearRevJob(a);
    ensureDriverReady(a);
    a.lastClamp       = 0;
    a.continuousMode  = true;
    a.continuousDir   = dir > 0 ? +1 : -1;
    a.lastReportedRev = a.stepper.position() / STEPS_PER_REV;
    a.lastContSaveMs  = millis();
    a.stepper.setSpeed((uint32_t)a.currentSpeed);
    a.stepper.runContinuous(a.continuousDir);
    Serial.print(F("[")); Serial.print(a.name);
    Serial.println(dir > 0 ? F("] continuous CW started.") : F("] continuous CCW started."));
    return MOVE_STARTED;
}

// FlexPWM is constant-velocity between ramp ticks, so stop() is
// immediate: "Stop" and "E-STOP" share this. Position is preserved.
static void stopAxis(Axis& a, const __FlashStringHelper* why) {
    cancelHoming(a);
    a.continuousMode = false;
    a.stepper.stop();
    Serial.print(F("[")); Serial.print(a.name); Serial.print(F("] ")); Serial.println(why);
}

// Serial 7/8 toggle: a second press in the same direction stops.
static void toggleContinuous(Axis& a, int dir) {
    const bool runningThisWay = a.continuousMode
        ? (a.continuousDir == dir)
        : (a.stepper.isRunning() && limitsActive(a) && a.lastClamp == dir);
    if (runningThisWay) {
        stopAxis(a, dir > 0 ? F("continuous CW stopped.") : F("continuous CCW stopped."));
        return;
    }
    startContinuous(a, dir);
}

// Declare the current position as home (0). Activates the travel
// window for elements with stops. Declare home a few steps *inside* the
// physical stop — the window bound is exactly 0.
static void zeroAxis(Axis& a) {
    cancelHoming(a);
    clearRevJob(a);
    a.stepper.stop();
    a.continuousMode = false;
    a.stepper.setPosition(0);
    a.lastClamp = 0;
    a.homeSet   = true;
    nvsSaveElement(a);
    savePositionIfChanged(a);
    Serial.print(F("[")); Serial.print(a.name);
    Serial.print(F("] position zeroed (declared home)"));
    if (limitsActive(a)) {
        Serial.print(F(" — travel window 0 .. ")); Serial.print(a.maxSteps); Serial.print(F(" steps ACTIVE"));
    } else if (kindHasStops(a.kind)) {
        Serial.print(F(" — set the rated travel (M) to activate the window"));
    }
    Serial.println(F("."));
}

// Forget home: the travel window is inactive until Z is pressed again.
// Lets the operator jog past the software 0 to find the real stop.
static void unsetHome(Axis& a) {
    a.homeSet   = false;
    a.lastClamp = 0;
    nvsSaveElement(a);
    Serial.print(F("[")); Serial.print(a.name);
    Serial.println(F("] home unset — travel window INACTIVE, jog with care."));
}

static void printElement(const Axis& a) {
    Serial.print(F("[")); Serial.print(a.name);
    Serial.print(F("] element: ")); Serial.print(kindName(a.kind));
    if (kindHasStops(a.kind)) {
        Serial.print(F(", rated travel ")); Serial.print(a.maxRev, 3);
        Serial.print(F(" rev = ")); Serial.print(a.maxSteps); Serial.print(F(" steps, home "));
        Serial.print(a.homeSet ? F("SET") : F("NOT SET"));
        Serial.println(limitsActive(a) ? F(" — window ACTIVE.")
                                       : F(" — window inactive (set home with O)."));
    } else {
        Serial.println(F(" — no stops, travel unlimited."));
    }
}

// Declare what the motor drives and, for kinds with stops, its rated
// travel in element revolutions. A kind change means a new device is on
// the shaft, so home has to be declared again. Stops the axis first.
static bool setElement(Axis& a, uint8_t kind, float maxRev) {
    if (kind >= EK_COUNT) return false;
    if (kindHasStops(kind) && !(maxRev > 0.0f && maxRev < MAX_RATED_REV)) return false;
    if (a.stepper.isRunning() || a.continuousMode) stopAxis(a, F("stopped for element change."));
    clearRevJob(a);
    if (kind != a.kind) a.homeSet = false;
    a.kind      = kind;
    a.maxRev    = kindHasStops(kind) ? maxRev : 0.0f;
    a.lastClamp = 0;
    applyMaxSteps(a);
    nvsSaveElement(a);
    printElement(a);
    return true;
}

// Non-blocking: kick off a homing move to 0 and return immediately.
// FlexPWM emits the pulse train autonomously; the reload-IRQ ticks
// position() down to 0 and stops. HTTP server is started BEFORE this
// so the browser sees the position counter decrement live during the
// homing move.
static void startHomeOnBoot(Axis& a) {
    long saved = a.stepper.position();
    if (saved == 0) {
        Serial.print(F("[")); Serial.print(a.name);
        Serial.println(F("] already at home (0)."));
        return;
    }
    Serial.print(F("[")); Serial.print(a.name);
    Serial.print(F("] homing: driving from "));
    Serial.print(saved); Serial.println(F(" steps → 0 …"));
    clearRevJob(a);
    ensureDriverReady(a);
    startRampedMove(a, 0);
    a.homing = true;
}

// Any operator-issued motion command must cancel a homing-in-progress
// before changing the target — otherwise the "homing reached 0" check
// in serviceAxis() never fires (target moved away from 0) and the
// homing flag would persist forever.
static void cancelHoming(Axis& a) {
    if (a.homing) {
        a.homing = false;
        Serial.print(F("[")); Serial.print(a.name);
        Serial.println(F("] homing cancelled by operator command."));
    }
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
    serviceRamp(a);
    // No more stepper.run() / runSpeed() calls — FlexPWM generates
    // pulses autonomously and the reload ISR keeps position() current.
    // serviceAxis is now just observability: rev counter, throttled
    // position-save, homing-complete + post-move save bookkeeping.

    if (a.continuousMode) {
        long rev = a.stepper.position() / STEPS_PER_REV;
        if (rev != a.lastReportedRev) {
            a.lastReportedRev = rev;
            Serial.print(F("[")); Serial.print(a.name);
            Serial.print(F("] Rev "));
            if (rev > 0) Serial.print('+');
            Serial.print(rev);
            Serial.print(F("  (")); Serial.print(a.stepper.position());
            Serial.println(F(" steps)"));
        }
        unsigned long now = millis();
        if (now - a.lastContSaveMs >= CONT_SAVE_INTERVAL_MS) {
            a.lastContSaveMs = now;
            savePositionIfChanged(a, true /*quiet*/);
        }
    }

    // Homing-complete detection: target must be 0 AND we must actually
    // be there. Operator-issued motion changes the target away from 0
    // and is handled by cancelHoming() at the call sites; here we only
    // succeed if the homing move completed untouched.
    if (a.homing && !a.stepper.isRunning() && a.stepper.position() == 0) {
        a.homing = false;
        savePositionIfChanged(a);
        Serial.print(F("[")); Serial.print(a.name);
        Serial.println(F("] home reached (position = 0)."));
    }

    bool nowRunning = a.continuousMode || a.stepper.isRunning();
    if (a.wasRunning && !nowRunning) {
        savePositionIfChanged(a);
        a.lastMotionMs = millis();
        // Travel-window indication: the last verb was trimmed to a bound
        // and the axis has now landed exactly on it.
        if (a.lastClamp != 0 && limitsActive(a)) {
            const long bound = a.lastClamp > 0 ? a.maxSteps : 0;
            if (a.stepper.position() == bound) {
                Serial.print(F("[")); Serial.print(a.name);
                Serial.print(F("] STOPPED AT ")); Serial.print(a.lastClamp > 0 ? F("MAX") : F("HOME"));
                Serial.print(F(" LIMIT (")); Serial.print(bound); Serial.println(F(" steps)."));
            }
        }
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
        Serial.print(F("  position    : ")); Serial.print(a.stepper.position()); Serial.println(F(" steps"));
        Serial.print(F("  speed       : ")); Serial.print(a.currentSpeed, 0);
        Serial.print(F(" steps/s (saved ")); Serial.print(a.savedSpeed, 0); Serial.println(F(")"));
        Serial.print(F("  accel       : ")); Serial.print(a.currentAccel, 0);
        Serial.print(F(" steps/s² (saved ")); Serial.print(a.savedAccel, 0); Serial.println(F(")"));
        Serial.print(F("  continuous  : "));
        if (a.continuousMode) Serial.println(a.continuousDir > 0 ? F("CW") : F("CCW"));
        else                  Serial.println(F("OFF"));
        Serial.print(F("  limit input : "));
        Serial.println(a.limitLastState == LOW ? F("ASSERTED (LOW)") : F("released (HIGH)"));
        Serial.print(F("  element     : ")); Serial.print(kindName(a.kind));
        if (kindHasStops(a.kind)) {
            Serial.print(F(", rated ")); Serial.print(a.maxRev, 3);
            Serial.print(F(" rev (")); Serial.print(a.maxSteps);
            Serial.print(F(" steps), home ")); Serial.print(a.homeSet ? F("SET") : F("NOT SET"));
        }
        Serial.println();
        Serial.print(F("  travel      : ")); Serial.print(travelState(a));
        Serial.print(F(" (")); Serial.print((float)a.stepper.position() / (float)STEPS_PER_REV, 3);
        Serial.print(F(" rev)"));
        if (a.lastClamp) { Serial.print(F(", last move bounded by ")); Serial.print(clampName(a.lastClamp)); Serial.print(F(" limit")); }
        Serial.println();
        Serial.print(F("  rotate job  : "));
        if (a.revJobRevs > 0) {
            float jdone, jleft; const char *jdir, *jstate;
            revJobProgress(a, jdone, jleft, jdir, jstate);
            Serial.print(jdone, 2); Serial.print(F(" / ")); Serial.print(a.revJobRevs);
            Serial.print(F(" revs ")); Serial.print(jdir);
            Serial.print(F(", ")); Serial.print(jleft, 2); Serial.print(F(" left ("));
            Serial.print(jstate); Serial.println(F(")"));
        } else {
            Serial.println(F("none"));
        }
    }
    Serial.println(F("──────────────────────────────────"));
}

static void printMenu() {
    Serial.println();
    Serial.println(F("=== T41 X/Y/Z Stepper Test Menu ==="));
    Serial.print  (F("(active axis: ")); Serial.print(selected->name); Serial.println(F(")"));
    Serial.println(F("  X/Y/Z  Switch active axis"));
    Serial.println(F("  1 / 2  Rotate CW / CCW 1 revolution"));
    Serial.println(F("  3 / 4  Rotate CW / CCW N revolutions (prompted)"));
    Serial.println(F("  5 / 6  Jog CW / CCW 100 steps"));
    Serial.println(F("  + / -  Single step CW / CCW (fine tuning)"));
    Serial.println(F("  7 / 8  Continuous CW / CCW (toggle)"));
    Serial.println(F("  9      Stop (decelerate)"));
    Serial.println(F("  0      EMERGENCY STOP (immediate)"));
    Serial.println(F("  S / A  Set speed / acceleration"));
    Serial.println(F("  W      Save current speed + accel to EEPROM (restored at boot)"));
    Serial.println(F("  T      Toggle ENA pin (test driver polarity)"));
    Serial.println(F("  O      Set position = home / origin (0), save to EEPROM — activates travel window"));
    Serial.println(F("  U      Unset home (travel window inactive until O)"));
    Serial.println(F("  K      Set element kind: 0 unset 1 inductor 2 vacuum cap 3 varcap w/ stops 4 varcap free 5 variometer"));
    Serial.println(F("  M      Set element rated travel (max revolutions)"));
    Serial.println(F("  E / D  Enable / disable all motors"));
    Serial.println(F("  Q      Toggle idle auto-release (global, persisted)"));
    Serial.println(F("  N      Print network status (backend, link, IP)"));
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
    Serial.println(val, 3);
    return val;
}

// ── Ethernet helpers ────────────────────────────────────────────────────

#define IP_FMT      "%u.%u.%u.%u"
#define IP_ARG(ip)  (ip)[0], (ip)[1], (ip)[2], (ip)[3]

static void bringUpEthernet() {
    net_hal::hw_mac(netMac);
    Serial.printf("[net] backend = %s\n", net_hal::lib_name());
    Serial.printf("[net] MAC     = %02X:%02X:%02X:%02X:%02X:%02X\n",
                  netMac[0], netMac[1], netMac[2], netMac[3], netMac[4], netMac[5]);

    if (!net_hal::begin()) {
        Serial.println(F("[net] begin() returned false — PHY/cable problem?"));
    }
    netLinkUp = net_hal::wait_link(ETH_LINK_TIMEOUT_MS);
    if (!netLinkUp) {
        Serial.printf("[net] no link after %lu ms — cable unplugged? continuing.\n",
                      (unsigned long)ETH_LINK_TIMEOUT_MS);
        return;
    }
    const int speed = net_hal::link_speed_mbps();
    if (speed > 0) {
        Serial.printf("[net] link  = up, %d Mbps, %s\n",
                      speed, net_hal::link_full_duplex() ? "full-duplex" : "half-duplex");
    } else {
        Serial.println(F("[net] link  = up (speed/duplex not reported by backend)"));
    }

    netDhcpOK = net_hal::wait_dhcp(ETH_DHCP_TIMEOUT_MS);
    if (netDhcpOK) {
        Serial.printf("[net] IP    = " IP_FMT "\n", IP_ARG(Ethernet.localIP()));
        Serial.printf("[net] GW    = " IP_FMT "\n", IP_ARG(Ethernet.gatewayIP()));
    } else {
        Serial.printf("[net] DHCP timed out after %lu ms — continuing without IP.\n",
                      (unsigned long)ETH_DHCP_TIMEOUT_MS);
    }
}

static void printNetStatus() {
    Serial.println(F("──── network ─────────────────────"));
    Serial.printf ("Backend : %s\n", net_hal::lib_name());
    Serial.printf ("MAC     : %02X:%02X:%02X:%02X:%02X:%02X\n",
                   netMac[0], netMac[1], netMac[2], netMac[3], netMac[4], netMac[5]);
    Serial.print  (F("Link    : "));
    Serial.println(net_hal::link_state() ? F("UP") : F("DOWN"));
    const int speed = net_hal::link_speed_mbps();
    if (speed > 0) {
        Serial.printf("          %d Mbps %s\n",
                      speed, net_hal::link_full_duplex() ? "FDX" : "HDX");
    }
    Serial.printf ("IP      : " IP_FMT "\n", IP_ARG(Ethernet.localIP()));
    Serial.printf ("Mask    : " IP_FMT "\n", IP_ARG(Ethernet.subnetMask()));
    Serial.printf ("Gateway : " IP_FMT "\n", IP_ARG(Ethernet.gatewayIP()));
    Serial.printf ("DNS     : " IP_FMT "\n", IP_ARG(Ethernet.dnsServerIP()));
    Serial.println(F("──────────────────────────────────"));
}

// ── HTTP control surface ────────────────────────────────────────────────
// Tiny GET-only API mirroring the Serial menu. The same code path that
// the Serial handler uses is reused (moveRevolutions, savePositionIfChanged,
// enableAll, etc.) so HTTP and Serial can never disagree about what a
// verb does. Jog uses a non-blocking variant (no runToPosition) so the
// HTTP handler returns immediately and the serviceAxis() loop ticks
// the move forward.

static const char INDEX_HTML[] =
R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>T41 Bench — X/Y/Z stepper</title>
<style>
*{box-sizing:border-box}
body{font-family:ui-monospace,Menlo,monospace;background:#111;color:#eee;margin:0;padding:1em;line-height:1.4}
h1{margin:0 0 .25em;font-weight:300;font-size:1.4em}
h2{margin:0 0 .35em;font-weight:300;font-size:1.1em}
.top{color:#888;font-size:.85em;margin-bottom:1em}
.top b{color:#eee}
.axis{border:1px solid #333;padding:.8em 1em;margin:.6em 0;border-radius:8px;background:#1a1a1a}
.axis.active{border-color:#4f4}
.axis.atlimit{border-color:#f44}
.row{display:flex;flex-wrap:wrap;gap:.4em;align-items:center;margin:.4em 0}
button{background:#2a2a2a;color:#eee;border:1px solid #555;padding:.45em .9em;border-radius:4px;cursor:pointer;font-family:inherit;font-size:.9em}
button:hover{background:#3a3a3a}
button.danger{background:#502020;border-color:#933}
button.danger:hover{background:#702828}
button.go{background:#1c3a1c;border-color:#494}
button.go:hover{background:#264826}
input[type=number],select{background:#222;color:#eee;border:1px solid #555;padding:.4em;border-radius:4px;width:7em;font-size:1em;font-family:inherit}
select{width:auto}
.kv{font-size:.85em;color:#aaa}
.kv b{color:#eee}
.la{color:#f44;font-weight:bold}
.lr{color:#4f4}
.sep{color:#555;margin:0 .4em}
.bdg{display:none;color:#000;padding:.1em .55em;border-radius:.4em;font-size:.7em;margin-left:.5em;vertical-align:middle;font-weight:bold}
.bdg.on{display:inline-block}
.bdg.hom{background:#fc3;animation:pulse 1s ease-in-out infinite}
.bdg.warn{background:#fc3}
.bdg.red{background:#f44;color:#fff;animation:pulse 1s ease-in-out infinite}
.bdg.run{background:#48c;color:#fff}
.bdg.dim{background:#555;color:#ddd}
.job{margin:.35em 0 .5em}
.pbar{height:8px;background:#222;border:1px solid #444;border-radius:4px;margin-top:.3em;overflow:hidden}
.fill{height:100%;width:0;background:#48c;transition:width .6s linear}
.fill.done{background:#4f4}
.fill.stopped{background:#c93}
.tfill{height:100%;width:0;background:#4a8;transition:width .6s linear}
.tfill.edge{background:#f44}
.msg{font-size:.8em;color:#fc3;min-height:1.2em}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.55}}
</style></head>
<body>
<h1>T41 Bench — X/Y/Z stepper</h1>
<div class="top">backend <b id="be">?</b> <span class="sep">·</span> ip <b id="ip">?</b> <span class="sep">·</span> link <b id="ln">?</b> <span class="sep">·</span> motors <b id="me">?</b> <span class="sep">·</span> idle-release <b id="ir">?</b></div>

<div id="axes"></div>

<div class="row">
  <button onclick="cmd('/api/enable?on=1')">Enable all</button>
  <button onclick="cmd('/api/enable?on=0')">Disable all</button>
  <button onclick="cmd('/api/release?on=1')">Idle-release ON</button>
  <button onclick="cmd('/api/release?on=0')">Idle-release OFF</button>
</div>

<script>
const KINDS=[['unset','— not set —'],['inductor','Roller inductor (stops)'],['vacuum_cap','Vacuum variable cap (stops)'],['varcap_limited','Variable cap with stops'],['varcap_free','Variable cap, free rotation'],['variometer','Variometer, free rotation']];
const LIMITED={inductor:1,vacuum_cap:1,varcap_limited:1};
let lastMsg={};
// Every verb reply lands in the axis' message line so a refused move
// ("at max limit") is visible next to the buttons, not just in Serial.
async function cmd(u,ax){try{const r=await fetch(u);const t=(await r.text()).trim();if(ax){lastMsg[ax]=(r.ok?'':'✗ ')+t;}await poll();}catch(e){}}
function jog(ax,n){if(!n||isNaN(n))return;cmd('/api/jog?axis='+ax+'&dir='+(n>0?'cw':'ccw')+'&steps='+Math.abs(n),ax);}
function jogN(ax,sgn){const v=parseInt(document.getElementById('j-'+ax).value);if(!(v>0))return;jog(ax,sgn*v);}
function rot(ax,sgn){const v=parseInt(document.getElementById('n-'+ax).value);if(!(v>0))return;cmd('/api/rotate?axis='+ax+'&revs='+v+'&dir='+(sgn>0?'cw':'ccw'),ax);}
function setSpd(ax){const el=document.getElementById('s-'+ax);if(el.value==='')return;cmd('/api/speed?axis='+ax+'&v='+el.value);}
function setAcc(ax){const el=document.getElementById('a-'+ax);if(el.value==='')return;cmd('/api/accel?axis='+ax+'&v='+el.value);}
function setElem(ax){const k=document.getElementById('k-'+ax).value;const m=document.getElementById('m-'+ax).value;
  if(LIMITED[k]&&!(parseFloat(m)>0)){lastMsg[ax]='✗ enter the rated travel (revolutions) for this element';poll();return;}
  if(LIMITED[k]&&!confirm('Set '+ax+' as '+k+' with '+m+' rev of travel?\nThe window protects the device only after you declare home (Set current pos as home) a few steps inside the physical stop.'))return;
  cmd('/api/element?axis='+ax+'&kind='+k+'&max_rev='+(m||0),ax);}
function unhome(ax){if(confirm('Unset home on '+ax+'? The travel window will be INACTIVE until you set home again.'))cmd('/api/unhome?axis='+ax,ax);}
// Refresh a field from the poll ONLY if the operator has not edited it.
function syncField(el,v){
  if(document.activeElement===el)return;
  const live=String(v);
  if(el.value===live){el.dataset.auto=live;return;}
  if(el.value===''||el.value===el.dataset.auto||el.dataset.auto===undefined){el.value=live;el.dataset.auto=live;}
}
async function poll(){try{const r=await fetch('/api/status');render(await r.json());}catch(e){}}
function render(s){
  document.getElementById('be').textContent=s.net.backend;
  document.getElementById('ip').textContent=s.net.ip;
  document.getElementById('ln').textContent=s.net.link;
  document.getElementById('me').textContent=s.motors?'ENABLED':'disabled';
  document.getElementById('ir').textContent=s.release?'ON':'OFF';
  const root=document.getElementById('axes');
  if(root.childElementCount!==s.axes.length){
    root.innerHTML='';
    for(const a of s.axes){
      const d=document.createElement('div');
      d.className='axis';d.id='ax-'+a.name;
      const opts=KINDS.map(k=>`<option value="${k[0]}">${k[1]}</option>`).join('');
      d.innerHTML=`
<h2><span class="axname">${a.name}</span> axis <span class="sel"></span><span class="bdg hom">HOMING</span><span class="bdg dim unc">NO ELEMENT SET — TRAVEL UNLIMITED</span><span class="bdg warn nohome">HOME NOT SET — WINDOW INACTIVE</span><span class="bdg run torun"></span><span class="bdg red limhit"></span></h2>
<div class="kv">pos <b class="pos">?</b> steps <span class="sep">·</span> turn <b class="trn">?</b> <span class="mxr"></span> <span class="sep">·</span> travel <b class="trv">?</b> <span class="sep">·</span> spd <b class="spd">?</b> live <b class="lsp">?</b> <span class="sep">·</span> accel <b class="acc">?</b> <span class="sep">·</span> cont <b class="con">?</b> <span class="sep">·</span> switch <span class="sw">?</span></div>
<div class="pbar twin"><div class="tfill"></div></div>
<div class="kv job">rotate job <b class="jreq">none</b> <span class="jinfo"></span><div class="pbar"><div class="fill"></div></div></div>
<div class="msg"></div>
<div class="row">
  <button onclick="cmd('/api/select?axis=${a.name}')">Select</button>
  <button onclick="jog('${a.name}',-100)">−100</button>
  <button onclick="jog('${a.name}',-10)">−10</button>
  <button onclick="jog('${a.name}',-1)">−1 step</button>
  <button onclick="jog('${a.name}',1)">+1 step</button>
  <button onclick="jog('${a.name}',10)">+10</button>
  <button onclick="jog('${a.name}',100)">+100</button>
  <input type="number" id="j-${a.name}" value="500" min="1" step="1" title="steps">
  <button onclick="jogN('${a.name}',-1)">−N steps</button>
  <button onclick="jogN('${a.name}',1)">+N steps</button>
</div>
<div class="row">
  <button onclick="cmd('/api/rotate?axis=${a.name}&revs=1&dir=ccw','${a.name}')">−1 rev</button>
  <button onclick="cmd('/api/rotate?axis=${a.name}&revs=1&dir=cw','${a.name}')">+1 rev</button>
  <input type="number" id="n-${a.name}" value="5" min="1" title="revolutions">
  <button onclick="rot('${a.name}',-1)">−N rev</button>
  <button onclick="rot('${a.name}',1)">+N rev</button>
  <button class="go" onclick="cmd('/api/continuous?axis=${a.name}&dir=ccw','${a.name}')">◀ Run CCW</button>
  <button class="go" onclick="cmd('/api/continuous?axis=${a.name}&dir=cw','${a.name}')">Run CW ▶</button>
  <button onclick="cmd('/api/stop?axis=${a.name}')">Stop</button>
  <button class="danger" onclick="cmd('/api/estop?axis=${a.name}')">E-STOP</button>
</div>
<div class="row">
  element <select id="k-${a.name}">${opts}</select>
  rated travel <input type="number" id="m-${a.name}" min="0" step="0.5" title="revolutions"> rev
  <button onclick="setElem('${a.name}')">Set element</button>
  <button onclick="cmd('/api/zero?axis=${a.name}','${a.name}')">Set current pos as home</button>
  <button onclick="unhome('${a.name}')">Unset home</button>
</div>
<div class="row">
  spd <input type="number" id="s-${a.name}" min="1" max="200000" step="50" onkeydown="if(event.key==='Enter')setSpd('${a.name}')"> <button onclick="setSpd('${a.name}')">Set</button>
  <button onclick="cmd('/api/save_speed?axis=${a.name}')">Save spd+acc</button> <span class="kv">saved <b class="ssp">?</b></span>
  acc <input type="number" id="a-${a.name}" min="1" step="50" onkeydown="if(event.key==='Enter')setAcc('${a.name}')"> <button onclick="setAcc('${a.name}')">Set</button>
</div>`;
      root.appendChild(d);
    }
  }
  for(const a of s.axes){
    const d=document.getElementById('ax-'+a.name);
    if(!d)continue;
    const e=a.element||{kind:'unset',travel:'unlimited',last_clamp:'none'};
    const lim=!!e.limited, hasStops=!!LIMITED[e.kind];
    d.classList.toggle('active',a.selected);
    d.querySelector('.sel').textContent=a.selected?'(active)':'';
    d.querySelector('.hom').classList.toggle('on',!!a.homing);
    d.querySelector('.unc').classList.toggle('on',e.kind==='unset');
    d.querySelector('.nohome').classList.toggle('on',hasStops&&!e.home_set);
    // Travel-window badges: latched "stopped at X", running-to-X, or plain at-bound.
    const atBound=(e.travel==='home'||e.travel==='max'||e.travel==='below_home'||e.travel==='above_max');
    const hit=d.querySelector('.limhit'),torun=d.querySelector('.torun');
    let showHit=false,showRun=false;
    if(lim&&e.last_clamp!=='none'){
      if(a.running){torun.textContent='→ running to '+e.last_clamp.toUpperCase()+' limit';showRun=true;}
      else if(e.travel===e.last_clamp){hit.textContent='STOPPED AT '+e.last_clamp.toUpperCase()+' LIMIT';showHit=true;}
      else if(e.travel==='below_home'||e.travel==='above_max'){hit.textContent='OUTSIDE WINDOW ('+e.travel.replace('_',' ')+') — only moves back in are allowed';showHit=true;}
      else{torun.textContent='run to '+e.last_clamp.toUpperCase()+' limit interrupted';showRun=true;}
    }else if(lim&&(e.travel==='home'||e.travel==='max')){hit.textContent='AT '+e.travel.toUpperCase();showHit=true;}
    else if(lim&&(e.travel==='below_home'||e.travel==='above_max')){hit.textContent='OUTSIDE WINDOW ('+e.travel.replace('_',' ')+') — only moves back in are allowed';showHit=true;}
    hit.classList.toggle('on',showHit);torun.classList.toggle('on',showRun);
    d.classList.toggle('atlimit',showHit);
    d.querySelector('.pos').textContent=a.position;
    d.querySelector('.trn').textContent=(e.turns!==undefined?e.turns.toFixed(3):'?');
    d.querySelector('.mxr').textContent=hasStops?'/ '+e.max_rev.toFixed(2)+' rev':'rev (no stops)';
    const trv=d.querySelector('.trv');trv.textContent=e.travel.replace('_',' ');
    trv.style.color=(lim&&atBound)?'#f44':(e.travel==='unhomed'?'#fc3':'#eee');
    const tw=d.querySelector('.twin'),tf=d.querySelector('.tfill');
    if(hasStops&&e.max_steps>0){tw.style.display='block';const f=Math.max(0,Math.min(1,a.position/e.max_steps));tf.style.width=(100*f)+'%';tf.className='tfill'+((lim&&atBound)?' edge':'');}
    else tw.style.display='none';
    d.querySelector('.spd').textContent=a.speed;
    d.querySelector('.lsp').textContent=a.live_speed;
    const ssp=d.querySelector('.ssp'); ssp.textContent=a.saved_speed+' / '+a.saved_accel;
    ssp.style.color=(a.saved_speed==a.speed&&a.saved_accel==a.accel)?'#4f4':'#fc3';
    d.querySelector('.acc').textContent=a.accel;
    d.querySelector('.con').textContent=a.continuous;
    const sw=d.querySelector('.sw');
    sw.textContent=a.limit;
    sw.className='sw '+(a.limit==='ASSERTED'?'la':'lr');
    const j=a.rev_job||{revs:0};
    const jr=d.querySelector('.jreq'),ji=d.querySelector('.jinfo'),jf=d.querySelector('.fill');
    if(j.revs>0){
      const tot=(j.eff>0)?j.eff:j.revs;
      const eff=(j.eff!==undefined&&j.eff<j.revs-0.005)?' (window limits it to '+j.eff.toFixed(2)+')':'';
      jr.textContent=j.done.toFixed(2)+' / '+j.revs+' revs '+j.dir+eff;
      ji.textContent=j.state==='running'?'· '+j.left.toFixed(2)+' to go'
                    :j.state==='done'?'· complete':'· stopped, '+j.left.toFixed(2)+' left';
      jf.style.width=Math.min(100,100*j.done/tot)+'%';
      jf.className='fill '+j.state;
    }else{jr.textContent='none';ji.textContent='';jf.style.width='0';jf.className='fill';}
    d.querySelector('.msg').textContent=lastMsg[a.name]||'';
    syncField(document.getElementById('s-'+a.name),a.speed);
    syncField(document.getElementById('a-'+a.name),a.accel);
    const ks=document.getElementById('k-'+a.name);
    if(document.activeElement!==ks){
      if(ks.value===e.kind){ks.dataset.auto=e.kind;}
      else if(ks.dataset.auto===undefined||ks.value===ks.dataset.auto){ks.value=e.kind;ks.dataset.auto=e.kind;}
    }
    syncField(document.getElementById('m-'+a.name),hasStops?e.max_rev:'');
  }
}
// 1000 ms poll. Pulses are hardware-generated (FlexPWM) so polling no
// longer stutters the motor, but each request still costs a few ms of
// main-loop time in write+flush.
poll();setInterval(poll,1000);
</script>
</body></html>)HTML";

// ---- URL / query helpers ------------------------------------------------

// Extract value of `key` from a query string ("axis=X&dir=cw"). Writes
// up to outsz-1 chars (NUL-terminated). Returns true if found.
static bool getParam(const char *query, const char *key, char *out, size_t outsz) {
    if (!query || !out || outsz == 0) return false;
    out[0] = '\0';
    const size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < outsz) out[i++] = *p++;
            out[i] = '\0';
            return true;
        }
        // skip to next & or end
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return false;
}

static Axis *findAxis(const char *name) {
    if (!name || !*name) return nullptr;
    for (int i = 0; i < NUM_AXES; i++) {
        if (axes[i]->name[0] == name[0] || axes[i]->name[0] == (name[0] ^ 0x20)) {
            return axes[i];
        }
    }
    return nullptr;
}

// ---- HTTP response helpers ----------------------------------------------

// Every write goes through net_hal::write_all: chunked below FNET's 2 KB
// per-socket send buffer and flushed between chunks, because
// NativeEthernet's socketSend() busy-waits forever on a single write of
// ≥ that size (QNEthernet can return short too). The bench found this;
// the helper now lives in the shared library so every sketch gets it.
using net_hal::write_all;

static void httpSendHeader(EthernetClient &c, int code, const char *status,
                           const char *ctype, int contentLen) {
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     code, status, ctype, contentLen);
    write_all(c, hdr, n);
}

static void httpSendText(EthernetClient &c, int code, const char *status,
                         const char *body) {
    const int n = (int)strlen(body);
    httpSendHeader(c, code, status, "text/plain; charset=utf-8", n);
    write_all(c, body, n);
}

static void httpServeIndex(EthernetClient &c) {
    const int n = (int)(sizeof(INDEX_HTML) - 1);
    httpSendHeader(c, 200, "OK", "text/html; charset=utf-8", n);
    write_all(c, INDEX_HTML, n);
}

// Rotate-N job progress for one axis. done/left in revolutions (float);
// state: none | running | done | stopped.
static void revJobProgress(const Axis &a, float &done, float &left,
                           const char *&dir, const char *&state) {
    done = 0.0f; left = 0.0f; dir = "-"; state = "none";
    if (a.revJobRevs <= 0) return;
    const long pos   = a.stepper.position();
    long moved = labs(pos - a.revJobStartPos);
    const long total = labs(a.revJobTargetPos - a.revJobStartPos);
    if (moved > total) moved = total;
    done  = (float)moved / (float)STEPS_PER_REV;
    left  = (float)(total - moved) / (float)STEPS_PER_REV;
    dir   = a.revJobDir > 0 ? "CW" : "CCW";
    state = a.stepper.isRunning() ? "running"
          : (pos == a.revJobTargetPos ? "done" : "stopped");
}

static void httpServeStatusJson(EthernetClient &c) {
    char json[3072];   // ~600 B per axis with the element block; 3 axes + header
    const IPAddress ip = Ethernet.localIP();
    int n = snprintf(json, sizeof(json),
        "{\"net\":{\"backend\":\"%s\",\"link\":\"%s\",\"ip\":\"%u.%u.%u.%u\"},"
        "\"motors\":%s,\"release\":%s,\"axes\":[",
        net_hal::lib_name(),
        net_hal::link_state() ? "up" : "down",
        ip[0], ip[1], ip[2], ip[3],
        motorEnabled ? "true" : "false",
        idleAutoRelease ? "true" : "false");
    for (int i = 0; i < NUM_AXES && n < (int)sizeof(json); i++) {
        const Axis &a = *axes[i];
        const char *cont = a.continuousMode ? (a.continuousDir > 0 ? "CW" : "CCW") : "OFF";
        const char *lim  = a.limitLastState == LOW ? "ASSERTED" : "released";
        float jdone, jleft; const char *jdir, *jstate;
        revJobProgress(a, jdone, jleft, jdir, jstate);
        const long   pos  = a.stepper.position();
        const double jeff = (double)labs(a.revJobTargetPos - a.revJobStartPos) / (double)STEPS_PER_REV;
        n += snprintf(json + n, sizeof(json) - n,
            "%s{\"name\":\"%s\",\"position\":%ld,\"speed\":%.0f,\"accel\":%.0f,\"saved_speed\":%.0f,\"saved_accel\":%.0f,\"live_speed\":%lu,"
            "\"continuous\":\"%s\",\"limit\":\"%s\",\"selected\":%s,\"homing\":%s,\"running\":%s,"
            "\"element\":{\"kind\":\"%s\",\"kind_id\":%u,\"limited\":%s,\"max_rev\":%.3f,\"max_steps\":%ld,"
            "\"home_set\":%s,\"travel\":\"%s\",\"last_clamp\":\"%s\",\"turns\":%.3f},"
            "\"rev_job\":{\"revs\":%d,\"eff\":%.2f,\"dir\":\"%s\",\"done\":%.2f,\"left\":%.2f,\"state\":\"%s\"}}",
            i > 0 ? "," : "",
            a.name, pos, a.currentSpeed, a.currentAccel, a.savedSpeed,
            a.savedAccel, (unsigned long)a.stepper.speed(),
            cont, lim, (&a == selected) ? "true" : "false",
            a.homing ? "true" : "false",
            a.stepper.isRunning() ? "true" : "false",
            kindName(a.kind), (unsigned)a.kind, limitsActive(a) ? "true" : "false",
            (double)a.maxRev, a.maxSteps,
            a.homeSet ? "true" : "false", travelState(a), clampName(a.lastClamp),
            (double)pos / (double)STEPS_PER_REV,
            a.revJobRevs, a.revJobRevs > 0 ? jeff : 0.0, jdir, (double)jdone, (double)jleft, jstate);
    }
    n += snprintf(json + n, sizeof(json) - n, "]}");
    httpSendHeader(c, 200, "OK", "application/json", n);
    write_all(c, json, n);
}

// ---- HTTP motion reply --------------------------------------------------
// HTTP verbs call the same jogSteps / moveRevolutions / startContinuous
// as the Serial menu; this maps the shared MoveResult onto a reply. A
// clamped move is 200 with the bound named (the UI shows it in the axis
// message line); a move refused at a bound is 409 so it stands out.

static void httpMoveReply(EthernetClient &c, const Axis &a, MoveResult r, const char *verb) {
    char body[80];
    switch (r) {
    case MOVE_STARTED:
        snprintf(body, sizeof(body), "%s\n", verb);
        httpSendText(c, 200, "OK", body); return;
    case MOVE_CLAMPED:
        snprintf(body, sizeof(body), "%s: travel window - will stop at %s limit\n", verb, clampName(a.lastClamp));
        httpSendText(c, 200, "OK", body); return;
    case MOVE_AT_LIMIT:
        snprintf(body, sizeof(body), "refused: already at %s limit\n", clampName(a.lastClamp));
        httpSendText(c, 409, "Conflict", body); return;
    case MOVE_DISABLED:
        httpSendText(c, 409, "Conflict", "motors disabled\n"); return;
    case MOVE_NOOP:
        snprintf(body, sizeof(body), "%s: already there\n", verb);
        httpSendText(c, 200, "OK", body); return;
    }
    httpSendText(c, 500, "Internal Server Error", "unknown move result\n");
}

// ---- Dispatcher --------------------------------------------------------

static void httpDispatch(EthernetClient &c, const char *path, const char *query) {
    Serial.print(F("[http] ")); Serial.print(path);
    if (*query) { Serial.print('?'); Serial.print(query); }
    Serial.println();

    if (strcmp(path, "/") == 0) { httpServeIndex(c); return; }
    if (strcmp(path, "/api/status") == 0) { httpServeStatusJson(c); return; }

    char axisName[4];
    char dirStr[8];
    char valStr[16];

    if (strcmp(path, "/api/select") == 0) {
        if (getParam(query, "axis", axisName, sizeof(axisName))) {
            Axis *a = findAxis(axisName);
            if (a) { selected = a; httpSendText(c, 200, "OK", "selected\n"); return; }
        }
        httpSendText(c, 400, "Bad Request", "bad axis\n"); return;
    }

    // /api/jog?axis=X&dir=cw|ccw[&steps=N]  — N defaults to 100; N = 1 is
    // the fine-tuning single step. Clamped to the travel window.
    if (strcmp(path, "/api/jog") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        getParam(query, "dir", dirStr, sizeof(dirStr));
        int dir = (strcmp(dirStr, "cw") == 0) ? +1 : (strcmp(dirStr, "ccw") == 0) ? -1 : 0;
        long steps = 100;
        if (getParam(query, "steps", valStr, sizeof(valStr))) steps = atol(valStr);
        if (!a || dir == 0 || steps <= 0 || steps > 100000000L) { httpSendText(c, 400, "Bad Request", "bad axis/dir/steps\n"); return; }
        httpMoveReply(c, *a, jogSteps(*a, (long)dir * steps), "jog"); return;
    }

    if (strcmp(path, "/api/rotate") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        getParam(query, "dir", dirStr, sizeof(dirStr));
        getParam(query, "revs", valStr, sizeof(valStr));
        int dir = (strcmp(dirStr, "cw") == 0) ? +1 : (strcmp(dirStr, "ccw") == 0) ? -1 : 0;
        int n   = atoi(valStr);
        if (!a || dir == 0 || n <= 0) { httpSendText(c, 400, "Bad Request", "bad args\n"); return; }
        httpMoveReply(c, *a, moveRevolutions(*a, n, dir), "rotate"); return;
    }

    if (strcmp(path, "/api/continuous") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        getParam(query, "dir", dirStr, sizeof(dirStr));
        int dir;
        if      (strcmp(dirStr, "cw")   == 0) dir = +1;
        else if (strcmp(dirStr, "ccw")  == 0) dir = -1;
        else if (strcmp(dirStr, "stop") == 0) dir =  0;
        else { httpSendText(c, 400, "Bad Request", "bad dir\n"); return; }
        if (!a) { httpSendText(c, 400, "Bad Request", "bad axis\n"); return; }
        if (dir == 0) { stopAxis(*a, F("continuous stopped.")); httpSendText(c, 200, "OK", "stopped\n"); return; }
        httpMoveReply(c, *a, startContinuous(*a, dir), limitsActive(*a) ? "run to end" : "continuous"); return;
    }

    if (strcmp(path, "/api/stop") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        if (!a) { httpSendText(c, 400, "Bad Request", "bad axis\n"); return; }
        stopAxis(*a, F("STOP."));
        httpSendText(c, 200, "OK", "stop\n"); return;
    }

    if (strcmp(path, "/api/estop") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        if (!a) { httpSendText(c, 400, "Bad Request", "bad axis\n"); return; }
        stopAxis(*a, F("EMERGENCY STOP."));   // FlexPWM is constant-velocity: stop() === e-stop
        httpSendText(c, 200, "OK", "estop\n"); return;
    }

    if (strcmp(path, "/api/zero") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        if (!a) { httpSendText(c, 400, "Bad Request", "bad axis\n"); return; }
        zeroAxis(*a);
        httpSendText(c, 200, "OK", limitsActive(*a) ? "home set - travel window active\n"
                                                    : "home set\n"); return;
    }

    // /api/unhome?axis=X — forget home; window inactive until /api/zero.
    if (strcmp(path, "/api/unhome") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        if (!a) { httpSendText(c, 400, "Bad Request", "bad axis\n"); return; }
        unsetHome(*a);
        httpSendText(c, 200, "OK", "home unset - travel window INACTIVE\n"); return;
    }

    // /api/element?axis=X&kind=<id|name>[&max_rev=F] — declare the element
    // on the shaft. Kinds with stops need max_rev > 0 (revolutions).
    if (strcmp(path, "/api/element") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        char kindStr[20];
        if (!a || !getParam(query, "kind", kindStr, sizeof(kindStr))) { httpSendText(c, 400, "Bad Request", "bad axis/kind\n"); return; }
        const int k = parseKind(kindStr);
        if (k < 0) { httpSendText(c, 400, "Bad Request", "unknown kind\n"); return; }
        float mr = 0.0f;
        if (getParam(query, "max_rev", valStr, sizeof(valStr))) mr = strtof(valStr, nullptr);
        if (!setElement(*a, (uint8_t)k, mr)) { httpSendText(c, 400, "Bad Request", "kinds with stops need max_rev > 0\n"); return; }
        httpSendText(c, 200, "OK", kindHasStops((uint8_t)k)
            ? (a->homeSet ? "element set - travel window active\n"
                          : "element set - now declare home to activate the window\n")
            : "element set - no stops, travel unlimited\n"); return;
    }

    if (strcmp(path, "/api/save_speed") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        if (!a) { httpSendText(c, 400, "Bad Request", "bad axis\n"); return; }
        saveSpeed(*a);
        httpSendText(c, 200, "OK", "speed saved\n"); return;
    }

    if (strcmp(path, "/api/speed") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        getParam(query, "v", valStr, sizeof(valStr));
        float v = strtof(valStr, nullptr);
        if (!a || v <= 0) { httpSendText(c, 400, "Bad Request", "bad args\n"); return; }
        if (v > 200000.0f) v = 200000.0f;   // FlexPwmStepper::setSpeed ceiling — keep status honest
        a->currentSpeed = v;
        if (!a->rampActive) a->stepper.setSpeed((uint32_t)v);   // mid-ramp: serviceRamp converges instead
        httpSendText(c, 200, "OK", "speed\n"); return;
    }

    if (strcmp(path, "/api/accel") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        getParam(query, "v", valStr, sizeof(valStr));
        float v = strtof(valStr, nullptr);
        if (!a || v <= 0) { httpSendText(c, 400, "Bad Request", "bad args\n"); return; }
        a->currentAccel = v;
        // accel is a no-op on FlexPwmStepper (constant velocity); we
        // still store currentAccel for display in /api/status + UI.
        httpSendText(c, 200, "OK", "accel\n"); return;
    }

    if (strcmp(path, "/api/enable") == 0) {
        getParam(query, "on", valStr, sizeof(valStr));
        enableAll(strcmp(valStr, "1") == 0);
        httpSendText(c, 200, "OK", "enable\n"); return;
    }

    if (strcmp(path, "/api/release") == 0) {
        getParam(query, "on", valStr, sizeof(valStr));
        bool on = strcmp(valStr, "1") == 0;
        idleAutoRelease = on;
        nvsSaveRelease(on);
        if (!on) for (int i = 0; i < NUM_AXES; i++) ensureDriverReady(*axes[i]);
        httpSendText(c, 200, "OK", "release\n"); return;
    }

    httpSendText(c, 404, "Not Found", "no such route\n");
}

// ---- Request reader ----------------------------------------------------

// Read the request line into `line` (NUL-terminated). Drains the rest
// of the headers (up to the blank line). Returns true if a request line
// was read; false on timeout. CR bytes are stripped; the line ends at
// the first \n.
static bool httpReadRequest(EthernetClient &c, char *line, size_t linesz) {
    const unsigned long start = millis();
    size_t i = 0;
    bool gotLF = false;
    while (c.connected() && (millis() - start) < 500) {
        if (!c.available()) { delay(1); continue; }
        int b = c.read();
        if (b < 0) break;
        if (b == '\r') continue;
        if (b == '\n') { gotLF = true; break; }
        if (i + 1 < linesz) line[i++] = (char)b;
    }
    line[i] = '\0';
    if (!gotLF) return false;
    // Drain headers. State: 1 = previous char was an unescaped \n; if we
    // see another \n immediately we've hit the blank line that ends the
    // header block.
    int state = 1;  // the request-line \n we just consumed counts
    while (c.connected() && (millis() - start) < 500) {
        if (!c.available()) { delay(1); continue; }
        int b = c.read();
        if (b < 0) break;
        if (b == '\r') continue;
        if (b == '\n') {
            if (state == 1) return true;  // blank line → headers done
            state = 1;
        } else {
            state = 0;
        }
    }
    return true;  // soft-timeout: still try to dispatch
}

static void httpPoll() {
    if (!httpReady) return;
    EthernetClient client = httpServer.accept();
    if (!client) return;

    char line[160];
    if (!httpReadRequest(client, line, sizeof(line))) {
        client.stop();
        return;
    }

    // Parse "GET /path?query HTTP/1.1"
    char *method = line;
    char *path = strchr(line, ' ');
    if (!path) { client.stop(); return; }
    *path++ = '\0';
    char *httpver = strchr(path, ' ');
    if (httpver) *httpver = '\0';
    char *query = strchr(path, '?');
    if (query) { *query++ = '\0'; } else { query = (char *)""; }

    if (strcmp(method, "GET") != 0) {
        httpSendText(client, 405, "Method Not Allowed", "GET only\n");
    } else {
        httpDispatch(client, path, query);
    }
    // CRITICAL: flush before stop. NativeEthernet's EthernetClient::
    // stop() does socketDisconnect + socketClose immediately and
    // discards anything still in FNET's send buffer — so a fully-
    // written response is dropped on the floor and the client (browser
    // / curl) sees a clean TCP close with 0 bytes. flush() blocks
    // until all outgoing bytes are ACK'd by the peer. QNEthernet
    // doesn't have the same bug but the flush is harmless there.
    client.flush();
    client.stop();
}

// ── setup / loop ────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);   // Teensy 4.1 USB CDC: let the host enumerate

    nvsInit();
    nvsInitSpeed();
    nvsInitElement();
    nvsInitAxisZ();
    idleAutoRelease = nvsLoadRelease();

    for (int i = 0; i < NUM_AXES; i++) {
        Axis& a = *axes[i];
        pinMode(a.pin_en, OUTPUT);
        pinMode(a.pin_limit, INPUT_PULLUP);
        a.limitLastState = digitalRead(a.pin_limit);

        // FlexPWM submodule init + ISR vector wiring. Pulse width is
        // fixed at 50% duty by FlexPWM (no setMinPulseWidth needed);
        // acceleration is not supported (constant velocity).
        a.stepper.init();
        a.currentSpeed = (float)nvsLoadSpeed(a.ee_speed_addr);
        a.currentAccel = (float)nvsLoadAccel(a.ee_accel_addr);
        a.savedSpeed   = a.currentSpeed;
        a.savedAccel   = a.currentAccel;
        a.stepper.setSpeed((uint32_t)a.currentSpeed);

        long savedPos = nvsLoadPosition(a.ee_pos_addr);
        a.stepper.setPosition(savedPos);
        a.lastSavedPos = savedPos;

        nvsLoadElement(a);
    }
    enableAll(true);

    Serial.println();
    Serial.println(F("Teensy 4.1 / T41 V2.09 — X+Y+Z stepper test"));
    for (int i = 0; i < NUM_AXES; i++) {
        const Axis& a = *axes[i];
        Serial.print(a.name);
        Serial.print(F(": STEP="));  Serial.print(a.pin_step);
        Serial.print(F("  DIR="));   Serial.print(a.pin_dir);
        Serial.print(F("  EN="));    Serial.print(a.pin_en);
        Serial.print(F("  LIMIT=")); Serial.print(a.pin_limit);
        Serial.print(F("  speed=")); Serial.print(a.currentSpeed, 0);
        Serial.print(F(" steps/s  accel=")); Serial.print(a.currentAccel, 0);
        Serial.println(F(" steps/s² (EEPROM)"));
    }
    Serial.print(F("Steps/rev: ")); Serial.println(STEPS_PER_REV);
    Serial.print(F("Idle auto-release: "));
    Serial.println(idleAutoRelease ? F("ON  (silent at rest)") : F("OFF (motor holds with current)"));
    for (int i = 0; i < NUM_AXES; i++) printElement(*axes[i]);

    // Ethernet + HTTP BEFORE homing so the browser sees position
    // counters decrement live during the boot-time homing move,
    // rather than freezing on the pre-reboot value until homing
    // completes. startHomeOnBoot below is non-blocking — the main
    // loop's serviceAxis() ticks the move forward.
    bringUpEthernet();
    if (netDhcpOK) {
        httpServer.begin();
        httpReady = true;
        Serial.printf("[http] listening on http://%u.%u.%u.%u/\n",
                      Ethernet.localIP()[0], Ethernet.localIP()[1],
                      Ethernet.localIP()[2], Ethernet.localIP()[3]);
    } else {
        Serial.println(F("[http] not started (no DHCP)."));
    }

    for (int i = 0; i < NUM_AXES; i++) startHomeOnBoot(*axes[i]);

    printMenu();
}

void loop() {
    // Service both axes every loop iteration.
    for (int i = 0; i < NUM_AXES; i++) serviceAxis(*axes[i]);
    pollLimits();
    httpPoll();

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
    case 'Z': case 'z':
        selected = &zAxis;
        Serial.println(F("Active axis: Z"));
        break;

    // Motion verbs share jogSteps / moveRevolutions / startContinuous /
    // stopAxis with the HTTP dispatcher; the travel window is enforced
    // inside beginBoundedMove for all of them.
    case '1': moveRevolutions(a, 1, +1); break;
    case '2': moveRevolutions(a, 1, -1); break;
    case '3': {
        int n = readIntFromSerial();
        if (n > 0) moveRevolutions(a, n, +1);
        break;
    }
    case '4': {
        int n = readIntFromSerial();
        if (n > 0) moveRevolutions(a, n, -1);
        break;
    }
    case '5': jogSteps(a, +100); break;
    case '6': jogSteps(a, -100); break;
    case '+': case '=': jogSteps(a, +1); break;   // '=' is the unshifted '+' key
    case '-': case '_': jogSteps(a, -1); break;
    case '7': toggleContinuous(a, +1); break;
    case '8': toggleContinuous(a, -1); break;
    case '9':
        // FlexPWM constant-velocity: stop() === e-stop. No decel ramp
        // to do (kept verb name for menu familiarity).
        stopAxis(a, F("STOP."));
        break;
    case '0':
        stopAxis(a, F("EMERGENCY STOP."));
        break;

    case 'S': case 's': {
        float spd = readFloatFromSerial();
        if (spd > 0) {
            if (spd > 200000.0f) spd = 200000.0f;   // FlexPwmStepper ceiling
            a.currentSpeed = spd;
            if (!a.rampActive) a.stepper.setSpeed((uint32_t)a.currentSpeed);
            Serial.print(F("[")); Serial.print(a.name);
            Serial.print(F("] speed = ")); Serial.print(a.currentSpeed, 0); Serial.println(F(" steps/s"));
        }
        break;
    }
    case 'A': case 'a': {
        float acc = readFloatFromSerial();
        if (acc > 0) {
            a.currentAccel = acc;
            // used by the trapezoidal ramp on rotate-N / homing moves
            Serial.print(F("[")); Serial.print(a.name);
            Serial.print(F("] accel = ")); Serial.print(a.currentAccel, 0); Serial.println(F(" steps/s²"));
        }
        break;
    }
    case 'W': case 'w': saveSpeed(a); break;
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
    case 'O': case 'o': zeroAxis(a); break;   // home / origin ('Z' selects the Z axis)
    case 'U': case 'u': unsetHome(a); break;
    case 'K': case 'k': {
        Serial.println(F("Element kind: 0 unset  1 roller inductor  2 vacuum-variable cap  3 variable cap with stops  4 variable cap free  5 variometer"));
        int k = readIntFromSerial();
        if (k < 0 || k >= EK_COUNT) { Serial.println(F("bad kind")); break; }
        float mr = a.maxRev;
        if (kindHasStops((uint8_t)k)) {
            Serial.println(F("Rated travel of the element in revolutions (e.g. 40 for a 40-turn vacuum cap, 0.5 for a 180° air variable):"));
            mr = readFloatFromSerial();
        }
        if (!setElement(a, (uint8_t)k, mr)) Serial.println(F("rejected — kinds with stops need a rated travel > 0"));
        break;
    }
    case 'M': case 'm': {
        if (!kindHasStops(a.kind)) { Serial.println(F("This element kind has no stops — set the kind first (K).")); break; }
        Serial.println(F("Rated travel in revolutions:"));
        float mr = readFloatFromSerial();
        if (!setElement(a, a.kind, mr)) Serial.println(F("rejected — rated travel must be > 0"));
        break;
    }

    case 'N': case 'n': printNetStatus(); break;

    case '?':           printStatus(); break;
    case 'H': case 'h': printMenu();   break;
    default: break;
    }
}
