#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>
#include <FastLED.h>
#include <Adafruit_PWMServoDriver.h>

/* ---------------- PINS / HW ---------------- */
#define AMP_PIN     16      // Ampel WS2812B data (4 LEDs)
#define CAB_PIN     17      // Schaltschranklicht WS2812B data (10 LEDs)
#define AMP_LEDS    4
#define CAB_LEDS    10

#define I2C_SDA     21
#define I2C_SCL     22

#define ONBOARD_LED 2       // bei vielen ESP32 Devboards GPIO2. Wenn bei dir anders: ändern.

#define PCA_ADDR    0x40
#define SERVO_FREQ  50

/* ---------------- SERVOS (PCA9685 Kanäle) ---------------- */
// Reihenfolge wie in deinem UI: Base, Schulter, Ellenbogen, Drehen, Kippen, Greifen
static const uint8_t SERVO_CH[6]   = {0, 1, 2, 3, 4, 5};
static const char*   SERVO_NAME[6] = {"Base","Schulter","Ellenbogen","Drehen","Kippen","Greifen"};

// Pulsbreiten (typisch). Wenn bei dir zu viel/zu wenig Weg: anpassen.
static const uint16_t SERVO_MIN_PULSE = 110;  // ~0°
static const uint16_t SERVO_MAX_PULSE = 510;  // ~180°

/* Achs-Limits (Grad). Stell die realistisch ein, sonst knallt's mechanisch. */
int servoMinDeg[6] = {  0,  0,  0,  0,  0,  0};
int servoMaxDeg[6] = {180,180,180,180,180,180};

/* Home-Positionen (Grad) */
int servoHomeDeg[6] = {90, 90, 90, 90, 90, 90};

/* Rampe */
float rampDegPerSec[6] = {90, 90, 90, 120, 120, 140}; // pro Achse, anpassen
const uint16_t SERVO_UPDATE_MS = 20;                   // 50 Hz

/* Home Toleranz */
const int HOME_TOL_DEG = 2;

/* ---------------- LEDS ---------------- */
CRGB ampLeds[AMP_LEDS];
CRGB cabLeds[CAB_LEDS];

CLEDController* ampCtl = nullptr;
CLEDController* cabCtl = nullptr;

/* ---------------- PCA ---------------- */
Adafruit_PWMServoDriver pca(PCA_ADDR);

/* ---------------- STATE ---------------- */
enum SafetyState : uint8_t { SAFETY_NORMAL, SAFETY_STOP, SAFETY_FAULT };
SafetyState safetyState = SAFETY_STOP;  // nach Boot gehen wir auf STOP/rot (konsequent)

uint8_t ampBrightness = 120; // 0..255
uint8_t cabBrightness = 80;  // 0..255

bool faultBlink = false;
uint32_t faultBlinkT = 0;

/* Servo Motion */
struct AxisMotion {
  float curDeg = 90.0f;
  float tgtDeg = 90.0f;
  bool  moving = false;
};
AxisMotion axis[6];

uint32_t lastServoUpdate = 0;

/* ---------------- WIFI / WEB ---------------- */
WebServer server(80);
const char* AP_SSID = "RobotCtrl";
const char* AP_PASS = "12345678";

/* ---------------- UTILS ---------------- */
static inline int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline uint16_t degToPwm(float deg) {
  if (deg < 0) deg = 0;
  if (deg > 180) deg = 180;
  const float span = (float)(SERVO_MAX_PULSE - SERVO_MIN_PULSE);
  return (uint16_t)(SERVO_MIN_PULSE + (span * (deg / 180.0f)));
}

void writeServoNow(uint8_t idx, float deg) {
  uint16_t pwm = degToPwm(deg);
  pca.setPWM(SERVO_CH[idx], 0, pwm);
}

bool anyAxisMoving() {
  for (int i=0;i<6;i++) if (axis[i].moving) return true;
  return false;
}

bool atHome() {
  for (int i=0;i<6;i++) {
    if (abs((int)round(axis[i].curDeg) - servoHomeDeg[i]) > HOME_TOL_DEG) return false;
  }
  return true;
}

/* ---------------- LED HELPERS ---------------- */
static inline CRGB scaleColor(CRGB c, uint8_t b) {
  c.nscale8_video(b);
  return c;
}

void setAmpelOneOf4(CRGB color) {
  // LED1 (index0) blau, LED2 (index1) grün, LED3 (index2) gelb, LED4 (index3) rot
  for (int i=0;i<AMP_LEDS;i++) ampLeds[i] = CRGB::Black;

  if (color == CRGB::Blue)   ampLeds[0] = scaleColor(CRGB::Blue,   ampBrightness);
  if (color == CRGB::Green)  ampLeds[1] = scaleColor(CRGB::Green,  ampBrightness);
  if (color == CRGB::Yellow) ampLeds[2] = scaleColor(CRGB::Yellow, ampBrightness);
  if (color == CRGB::Red)    ampLeds[3] = scaleColor(CRGB::Red,    ampBrightness);
}

void updateCabinet() {
  CRGB w = scaleColor(CRGB::White, cabBrightness);
  for (int i=0;i<CAB_LEDS;i++) cabLeds[i] = w;
}

void updateOnboardLED() {
  if (safetyState == SAFETY_FAULT) {
    digitalWrite(ONBOARD_LED, ((millis()/250)%2) ? HIGH : LOW);
  } else {
    digitalWrite(ONBOARD_LED, HIGH); // immer an, solange ESP läuft
  }
}

void updateLeds() {
  if (safetyState == SAFETY_FAULT) {
    if (millis() - faultBlinkT > 300) {
      faultBlinkT = millis();
      faultBlink = !faultBlink;
    }
    for (int i=0;i<AMP_LEDS;i++) ampLeds[i] = CRGB::Black;
    if (faultBlink) ampLeds[3] = scaleColor(CRGB::Red, ampBrightness); // rot blink
  } else if (safetyState == SAFETY_STOP) {
    setAmpelOneOf4(CRGB::Red); // STOP = rot dauerhaft
  } else {
    // SAFETY_NORMAL: Anzeige abhängig von Aktivität / Home
    if (anyAxisMoving()) {
      setAmpelOneOf4(CRGB::Green); // RUN (nur während Bewegung)
    } else if (atHome()) {
      setAmpelOneOf4(CRGB::Blue);  // HOME
    } else {
      setAmpelOneOf4(CRGB::Yellow);// WAIT (steht, nicht home)
    }
  }

  updateCabinet();

  // pro-strip Helligkeit sauber getrennt:
  if (ampCtl) ampCtl->setBrightness(255);
  if (cabCtl) cabCtl->setBrightness(255);

  FastLED.show();
  updateOnboardLED();
}

/* ---------------- SAFETY / STATE RULES ---------------- */
void setFault() {
  safetyState = SAFETY_FAULT;
  // sofort Bewegungen stoppen (Targets bleiben, aber wir fahren nicht weiter)
  for (int i=0;i<6;i++) axis[i].moving = false;
}

void resetFaultToStop() {
  // deine Vorgabe: Reset endet IMMER in STOP
  if (safetyState == SAFETY_FAULT) {
    safetyState = SAFETY_STOP;
    for (int i=0;i<6;i++) axis[i].moving = false;
  }
}

void setStop() {
  if (safetyState != SAFETY_FAULT) {
    safetyState = SAFETY_STOP;
    for (int i=0;i<6;i++) axis[i].moving = false;
  }
}

void setNormal() {
  if (safetyState != SAFETY_FAULT) {
    safetyState = SAFETY_NORMAL;
  }
}

/* ---------------- MOTION ---------------- */
void setTargetDeg(uint8_t i, int deg) {
  deg = clampi(deg, servoMinDeg[i], servoMaxDeg[i]);
  axis[i].tgtDeg = (float)deg;

  // wenn wir normal sind: Bewegung freigeben
  // wenn STOP: Ziel setzen ja, aber bewegen erst, wenn wieder NORMAL (Start/Home)
  if (safetyState == SAFETY_NORMAL) {
    axis[i].moving = (fabs(axis[i].tgtDeg - axis[i].curDeg) >= 0.5f);
  }
}

void startMoveToTargets() {
  // Start: wechselt STOP->NORMAL, und fährt alle Achsen, die nicht am Ziel sind.
  setNormal();
  for (int i=0;i<6;i++) {
    axis[i].moving = (fabs(axis[i].tgtDeg - axis[i].curDeg) >= 0.5f);
  }
}

void commandHome() {
  if (safetyState == SAFETY_FAULT) return;
  setNormal(); // Home darf fahren => normal
  for (int i=0;i<6;i++) {
    axis[i].tgtDeg = (float)clampi(servoHomeDeg[i], servoMinDeg[i], servoMaxDeg[i]);
    axis[i].moving = (fabs(axis[i].tgtDeg - axis[i].curDeg) >= 0.5f);
  }
}

void servoMotionLoop() {
  if (safetyState == SAFETY_FAULT) return;  // nix bewegen
  if (safetyState == SAFETY_STOP) return;   // nix bewegen

  const uint32_t now = millis();
  if (now - lastServoUpdate < SERVO_UPDATE_MS) return;
  const float dt = (now - lastServoUpdate) / 1000.0f;
  lastServoUpdate = now;

  for (int i=0;i<6;i++) {
    if (!axis[i].moving) continue;

    float cur = axis[i].curDeg;
    float tgt = axis[i].tgtDeg;
    float maxStep = rampDegPerSec[i] * dt;

    float diff = tgt - cur;
    if (fabs(diff) <= maxStep) {
      cur = tgt;
      axis[i].moving = false;
    } else {
      cur += (diff > 0 ? maxStep : -maxStep);
    }

    // clamp final
    cur = (float)clampi((int)round(cur), servoMinDeg[i], servoMaxDeg[i]);
    axis[i].curDeg = cur;

    writeServoNow((uint8_t)i, cur);
  }
}

/* ---------------- WEB UI ---------------- */
String stateString() {
  if (safetyState == SAFETY_FAULT) return "FAULT";
  if (safetyState == SAFETY_STOP)  return "STOP";
  // normal:
  if (anyAxisMoving()) return "RUN";
  if (atHome()) return "HOME";
  return "WAIT";
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
  "</style></head><body>"
  "<h2>RobotCtrl</h2>"

  "<div class='card'>"
  "<div><b>Status:</b> <span id='st'>...</span></div>"
  "<button class='g' onclick=\"cmd('start')\">Start</button>"
  "<button class='r' onclick=\"cmd('stop')\">Stop</button>"
  "<button class='b' onclick=\"cmd('home')\">Home</button>"
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
  "<h3>Servos (Targets)</h3>"
  "<div id='servos'></div>"
  "<button class='w' onclick=\"cmd('apply')\">Apply / fahren (Start)</button>"
  "<button class='b' onclick=\"cmd('home')\">HOME Position fahren</button>"
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
  "function cmd(s){fetch('/cmd?c='+s).then(r=>r.text()).then(_=>poll());}"
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
  "function poll(){fetch('/status').then(r=>r.json()).then(j=>{"
  "  qs('#st').innerText=j.state;"
  "  qs('#ab').value=j.ampB; qs('#cb').value=j.cabB;"
  "  qs('#abv').innerText=j.ampB; qs('#cbv').innerText=j.cabB;"
  "  for(let i=0;i<6;i++){const e=qs('#sv'+i); if(e) e.innerText=j.tgt[i];}"
  "});}"
  "mkServos(); setB(); poll(); setInterval(poll,800);"
  "</script></body></html>"
  );
  return s;
}

/* ---------------- WEB HANDLERS ---------------- */
void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleStatus() {
  String json = "{";
  json += "\"state\":\""; json += stateString(); json += "\",";
  json += "\"ampB\":"; json += ampBrightness; json += ",";
  json += "\"cabB\":"; json += cabBrightness; json += ",";
  json += "\"tgt\":[";
  for (int i=0;i<6;i++) { json += (int)round(axis[i].tgtDeg); if (i<5) json += ","; }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleCmd() {
  if (!server.hasArg("c")) { server.send(400,"text/plain","Missing c"); return; }
  String c = server.arg("c");

  // FAULT: nur reset erlaubt
  if (safetyState == SAFETY_FAULT && c != "reset") {
    server.send(403,"text/plain","Reset required");
    return;
  }

  if (c == "fault") { setFault(); server.send(200,"text/plain","OK"); return; }
  if (c == "reset") { resetFaultToStop(); server.send(200,"text/plain","OK"); return; }

  if (c == "stop")  { setStop(); server.send(200,"text/plain","OK"); return; }

  if (c == "start" || c == "apply") {
    startMoveToTargets(); // STOP->NORMAL, fährt auf Targets
    server.send(200,"text/plain","OK"); return;
  }

  if (c == "home") {
    commandHome();
    server.send(200,"text/plain","OK"); return;
  }

  server.send(400,"text/plain","Unknown cmd");
}

void handleBrightness() {
  if (server.hasArg("amp")) ampBrightness = (uint8_t)server.arg("amp").toInt();
  if (server.hasArg("cab")) cabBrightness = (uint8_t)server.arg("cab").toInt();
  server.send(200,"text/plain","OK");
}

void handleServo() {
  if (!server.hasArg("i") || !server.hasArg("deg")) { server.send(400,"text/plain","Missing"); return; }
  int i = server.arg("i").toInt();
  int d = server.arg("deg").toInt();
  if (i < 0 || i > 5) { server.send(400,"text/plain","Bad index"); return; }

  // in FAULT sperren wir alles
  if (safetyState == SAFETY_FAULT) { server.send(403,"text/plain","Reset required"); return; }

  // Ziel setzen (bewegt erst bei Start / Home / Normal)
  setTargetDeg((uint8_t)i, d);
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

/* ---------------- SETUP/LOOP ---------------- */
void setup() {
  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, HIGH);

  // LEDs
  ampCtl = &FastLED.addLeds<WS2812B, AMP_PIN, GRB>(ampLeds, AMP_LEDS);
  cabCtl = &FastLED.addLeds<WS2812B, CAB_PIN, GRB>(cabLeds, CAB_LEDS);
  FastLED.setBrightness(255);

  // I2C + PCA
  Wire.begin(I2C_SDA, I2C_SCL);
  pca.begin();
  pca.setPWMFreq(SERVO_FREQ);

  // init axis (cur=tgt=home)
  for (int i=0;i<6;i++) {
    axis[i].curDeg = (float)servoHomeDeg[i];
    axis[i].tgtDeg = (float)servoHomeDeg[i];
    axis[i].moving = false;
    writeServoNow((uint8_t)i, axis[i].curDeg);
  }

  lastServoUpdate = millis();

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Web routes
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/cmd", handleCmd);
  server.on("/brightness", handleBrightness);
  server.on("/servo", handleServo);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  server.begin();

  // Startzustand: STOP (rot)
  safetyState = SAFETY_STOP;
}

void loop() {
  server.handleClient();
  servoMotionLoop();
  updateLeds();
}
