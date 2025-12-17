#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Adafruit_NeoPixel.h>

// ======================= CONFIG =======================

// WiFi AP
static const char* AP_SSID = "RobotArm";
static const char* AP_PASS = "12345678"; // min. 8 Zeichen

// NeoPixel Pins
#define PIN_AMPEL        16   // Ampel WS2812B (4 LEDs)
#define PIN_CABINET      17   // Schaltschrank WS2812B (10 LEDs)

// NeoPixel Counts
#define NUM_AMPEL        4
#define NUM_CABINET      10

// Onboard LED (meist GPIO2; falls bei dir anders: hier aendern)
#ifndef LED_BUILTIN
  #define LED_BUILTIN 2
#endif

// Keyswitch (3-Position): AUTO / OFF / MANUAL
// Verdrahtung: jeweilige Stellung schliesst Kontakt gegen GND
#define PIN_KEY_AUTO     25
#define PIN_KEY_MANUAL   26

static inline bool keyAuto()   { return digitalRead(PIN_KEY_AUTO)   == LOW; }
static inline bool keyManual() { return digitalRead(PIN_KEY_MANUAL) == LOW; }

// ======================= STATE =======================

enum State : uint8_t {
  ST_WAIT = 0,   // Gelb: aus/warten
  ST_HOME,       // Blau: in Home-Position
  ST_RUN,        // Gruen: laeuft
  ST_FAULT       // Rot: Stoerung
};

volatile State state = ST_WAIT;
State preFaultState = ST_WAIT;   // merkt den Zustand vor FAULT (wichtig fuer Quit-Verhalten)

uint32_t lastBlinkMs = 0;
bool blinkOn = false;

uint8_t brightAmpel   = 40;   // 0..255
uint8_t brightCabinet = 60;   // 0..255

Adafruit_NeoPixel ampel(NUM_AMPEL, PIN_AMPEL, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel cabinet(NUM_CABINET, PIN_CABINET, NEO_GRB + NEO_KHZ800);

WebServer server(80);

// ======================= COLORS =======================

uint32_t colGreen()  { return ampel.Color(0, 255, 0); }
uint32_t colBlue()   { return ampel.Color(0, 0, 255); }
uint32_t colYellow() { return ampel.Color(255, 140, 0); }
uint32_t colRed()    { return ampel.Color(255, 0, 0); }
uint32_t colOff()    { return ampel.Color(0, 0, 0); }

// Ampel Belegung (vom Eingang aus):
// LED0 = Blau, LED1 = Gruen, LED2 = Gelb, LED3 = Rot
void setAmpel(State st) {
  ampel.setBrightness(brightAmpel);

  for (int i = 0; i < NUM_AMPEL; i++) ampel.setPixelColor(i, colOff());

  switch (st) {
    case ST_HOME:  ampel.setPixelColor(0, colBlue());   break;
    case ST_RUN:   ampel.setPixelColor(1, colGreen());  break;
    case ST_WAIT:  ampel.setPixelColor(2, colYellow()); break;
    case ST_FAULT: ampel.setPixelColor(3, colRed());    break;
  }

  ampel.show();
}

// Schaltschrank: weiss, dimmbar ueber Brightness
void setCabinet(uint8_t b) {
  brightCabinet = b;
  cabinet.setBrightness(brightCabinet);
  for (int i = 0; i < NUM_CABINET; i++) cabinet.setPixelColor(i, cabinet.Color(255, 255, 255));
  cabinet.show();
}

// Onboard LED: immer AN, ausser bei FAULT -> blink
void updateOnboardLed() {
  if (state != ST_FAULT) {
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }
  uint32_t now = millis();
  if (now - lastBlinkMs >= 200) {
    lastBlinkMs = now;
    blinkOn = !blinkOn;
    digitalWrite(LED_BUILTIN, blinkOn ? HIGH : LOW);
  }
}

const char* stateName(State st) {
  switch (st) {
    case ST_WAIT:  return "WAIT";
    case ST_HOME:  return "HOME";
    case ST_RUN:   return "RUN";
    case ST_FAULT: return "FAULT";
  }
  return "UNKNOWN";
}

// ======================= KEYSWITCH =======================

bool keyswitchOk() {
  // beide aktiv = ungueltig
  if (keyAuto() && keyManual()) return false;
  return true;
}

bool keyswitchAllowsRun() {
  if (!keyswitchOk()) return false;
  return (keyAuto() || keyManual());
}

void enterFault(const String& why) {
  // Zustand vor Fault merken (nur wenn wir nicht schon Fault sind)
  if (state != ST_FAULT) preFaultState = state;

  state = ST_FAULT;
  setAmpel(state);
  Serial.print("FAULT: ");
  Serial.println(why);
}

// ======================= JSON API =======================

void sendJson() {
  String mode = "OFF";
  if (keyAuto() && !keyManual()) mode = "AUTO";
  if (!keyAuto() && keyManual()) mode = "MANUAL";
  if (keyAuto() && keyManual())  mode = "INVALID";

  String json = "{";
  json += "\"state\":\"" + String(stateName(state)) + "\",";
  json += "\"preFault\":\"" + String(stateName(preFaultState)) + "\",";
  json += "\"key\":\"" + mode + "\",";
  json += "\"brightAmpel\":" + String(brightAmpel) + ",";
  json += "\"brightCabinet\":" + String(brightCabinet);
  json += "}";

  server.send(200, "application/json", json);
}

// ======================= CONTROL HANDLERS =======================

void handleStop() {
  // Stop darf FAULT NICHT verlassen
  if (state == ST_FAULT) {
    server.send(409, "text/plain", "Cannot leave FAULT. Use RESET/QUIT.");
    return;
  }
  state = ST_WAIT;
  setAmpel(state);
  server.send(200, "text/plain", "OK");
}

void handleReset() {
  // Reset/Quitt: FAULT -> Zustand wiederherstellen:
  // war es vorher HOME -> HOME
  // sonst -> WAIT
  if (state == ST_FAULT) {
    state = (preFaultState == ST_HOME) ? ST_HOME : ST_WAIT;
    setAmpel(state);
    server.send(200, "text/plain", "OK");
    return;
  }

  // Reset aus anderen States -> WAIT (konservativ)
  state = ST_WAIT;
  setAmpel(state);
  server.send(200, "text/plain", "OK");
}

void handleHome() {
  if (state == ST_FAULT) { server.send(409, "text/plain", "FAULT: reset first"); return; }
  if (!keyswitchAllowsRun()) { server.send(403, "text/plain", "Keyswitch not enabled"); return; }

  // hier wuerdest du real die Servos in Home fahren
  state = ST_HOME;
  setAmpel(state);
  server.send(200, "text/plain", "OK");
}

void handleStart() {
  if (state == ST_FAULT) { server.send(409, "text/plain", "FAULT: reset first"); return; }
  if (!keyswitchAllowsRun()) { server.send(403, "text/plain", "Keyswitch not enabled"); return; }

  // optional: Start nur nach HOME erzwingen:
  // if (state != ST_HOME) { server.send(409, "text/plain", "Go HOME first"); return; }

  state = ST_RUN;
  setAmpel(state);
  server.send(200, "text/plain", "OK");
}

void handleFault() {
  enterFault("Forced via Web");
  server.send(200, "text/plain", "OK");
}

void handleSetBrightAmpel() {
  if (!server.hasArg("v")) { server.send(400, "text/plain", "Missing v"); return; }
  int v = server.arg("v").toInt();
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  brightAmpel = (uint8_t)v;
  setAmpel(state);
  server.send(200, "text/plain", "OK");
}

void handleSetBrightCabinet() {
  if (!server.hasArg("v")) { server.send(400, "text/plain", "Missing v"); return; }
  int v = server.arg("v").toInt();
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  setCabinet((uint8_t)v);
  server.send(200, "text/plain", "OK");
}

// ======================= WEB UI =======================

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>RobotArm</title>
  <style>
    body{font-family:Arial;margin:18px;background:#111;color:#eee}
    .card{background:#1b1b1b;border:1px solid #333;border-radius:12px;padding:14px;margin:10px 0}
    button{padding:12px 14px;border-radius:10px;border:0;margin:6px;font-weight:700}
    .row{display:flex;flex-wrap:wrap;gap:10px}
    .b{background:#2b2b2b;color:#fff}
    .g{background:#1f7a3a;color:#fff}
    .r{background:#8a1f1f;color:#fff}
    .y{background:#a07b12;color:#111}
    .bl{background:#1f3f8a;color:#fff}
    input[type=range]{width:100%}
    code{color:#9ef}
    a{color:#9ef}
  </style>
</head>
<body>
  <h2>RobotArm Control</h2>

  <div class="card">
    <div>Status: <code id="st">?</code> | Prev: <code id="pf">?</code> | Key: <code id="key">?</code></div>
  </div>

  <div class="card">
    <div class="row">
      <button class="g" onclick="api('/api/start')">START</button>
      <button class="y" onclick="api('/api/stop')">STOP</button>
      <button class="bl" onclick="api('/api/home')">HOME</button>
      <button class="r" onclick="api('/api/fault')">FORCE FAULT</button>
      <button class="b" onclick="api('/api/reset')">RESET / QUIT</button>
    </div>
  </div>

  <div class="card">
    <div>Ampel Helligkeit: <span id="ba">?</span></div>
    <input type="range" min="0" max="255" id="ra" oninput="setAmpel(this.value)">
  </div>

  <div class="card">
    <div>Schaltschrank Licht: <span id="bc">?</span></div>
    <input type="range" min="0" max="255" id="rc" oninput="setCab(this.value)">
  </div>

  <div class="card">
    <a href="/ota">Web OTA</a>
  </div>

<script>
async function refresh(){
  const r = await fetch('/api/state');
  const j = await r.json();
  document.getElementById('st').textContent = j.state;
  document.getElementById('pf').textContent = j.preFault;
  document.getElementById('key').textContent = j.key;
  document.getElementById('ba').textContent = j.brightAmpel;
  document.getElementById('bc').textContent = j.brightCabinet;
  document.getElementById('ra').value = j.brightAmpel;
  document.getElementById('rc').value = j.brightCabinet;
}
async function api(p){
  const r = await fetch(p, {method:'POST'});
  if(!r.ok){ alert(await r.text()); }
  refresh();
}
let t;
async function setAmpel(v){
  clearTimeout(t);
  document.getElementById('ba').textContent = v;
  t=setTimeout(()=>fetch('/api/bright/ampel?v='+v,{method:'POST'}),150);
}
let t2;
async function setCab(v){
  clearTimeout(t2);
  document.getElementById('bc').textContent = v;
  t2=setTimeout(()=>fetch('/api/bright/cabinet?v='+v,{method:'POST'}),150);
}
setInterval(refresh, 800);
refresh();
</script>

</body>
</html>
)HTML";

static const char OTA_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA</title>
<style>
body{font-family:Arial;margin:18px;background:#111;color:#eee}
.card{background:#1b1b1b;border:1px solid #333;border-radius:12px;padding:14px;margin:10px 0}
input,button{padding:10px;border-radius:10px;border:0}
button{background:#2b2b2b;color:#fff;font-weight:700}
a{color:#9ef}
</style>
</head><body>
<h2>Web OTA</h2>
<div class="card">
<form method="POST" action="/update" enctype="multipart/form-data">
  <input type="file" name="update">
  <button type="submit">Upload</button>
</form>
</div>
<div class="card"><a href="/">Back</a></div>
</body></html>
)HTML";

// ======================= SETUP / LOOP =======================

void setup() {
  Serial.begin(115200);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  pinMode(PIN_KEY_AUTO, INPUT_PULLUP);
  pinMode(PIN_KEY_MANUAL, INPUT_PULLUP);

  ampel.begin();
  cabinet.begin();

  setCabinet(brightCabinet);
  setAmpel(state);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(100);

  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", INDEX_HTML); });
  server.on("/ota", HTTP_GET, [](){ server.send(200, "text/html", OTA_HTML); });

  server.on("/api/state", HTTP_GET, sendJson);

  server.on("/api/start", HTTP_POST, handleStart);
  server.on("/api/stop",  HTTP_POST, handleStop);
  server.on("/api/home",  HTTP_POST, handleHome);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/fault", HTTP_POST, handleFault);

  server.on("/api/bright/ampel",   HTTP_POST, handleSetBrightAmpel);
  server.on("/api/bright/cabinet", HTTP_POST, handleSetBrightCabinet);

  // OTA upload endpoint
  server.on("/update", HTTP_POST,
    []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK - Rebooting");
      delay(200);
      ESP.restart();
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (!Update.end(true)) {
          Update.printError(Serial);
        }
      }
    }
  );

  server.begin();
}

void loop() {
  server.handleClient();

  // Keyswitch Plausibilitaet
  if (!keyswitchOk()) {
    if (state != ST_FAULT) enterFault("Keyswitch invalid (AUTO+MANUAL)");
  }

  // Schluessel OFF: stoppt RUN/HOME -> WAIT (soft)
  if (state != ST_FAULT) {
    if (!keyAuto() && !keyManual()) {
      if (state == ST_RUN || state == ST_HOME) {
        state = ST_WAIT;
        setAmpel(state);
      }
    }
  }

  updateOnboardLed();
}
```0
