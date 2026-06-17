// TB6600 + NEMA 23 stepper-motor test bench for ESP32-C6 DevKitC-1.
//
// Interactive serial menu (115200 baud) lets you exercise direction,
// speed, revolutions, acceleration, jog, and enable/disable without
// re-flashing.  Serial routes through the C6's built-in USB Serial/JTAG.
//
// TB6600 wiring (common-cathode: signal− to GND, signal+ to GPIO):
//
//   TB6600 pin  │  ESP32-C6 GPIO       │  Notes
//   ────────────┼──────────────────────┼───────────────────────────
//   ENA+        │  GPIO6               │  LOW = enabled (opto on)
//   ENA-        │  GND                 │
//   DIR+        │  GPIO7               │  HIGH/LOW = CW/CCW
//   DIR-        │  GND                 │
//   PUL+        │  GPIO10              │  Step pulse
//   PUL-        │  GND                 │
//   VCC / GND   │  24–48 VDC PSU       │  Match motor voltage rating
//   A+/A-/B+/B- │  NEMA 23 coils       │
//
// Pin choice notes (ESP32-C6 DevKitC-1):
//   - GPIO 6, 7, 10 are general-purpose digital I/O, not strapping pins.
//   - Avoid GPIO 4, 5, 8, 9, 15 (strapping pins) and GPIO 12/13 (USB).
//   - 3.3 V logic — see platformio.ini for the level note.
//
// TB6600 DIP-switch micro-stepping reference:
//   S1   S2   S3   │  Micro-step
//   ON   ON   OFF  │  Full step   (200 steps/rev for 1.8° motor)
//   ON   OFF  ON   │  1/2  step   (400)
//   OFF  ON   ON   │  1/2  step   (400)
//   ON   OFF  OFF  │  1/4  step   (800)
//   OFF  ON   OFF  │  1/8  step   (1600)
//   OFF  OFF  ON   │  1/16 step   (3200)
//   OFF  OFF  OFF  │  1/32 step   (6400)  ← default below

#include <Arduino.h>
#include <AccelStepper.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"

// ── Captive-portal / OTA web-server config ──────────────────────────
static const char* AP_SSID     = "ESP32C6-Stepper-Setup";
static const char* AP_PASSWORD = "stepper-setup";  // ≥8 chars; for the setup network only
static const char* HOSTNAME    = "esp32c6-stepper";
static const uint16_t HTTP_PORT = 80;

// WiFi credentials are stored in NVS (Preferences), never compiled in.
// Password is write-only via the web UI — never read back to clients.
static Preferences prefs;       // namespace "wifi" — SSID/PW
static Preferences posPrefs;    // namespace "stepper" — last-known step position
static WebServer   server(HTTP_PORT);
static bool        inAPMode = false;

// Last position written to NVS — used to debounce writes (only save if changed).
static long lastSavedPos = 0;
// True while a move (or continuous run) is in progress — used to detect the
// running → idle edge so we save exactly once per move.
static bool wasRunning   = false;
// Tracks the integer revolution count last reported during continuous mode,
// so we print once per revolution boundary instead of every step.
static long lastReportedRev = 0;
// Time of the last NVS save during continuous mode.  Saves block the loop
// (~10 ms each) so we keep them sparse — at this interval each save costs
// at most one mild stutter per second.  Power-cycle loss = ≤ 1 sec × speed.
static unsigned long lastContSaveMs = 0;
static const unsigned long CONT_SAVE_INTERVAL_MS = 1000;

// ── Pin assignments (ESP32-C6 DevKitC-1) ────────────────────────────
static const uint8_t PIN_ENA  = 6;   // GPIO6
static const uint8_t PIN_DIR  = 7;   // GPIO7
static const uint8_t PIN_STEP = 10;  // GPIO10

// ── Defaults (match your DIP-switch setting) ────────────────────────
static const int STEPS_PER_REV   = 1600;   // 1/32 micro-stepping
static const float DEFAULT_SPEED = 800.0;  // steps/sec
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
    Serial.println(F("  Z  Set current position = home (0), save to NVS"));
    Serial.println(F("  I  Show WiFi info + web UI URL"));
    Serial.println(F("  ?  Print current status"));
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

static void printNetwork() {
    Serial.println(F("──────────────────────────────────"));
    Serial.print(F("Hostname : ")); Serial.print(HOSTNAME);
    Serial.println(F(".local"));
    if (inAPMode) {
        Serial.println(F("Mode     : AP (setup)"));
        Serial.print(F("AP SSID  : ")); Serial.println(AP_SSID);
        Serial.print(F("AP pwd   : ")); Serial.println(AP_PASSWORD);
        Serial.print(F("AP IP    : ")); Serial.println(WiFi.softAPIP());
        Serial.println(F("Web UI   : http://192.168.4.1/"));
    } else if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("Mode     : STA"));
        Serial.println();
        Serial.print(F("SSID     : ")); Serial.println(WiFi.SSID());
        Serial.print(F("IP       : ")); Serial.println(WiFi.localIP());
        Serial.print(F("RSSI     : ")); Serial.print(WiFi.RSSI());
        Serial.println(F(" dBm"));
        Serial.print(F("Web UI   : http://"));
        Serial.print(WiFi.localIP()); Serial.println(F("/"));
    } else {
        Serial.println(F("WiFi     : NOT CONNECTED"));
    }
    Serial.println(F("──────────────────────────────────"));
}

// ── HTML helpers ─────────────────────────────────────────────────────

static String htmlEscape(const String& s) {
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&#39;";  break;
        default:   out += c;
        }
    }
    return out;
}

static String pageHeader(const String& title) {
    String h = F("<!doctype html><html><head><meta charset=utf-8>"
                 "<meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<title>");
    h += htmlEscape(title);
    h += F("</title><style>"
           "body{font-family:system-ui,sans-serif;max-width:560px;margin:1em auto;padding:0 1em;color:#222}"
           "h1{font-size:1.3em} h2{font-size:1.05em;margin-top:0}"
           "form,.box{margin:1em 0;padding:1em;border:1px solid #ccc;border-radius:6px}"
           "label{display:block;margin:0.6em 0 0.2em;font-size:0.95em}"
           "input{width:100%;padding:0.5em;box-sizing:border-box;font-size:1em}"
           "button{margin-top:1em;padding:0.6em 1.2em;font-size:1em;cursor:pointer}"
           ".muted{color:#777;font-size:0.9em}"
           ".pill{display:inline-block;padding:2px 8px;border-radius:10px;background:#eef;font-size:0.85em}"
           "</style></head><body><h1>");
    h += htmlEscape(title);
    h += F("</h1>");
    return h;
}

// ── Web-server handlers ──────────────────────────────────────────────

static void handleRoot() {
    String savedSSID = prefs.getString("ssid", "");
    bool   hasPw     = prefs.getString("pw", "").length() > 0;

    String html = pageHeader(F("ESP32-C6 Stepper Test"));

    html += F("<div class=box><h2>Status</h2>");
    if (inAPMode) {
        html += F("<p><span class=pill>AP setup mode</span></p>");
        html += F("<p>Configure your WiFi below. The board will reboot and join your network.</p>");
    } else {
        html += F("<p><span class=pill>Connected</span></p>");
        html += F("<p><b>SSID:</b> ");    html += htmlEscape(WiFi.SSID()); html += F("</p>");
        html += F("<p><b>IP:</b> ");      html += WiFi.localIP().toString(); html += F("</p>");
        html += F("<p><b>RSSI:</b> ");    html += String(WiFi.RSSI()); html += F(" dBm</p>");
        html += F("<p><b>Hostname:</b> "); html += HOSTNAME; html += F(".local</p>");
    }
    html += F("</div>");

    // WiFi config — password is NEVER echoed back.
    html += F("<form method=POST action='/wifi'>");
    html += F("<h2>WiFi Configuration</h2>");
    html += F("<label>SSID</label>");
    html += F("<input type=text name=ssid value='");
    html += htmlEscape(savedSSID);
    html += F("' autocomplete=off required>");
    html += F("<label>Password ");
    html += hasPw ? F("<span class=muted>(saved — leave blank to keep current)</span>")
                  : F("<span class=muted>(none saved)</span>");
    html += F("</label>");
    html += F("<input type=password name=pw value='' autocomplete=new-password>");
    html += F("<button type=submit>Save &amp; Reboot</button>");
    html += F("</form>");

    // OTA firmware upload — only when connected (STA mode).
    if (!inAPMode && WiFi.status() == WL_CONNECTED) {
        html += F("<form method=POST action='/update' enctype='multipart/form-data'>");
        html += F("<h2>OTA Firmware Upload</h2>");
        html += F("<label>Firmware (.bin)</label>");
        html += F("<input type=file name=update accept='.bin' required>");
        html += F("<button type=submit>Upload &amp; Flash</button>");
        html += F("<p class=muted>The stepper driver is disabled automatically during upload.</p>");
        html += F("</form>");
    }

    // Recovery: clear stored creds.
    html += F("<form method=POST action='/clear' onsubmit=\"return confirm('Clear saved WiFi credentials?')\">");
    html += F("<h2>Reset WiFi Settings</h2>");
    html += F("<p class=muted>Forgets the stored SSID and password and reboots into AP setup mode.</p>");
    html += F("<button type=submit>Clear &amp; Reboot</button>");
    html += F("</form>");

    html += F("</body></html>");
    server.send(200, F("text/html"), html);
}

static void handleWifiSave() {
    String ssid = server.arg("ssid");
    String pw   = server.arg("pw");
    if (ssid.length() == 0) {
        server.send(400, F("text/plain"), F("SSID is required."));
        return;
    }
    prefs.putString("ssid", ssid);
    // Only overwrite password if a new one was actually supplied.
    if (pw.length() > 0) {
        prefs.putString("pw", pw);
    }
    String html = pageHeader(F("Saved"));
    html += F("<p>Credentials saved. Rebooting in 2 seconds…</p>");
    html += F("<p class=muted>Reconnect to your home WiFi to find the board at "
             "<code>");
    html += HOSTNAME;
    html += F(".local</code>.</p></body></html>");
    server.send(200, F("text/html"), html);
    delay(1500);
    ESP.restart();
}

static void handleClear() {
    prefs.remove("ssid");
    prefs.remove("pw");
    String html = pageHeader(F("Cleared"));
    html += F("<p>WiFi settings cleared. Rebooting into setup AP…</p></body></html>");
    server.send(200, F("text/html"), html);
    delay(1500);
    ESP.restart();
}

static void handleUpdateUpload() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        // Safely disable the motor before flashing.
        continuousMode = false;
        stepper.stop();
        enableMotor(false);
        Serial.print(F("[OTA] Begin: ")); Serial.println(up.filename);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.print(F("[OTA] Success, "));
            Serial.print(up.totalSize);
            Serial.println(F(" bytes."));
        } else {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Serial.println(F("[OTA] Aborted."));
    }
}

static void handleUpdateFinal() {
    bool ok = !Update.hasError();
    String html = pageHeader(ok ? F("Upload complete") : F("Upload failed"));
    if (ok) {
        html += F("<p>Firmware updated successfully. Rebooting…</p>");
    } else {
        html += F("<p>Update failed. Check the serial log for details.</p>");
        html += F("<p><a href='/'>Back</a></p>");
    }
    html += F("</body></html>");
    server.send(200, F("text/html"), html);
    if (ok) {
        delay(1500);
        ESP.restart();
    }
}

static void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/wifi", HTTP_POST, handleWifiSave);
    server.on("/clear", HTTP_POST, handleClear);
    // Two-callback form: first handles the multipart upload body, second sends the response.
    server.on("/update", HTTP_POST, handleUpdateFinal, handleUpdateUpload);
    server.onNotFound([]() {
        server.sendHeader(F("Location"), F("/"), true);
        server.send(302, F("text/plain"), F("Redirecting…"));
    });
    server.begin();

    if (!inAPMode) {
        if (MDNS.begin(HOSTNAME)) {
            MDNS.addService("http", "tcp", HTTP_PORT);
        }
    }
    Serial.print(F("Web server ready on port "));
    Serial.println(HTTP_PORT);
}

static bool tryStaConnect() {
    String ssid = prefs.getString("ssid", "");
    String pw   = prefs.getString("pw", "");
    if (ssid.length() == 0) {
        Serial.println(F("No saved WiFi credentials."));
        return false;
    }

    Serial.print(F("Connecting to WiFi \""));
    Serial.print(ssid);
    Serial.print(F("\" "));
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(ssid.c_str(), pw.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("Connected. IP = "));
        Serial.println(WiFi.localIP());
        return true;
    }
    Serial.println(F("WiFi connect FAILED."));
    WiFi.disconnect(true);
    return false;
}

// XIAO ESP32-C6 has an on-board RF switch (Richtek RTC6608) selecting
// between the PCB antenna and the external IPEX connector.  The control
// pins float at reset, which leaves the RF path undefined → WiFi appears
// dead or invisible.  Force "RF switch enabled + PCB antenna" here.
// On the official ESP32-C6-DevKitC-1 these GPIOs are unused, so this is
// a no-op.
static void configureRFSwitch() {
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);   // WIFI_ENABLE = LOW → enable the RF switch
    pinMode(14, OUTPUT);
    digitalWrite(14, LOW);  // WIFI_ANT_CONFIG = LOW → on-board PCB antenna
    delay(10);
}

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
        Serial.println(F("[WiFi] AP started"));
        break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
        Serial.println(F("[WiFi] AP stopped"));
        break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
        Serial.print(F("[WiFi] Client connected: "));
        for (int i = 0; i < 6; i++) {
            if (i) Serial.print(':');
            if (info.wifi_ap_staconnected.mac[i] < 16) Serial.print('0');
            Serial.print(info.wifi_ap_staconnected.mac[i], HEX);
        }
        Serial.println();
        break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
        Serial.println(F("[WiFi] Client disconnected"));
        break;
    case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
        Serial.print(F("[WiFi] Probe request, RSSI = "));
        Serial.println(info.wifi_ap_probereqrecved.rssi);
        break;
    default:
        break;
    }
}

static void startAPMode() {
    Serial.println(F("\n══════════════════ WiFi SETUP MODE ══════════════════"));
    Serial.print(F("  Join this WiFi network:  "));
    Serial.println(AP_SSID);
    Serial.print(F("  Password              :  "));
    Serial.println(AP_PASSWORD);
    Serial.println(F("  Then browse to        :  http://192.168.4.1/"));
    Serial.println(F("═════════════════════════════════════════════════════"));

    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_AP);
    delay(100);

    // Set regulatory country before bringing the radio up.
    wifi_country_t country = {};
    strncpy(country.cc, "IN", sizeof(country.cc));
    country.schan = 1;
    country.nchan = 13;
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;
    esp_wifi_set_country(&country);

    // ssid, password, channel=6, hidden=0, max_connections=4
    if (!WiFi.softAP(AP_SSID, AP_PASSWORD, 6, 0, 4)) {
        Serial.println(F("[WiFi] softAP() FAILED"));
    }
    // Force legacy 11b/g/n after softAP() so the protocol actually sticks.
    esp_err_t pr = esp_wifi_set_protocol(WIFI_IF_AP,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    Serial.print(F("[WiFi] set_protocol → "));
    Serial.println(pr == ESP_OK ? F("OK") : F("FAIL"));

    // Max TX power (in 0.25 dBm steps; 84 = 21 dBm = max).
    esp_wifi_set_max_tx_power(84);

    Serial.print(F("AP IP   : "));
    Serial.println(WiFi.softAPIP());
    Serial.print(F("AP MAC  : "));
    Serial.println(WiFi.softAPmacAddress());
    uint8_t primary;
    wifi_second_chan_t secondary;
    esp_wifi_get_channel(&primary, &secondary);
    Serial.print(F("AP chan : "));
    Serial.println(primary);
    int8_t txp;
    esp_wifi_get_max_tx_power(&txp);
    Serial.print(F("AP txpwr: "));
    Serial.print(txp / 4);
    Serial.println(F(" dBm"));
    inAPMode = true;
}

// Persist the current step position to NVS, but only if it has changed
// since the last save (debounce → keeps flash-wear low).
// quiet=true suppresses the Serial.println so we don't add ~2 ms of UART
// blocking to the time-critical continuous-mode loop.
static void savePositionIfChanged(bool quiet = false) {
    long p = stepper.currentPosition();
    if (p == lastSavedPos) return;
    posPrefs.putLong("pos", p);
    lastSavedPos = p;
    if (!quiet) {
        Serial.print(F("[pos] saved: "));
        Serial.println(p);
    }
}

// Boot-time homing: drive the motor from the saved position back to step 0.
// Home reference = 0.  This is open-loop — the stepper just retraces the
// step count it had at last shutdown.  Blocking; safe because we run this
// before the web server / control loop start.
static void homeOnBoot() {
    long saved = stepper.currentPosition();
    if (saved == 0) {
        Serial.println(F("Already at home (0).  No homing motion needed."));
        return;
    }
    Serial.print(F("Homing: driving from "));
    Serial.print(saved);
    Serial.println(F(" steps → 0 …"));
    stepper.moveTo(0);
    stepper.runToPosition();   // blocks until at position 0
    savePositionIfChanged();   // writes 0 to NVS
    Serial.println(F("Home reached (position = 0)."));
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
    // ESP32 USB CDC: give the host a moment to enumerate; do NOT block on
    // !Serial — that hangs if the monitor isn't open yet.
    delay(500);

    configureRFSwitch();

    pinMode(PIN_ENA, OUTPUT);
    enableMotor(true);

    stepper.setMinPulseWidth(5);
    stepper.setMaxSpeed(currentSpeed);
    stepper.setAcceleration(currentAccel);

    // Restore last-known step position from NVS.  Home reference = 0.
    // If no saved value exists (fresh chip / after 'Clear'), default to 0,
    // i.e. assume the mechanism is at home on first power-up.
    posPrefs.begin("stepper", false);
    long savedPos = posPrefs.getLong("pos", 0);
    stepper.setCurrentPosition(savedPos);
    lastSavedPos = savedPos;
    Serial.print(F("Restored position: "));
    Serial.print(savedPos);
    Serial.println(F(" steps (home = 0)"));

    // Drive the motor back to step 0 — the boot-time homing action.
    homeOnBoot();

    Serial.println();
    Serial.println(F("TB6600 + NEMA 23 Stepper Test — ESP32-C6 DevKitC-1"));
    Serial.println(F("Pins: ENA+=GPIO6  DIR+=GPIO7  PUL+=GPIO10  (−  to GND)"));
    Serial.print(F("Steps/rev: ")); Serial.println(STEPS_PER_REV);

    prefs.begin("wifi", false);
    if (!tryStaConnect()) {
        startAPMode();
    }
    setupWebServer();
    printNetwork();
    printMenu();
}

void loop() {
    server.handleClient();

    if (continuousMode) {
        stepper.setSpeed(continuousDir > 0 ? currentSpeed : -currentSpeed);
        stepper.runSpeed();
        // Report on every revolution boundary.  Integer division works for
        // negative positions too (rounds toward zero), so CCW shows -1, -2, …
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
        // Persist position on a time interval (not per-revolution) so a reset
        // partway through a revolution still has a near-current position to
        // home back to.
        unsigned long now = millis();
        if (now - lastContSaveMs >= CONT_SAVE_INTERVAL_MS) {
            lastContSaveMs = now;
            savePositionIfChanged(true /*quiet*/);
        }
    } else {
        stepper.run();
    }

    // Detect the running → idle edge and persist the new position.
    bool nowRunning = continuousMode || stepper.distanceToGo() != 0;
    if (wasRunning && !nowRunning) {
        savePositionIfChanged();
    }
    wasRunning = nowRunning;

    if (!Serial.available()) return;

    char cmd = Serial.read();
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
        savePositionIfChanged();
        Serial.println(F("Position zeroed (declared home)."));
        break;
    case 'I': case 'i':
        printNetwork();
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
