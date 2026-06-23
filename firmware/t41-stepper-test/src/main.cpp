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
//   N        Print network status (Ethernet backend, link, IP)
//   ?        Print status (both axes + limits)
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
//     menu (jog ±100, ±1/N revs, continuous CW/CCW, stop, e-stop,
//     speed/accel, enable/disable, idle-release toggle, zero/home).
//   - JSON status at /api/status (polled by the UI every 500 ms).
//   - GET-only verb endpoints under /api/* — same code paths as the
//     Serial handlers, so HTTP and Serial can never disagree.
//   - LAN-only, no auth (same posture as LP-100A-Server).
//   - HTTP-issued jog uses a non-blocking variant (no runToPosition)
//     so the response returns immediately; serviceAxis() ticks the
//     move forward in the main loop.
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

// Ethernet HAL shim — pick QNEthernet (lwIP) or NativeEthernet (FNET)
// at compile time via the env's build_flags. Same source compiles
// against either backend. See firmware/lib/net_hal/.
#include "net_hal.h"

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
    Serial.println(val, 1);
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
<title>T41 Bench — X/Y stepper</title>
<style>
*{box-sizing:border-box}
body{font-family:ui-monospace,Menlo,monospace;background:#111;color:#eee;margin:0;padding:1em;line-height:1.4}
h1{margin:0 0 .25em;font-weight:300;font-size:1.4em}
h2{margin:0 0 .35em;font-weight:300;font-size:1.1em}
.bar{color:#888;font-size:.85em;margin-bottom:1em}
.bar b{color:#eee}
.axis{border:1px solid #333;padding:.8em 1em;margin:.6em 0;border-radius:8px;background:#1a1a1a}
.axis.active{border-color:#4f4}
.row{display:flex;flex-wrap:wrap;gap:.4em;align-items:center;margin:.4em 0}
button{background:#2a2a2a;color:#eee;border:1px solid #555;padding:.45em .9em;border-radius:4px;cursor:pointer;font-family:inherit;font-size:.9em}
button:hover{background:#3a3a3a}
button.danger{background:#502020;border-color:#933}
button.danger:hover{background:#702828}
button.go{background:#1c3a1c;border-color:#494}
button.go:hover{background:#264826}
input[type=number]{background:#222;color:#eee;border:1px solid #555;padding:.4em;border-radius:4px;width:4.5em;font-family:inherit}
.kv{font-size:.85em;color:#aaa}
.kv b{color:#eee}
.la{color:#f44;font-weight:bold}
.lr{color:#4f4}
.sep{color:#555;margin:0 .4em}
</style></head>
<body>
<h1>T41 Bench — X/Y stepper</h1>
<div class="bar">backend <b id="be">?</b> <span class="sep">·</span> ip <b id="ip">?</b> <span class="sep">·</span> link <b id="ln">?</b> <span class="sep">·</span> motors <b id="me">?</b> <span class="sep">·</span> idle-release <b id="ir">?</b></div>

<div id="axes"></div>

<div class="row">
  <button onclick="cmd('/api/enable?on=1')">Enable all</button>
  <button onclick="cmd('/api/enable?on=0')">Disable all</button>
  <button onclick="cmd('/api/release?on=1')">Idle-release ON</button>
  <button onclick="cmd('/api/release?on=0')">Idle-release OFF</button>
</div>

<script>
async function cmd(u){try{await fetch(u);await poll();}catch(e){}}
async function poll(){
  try{
    const r=await fetch('/api/status');
    const s=await r.json();
    render(s);
  }catch(e){}
}
function render(s){
  document.getElementById('be').textContent=s.net.backend;
  document.getElementById('ip').textContent=s.net.ip;
  document.getElementById('ln').textContent=s.net.link;
  document.getElementById('me').textContent=s.motors?'ENABLED':'disabled';
  document.getElementById('ir').textContent=s.release?'ON':'OFF';
  const root=document.getElementById('axes');
  // Build only once; update text after.
  if(root.childElementCount!==s.axes.length){
    root.innerHTML='';
    for(const a of s.axes){
      const d=document.createElement('div');
      d.className='axis';d.id='ax-'+a.name;
      d.innerHTML=`
<h2><span class="axname">${a.name}</span> axis <span class="sel"></span></h2>
<div class="kv">pos <b class="pos">?</b> steps <span class="sep">·</span> spd <b class="spd">?</b> <span class="sep">·</span> accel <b class="acc">?</b> <span class="sep">·</span> cont <b class="con">?</b> <span class="sep">·</span> limit <span class="lim">?</span></div>
<div class="row">
  <button onclick="cmd('/api/select?axis=${a.name}')">Select</button>
  <button onclick="cmd('/api/jog?axis=${a.name}&dir=cw')">Jog +100</button>
  <button onclick="cmd('/api/jog?axis=${a.name}&dir=ccw')">Jog −100</button>
  <button onclick="cmd('/api/rotate?axis=${a.name}&revs=1&dir=cw')">+1 rev</button>
  <button onclick="cmd('/api/rotate?axis=${a.name}&revs=1&dir=ccw')">−1 rev</button>
  <input type="number" id="n-${a.name}" value="5" min="1">
  <button onclick="cmd('/api/rotate?axis=${a.name}&revs='+document.getElementById('n-${a.name}').value+'&dir=cw')">+N rev</button>
  <button onclick="cmd('/api/rotate?axis=${a.name}&revs='+document.getElementById('n-${a.name}').value+'&dir=ccw')">−N rev</button>
</div>
<div class="row">
  <button class="go" onclick="cmd('/api/continuous?axis=${a.name}&dir=cw')">▶ Cont CW</button>
  <button class="go" onclick="cmd('/api/continuous?axis=${a.name}&dir=ccw')">◀ Cont CCW</button>
  <button onclick="cmd('/api/continuous?axis=${a.name}&dir=stop')">Stop cont</button>
  <button onclick="cmd('/api/stop?axis=${a.name}')">Stop (decel)</button>
  <button class="danger" onclick="cmd('/api/estop?axis=${a.name}')">E-STOP</button>
  <button onclick="cmd('/api/zero?axis=${a.name}')">Zero (home)</button>
</div>
<div class="row">
  spd <input type="number" id="s-${a.name}" min="1" step="50"> <button onclick="cmd('/api/speed?axis=${a.name}&v='+document.getElementById('s-${a.name}').value)">Set</button>
  acc <input type="number" id="a-${a.name}" min="1" step="50"> <button onclick="cmd('/api/accel?axis=${a.name}&v='+document.getElementById('a-${a.name}').value)">Set</button>
</div>`;
      root.appendChild(d);
    }
  }
  for(const a of s.axes){
    const d=document.getElementById('ax-'+a.name);
    if(!d)continue;
    d.classList.toggle('active',a.selected);
    d.querySelector('.sel').textContent=a.selected?'(active)':'';
    d.querySelector('.pos').textContent=a.position;
    d.querySelector('.spd').textContent=a.speed;
    d.querySelector('.acc').textContent=a.accel;
    d.querySelector('.con').textContent=a.continuous;
    const lim=d.querySelector('.lim');
    lim.textContent=a.limit;
    lim.className='lim '+(a.limit==='ASSERTED'?'la':'lr');
    const si=document.getElementById('s-'+a.name);
    const ai=document.getElementById('a-'+a.name);
    if(document.activeElement!==si)si.value=a.speed;
    if(document.activeElement!==ai)ai.value=a.accel;
  }
}
poll();setInterval(poll,500);
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

// FNET's default per-socket send buffer is 2 KB (NativeEthernet.h
// FNET_SOCKET_DEFAULT_SIZE). NativeEthernet's socketSend() has a
// busy-wait spin (`while(socketSendAvailable(s) < len){}`) that hangs
// forever if `len` ≥ buffer size — so we MUST chunk every write below
// the buffer ceiling and flush between chunks to drain it. 1 KB chunks
// leave plenty of headroom inside the 2 KB buffer.
//
// QNEthernet's write() doesn't have this bug but can also return short
// under lwIP buffer pressure, so the same chunked loop is correct
// across both backends.
static constexpr size_t HTTP_WRITE_CHUNK = 1024;

static bool writeAll(EthernetClient &c, const uint8_t *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        if (!c.connected()) return false;
        size_t want = n - sent;
        if (want > HTTP_WRITE_CHUNK) want = HTTP_WRITE_CHUNK;
        size_t w = c.write(buf + sent, want);
        if (w == 0) return false;
        sent += w;
        if (sent < n) c.flush();  // drain so the next chunk has room
    }
    return true;
}

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
    writeAll(c, reinterpret_cast<const uint8_t *>(hdr), n);
}

static void httpSendText(EthernetClient &c, int code, const char *status,
                         const char *body) {
    const int n = (int)strlen(body);
    httpSendHeader(c, code, status, "text/plain; charset=utf-8", n);
    writeAll(c, reinterpret_cast<const uint8_t *>(body), n);
}

static void httpServeIndex(EthernetClient &c) {
    const int n = (int)(sizeof(INDEX_HTML) - 1);
    httpSendHeader(c, 200, "OK", "text/html; charset=utf-8", n);
    writeAll(c, reinterpret_cast<const uint8_t *>(INDEX_HTML), n);
}

static void httpServeStatusJson(EthernetClient &c) {
    char json[640];
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
        n += snprintf(json + n, sizeof(json) - n,
            "%s{\"name\":\"%s\",\"position\":%ld,\"speed\":%.0f,\"accel\":%.0f,"
            "\"continuous\":\"%s\",\"limit\":\"%s\",\"selected\":%s}",
            i > 0 ? "," : "",
            a.name, a.stepper.currentPosition(), a.currentSpeed, a.currentAccel,
            cont, lim, (&a == selected) ? "true" : "false");
    }
    n += snprintf(json + n, sizeof(json) - n, "]}");
    httpSendHeader(c, 200, "OK", "application/json", n);
    writeAll(c, reinterpret_cast<const uint8_t *>(json), n);
}

// ---- Non-blocking motion variants for HTTP -----------------------------
// Same outcome as the Serial menu, but no runToPosition() so the HTTP
// handler returns immediately. serviceAxis() ticks the move forward.

static bool webJog(Axis &a, int dir) {
    if (!motorEnabled) return false;
    ensureDriverReady(a);
    a.continuousMode = false;
    a.stepper.move((long)dir * 100);
    a.lastMotionMs = millis();
    a.wasRunning = true;
    return true;
}

static bool webContinuous(Axis &a, int dir) {
    if (dir == 0) {
        a.continuousMode = false;
        return true;
    }
    if (!motorEnabled) return false;
    ensureDriverReady(a);
    a.continuousMode = true;
    a.continuousDir  = dir > 0 ? +1 : -1;
    a.lastReportedRev = a.stepper.currentPosition() / STEPS_PER_REV;
    a.lastContSaveMs  = millis();
    return true;
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

    if (strcmp(path, "/api/jog") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        getParam(query, "dir", dirStr, sizeof(dirStr));
        int dir = (strcmp(dirStr, "cw") == 0) ? +1 : (strcmp(dirStr, "ccw") == 0) ? -1 : 0;
        if (!a || dir == 0) { httpSendText(c, 400, "Bad Request", "bad axis/dir\n"); return; }
        if (!webJog(*a, dir)) { httpSendText(c, 409, "Conflict", "motors disabled\n"); return; }
        httpSendText(c, 200, "OK", "jog\n"); return;
    }

    if (strcmp(path, "/api/rotate") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        getParam(query, "dir", dirStr, sizeof(dirStr));
        getParam(query, "revs", valStr, sizeof(valStr));
        int dir = (strcmp(dirStr, "cw") == 0) ? +1 : (strcmp(dirStr, "ccw") == 0) ? -1 : 0;
        int n   = atoi(valStr);
        if (!a || dir == 0 || n <= 0) { httpSendText(c, 400, "Bad Request", "bad args\n"); return; }
        a->continuousMode = false;
        moveRevolutions(*a, n, dir);
        httpSendText(c, 200, "OK", "rotate\n"); return;
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
        if (!webContinuous(*a, dir)) { httpSendText(c, 409, "Conflict", "motors disabled\n"); return; }
        httpSendText(c, 200, "OK", "continuous\n"); return;
    }

    if (strcmp(path, "/api/stop") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        if (!a) { httpSendText(c, 400, "Bad Request", "bad axis\n"); return; }
        a->continuousMode = false;
        a->stepper.stop();
        httpSendText(c, 200, "OK", "stop\n"); return;
    }

    if (strcmp(path, "/api/estop") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        if (!a) { httpSendText(c, 400, "Bad Request", "bad axis\n"); return; }
        a->continuousMode = false;
        a->stepper.setCurrentPosition(a->stepper.currentPosition());  // zero velocity
        httpSendText(c, 200, "OK", "estop\n"); return;
    }

    if (strcmp(path, "/api/zero") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        if (!a) { httpSendText(c, 400, "Bad Request", "bad axis\n"); return; }
        a->stepper.setCurrentPosition(0);
        savePositionIfChanged(*a);
        httpSendText(c, 200, "OK", "zeroed\n"); return;
    }

    if (strcmp(path, "/api/speed") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        getParam(query, "v", valStr, sizeof(valStr));
        float v = strtof(valStr, nullptr);
        if (!a || v <= 0) { httpSendText(c, 400, "Bad Request", "bad args\n"); return; }
        a->currentSpeed = v;
        a->stepper.setMaxSpeed(v);
        httpSendText(c, 200, "OK", "speed\n"); return;
    }

    if (strcmp(path, "/api/accel") == 0) {
        Axis *a = getParam(query, "axis", axisName, sizeof(axisName)) ? findAxis(axisName) : nullptr;
        getParam(query, "v", valStr, sizeof(valStr));
        float v = strtof(valStr, nullptr);
        if (!a || v <= 0) { httpSendText(c, 400, "Bad Request", "bad args\n"); return; }
        a->currentAccel = v;
        a->stepper.setAcceleration(v);
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

    case 'N': case 'n': printNetStatus(); break;

    case '?':           printStatus(); break;
    case 'H': case 'h': printMenu();   break;
    default: break;
    }
}
