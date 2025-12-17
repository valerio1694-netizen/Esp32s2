#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>
#include <FastLED.h>
#include <Adafruit_PWMServoDriver.h>

/* =========================
   HW / PINS
   ========================= */
#define AMP_PIN     16      // WS2812B Ampel (4 LEDs)
#define CAB_PIN     17      // WS2812B Schaltschranklicht (10 LEDs)
#define AMP_LEDS    4
#define CAB_LEDS    10

#define I2C_SDA     21
#define I2C_SCL     22

#define ONBOARD_LED 2       // Viele ESP32 Devboards: GPIO2. Wenn bei dir anders -> ändern.

#define PCA_ADDR    0x40
#define SERVO_FREQ  50

/* =========================
   SERVOS (PCA9685 Kanäle)
   Reihenfolge UI: Base, Schulter, Ellenbogen, Drehen, Kippen, Greifen
   ========================= */
static const uint8_t SERVO_CH[6] = {0, 1, 2, 3, 4, 5};
static const char*   SERVO_NAME[6] = {"Base","Schulter","Ellenbogen","Drehen","Kippen","Greifen"};

// Pulsbreiten (typisch). Wenn Servos knacken / Enden falsch: anpassen.
static const uint16_t SERVO_MIN_PULSE = 110; // ~0°
static const uint16_t SERVO_MAX_PULSE = 510; // ~180°

/* =========================
   LED BUFFERS
   ========================= */
CRGB ampLeds[AMP_LEDS];
CRGB cabLeds[CAB_LEDS];

/* =========================
   PCA
   ========================= */
Adafruit_PWMServoDriver pca(PCA_ADDR);

/* =========================
   STATE / LOGIC
   ========================= */
enum State : uint8_t { STATE_WAIT, STATE_HOME, STATE_RUN, STATE_STOP, STATE_FAULT };
State currentState = STATE_WAIT;

enum KeyMode : uint8_t { KEY_OFF, KEY_MAN, KEY_AUTO };
KeyMode keyMode = KEY_MAN;

/* Brightness */
uint8_t ampBrightness = 120;  // 0..255
uint8_t cabBrightness = 80;   // 0..255

/* Fault blink */
bool faultBlink = false;
uint32_t faultBlinkT = 0;

/* Servo positions + motion ramp */
float servoCur[6]   = {90, 90, 90, 90, 90, 90};
float servoTgt[6]   = {90, 90, 90, 90, 90, 90};
int   servoHome[6]  = {90, 90, 90, 90, 90, 90};

// Limits pro Servo (anpassen wenn Mechanik sonst anschlägt)
int servoMinDeg[6] = {0, 0, 0, 0, 0, 0};
int servoMaxDeg[6] = {180,180,180,180,180,180};

// Rampen-Speed (Grad pro Sekunde)
float servoSpeedDegPerSec = 140.0f;

// Bewegungserkennung
bool anyMoving = false;
uint32_t lastMotionMs = 0;

/* =========================
   WIFI / WEB
   ========================= */
WebServer server(80);
const char* AP_SSID = "RobotCtrl";
const char* AP_PASS = "12345678";

/* =========================
   UTILS
   ========================= */
static inline float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline uint16_t degToPwm(float deg) {
  deg = clampf(deg, 0.0f, 180.0f);
  float t = deg / 180.0f;
  return (uint16_t)(SERVO_MIN_PULSE + (SERVO_MAX_PULSE - SERVO_MIN_PULSE) * t);
}

void writeServoPwm(uint8_t idx, float deg) {
  uint16_t pwm = degToPwm(deg);
  pca.setPWM(SERVO_CH[idx], 0, pwm);
}

bool isAtHomePose(float tolDeg = 0.8f) {
  for (int i=0;i<6;i++) {
    if (fabs(servoCur[i] - (float)servoHome[i]) > tolDeg) return false;
  }
  return true;
}

bool isMoving(float tolDeg = 0.5f) {
  for (int i=0;i<6;i++) {
    if (fabs(servoCur[i] - servoTgt[i]) > tolDeg) return true;
  }
  return false;
}

bool motionAllowedFor(KeyMode km, bool isServoSliderMove) {
  if (currentState == STATE_FAULT) return false;
  if (currentState == STATE_STOP)  return false;
  if (km == KEY_OFF) return false;

  // Typisch: AUTO = keine Handverstellung
  if (km == KEY_AUTO && isServoSliderMove) return false;

  return true;
}

void freezeMotion() {
  for (int i=0;i<6;i++) servoTgt[i] = servoCur[i];
}

/* =========================
   LED HELPERS (FastLED korrekt!)
   ========================= */
CRGB scale(CRGB c, uint8_t b) {
  c.nscale8_video(b);
  return c;
}

// Ampel: LED0=Blau(HOME), LED1=Grün(RUN), LED2=Gelb(WAIT), LED3=Rot(STOP/FAULT)
void setAmpelOne(int idx, CRGB col) {
  for (int i=0;i<AMP_LEDS;i++) ampLeds[i] = CRGB::Black;
  ampLeds[idx] = scale(col, ampBrightness);
}

void updateCabinet() {
  CRGB w = scale(CRGB::White, cabBrightness);
  for (int i=0;i<CAB_LEDS;i++) cabLeds[i] = w;
}

void updateOnboard() {
  // Onboard LED ist nur an/aus.
  // Vorgabe: immer an solange kein FAULT, bei FAULT blinkt.
  if (currentState == STATE_FAULT) {
    digitalWrite(ONBOARD_LED, (millis()/250)%2 ? HIGH : LOW);
  } else {
    digitalWrite(ONBOARD_LED, HIGH);
  }
}

void updateLeds() {
  if (currentState == STATE_FAULT) {
    if (millis() - faultBlinkT > 300) {
      faultBlinkT = millis();
      faultBlink = !faultBlink;
    }
    for (int i=0;i<AMP_LEDS;i++) ampLeds[i] = CRGB::Black;
    if (faultBlink) ampLeds[3] = scale(CRGB::Red, ampBrightness); // rot blink
  } else {
    switch (currentState) {
      case STATE_HOME: setAmpelOne(0, CRGB::Blue);   break;
      case STATE_RUN:  setAmpelOne(1, CRGB::Green);  break;
      case STATE_WAIT: setAmpelOne(2, CRGB::Yellow); break;
      case STATE_STOP: setAmpelOne(3, CRGB::Red);    break; // STOP = rot (wie gewünscht)
      default:         setAmpelOne(2, CRGB::Yellow); break;
    }
  }

  updateCabinet();
  FastLED.show();
  updateOnboard();
}

/* =========================
   STATE RULES
   ========================= */
bool requireReset() {
  return (currentState == STATE_FAULT);
}

void setFault() {
  if (currentState != STATE_FAULT) {
    currentState = STATE_FAULT;
    freezeMotion();
  }
}

// WICHTIG: Reset nach FAULT geht IMMER auf STOP (rot).
void resetFaultToStop() {
  if (currentState == STATE_FAULT) {
    currentState = STATE_STOP;
    freezeMotion();
  }
}

/* =========================
   MOTION ENGINE (Rampen)
   ========================= */
void setServoTarget(int idx, float deg) {
  deg = clampf(deg, (float)servoMinDeg[idx], (float)servoMaxDeg[idx]);
  servoTgt[idx] = deg;

  // Sobald wir ein Ziel setzen -> RUN (Bewegung zeigt RUN), außer STOP/FAULT
  if (currentState != STATE_FAULT && currentState != STATE_STOP) {
    currentState = STATE_RUN;
  }
}

void setAllHomeTargets() {
  for (int i=0;i<6;i++) setServoTarget(i, (float)servoHome[i]);
}

void updateMotion() {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  float dt = (lastMs == 0) ? 0.0f : (now - lastMs) / 1000.0f;
  lastMs = now;

  if (dt <= 0.0f) return;

  if (currentState == STATE_FAULT || currentState == STATE_STOP) {
    anyMoving = false;
    return;
  }

  anyMoving = false;

  float maxStep = servoSpeedDegPerSec * dt;

  for (int i=0;i<6;i++) {
    float diff = servoTgt[i] - servoCur[i];
    if (fabs(diff) > 0.5f) {
      anyMoving = true;
      float step = clampf(diff, -maxStep, maxStep);
      servoCur[i] += step;
      writeServoPwm(i, servoCur[i]);
    } else {
      // Snap final
      servoCur[i] = servoTgt[i];
      writeServoPwm(i, servoCur[i]);
    }
  }

  if (anyMoving) lastMotionMs = now;

  // RUN/WAIТ Automatik:
  // - während Bewegung: RUN
  // - wenn still und nicht STOP/FAULT:
  //   - wenn in Home Pose: HOME
  //   - sonst: WAIT
  if (!anyMoving && currentState != STATE_FAULT && currentState != STATE_STOP) {
    if (isAtHomePose()) currentState = STATE_HOME;
    else                currentState = STATE_WAIT;
  } else if (anyMoving && currentState != STATE_FAULT && currentState != STATE_STOP) {
    currentState = STATE_RUN;
  }
}

/* =========================
   WEB UI
   ========================= */
const char* stateName(State s) {
  switch(s) {
    case STATE_WAIT: return "WAIT";
    case STATE_HOME: return "HOME";
    case STATE_RUN:  return "RUN";
    case STATE_STOP: return "STOP";
    case STATE_FAULT:return "FAULT";
    default: return "?";
  }
}
const char* keyName(KeyMode k) {
  switch(k) {
    case KEY_OFF:  return "OFF";
    case KEY_MAN:  return "MAN";
    case KEY_AUTO: return "AUTO";
    default: return "?";
  }
}

String htmlPage() {
  String s;
  s += F(
  "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>RobotCtrl</title>"
  "<style>"
  "body{font-family:Arial;margin:16px;background:#111;color:#eee}"
  ".card{background:#1b1b1b;padding:14px;border-radius:10px;margin-bottom:12px}"
  "button{padding:10px 14px;margin:6px;border-radius:8px;border:0;font-size:16px}"
  ".b{background:#2d6cdf;color:#fff}"
  ".g{background:#23a559;color:#fff}"
  ".y{background:#d6b400;color:#111}"
  ".r{background:#d23b3b;color:#fff}"
  ".w{background:#777;color:#fff}"
  "input[type=range]{width:100%}"
  "label{display:block;margin-top:10px}"
  "select{width:100%;padding:10px;border-radius:8px;background:#111;color:#eee;border:1px solid #333}"
  "</style></head><body>"
  "<h2>RobotCtrl</h2>"

  "<div class='card'>"
  "<div><b>Status:</b> <span id='st'>...</span></div>"
  "<div><b>Key:</b> <span id='km'>...</span></div>"
  "<label>Key-Switch</label>"
  "<select id='keysel' onchange='setKey()'>"
  "<option value='off'>OFF</option>"
  "<option value='man'>MAN</option>"
  "<option value='auto'>AUTO</option>"
  "</select>"
  "<button class='g' onclick=\"cmd('start')\">Start</button>"
  "<button class='r' onclick=\"cmd('stop')\">Stop</button>"
  "<button class='b' onclick=\"cmd('home')\">Home</button>"
  "<button class='y' onclick=\"cmd('wait')\">Wait</button>"
  "<button class='r' onclick=\"cmd('fault')\">FAULT</button>"
  "<button class='b' onclick=\"cmd('reset')\">Reset/Quit</button>"
  "</div>"

  "<div class='card'>"
  "<h3>Helligkeit</h3>"
  "<label>Ampel: <span id='abv'>0</span></label>"
  "<input id='ab' type='range' min='0' max='255' value='120' oninput='setB()'>"
  "<label>Schaltschrank: <span id='cbv'>0</span></label>"
  "<input id='cb' type='range' min='0' max='255' value='80' oninput='setB()'>"
  "</div>"

  "<div class='card'>"
  "<h3>Servos (nur in MAN)</h3>"
  "<div id='servos'></div>"
  "</div>"

  "<div class='card'>"
  "<h3>Web OTA</h3>"
  "<form method='POST' action='/update' enctype='multipart/form-data'>"
  "<input type='file' name='update'>"
  "<button class='w' type='submit'>Upload</button>"
  "</form>"
  "</div>"

  "<script>"
  "const names=['Base','Schulter','Ellenbogen','Drehen','Kippen','Greifen'];"
  "function qs(x){return document.querySelector(x)}"
  "function cmd(s){fetch('/state?s='+s).then(r=>r.text()).then(_=>poll());}"
  "function setB(){"
  "  const a=qs('#ab').value, c=qs('#cb').value;"
  "  qs('#abv').innerText=a; qs('#cbv').innerText=c;"
  "  fetch('/brightness?amp='+a+'&cab='+c);"
  "}"
  "function mkServos(){"
  "  let h='';"
  "  for(let i=0;i<6;i++){"
  "    h+=`<label>${names[i]}: <span id='sv${i}'>90</span>&deg;</label>`;"
  "    h+=`<input type='range' min='0' max='180' value='90' oninput='setServo(${i},this.value)'>`;"
  "  }"
  "  qs('#servos').innerHTML=h;"
  "}"
  "function setServo(i,v){qs('#sv'+i).innerText=v; fetch(`/servo?i=${i}&deg=${v}`);}"
  "function setKey(){"
  "  const m=qs('#keysel').value;"
  "  fetch('/key?m='+m).then(_=>poll());"
  "}"
  "function poll(){fetch('/status').then(r=>r.json()).then(j=>{"
  "  qs('#st').innerText=j.state;"
  "  qs('#km').innerText=j.key;"
  "  qs('#ab').value=j.ampB; qs('#cb').value=j.cabB;"
  "  qs('#abv').innerText=j.ampB; qs('#cbv').innerText=j.cabB;"
  "  qs('#keysel').value=j.key.toLowerCase();"
  "  for(let i=0;i<6;i++){const e=qs('#sv'+i); if(e) e.innerText=j.servo[i];}"
  "});}"
  "mkServos(); setB(); poll(); setInterval(poll,800);"
  "</script></body></html>"
  );
  return s;
}

/* =========================
   WEB HANDLERS
   ========================= */
void handleRoot() { server.send(200, "text/html", htmlPage()); }

void handleStatus() {
  String json = "{";
  json += "\"state\":\""; json += stateName(currentState); json += "\",";
  json += "\"key\":\"";   json += keyName(keyMode);        json += "\",";
  json += "\"ampB\":";    json += ampBrightness;           json += ",";
  json += "\"cabB\":";    json += cabBrightness;           json += ",";
  json += "\"servo\":[";
  for (int i=0;i<6;i++) { json += (int)round(servoCur[i]); if (i<5) json += ","; }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleKey() {
  if (!server.hasArg("m")) { server.send(400,"text/plain","Missing m"); return; }
  String m = server.arg("m");
  if      (m == "off")  keyMode = KEY_OFF;
  else if (m == "man")  keyMode = KEY_MAN;
  else if (m == "auto") keyMode = KEY_AUTO;
  else { server.send(400,"text/plain","Bad m"); return; }

  server.send(200,"text/plain","OK");
}

void handleBrightness() {
  if (server.hasArg("amp")) ampBrightness = (uint8_t)server.arg("amp").toInt();
  if (server.hasArg("cab")) cabBrightness = (uint8_t)server.arg("cab").toInt();
  server.send(200,"text/plain","OK");
}

void handleServo() {
  if (!server.hasArg("i") || !server.hasArg("deg")) { server.send(400,"text/plain","Missing"); return; }
  int i = server.arg("i").toInt();
  float d = server.arg("deg").toFloat();
  if (i < 0 || i > 5) { server.send(400,"text/plain","Bad index"); return; }

  if (requireReset()) { server.send(403,"text/plain","Reset required"); return; }
  if (!motionAllowedFor(keyMode, true)) { server.send(403,"text/plain","Key not allowing manual move"); return; }

  setServoTarget(i, d);
  server.send(200,"text/plain","OK");
}

void handleHome() {
  if (requireReset()) { server.send(403,"text/plain","Reset required"); return; }
  if (!motionAllowedFor(keyMode, false)) { server.send(403,"text/plain","Key OFF or STOP"); return; }

  setAllHomeTargets();
  currentState = STATE_RUN; // während Fahrt
  server.send(200,"text/plain","OK");
}

void handleState() {
  if (!server.hasArg("s")) { server.send(400,"text/plain","Missing s"); return; }
  String s = server.arg("s");

  // FAULT verlassen nur mit reset
  if (requireReset() && s != "reset") {
    server.send(403, "text/plain", "Reset required");
    return;
  }

  if (s == "fault") { setFault(); server.send(200,"text/plain","OK"); return; }
  if (s == "reset") { resetFaultToStop(); server.send(200,"text/plain","OK"); return; }

  if (s == "stop") {
    currentState = STATE_STOP;
    freezeMotion();
    server.send(200,"text/plain","OK");
    return;
  }

  // Start/Home/Wait sind nur erlaubt wenn Key nicht OFF und nicht STOP/FAULT
  if (!motionAllowedFor(keyMode, false)) {
    server.send(403,"text/plain","Key OFF or STOP");
    return;
  }

  if (s == "home") {
    setAllHomeTargets();
    currentState = STATE_RUN;
  } else if (s == "start") {
    // Start = RUN "anstoßen". Wenn nichts fährt, fällt er nach kurzer Zeit auf WAIT/HOME zurück (über motion update).
    currentState = STATE_RUN;
  } else if (s == "wait") {
    // WAIT ist nur ein Statuswunsch; Bewegung bleibt wie sie ist.
    if (currentState != STATE_STOP && currentState != STATE_FAULT) currentState = STATE_WAIT;
  }

  server.send(200,"text/plain","OK");
}

/* OTA */
void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}
void handleUpdateDone() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK - rebooting");
  delay(300);
  ESP.restart();
}

/* =========================
   SETUP / LOOP
   ========================= */
void setup() {
  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, HIGH);

  // FastLED (global brightness bleibt 255, wir skalieren pro-strip per nscale)
  FastLED.addLeds<WS2812B, AMP_PIN, GRB>(ampLeds, AMP_LEDS);
  FastLED.addLeds<WS2812B, CAB_PIN, GRB>(cabLeds, CAB_LEDS);
  FastLED.setBrightness(255);

  // I2C + PCA
  Wire.begin(I2C_SDA, I2C_SCL);
  pca.begin();
  pca.setPWMFreq(SERVO_FREQ);

  // Initial: Home pose schreiben
  for (int i=0;i<6;i++) {
    servoCur[i] = (float)servoHome[i];
    servoTgt[i] = (float)servoHome[i];
    writeServoPwm(i, servoCur[i]);
  }
  currentState = STATE_HOME;

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Routes
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/brightness", handleBrightness);
  server.on("/servo", handleServo);
  server.on("/home", handleHome);
  server.on("/state", handleState);
  server.on("/key", handleKey);

  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  server.begin();
}

void loop() {
  server.handleClient();

  updateMotion();  // Ramp + RUN/WAIT/HOME Automatik
  updateLeds();    // Ampel + Cabinet + Onboard LED
}
