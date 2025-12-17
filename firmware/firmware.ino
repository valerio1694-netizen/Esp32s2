#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>
#include <FastLED.h>
#include <Adafruit_PWMServoDriver.h>

/* ---------------- PINS / HW ---------------- */
#define AMP_PIN     16      // Ampel WS2812B data
#define CAB_PIN     17      // Schaltschranklicht WS2812B data
#define AMP_LEDS    4
#define CAB_LEDS    10

#define I2C_SDA     21
#define I2C_SCL     22

#define ONBOARD_LED 2       // viele ESP32 Devboards: GPIO2. Falls bei dir anders -> ändern.

#define PCA_ADDR    0x40
#define SERVO_FREQ  50

/* ---------------- SERVOS (PCA9685 Kanäle) ---------------- */
// Reihenfolge wie in deinem UI: Base, Schulter, Ellenbogen, Drehen, Kippen, Greifen
static const uint8_t SERVO_CH[6] = {0, 1, 2, 3, 4, 5};
static const char* SERVO_NAME[6] = {"Base","Schulter","Ellenbogen","Drehen","Kippen","Greifen"};

// Pulsbreiten (typisch). Wenn bei dir knallt: anpassen.
static const uint16_t SERVO_MIN = 110;  // ~0°
static const uint16_t SERVO_MAX = 510;  // ~180°

/* ---------------- LEDS ---------------- */
CRGB ampLeds[AMP_LEDS];
CRGB cabLeds[CAB_LEDS];

/* ---------------- PCA ---------------- */
Adafruit_PWMServoDriver pca(PCA_ADDR);

/* ---------------- STATE ---------------- */
enum State : uint8_t { STATE_WAIT, STATE_HOME, STATE_RUN, STATE_STOP, STATE_FAULT };
State currentState = STATE_WAIT;
State stateBeforeFault = STATE_WAIT;

uint8_t ampBrightness = 120;     // 0..255
uint8_t cabBrightness = 80;      // 0..255

bool faultBlink = false;
uint32_t blinkT = 0;

/* Servo positions in degrees */
int servoDeg[6]     = {90, 90, 90, 90, 90, 90};
int servoHomeDeg[6] = {90, 90, 90, 90, 90, 90};
bool servosEnabled = true; // für Test: wir lassen enabled, aber du kannst später darüber "Freigabe" bauen

/* ---------------- WIFI / WEB ---------------- */
WebServer server(80);
const char* AP_SSID = "RobotCtrl";
const char* AP_PASS = "12345678";

/* ---------------- UTILS ---------------- */
static inline uint16_t degToPwm(int deg) {
  if (deg < 0) deg = 0;
  if (deg > 180) deg = 180;
  // linear map
  return (uint16_t)(SERVO_MIN + ( (SERVO_MAX - SERVO_MIN) * (float)deg / 180.0f ));
}

void writeServo(uint8_t idx, int deg) {
  servoDeg[idx] = deg;
  if (!servosEnabled) return;
  uint16_t pwm = degToPwm(deg);
  pca.setPWM(SERVO_CH[idx], 0, pwm);
}

void moveAllHome() {
  for (int i=0;i<6;i++) writeServo(i, servoHomeDeg[i]);
}

CRGB scaleColor(CRGB c, uint8_t b) {
  c.nscale8_video(b);
  return c;
}

/* ---------------- LED LOGIC ---------------- */
void setAmpelSolid(CRGB color) {
  CRGB c = scaleColor(color, ampBrightness);

  // gewünschte Reihenfolge (deine Vorgabe):
  // LED1 blau, LED2 grün, LED3 gelb, LED4 rot
  // => Wir zeigen IMMER nur die passende LED, nicht alle.
  // HOME=blau => LED0 an
  // RUN=grün  => LED1 an
  // WAIT=gelb => LED2 an
  // STOP=rot  => LED3 an
  for (int i=0;i<AMP_LEDS;i++) ampLeds[i] = CRGB::Black;

  if (color == CRGB::Blue)   ampLeds[0] = c;
  if (color == CRGB::Green)  ampLeds[1] = c;
  if (color == CRGB::Yellow) ampLeds[2] = c;
  if (color == CRGB::Red)    ampLeds[3] = c;
}

void updateCabinet() {
  CRGB w = scaleColor(CRGB::White, cabBrightness);
  for (int i=0;i<CAB_LEDS;i++) cabLeds[i] = w;
}

void updateOnboard() {
  // “rot” geht bei Onboard nicht – ist nur an/aus.
  // Vorgabe: immer an solange normal; bei Fault blinkt.
  if (currentState == STATE_FAULT) {
    digitalWrite(ONBOARD_LED, (millis()/250)%2 ? HIGH : LOW);
  } else {
    digitalWrite(ONBOARD_LED, HIGH);
  }
}

void updateLeds() {
  if (currentState == STATE_FAULT) {
    if (millis() - blinkT > 300) {
      blinkT = millis();
      faultBlink = !faultBlink;
    }
    // Fault: ROT blinkend auf LED4 (Index 3)
    for (int i=0;i<AMP_LEDS;i++) ampLeds[i] = CRGB::Black;
    if (faultBlink) ampLeds[3] = scaleColor(CRGB::Red, ampBrightness);
  } else {
    switch(currentState) {
      case STATE_WAIT: setAmpelSolid(CRGB::Yellow); break;
      case STATE_HOME: setAmpelSolid(CRGB::Blue);   break;
      case STATE_RUN:  setAmpelSolid(CRGB::Green);  break;
      case STATE_STOP: setAmpelSolid(CRGB::Red);    break;
      default: break;
    }
  }

  updateCabinet();
  FastLED.show();
  updateOnboard();
}

/* ---------------- STATE RULES ---------------- */
bool requireReset() {
  return currentState == STATE_FAULT;
}

void setFault() {
  if (currentState != STATE_FAULT) stateBeforeFault = currentState;
  currentState = STATE_FAULT;
}

void resetFault() {
  if (currentState == STATE_FAULT) {
    currentState = stateBeforeFault; // genau wie du willst
  }
}

/* ---------------- WEB UI ---------------- */
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
  "<button class='g' onclick=\"cmd('run')\">Start (RUN)</button>"
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
  "<h3>Servos</h3>"
  "<div id='servos'></div>"
  "<button class='b' onclick=\"goHome()\">HOME Position fahren</button>"
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
  "function cmd(s){fetch('/state?s='+s).then(r=>r.text()).then(t=>{poll();});}"
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
  "function goHome(){fetch('/home').then(_=>poll());}"
  "function poll(){fetch('/status').then(r=>r.json()).then(j=>{"
  "  qs('#st').innerText=j.state;"
  "  qs('#ab').value=j.ampB; qs('#cb').value=j.cabB;"
  "  qs('#abv').innerText=j.ampB; qs('#cbv').innerText=j.cabB;"
  "  for(let i=0;i<6;i++){const e=qs('#sv'+i); if(e) e.innerText=j.servo[i];}"
  "});}"
  "mkServos(); setB(); poll(); setInterval(poll,1000);"
  "</script></body></html>"
  );
  return s;
}

/* ---------------- WEB HANDLERS ---------------- */
void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

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

void handleStatus() {
  String json = "{";
  json += "\"state\":\""; json += stateName(currentState); json += "\",";
  json += "\"ampB\":"; json += ampBrightness; json += ",";
  json += "\"cabB\":"; json += cabBrightness; json += ",";
  json += "\"servo\":[";
  for (int i=0;i<6;i++) { json += servoDeg[i]; if (i<5) json += ","; }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleState() {
  if (!server.hasArg("s")) { server.send(400,"text/plain","Missing s"); return; }
  String s = server.arg("s");

  // FAULT-Exit-Rule: ohne reset geht nix raus, auch nicht STOP
  if (requireReset() && s != "reset") {
    server.send(403, "text/plain", "Reset required");
    return;
  }

  if (s == "fault") { setFault(); server.send(200,"text/plain","OK"); return; }
  if (s == "reset") { resetFault(); server.send(200,"text/plain","OK"); return; }

  if (s == "wait") currentState = STATE_WAIT;
  else if (s == "home") currentState = STATE_HOME;
  else if (s == "run") currentState = STATE_RUN;
  else if (s == "stop") currentState = STATE_STOP;

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
  int d = server.arg("deg").toInt();
  if (i < 0 || i > 5) { server.send(400,"text/plain","Bad index"); return; }

  // in FAULT sperren wir alles
  if (requireReset()) { server.send(403,"text/plain","Reset required"); return; }

  writeServo((uint8_t)i, d);
  server.send(200,"text/plain","OK");
}

void handleHomeMove() {
  // in FAULT sperren
  if (requireReset()) { server.send(403,"text/plain","Reset required"); return; }

  moveAllHome();
  currentState = STATE_HOME;
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
  FastLED.addLeds<WS2812B, AMP_PIN, GRB>(ampLeds, AMP_LEDS);
  FastLED.addLeds<WS2812B, CAB_PIN, GRB>(cabLeds, CAB_LEDS);
  FastLED.setBrightness(255); // wir skalieren pro-strip selbst

  // I2C + PCA
  Wire.begin(I2C_SDA, I2C_SCL);
  pca.begin();
  pca.setPWMFreq(SERVO_FREQ);

  // initial servos to home
  moveAllHome();
  currentState = STATE_HOME;

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Web routes
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/state", handleState);
  server.on("/brightness", handleBrightness);
  server.on("/servo", handleServo);
  server.on("/home", handleHomeMove);

  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  server.begin();
}

void loop() {
  server.handleClient();
  updateLeds();
}
