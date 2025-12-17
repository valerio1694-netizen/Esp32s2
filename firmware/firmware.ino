#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <FastLED.h>

/* ===================== CONFIG ===================== */

#define PIN_TOWER        16
#define NUM_TOWER        4

#define PIN_CAB          17
#define NUM_CAB          10

#define PIN_ONBOARD_LED  2   // ESP32 DevKit onboard LED (meist GPIO2)

#define AP_SSID "RobotArm"
#define AP_PASS "12345678"

/* ===================== LED ===================== */

CRGB towerLeds[NUM_TOWER];
CRGB cabLeds[NUM_CAB];

uint8_t towerBrightness = 120;
uint8_t cabBrightness   = 120;
bool cabOn = true;

/* ===================== STATE ===================== */

enum RobotState {
  STATE_HOME,
  STATE_RUN,
  STATE_WAIT,
  STATE_ERROR
};

RobotState state = STATE_WAIT;
bool faultLatched = false;
bool softStop = true;

WebServer server(80);

/* ===================== WEB UI (HTML) ===================== */

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>RobotArm Panel</title>
  <style>
    body{font-family:Arial,Helvetica,sans-serif;background:#0b0b0b;color:#eee;margin:0;padding:16px}
    .card{background:#151515;border:1px solid #2a2a2a;border-radius:12px;padding:14px;margin-bottom:12px}
    .row{display:flex;gap:10px;flex-wrap:wrap}
    button{padding:12px 14px;border:0;border-radius:10px;font-weight:700;cursor:pointer}
    .g{background:#19a44a;color:#fff}
    .r{background:#c53535;color:#fff}
    .b{background:#2d6cdf;color:#fff}
    .y{background:#d3a11f;color:#111}
    .k{background:#444;color:#fff}
    input[type=range]{width:100%}
    .label{opacity:.8;margin:6px 0}
    .small{font-size:12px;opacity:.8}
    a{color:#6aa6ff}
  </style>
</head>
<body>
  <div class="card">
    <h2 style="margin:0 0 10px 0">RobotArm</h2>
    <div class="row">
      <button class="g" onclick="call('/start')">START</button>
      <button class="r" onclick="call('/stop')">STOP (soft)</button>
      <button class="b" onclick="call('/home')">HOME</button>
      <button class="b" onclick="call('/reset')">RESET / QUIT</button>
      <button class="r" onclick="call('/fault')">FAULT (Test)</button>
    </div>
    <div class="small" style="margin-top:10px">
      Web OTA: <a href="/ota">hier</a>
    </div>
  </div>

  <div class="card">
    <div class="label">Ampel Helligkeit (GPIO16)</div>
    <input type="range" min="0" max="255" value="120" oninput="setTower(this.value)">
  </div>

  <div class="card">
    <div class="label">Schaltschrank Licht (GPIO17)</div>
    <div class="row" style="margin-bottom:10px">
      <button class="k" onclick="call('/cabOn?on=1')">AN</button>
      <button class="k" onclick="call('/cabOn?on=0')">AUS</button>
    </div>
    <div class="label">Helligkeit</div>
    <input type="range" min="0" max="255" value="120" oninput="setCab(this.value)">
  </div>

<script>
  function call(path){
    fetch(path).then(()=>{}).catch(()=>{});
  }
  function setTower(v){
    fetch('/tower?b=' + v).then(()=>{}).catch(()=>{});
  }
  function setCab(v){
    fetch('/cab?b=' + v).then(()=>{}).catch(()=>{});
  }
</script>
</body>
</html>
)HTML";

const char OTA_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>OTA Update</title>
  <style>
    body{font-family:Arial;background:#0b0b0b;color:#eee;margin:0;padding:16px}
    .card{background:#151515;border:1px solid #2a2a2a;border-radius:12px;padding:14px}
    input,button{width:100%;padding:12px;margin-top:10px;border-radius:10px;border:0}
    button{background:#2d6cdf;color:#fff;font-weight:700;cursor:pointer}
    a{color:#6aa6ff}
  </style>
</head>
<body>
  <div class="card">
    <h2 style="margin-top:0">OTA Update</h2>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="update" required>
      <button type="submit">Upload</button>
    </form>
    <p><a href="/">zurueck</a></p>
  </div>
</body>
</html>
)HTML";

/* ===================== HELPERS ===================== */

void updateOnboardLED() {
  static uint32_t last = 0;
  static bool led = true;

  if (!faultLatched && state != STATE_ERROR) {
    digitalWrite(PIN_ONBOARD_LED, HIGH); // dauerhaft an wenn normal
    return;
  }

  if (millis() - last > 250) {
    last = millis();
    led = !led;
    digitalWrite(PIN_ONBOARD_LED, led ? HIGH : LOW); // blinken bei Stoerung
  }
}

void drawTower() {
  fill_solid(towerLeds, NUM_TOWER, CRGB::Black);

  // Reihenfolge vom Eingang: 1=Blau, 2=Gruen, 3=Gelb, 4=Rot
  // index 0 = Blau, 1 = Gruen, 2 = Gelb, 3 = Rot
  int idx = -1;
  CRGB c = CRGB::Black;

  if (state == STATE_HOME)  { idx = 0; c = CRGB(0,0,255); }
  if (state == STATE_RUN)   { idx = 1; c = CRGB(0,255,0); }
  if (state == STATE_WAIT)  { idx = 2; c = CRGB(255,180,0); }
  if (state == STATE_ERROR) { idx = 3; c = CRGB(255,0,0); }

  c.nscale8_video(towerBrightness);
  if (idx >= 0) towerLeds[idx] = c;
}

void drawCabinet() {
  if (!cabOn) {
    fill_solid(cabLeds, NUM_CAB, CRGB::Black);
    return;
  }

  CRGB w = CRGB::White;
  w.nscale8_video(cabBrightness);
  fill_solid(cabLeds, NUM_CAB, w);
}

void render() {
  drawTower();
  drawCabinet();
  FastLED.show();
}

/* ===================== WEB HANDLERS ===================== */

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleOTA() {
  server.send_P(200, "text/html", OTA_HTML);
}

void handleStart() {
  if (!faultLatched) {
    softStop = false;
    state = STATE_RUN;
    render();
  }
  server.send(200, "text/plain", "OK");
}

void handleStop() {
  softStop = true;
  state = STATE_WAIT;
  render();
  server.send(200, "text/plain", "OK");
}

void handleHome() {
  if (!faultLatched) {
    softStop = true;
    state = STATE_HOME;
    render();
  }
  server.send(200, "text/plain", "OK");
}

void handleReset() {
  // wichtig: nach Stoerung -> WAIT (gelb), nicht HOME
  faultLatched = false;
  softStop = true;
  state = STATE_WAIT;
  render();
  server.send(200, "text/plain", "OK");
}

void handleFault() {
  faultLatched = true;
  state = STATE_ERROR;
  render();
  server.send(200, "text/plain", "OK");
}

void handleTowerBrightness() {
  if (server.hasArg("b")) {
    int v = server.arg("b").toInt();
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    towerBrightness = (uint8_t)v;
    render();
  }
  server.send(200, "text/plain", "OK");
}

void handleCabBrightness() {
  if (server.hasArg("b")) {
    int v = server.arg("b").toInt();
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    cabBrightness = (uint8_t)v;
    render();
  }
  server.send(200, "text/plain", "OK");
}

void handleCabOn() {
  if (server.hasArg("on")) {
    cabOn = (server.arg("on").toInt() != 0);
    render();
  }
  server.send(200, "text/plain", "OK");
}

/* ===================== OTA UPLOAD ===================== */

void handleUpdateUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    Update.begin();
  } else if (u.status == UPLOAD_FILE_WRITE) {
    Update.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

void handleUpdateDone() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
  delay(500);
  ESP.restart();
}

/* ===================== SETUP / LOOP ===================== */

void setup() {
  pinMode(PIN_ONBOARD_LED, OUTPUT);
  digitalWrite(PIN_ONBOARD_LED, HIGH);

  FastLED.addLeds<WS2812B, PIN_TOWER, GRB>(towerLeds, NUM_TOWER);
  FastLED.addLeds<WS2812B, PIN_CAB, GRB>(cabLeds, NUM_CAB);
  FastLED.setBrightness(255);
  render();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  server.on("/", handleRoot);
  server.on("/ota", handleOTA);

  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/home", handleHome);
  server.on("/reset", handleReset);
  server.on("/fault", handleFault);

  server.on("/tower", handleTowerBrightness);
  server.on("/cab", handleCabBrightness);
  server.on("/cabOn", handleCabOn);

  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  server.begin();
}

void loop() {
  server.handleClient();
  updateOnboardLED();
}
