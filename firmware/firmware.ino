#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <FastLED.h>

/* ===================== CONFIG ===================== */

#define PIN_TOWER   16
#define NUM_TOWER   4

#define PIN_CAB     17
#define NUM_CAB     10

#define PIN_ONBOARD_LED 2   // ESP32 DevKit LED

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

/* ===================== HELPERS ===================== */

void updateOnboardLED() {
  static uint32_t last = 0;
  static bool led = true;

  if (state != STATE_ERROR && !faultLatched) {
    digitalWrite(PIN_ONBOARD_LED, HIGH);
    return;
  }

  if (millis() - last > 250) {
    last = millis();
    led = !led;
    digitalWrite(PIN_ONBOARD_LED, led ? HIGH : LOW);
  }
}

void drawTower() {
  fill_solid(towerLeds, NUM_TOWER, CRGB::Black);

  CRGB c = CRGB::Black;
  int idx = -1;

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

/* ===================== WEB ===================== */

void handleRoot() {
  server.send(200, "text/plain", "RobotArm running");
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
    towerBrightness = server.arg("b").toInt();
    render();
  }
  server.send(200, "text/plain", "OK");
}

void handleCabBrightness() {
  if (server.hasArg("b")) {
    cabBrightness = server.arg("b").toInt();
    render();
  }
  server.send(200, "text/plain", "OK");
}

/* ===================== OTA ===================== */

void handleUpdate() {
  server.send(200, "text/plain", "Upload via POST");
}

void handleUpdateUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) Update.begin();
  else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
  else if (u.status == UPLOAD_FILE_END) Update.end(true);
}

/* ===================== SETUP ===================== */

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
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/home", handleHome);
  server.on("/reset", handleReset);
  server.on("/fault", handleFault);
  server.on("/tower", handleTowerBrightness);
  server.on("/cab", handleCabBrightness);
  server.on("/update", HTTP_POST, handleUpdate, handleUpdateUpload);

  server.begin();
}

/* ===================== LOOP ===================== */

void loop() {
  server.handleClient();
  updateOnboardLED();
}
