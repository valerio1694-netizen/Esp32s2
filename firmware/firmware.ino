/*
  RobotArm Controller (ESP32)
  - Web UI + Web OTA
  - Status Ampel: 4x WS2812B on GPIO16
      LED1 (index 0): BLUE  = HOME
      LED2 (index 1): GREEN = RUN
      LED3 (index 2): YELLOW= WAIT
      LED4 (index 3): RED   = FAULT (blinking)
  - Cabinet light: 10x WS2812B on GPIO17 (white, dimmable)
  - Separate brightness sliders (ampel != cabinet)
  - State machine rules:
      * From FAULT you cannot go to STOP/RUN/HOME without QUIT/RESET
      * QUIT clears FAULT and restores display:
          - if it was HOME before fault -> show HOME
          - else -> show WAIT (stopped)
      * QUIT does NOT auto-run and does NOT auto-home
  - Onboard LED:
      * Always ON (solid) while powered
      * In FAULT it blinks (instead of solid)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <FastLED.h>
#include <Preferences.h>

// ----------------------------- USER CONFIG -----------------------------

// WiFi AP (immer verfügbar)
static const char* AP_SSID = "RobotArm";
static const char* AP_PASS = "12345678";  // min. 8 Zeichen

// WS2812 Pins
#define PIN_AMPEL     16
#define PIN_CABINET   17

// LED counts
#define NUM_AMPEL     4
#define NUM_CABINET   10

// Onboard LED (Board abhängig)
#define ONBOARD_LED_PIN 2
// Manche Boards haben LED active-low. Falls deine LED genau andersrum ist: true setzen.
#define ONBOARD_LED_ACTIVE_LOW false

// Fault blink timing
static const uint32_t FAULT_BLINK_MS = 350;

// ----------------------------------------------------------------------

WebServer server(80);
Preferences prefs;

// FastLED: Wir nutzen RAW-Buffer + DISP-Buffer, damit wir pro Strip getrennt dimmen können.
CRGB ampelRaw[NUM_AMPEL];
CRGB cabinetRaw[NUM_CABINET];

CRGB ampelDisp[NUM_AMPEL];
CRGB cabinetDisp[NUM_CABINET];

// getrennte Helligkeit 0..255
uint8_t brightnessAmpel   = 80;
uint8_t brightnessCabinet = 120;

// Cabinet on/off
bool cabinetEnabled = true;

// Key switch (simuliert über Web UI)
enum KeyMode : uint8_t { KEY_OFF = 0, KEY_AUTO = 1, KEY_MANUAL = 2 };
KeyMode keyMode = KEY_OFF;

// Robot state machine
enum RobotState : uint8_t { ST_WAIT = 0, ST_HOME = 1, ST_RUN = 2, ST_FAULT = 3 };
RobotState state = ST_WAIT;
RobotState stateBeforeFault = ST_WAIT;

// Fault flag (redundant aber praktisch)
bool faultActive = false;

// Blink toggle
bool faultBlinkOn = false;
uint32_t lastBlinkMs = 0;

// ----------------------------------------------------------------------
// Helpers

static inline void onboardWrite(bool on) {
  bool level = on;
  if (ONBOARD_LED_ACTIVE_LOW) level = !level;
  digitalWrite(ONBOARD_LED_PIN, level ? HIGH : LOW);
}

String stateToString(RobotState s) {
  switch (s) {
    case ST_WAIT:  return "WAIT";
    case ST_HOME:  return "HOME";
    case ST_RUN:   return "RUN";
    case ST_FAULT: return "FAULT";
  }
  return "UNKNOWN";
}

String keyToString(KeyMode k) {
  switch (k) {
    case KEY_OFF:    return "OFF";
    case KEY_AUTO:   return "AUTO";
    case KEY_MANUAL: return "MANUAL";
  }
  return "UNKNOWN";
}

// ----------------------------------------------------------------------
// LED Rendering (separate brightness per strip)

void clearAllRaw() {
  fill_solid(ampelRaw, NUM_AMPEL, CRGB::Black);
  fill_solid(cabinetRaw, NUM_CABINET, CRGB::Black);
}

void renderAmpelRaw() {
  fill_solid(ampelRaw, NUM_AMPEL, CRGB::Black);

  // Mapping wie gewünscht:
  // index 0: BLUE  (HOME)
  // index 1: GREEN (RUN)
  // index 2: YELLOW(WAIT)
  // index 3: RED   (FAULT blink)
  if (state == ST_HOME) {
    ampelRaw[0] = CRGB::Blue;
  } else if (state == ST_RUN) {
    ampelRaw[1] = CRGB::Green;
  } else if (state == ST_WAIT) {
    ampelRaw[2] = CRGB::Yellow;
  } else if (state == ST_FAULT) {
    ampelRaw[3] = faultBlinkOn ? CRGB::Red : CRGB::Black;
  }
}

void renderCabinetRaw() {
  if (!cabinetEnabled) {
    fill_solid(cabinetRaw, NUM_CABINET, CRGB::Black);
    return;
  }
  fill_solid(cabinetRaw, NUM_CABINET, CRGB::White);
}

void applyBrightnessAndShow() {
  // copy RAW -> DISP and scale per strip
  for (int i = 0; i < NUM_AMPEL; i++) {
    ampelDisp[i] = ampelRaw[i];
    ampelDisp[i].nscale8_video(brightnessAmpel);
  }
  for (int i = 0; i < NUM_CABINET; i++) {
    cabinetDisp[i] = cabinetRaw[i];
    cabinetDisp[i].nscale8_video(brightnessCabinet);
  }
  FastLED.show();
}

void updateLedsNow() {
  renderAmpelRaw();
  renderCabinetRaw();
  applyBrightnessAndShow();
}

// ----------------------------------------------------------------------
// State machine rules

void enterFault(const String& reason) {
  (void)reason; // optional log
  if (state != ST_FAULT) stateBeforeFault = state;
  state = ST_FAULT;
  faultActive = true;
  // Onboard LED will blink in loop
}

void clearFault() {
  // QUIT/RESET:
  faultActive = false;
  state = (stateBeforeFault == ST_HOME) ? ST_HOME : ST_WAIT;
}

bool canOperate() {
  // Nicht starten wenn Key OFF
  return keyMode != KEY_OFF;
}

void cmdStart() {
  if (state == ST_FAULT) return;          // block
  if (!canOperate()) return;              // key off => block
  state = ST_RUN;
}

void cmdStop() {
  if (state == ST_FAULT) return;          // block (Fault bleibt Fault)
  state = ST_WAIT;
}

void cmdHome() {
  if (state == ST_FAULT) return;          // block
  // hier würdest du real die Servos auf HOME fahren
  state = ST_HOME;
}

void cmdQuit() {
  if (state != ST_FAULT) return;          // nur wenn Fault aktiv
  clearFault();
}

// ----------------------------------------------------------------------
// Persist settings

void loadSettings() {
  prefs.begin("robotarm", true);
  brightnessAmpel   = prefs.getUChar("bAmpel", 80);
  brightnessCabinet = prefs.getUChar("bCab", 120);
  cabinetEnabled    = prefs.getBool("cabOn", true);
  keyMode           = (KeyMode)prefs.getUChar("key", (uint8_t)KEY_OFF);
  prefs.end();
}

void saveSettings() {
  prefs.begin("robotarm", false);
  prefs.putUChar("bAmpel", brightnessAmpel);
  prefs.putUChar("bCab", brightnessCabinet);
  prefs.putBool("cabOn", cabinetEnabled);
  prefs.putUChar("key", (uint8_t)keyMode);
  prefs.end();
}

// ----------------------------------------------------------------------
// Web UI

String htmlPage() {
  String s;
  s.reserve(6000);

  s += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>RobotArm</title>"
         "<style>"
         "body{font-family:system-ui,Arial;margin:16px;background:#0b0f14;color:#e8eef6;}"
         ".card{background:#121a24;border:1px solid #1e2a3a;border-radius:14px;padding:14px;margin:12px 0;}"
         "button{padding:12px 14px;border-radius:12px;border:1px solid #2a3b53;background:#1a2635;color:#e8eef6;"
         "margin:6px 6px 6px 0;cursor:pointer;font-weight:600;}"
         "button.red{background:#3b1a1a;border-color:#6a2a2a;}"
         "button.green{background:#1a3b25;border-color:#2a6a3f;}"
         "button.blue{background:#1a2a3b;border-color:#2a4a6a;}"
         "button.yellow{background:#3b321a;border-color:#6a5a2a;}"
         "input[type=range]{width:100%;}"
         ".row{display:flex;gap:10px;flex-wrap:wrap;}"
         ".pill{display:inline-block;padding:6px 10px;border-radius:999px;background:#0f1620;border:1px solid #243447;}"
         "a{color:#8bbcff;}"
         "</style></head><body>");

  s += F("<h2>RobotArm Controller</h2>");

  s += F("<div class='card'><div class='row'>");
  s += F("<div class='pill'>State: <span id='st'>?</span></div>");
  s += F("<div class='pill'>Key: <span id='key'>?</span></div>");
  s += F("</div><div style='margin-top:10px;'>");

  s += F("<button class='green' onclick=\"btn('start')\">START</button>");
  s += F("<button class='red' onclick=\"btn('stop')\">STOP</button>");
  s += F("<button class='blue' onclick=\"btn('home')\">HOME</button>");
  s += F("<button class='yellow' onclick=\"btn('quit')\">RESET/QUIT</button>");
  s += F("<button class='red' onclick=\"btn('fault')\">TRIGGER FAULT</button>");

  s += F("</div></div>");

  s += F("<div class='card'>"
         "<h3>Key Switch (simuliert)</h3>"
         "<div class='row'>"
         "<button onclick=\"setKey(0)\">OFF</button>"
         "<button onclick=\"setKey(1)\">AUTO</button>"
         "<button onclick=\"setKey(2)\">MANUAL</button>"
         "</div></div>");

  s += F("<div class='card'>"
         "<h3>Helligkeit</h3>"
         "<div>Ampel: <span id='ba'>?</span></div>"
         "<input type='range' min='0' max='255' id='rAmpel' oninput='chgAmpel(this.value)'>"
         "<div style='margin-top:10px;'>Schaltschrank: <span id='bc'>?</span></div>"
         "<input type='range' min='0' max='255' id='rCab' oninput='chgCab(this.value)'>"
         "<div style='margin-top:10px;'>"
         "<button onclick=\"toggleCab()\">Cabinet On/Off</button>"
         "</div>"
         "</div>");

  s += F("<div class='card'>"
         "<h3>Web OTA</h3>"
         "<div><a href='/update'>Firmware hochladen</a></div>"
         "</div>");

  s += F("<script>"
         "async function refresh(){"
         "  const r=await fetch('/api/status');"
         "  const j=await r.json();"
         "  document.getElementById('st').textContent=j.state;"
         "  document.getElementById('key').textContent=j.key;"
         "  document.getElementById('ba').textContent=j.bAmpel;"
         "  document.getElementById('bc').textContent=j.bCab;"
         "  document.getElementById('rAmpel').value=j.bAmpel;"
         "  document.getElementById('rCab').value=j.bCab;"
         "}"
         "async function btn(name){await fetch('/api/button?name='+encodeURIComponent(name)); refresh();}"
         "async function setKey(v){await fetch('/api/key?mode='+v); refresh();}"
         "async function chgAmpel(v){await fetch('/api/brightness?ampel='+v);}"
         "async function chgCab(v){await fetch('/api/brightness?cab='+v);}"
         "async function toggleCab(){await fetch('/api/cabinet?toggle=1'); refresh();}"
         "setInterval(refresh,800); refresh();"
         "</script>");

  s += F("</body></html>");
  return s;
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleStatus() {
  String json = "{";
  json += "\"state\":\"" + stateToString(state) + "\",";
  json += "\"key\":\"" + keyToString(keyMode) + "\",";
  json += "\"bAmpel\":" + String((int)brightnessAmpel) + ",";
  json += "\"bCab\":" + String((int)brightnessCabinet) + ",";
  json += "\"cabOn\":" + String(cabinetEnabled ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleButton() {
  if (!server.hasArg("name")) { server.send(400, "text/plain", "missing name"); return; }
  String name = server.arg("name");

  if (name == "fault") {
    enterFault("manual");
  } else if (name == "quit") {
    cmdQuit();
  } else if (name == "start") {
    cmdStart();
  } else if (name == "stop") {
    cmdStop();
  } else if (name == "home") {
    cmdHome();
  }

  updateLedsNow();
  server.send(200, "text/plain", "ok");
}

void handleKey() {
  if (!server.hasArg("mode")) { server.send(400, "text/plain", "missing mode"); return; }
  int m = server.arg("mode").toInt();
  if (m < 0) m = 0;
  if (m > 2) m = 2;
  keyMode = (KeyMode)m;
  saveSettings();
  updateLedsNow();
  server.send(200, "text/plain", "ok");
}

void handleBrightness() {
  bool changed = false;

  if (server.hasArg("ampel")) {
    int v = server.arg("ampel").toInt();
    if (v < 0) v = 0; if (v > 255) v = 255;
    brightnessAmpel = (uint8_t)v;
    changed = true;
  }
  if (server.hasArg("cab")) {
    int v = server.arg("cab").toInt();
    if (v < 0) v = 0; if (v > 255) v = 255;
    brightnessCabinet = (uint8_t)v;
    changed = true;
  }

  if (changed) {
    saveSettings();
    updateLedsNow();
  }
  server.send(200, "text/plain", "ok");
}

void handleCabinet() {
  if (server.hasArg("toggle")) {
    cabinetEnabled = !cabinetEnabled;
    saveSettings();
    updateLedsNow();
  }
  server.send(200, "text/plain", "ok");
}

// -------------------- Web OTA --------------------

static const char* updateForm =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>OTA Update</title>"
"<style>body{font-family:system-ui;margin:16px;background:#0b0f14;color:#e8eef6}"
"form{background:#121a24;border:1px solid #1e2a3a;border-radius:14px;padding:14px;max-width:520px}"
"input,button{margin-top:10px;font-size:16px}"
"button{padding:10px 14px;border-radius:12px;border:1px solid #2a3b53;background:#1a2635;color:#e8eef6;font-weight:700}"
"</style></head><body>"
"<h2>Firmware OTA Upload</h2>"
"<form method='POST' action='/update' enctype='multipart/form-data'>"
"<input type='file' name='update'>"
"<button type='submit'>Upload</button>"
"</form>"
"<p><a href='/'>Zurück</a></p>"
"</body></html>";

void handleUpdatePage() {
  server.send(200, "text/html", updateForm);
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    // Stop everything that could disturb flashing
    cmdStop();
    updateLedsNow();

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      // failed
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    bool ok = Update.end(true);
    server.send(200, "text/plain", ok ? "OK. Rebooting..." : "FAIL");
    delay(600);
    ESP.restart();
  }
}

// ----------------------------------------------------------------------
// Setup / Loop

void setupWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
}

void setupWeb() {
  server.on("/", handleRoot);

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/button", HTTP_GET, handleButton);
  server.on("/api/key", HTTP_GET, handleKey);
  server.on("/api/brightness", HTTP_GET, handleBrightness);
  server.on("/api/cabinet", HTTP_GET, handleCabinet);

  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, []() {}, handleUpdateUpload);

  server.begin();
}

void setupFastLED() {
  FastLED.addLeds<WS2812B, PIN_AMPEL, GRB>(ampelDisp, NUM_AMPEL);
  FastLED.addLeds<WS2812B, PIN_CABINET, GRB>(cabinetDisp, NUM_CABINET);
  FastLED.setBrightness(255); // wir skalieren selbst pro Strip
  clearAllRaw();
  updateLedsNow();
}

void setupOnboardLED() {
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  // normal: dauerhaft an
  onboardWrite(true);
}

void setup() {
  setupOnboardLED();
  loadSettings();

  setupFastLED();
  setupWiFiAP();
  setupWeb();

  // Startzustand:
  state = ST_WAIT;
  stateBeforeFault = ST_WAIT;
  faultActive = false;
  updateLedsNow();
}

void loop() {
  server.handleClient();

  // Onboard LED behavior:
  if (state == ST_FAULT) {
    // blink
    uint32_t now = millis();
    if (now - lastBlinkMs >= FAULT_BLINK_MS) {
      lastBlinkMs = now;
      faultBlinkOn = !faultBlinkOn;
      onboardWrite(faultBlinkOn);
      updateLedsNow(); // Ampel red blink
    }
  } else {
    // always solid ON
    onboardWrite(true);
    // also keep fault blink off
    faultBlinkOn = false;
  }

  // If state is fault, keep ampel blinking even if no client refresh
  if (state == ST_FAULT) {
    // handled above
  }
}
