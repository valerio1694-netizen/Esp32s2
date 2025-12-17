/*
  ESP32 RobotArm Controller (Web UI + Web OTA + PCA9685 + WS2812B)
  - Ampel: 4x WS2812B an GPIO16 (Index: 0=Blau,1=Grün,2=Gelb,3=Rot) -> Gelb wird nicht benutzt
  - Schaltschranklicht: 10x WS2812B an GPIO17 (weiß, PWM-dimmbar über Helligkeit)
  - Status:
      RUN  = Grün dauerhaft
      HOME = Blau dauerhaft
      WAIT/STOP = Rot dauerhaft   (gewünscht)
      FAULT = Rot blinkend        (gewünscht)
  - Onboard LED: dauerhaft AN solange Power; bei FAULT blinkt
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>

#include <Adafruit_PWMServoDriver.h>
#include <FastLED.h>

// ------------------- WLAN -------------------
static const char* WIFI_SSID = "RobotArm";
static const char* WIFI_PASS = "12345678";

// ------------------- Pins -------------------
static const int PIN_AMPEL   = 16;   // 4x WS2812B
static const int PIN_CABINET = 17;   // 10x WS2812B

#ifndef LED_BUILTIN
  #define LED_BUILTIN 2
#endif

// ------------------- LEDs -------------------
static const int AMPEL_LEDS   = 4;
static const int CABINET_LEDS = 10;

CRGB ampel[AMPEL_LEDS];
CRGB cabinet[CABINET_LEDS];

uint8_t ampelBrightness   = 40;   // 0..255
uint8_t cabinetBrightness = 80;   // 0..255

// ------------------- PCA9685 / Servos -------------------
Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);

// Standard 50Hz Servo: Pulsbreite ~500..2500us
static const uint16_t SERVO_MIN_US = 500;
static const uint16_t SERVO_MAX_US = 2500;
static const uint16_t SERVO_FREQ   = 50;

static const int SERVO_COUNT = 6;
// PCA Channels
static const uint8_t servoCh[SERVO_COUNT] = {0, 1, 2, 3, 4, 5}; // Base, Schulter, Ellenbogen, Drehen, Kippen, Greifen

// Aktuelle Winkel
int servoDeg[SERVO_COUNT] = {90, 90, 90, 90, 90, 90};

// Home-Werte (kannst du später per "Save Home" speichern; hier default 90)
int homeDeg[SERVO_COUNT]  = {90, 90, 90, 90, 90, 90};

// Soft-Limits (vorerst 0..180, kannst du später enger machen)
int minDeg[SERVO_COUNT]   = {0, 0, 0, 0, 0, 0};
int maxDeg[SERVO_COUNT]   = {180, 180, 180, 180, 180, 180};

// ------------------- Zustände -------------------
enum State : uint8_t { ST_WAIT, ST_RUN, ST_HOME, ST_FAULT };
State state = ST_WAIT;

// Merker für “was war vor FAULT”
State stateBeforeFault = ST_WAIT;

// Key-Schalter Simulation (Web)
enum Mode : uint8_t { MODE_OFF, MODE_AUTO, MODE_MANUAL };
Mode keyMode = MODE_OFF; // 0=Aus, 1=Auto, 2=Manuell

// ------------------- Web -------------------
WebServer server(80);

// ------------------- Timing -------------------
unsigned long lastBlinkMs = 0;
bool faultBlinkOn = false;

// ------------------- Helpers -------------------
static int clampi(int v, int lo, int hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

static uint16_t usToPcaTicks(uint16_t us) {
  // PCA9685 hat 4096 Ticks pro Periode
  // Periode (us) = 1e6 / freq
  const float period_us = 1000000.0f / (float)SERVO_FREQ;
  const float ticks = (4096.0f * (float)us) / period_us;
  return (uint16_t)clampi((int)(ticks + 0.5f), 0, 4095);
}

static void writeServoDeg(int idx, int deg) {
  deg = clampi(deg, minDeg[idx], maxDeg[idx]);
  servoDeg[idx] = deg;

  const uint16_t us = (uint16_t)map(deg, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  const uint16_t ticks = usToPcaTicks(us);
  pca.setPWM(servoCh[idx], 0, ticks);
}

static void applyAllServos() {
  for (int i = 0; i < SERVO_COUNT; i++) writeServoDeg(i, servoDeg[i]);
}

static void setAllCabinetWhite() {
  for (int i = 0; i < CABINET_LEDS; i++) cabinet[i] = CRGB::White;
}

static void renderAmpel() {
  // Alles aus
  for (int i = 0; i < AMPEL_LEDS; i++) ampel[i] = CRGB::Black;

  // Index-Definition: 0=Blau,1=Grün,2=Gelb(ungenutzt),3=Rot
  if (state == ST_HOME) {
    ampel[0] = CRGB::Blue;
  } else if (state == ST_RUN) {
    ampel[1] = CRGB::Green;
  } else if (state == ST_WAIT) {
    // WICHTIG: STOP/WAIT = ROT dauerhaft (dein Wunsch)
    ampel[3] = CRGB::Red;
  } else if (state == ST_FAULT) {
    ampel[3] = faultBlinkOn ? CRGB::Red : CRGB::Black;
  }
}

static void updateLeds() {
  renderAmpel();
  FastLED.setBrightness(ampelBrightness);
  FastLED.show();

  // Cabinet separat
  setAllCabinetWhite();
  FastLED[1].setBrightness(cabinetBrightness);
  FastLED.show();
}

static void setState(State s) {
  if (state == ST_FAULT && s != ST_FAULT) {
    // FAULT darf nur über Reset/Quit verlassen werden -> wird im Handler erzwungen
  }
  state = s;
}

static void enterFault() {
  if (state != ST_FAULT) {
    stateBeforeFault = state;
    state = ST_FAULT;
  }
}

static void clearFaultViaReset() {
  // Nach Quittierung NICHT automatisch Home/RUN erzwingen.
  // Rücksprung:
  // - Wenn vorher HOME -> HOME
  // - Wenn vorher RUN -> WAIT (sicher)
  // - Sonst -> vorheriger Zustand
  if (stateBeforeFault == ST_RUN) state = ST_WAIT;
  else state = stateBeforeFault;
}

// ------------------- Web UI HTML -------------------
static String htmlPage() {
  String s;
  s.reserve(6000);

  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>RobotArm</title>";
  s += "<style>body{font-family:Arial;background:#111;color:#eee;margin:16px}"
       ".card{background:#1b1b1b;border-radius:12px;padding:14px;margin:12px 0}"
       "button{padding:12px 14px;border:0;border-radius:10px;margin:6px;font-size:16px}"
       ".g{background:#2e7d32;color:#fff}.r{background:#b71c1c;color:#fff}.b{background:#1565c0;color:#fff}.y{background:#f9a825;color:#000}.k{background:#333;color:#fff}"
       "input[type=range]{width:100%}"
       ".row{display:flex;gap:10px;flex-wrap:wrap}"
       ".pill{display:inline-block;padding:6px 10px;border-radius:999px;background:#333;margin-left:8px}"
       "</style></head><body>";

  s += "<h2>RobotArm Control <span class='pill' id='st'>...</span></h2>";

  // Key mode
  s += "<div class='card'><h3>Schluesselschalter (Simulation)</h3>";
  s += "<div class='row'>";
  s += "<button class='k' onclick=\"setMode('off')\">OFF</button>";
  s += "<button class='b' onclick=\"setMode('auto')\">AUTO</button>";
  s += "<button class='g' onclick=\"setMode('man')\">MANUAL</button>";
  s += "</div></div>";

  // Controls
  s += "<div class='card'><h3>Aktionen</h3>";
  s += "<div class='row'>";
  s += "<button class='g' onclick=\"cmd('start')\">Start</button>";
  s += "<button class='r' onclick=\"cmd('stop')\">Stop</button>";
  s += "<button class='b' onclick=\"cmd('home')\">Home fahren</button>";
  s += "<button class='y' onclick=\"cmd('savehome')\">Home speichern</button>";
  s += "<button class='b' onclick=\"cmd('reset')\">Reset / Quit</button>";
  s += "<button class='r' onclick=\"cmd('fault')\">FAULT testen</button>";
  s += "</div></div>";

  // Servo sliders
  const char* names[SERVO_COUNT] = {"Base","Schulter","Ellenbogen","Drehen","Kippen","Greifen"};
  s += "<div class='card'><h3>Servos (MANUAL)</h3>";
  for (int i=0;i<SERVO_COUNT;i++) {
    s += "<div style='margin:10px 0'><b>";
    s += names[i];
    s += ":</b> <span id='v";
    s += i;
    s += "'>90</span>&deg;";
    s += "<input type='range' min='0' max='180' value='90' id='s";
    s += i;
    s += "' oninput=\"setServo(";
    s += i;
    s += ",this.value)\"></div>";
  }
  s += "</div>";

  // Brightness
  s += "<div class='card'><h3>Helligkeit</h3>";
  s += "<b>Ampel</b> <span id='ba'>0</span><input type='range' min='0' max='255' value='40' id='ra' oninput=\"setBright('ampel',this.value)\">";
  s += "<b>Schaltschrank</b> <span id='bc'>0</span><input type='range' min='0' max='255' value='80' id='rc' oninput=\"setBright('cab',this.value)\">";
  s += "</div>";

  // OTA
  s += "<div class='card'><h3>Web OTA</h3>";
  s += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  s += "<input type='file' name='update' accept='.bin' required>";
  s += "<button class='b' type='submit'>Upload .bin</button></form>";
  s += "</div>";

  // Script
  s += "<script>";
  s += "async function j(u){return fetch(u).then(r=>r.json());}\n";
  s += "async function cmd(c){await fetch('/cmd?c='+c);} \n";
  s += "async function setMode(m){await fetch('/mode?m='+m);} \n";
  s += "async function setServo(i,v){document.getElementById('v'+i).innerText=v; await fetch('/servo?i='+i+'&v='+v);} \n";
  s += "async function setBright(w,v){ if(w==='ampel'){document.getElementById('ba').innerText=v;} else {document.getElementById('bc').innerText=v;} await fetch('/bright?w='+w+'&v='+v);} \n";
  s += "async function poll(){let d=await j('/status'); document.getElementById('st').innerText=d.state+' / '+d.mode; ";
  s += "for(let i=0;i<6;i++){let el=document.getElementById('s'+i); if(el){el.value=d.servo[i]; document.getElementById('v'+i).innerText=d.servo[i];}}";
  s += "document.getElementById('ra').value=d.ampelB; document.getElementById('rc').value=d.cabB; document.getElementById('ba').innerText=d.ampelB; document.getElementById('bc').innerText=d.cabB;}\n";
  s += "setInterval(poll,700); poll();";
  s += "</script>";

  s += "</body></html>";
  return s;
}

// ------------------- Handlers -------------------
static String stateToStr(State st) {
  switch(st){
    case ST_WAIT: return "STOP";
    case ST_RUN:  return "RUN";
    case ST_HOME: return "HOME";
    case ST_FAULT:return "FAULT";
  }
  return "?";
}
static String modeToStr(Mode m) {
  switch(m){
    case MODE_OFF: return "OFF";
    case MODE_AUTO:return "AUTO";
    case MODE_MANUAL:return "MANUAL";
  }
  return "?";
}

static void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

static void handleStatus() {
  String json = "{";
  json += "\"state\":\"" + stateToStr(state) + "\",";
  json += "\"mode\":\""  + modeToStr(keyMode) + "\",";
  json += "\"ampelB\":" + String(ampelBrightness) + ",";
  json += "\"cabB\":" + String(cabinetBrightness) + ",";
  json += "\"servo\":[";
  for (int i=0;i<SERVO_COUNT;i++){
    json += String(servoDeg[i]);
    if(i<SERVO_COUNT-1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

static void handleMode() {
  String m = server.arg("m");
  if (m == "off") keyMode = MODE_OFF;
  else if (m == "auto") keyMode = MODE_AUTO;
  else if (m == "man") keyMode = MODE_MANUAL;
  server.send(200, "text/plain", "OK");
}

static void handleBright() {
  String w = server.arg("w");
  int v = server.arg("v").toInt();
  v = clampi(v, 0, 255);
  if (w == "ampel") ampelBrightness = (uint8_t)v;
  else if (w == "cab") cabinetBrightness = (uint8_t)v;
  server.send(200, "text/plain", "OK");
}

static void handleServo() {
  // Servo nur in MANUAL (und nicht in FAULT)
  if (keyMode != MODE_MANUAL || state == ST_FAULT) {
    server.send(403, "text/plain", "MANUAL only / or FAULT");
    return;
  }
  int i = server.arg("i").toInt();
  int v = server.arg("v").toInt();
  if (i < 0 || i >= SERVO_COUNT) { server.send(400, "text/plain", "bad idx"); return; }
  writeServoDeg(i, v);
  server.send(200, "text/plain", "OK");
}

static void handleCmd() {
  String c = server.arg("c");

  // --- FAULT TEST ---
  if (c == "fault") {
    enterFault();
    server.send(200, "text/plain", "OK");
    return;
  }

  // --- RESET / QUIT ---
  if (c == "reset") {
    if (state == ST_FAULT) {
      clearFaultViaReset();
    }
    server.send(200, "text/plain", "OK");
    return;
  }

  // Ab hier: wenn FAULT -> blockieren (du wolltest: nicht von fault auf stop ohne quittieren)
  if (state == ST_FAULT) {
    server.send(403, "text/plain", "FAULT: need reset/quit");
    return;
  }

  // --- START ---
  if (c == "start") {
    // Start nur wenn Key in AUTO oder MANUAL (dein späteres Safety-Setup kann das härter machen)
    if (keyMode == MODE_OFF) { server.send(403, "text/plain", "Key OFF"); return; }
    state = ST_RUN;
    server.send(200, "text/plain", "OK");
    return;
  }

  // --- STOP ---
  if (c == "stop") {
    // Soft-Stop: STOP/WAIT = rot dauerhaft
    state = ST_WAIT;
    server.send(200, "text/plain", "OK");
    return;
  }

  // --- HOME FAHREN ---
  if (c == "home") {
    // Home fahren erlaubt wenn nicht OFF
    if (keyMode == MODE_OFF) { server.send(403, "text/plain", "Key OFF"); return; }
    for (int i=0;i<SERVO_COUNT;i++) writeServoDeg(i, homeDeg[i]);
    state = ST_HOME;
    server.send(200, "text/plain", "OK");
    return;
  }

  // --- HOME SPEICHERN ---
  if (c == "savehome") {
    // Save nur in MANUAL
    if (keyMode != MODE_MANUAL) { server.send(403, "text/plain", "MANUAL only"); return; }
    for (int i=0;i<SERVO_COUNT;i++) homeDeg[i] = servoDeg[i];
    server.send(200, "text/plain", "OK");
    return;
  }

  server.send(400, "text/plain", "Unknown cmd");
}

// OTA upload handler
static void handleUpdate() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      server.send(200, "text/plain", "OK. Rebooting...");
      delay(300);
      ESP.restart();
    } else {
      server.send(500, "text/plain", "Update failed");
    }
  }
}

static void setupWeb() {
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/mode", handleMode);
  server.on("/bright", handleBright);
  server.on("/servo", handleServo);
  server.on("/cmd", handleCmd);

  server.on("/update", HTTP_POST,
    [](){ server.send(200, "text/plain", ""); },
    handleUpdate
  );

  server.begin();
}

// ------------------- Setup/Loop -------------------
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // "ON" solange Power (bei FAULT blinken wir in loop)

  Serial.begin(115200);
  delay(200);

  Wire.begin(21, 22);

  // PCA init
  pca.begin();
  pca.setPWMFreq(SERVO_FREQ);
  delay(10);
  applyAllServos();

  // FastLED: wir nutzen 2 Controller (Ampel + Cabinet)
  FastLED.addLeds<WS2812B, PIN_AMPEL, GRB>(ampel, AMPEL_LEDS);
  FastLED.addLeds<WS2812B, PIN_CABINET, GRB>(cabinet, CABINET_LEDS);

  // WLAN AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  setupWeb();
  updateLeds();
}

void loop() {
  server.handleClient();

  // Fault blink timing
  unsigned long now = millis();
  if (now - lastBlinkMs >= 350) {
    lastBlinkMs = now;
    faultBlinkOn = !faultBlinkOn;
  }

  // Onboard LED: immer an, außer bei FAULT -> blinkt
  if (state == ST_FAULT) {
    digitalWrite(LED_BUILTIN, faultBlinkOn ? HIGH : LOW);
  } else {
    digitalWrite(LED_BUILTIN, HIGH);
  }

  // LEDs aktualisieren (nicht zu oft, aber regelmäßig)
  static unsigned long lastLedMs = 0;
  if (now - lastLedMs > 100) {
    lastLedMs = now;
    updateLeds();
  }
}
