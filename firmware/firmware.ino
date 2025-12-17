#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <FastLED.h>

// ===== WiFi AP =====
static const char* AP_SSID = "RobotArm";
static const char* AP_PASS = "12345678";
WebServer server(80);

// ===== LED Setup =====
#define PIN_TOWER   16
#define NUM_TOWER   4

#define PIN_CAB     17
#define NUM_CAB     10

// Onboard LED (bei den meisten ESP32 DevKits: GPIO2)
#define PIN_ONBOARD_LED 2

CRGB towerLeds[NUM_TOWER];
CRGB cabLeds[NUM_CAB];

// FastLED Brightness ist global -> wir skalieren JEDE LED-FARBE selbst.
// Global bleibt 255.
uint8_t towerBrightness = 120;  // 0..255 (nur Ampel)
bool cabOn = true;
uint8_t cabBrightness = 120;    // 0..255 (nur Schaltschrank)

// ===== State Machine =====
enum RobotState : uint8_t { STATE_HOME=0, STATE_RUN=1, STATE_WAIT=2, STATE_ERR=3 };
volatile RobotState state = STATE_HOME;

enum ModeSel : uint8_t { MODE_0=0, MODE_MAN=1, MODE_AUTO=2 };
volatile ModeSel modeSel = MODE_0;

volatile bool softStop = false;
volatile bool faultLatched = false;

// ===== Helpers =====
const char* stateName(RobotState s){
  switch(s){ case STATE_HOME:return "HOME"; case STATE_RUN:return "RUN"; case STATE_WAIT:return "WAIT"; case STATE_ERR:return "ERROR"; default:return "?"; }
}
const char* modeName(ModeSel m){
  switch(m){ case MODE_0:return "0"; case MODE_MAN:return "MAN"; case MODE_AUTO:return "AUTO"; default:return "?"; }
}
uint8_t clampU8(int x){ if(x<0) return 0; if(x>255) return 255; return (uint8_t)x; }

// Onboard LED: immer an, außer bei Störung -> blinkt (2 Hz)
void setOnboardLed() {
  static uint32_t last = 0;
  static bool on = true;

  bool fault = (state == STATE_ERR) || faultLatched;

  if (!fault) {
    digitalWrite(PIN_ONBOARD_LED, HIGH);   // dauerhaft an
    return;
  }

  uint32_t now = millis();
  if (now - last >= 250) { // toggle alle 250ms => 2Hz
    last = now;
    on = !on;
    digitalWrite(PIN_ONBOARD_LED, on ? HIGH : LOW);
  }
}

void applyCabinet(){
  if(!cabOn){
    fill_solid(cabLeds, NUM_CAB, CRGB::Black);
    return;
  }
  CRGB w = CRGB::White;
  w.nscale8_video(cabBrightness);   // Cabinet-Dimmung
  fill_solid(cabLeds, NUM_CAB, w);
}

void applyTower(){
  fill_solid(towerLeds, NUM_TOWER, CRGB::Black);

  CRGB c = CRGB::Black;
  int idx = -1;

  // 0=Blau, 1=Grün, 2=Gelb, 3=Rot
  if(state == STATE_HOME) { idx = 0; c = CRGB(0,0,255); }
  if(state == STATE_RUN)  { idx = 1; c = CRGB(0,255,0); }
  if(state == STATE_WAIT) { idx = 2; c = CRGB(255,180,0); }
  if(state == STATE_ERR)  { idx = 3; c = CRGB(255,0,0); }

  // Ampel-Dimmung nur über towerBrightness
  c.nscale8_video(towerBrightness);

  if(idx >= 0) towerLeds[idx] = c;
}

void renderAll(){
  applyTower();
  applyCabinet();

  FastLED.setBrightness(255); // global fix
  FastLED.show();
}

void setState(RobotState s){
  state = s;
  renderAll();
}

// ===== "Panel Taster" Logik =====
void pressStop(){
  softStop = true;
  if(!faultLatched) setState(STATE_WAIT);
}

void pressStart(){
  if(modeSel == MODE_0) return;
  if(faultLatched) return;

  softStop = false;
  setState(STATE_RUN);
}

void pressHome(){
  if(modeSel == MODE_0) return;
  if(faultLatched) return;

  softStop = false;
  setState(STATE_HOME);
}

void pressReset(){
  // Reset quittiert nur latched Fault, danach bleibt er AUS => WAIT
  if(faultLatched){
    faultLatched = false;
    softStop = true;
    setState(STATE_WAIT);
    return;
  }

  // konservativ: Reset ändert sonst nix
  if(state == STATE_WAIT){
    softStop = true;
    setState(STATE_WAIT);
  }
}

// Simulierter Fehler (für Tests)
void triggerFault(){
  faultLatched = true;
  setState(STATE_ERR);
}

// ===== Web UI =====
String htmlIndex(){
  String s;
  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>RobotArm</title>";
  s += "<style>body{font-family:Arial;background:#111;color:#eee;padding:16px}";
  s += ".card{background:#1b1b1b;padding:14px;border-radius:12px;margin:10px 0}";
  s += "button{padding:12px 14px;margin:6px 0;width:100%;border:0;border-radius:10px;background:#2b2b2b;color:#eee;font-size:18px}";
  s += "input[type=range]{width:100%} a{color:#7cf}</style></head><body>";

  s += "<h2>RobotArm</h2>";

  s += "<div class='card'><b>State:</b> " + String(stateName(state)) + " &nbsp; <b>Mode:</b> " + String(modeName(modeSel)) + "<br>";
  s += "<b>softStop:</b> " + String(softStop ? "true":"false") + " &nbsp; <b>faultLatched:</b> " + String(faultLatched ? "true":"false");
  s += "</div>";

  s += "<div class='card'><b>Panel Taster (Web Simulation)</b><br>";
  s += "<button onclick=\"fetch('/btn/start').then(()=>location.reload())\">🟢 START</button>";
  s += "<button onclick=\"fetch('/btn/stop').then(()=>location.reload())\">🔴 STOP (Soft)</button>";
  s += "<button onclick=\"fetch('/btn/reset').then(()=>location.reload())\">🔵 RESET / QUIT</button>";
  s += "<button onclick=\"fetch('/btn/home').then(()=>location.reload())\">🔷 HOME</button>";
  s += "<button onclick=\"fetch('/btn/fault').then(()=>location.reload())\">⚠️ Störung auslösen (Test)</button>";
  s += "</div>";

  s += "<div class='card'><b>Mode (Schlüsselschalter simuliert)</b><br>";
  s += "<button onclick=\"fetch('/mode?m=0').then(()=>location.reload())\">0 (gesperrt)</button>";
  s += "<button onclick=\"fetch('/mode?m=1').then(()=>location.reload())\">MAN</button>";
  s += "<button onclick=\"fetch('/mode?m=2').then(()=>location.reload())\">AUTO</button>";
  s += "</div>";

  s += "<div class='card'><b>Ampel Helligkeit (nur Ampel):</b> <span id='tb'>" + String(towerBrightness) + "</span>";
  s += "<input type='range' min='0' max='255' value='" + String(towerBrightness) + "' ";
  s += "oninput=\"document.getElementById('tb').innerText=this.value; fetch('/api/tower?b='+this.value)\">";
  s += "</div>";

  s += "<div class='card'><b>Schaltschrank Licht (10x weiß):</b><br>";
  s += "<button onclick=\"fetch('/api/cab?toggle=1').then(()=>location.reload())\">" + String(cabOn ? "AUS" : "AN") + "</button>";
  s += "<b>Helligkeit (nur Schaltschrank):</b> <span id='cb'>" + String(cabBrightness) + "</span>";
  s += "<input type='range' min='0' max='255' value='" + String(cabBrightness) + "' ";
  s += "oninput=\"document.getElementById('cb').innerText=this.value; fetch('/api/cab?b='+this.value)\">";
  s += "</div>";

  s += "<div class='card'><b>Web OTA:</b> <a href='/update'>/update</a></div>";
  s += "</body></html>";
  return s;
}

String htmlUpdate(){
  String s;
  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>OTA Update</title><style>body{font-family:Arial;margin:20px}</style></head><body>";
  s += "<h2>Firmware Update (Web OTA)</h2>";
  s += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  s += "<input type='file' name='update' accept='.bin' required>";
  s += "<input type='submit' value='Upload'>";
  s += "</form>";
  s += "<p><a href='/'>Zur&uuml;ck</a></p>";
  s += "</body></html>";
  return s;
}

// ===== Handlers =====
void handleRoot(){ server.send(200,"text/html", htmlIndex()); }

void handleTower(){
  if(server.hasArg("b")) { towerBrightness = clampU8(server.arg("b").toInt()); renderAll(); }
  server.send(200,"text/plain","OK");
}
void handleCab(){
  if(server.hasArg("toggle")) cabOn = !cabOn;
  if(server.hasArg("b")) cabBrightness = clampU8(server.arg("b").toInt());
  renderAll();
  server.send(200,"text/plain","OK");
}

void handleMode(){
  if(server.hasArg("m")){
    int m = server.arg("m").toInt();
    if(m==0) modeSel = MODE_0;
    if(m==1) modeSel = MODE_MAN;
    if(m==2) modeSel = MODE_AUTO;

    // Mode 0 sperrt sofort -> WAIT (gelb)
    if(modeSel == MODE_0){
      softStop = true;
      if(!faultLatched) setState(STATE_WAIT);
    }
  }
  server.send(200,"text/plain","OK");
}

void handleBtnStart(){ pressStart(); server.send(200,"text/plain","OK"); }
void handleBtnStop(){  pressStop();  server.send(200,"text/plain","OK"); }
void handleBtnReset(){ pressReset(); server.send(200,"text/plain","OK"); }
void handleBtnHome(){  pressHome();  server.send(200,"text/plain","OK"); }
void handleBtnFault(){ triggerFault(); server.send(200,"text/plain","OK"); }

// OTA
void handleUpdateGet(){ server.send(200,"text/html", htmlUpdate()); }
void handleUpdatePost(){
  server.sendHeader("Connection","close");
  server.send(200,"text/plain", Update.hasError() ? "FAIL" : "OK - Rebooting");
  delay(300);
  ESP.restart();
}
void handleUpdateUpload(){
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
  else if(upload.status == UPLOAD_FILE_WRITE) Update.write(upload.buf, upload.currentSize);
  else if(upload.status == UPLOAD_FILE_END) Update.end(true);
}

void setup(){
  pinMode(PIN_ONBOARD_LED, OUTPUT);
  digitalWrite(PIN_ONBOARD_LED, HIGH);

  FastLED.addLeds<WS2812B, PIN_TOWER, GRB>(towerLeds, NUM_TOWER);
  FastLED.addLeds<WS2812B, PIN_CAB,   GRB>(cabLeds,   NUM_CAB);
  FastLED.clear(true);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  server.on("/", HTTP_GET, handleRoot);

  server.on("/api/tower", HTTP_GET, handleTower);
  server.on("/api/cab",   HTTP_GET, handleCab);

  server.on("/mode",      HTTP_GET, handleMode);

  server.on("/btn/start", HTTP_GET, handleBtnStart);
  server.on("/btn/stop",  HTTP_GET, handleBtnStop);
  server.on("/btn/reset", HTTP_GET, handleBtnReset);
  server.on("/btn/home",  HTTP_GET, handleBtnHome);
  server.on("/btn/fault", HTTP_GET, handleBtnFault);

  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);

  server.begin();
  renderAll();
}

void loop(){
  server.handleClient();
  setOnboardLed(); // dauerhaft an / bei Störung blinken
}
```0
