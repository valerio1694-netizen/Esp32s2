/************************************************************
 * ESP32 RobotArm – Signalampel + Schaltschranklicht + Web + WebOTA
 *
 * Signalampel (4x WS2812B) an GPIO16:
 *   LED0 = Blau  -> HOME
 *   LED1 = Grün  -> RUN
 *   LED2 = Gelb  -> WAIT
 *   LED3 = Rot   -> ERROR
 *
 * Schaltschranklicht (10x WS2812B) an GPIO17:
 *   nur Weiß, dimmbar, an/aus
 *
 * AP:
 *   SSID: RobotArm
 *   PASS: 12345678
 *
 * Web:
 *   http://192.168.4.1/
 * OTA:
 *   http://192.168.4.1/update
 ************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <FastLED.h>

// ===== WiFi AP =====
static const char* AP_SSID = "RobotArm";
static const char* AP_PASS = "12345678"; // >= 8 Zeichen
WebServer server(80);

// ===== LED Setup =====
#define PIN_TOWER   16
#define NUM_TOWER   4

#define PIN_CAB     17
#define NUM_CAB     10

CRGB towerLeds[NUM_TOWER];
CRGB cabLeds[NUM_CAB];

// globale Brightness (FastLED ist global) -> wir lösen Cabinet-Dimmung über Farbskalierung
uint8_t towerBrightness = 120;  // 0..255
bool cabOn = true;
uint8_t cabBrightness = 120;    // 0..255 (wird in Farbe skaliert)

// ===== Robot State =====
enum RobotState : uint8_t {
  STATE_HOME = 0,
  STATE_RUN  = 1,
  STATE_WAIT = 2,
  STATE_ERR  = 3
};

volatile RobotState state = STATE_HOME;

// ===== Helpers =====
const char* stateName(RobotState s) {
  switch (s) {
    case STATE_HOME: return "HOME";
    case STATE_RUN:  return "RUN";
    case STATE_WAIT: return "WAIT";
    case STATE_ERR:  return "ERROR";
    default:         return "?";
  }
}

uint8_t clampU8(int x) {
  if (x < 0) return 0;
  if (x > 255) return 255;
  return (uint8_t)x;
}

void applyCabinet() {
  if (!cabOn) {
    fill_solid(cabLeds, NUM_CAB, CRGB::Black);
    return;
  }
  CRGB w = CRGB::White;
  w.nscale8_video(cabBrightness);   // skaliert Weiß -> dimmbar unabhängig
  fill_solid(cabLeds, NUM_CAB, w);
}

void applyTower() {
  // alles aus
  fill_solid(towerLeds, NUM_TOWER, CRGB::Black);

  // nur genau eine LED an – Reihenfolge vom Eingang aus:
  // 0=Blau, 1=Grün, 2=Gelb, 3=Rot
  switch (state) {
    case STATE_HOME: towerLeds[0] = CRGB(0, 0, 255); break;
    case STATE_RUN:  towerLeds[1] = CRGB(0, 255, 0); break;
    case STATE_WAIT: towerLeds[2] = CRGB(255, 180, 0); break;
    case STATE_ERR:  towerLeds[3] = CRGB(255, 0, 0); break;
  }
}

void renderAll() {
  applyTower();
  applyCabinet();

  // globale Brightness nur für Tower (Cabinet ist bereits in der Farbe skaliert)
  FastLED.setBrightness(towerBrightness);
  FastLED.show();
}

void setState(RobotState s) {
  state = s;
  renderAll();
}

// ===== Web UI =====
String htmlIndex() {
  String s;
  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>RobotArm</title>";
  s += "<style>body{font-family:Arial;background:#111;color:#eee;padding:16px}";
  s += ".card{background:#1b1b1b;padding:14px;border-radius:12px;margin:10px 0}";
  s += "button{padding:12px 14px;margin:6px 0;width:100%;border:0;border-radius:10px;background:#2b2b2b;color:#eee;font-size:18px}";
  s += "input[type=range]{width:100%} a{color:#7cf}</style></head><body>";

  s += "<h2>RobotArm</h2>";

  s += "<div class='card'><b>Status:</b> " + String(stateName(state)) + "<br>";
  s += "<button onclick=\"fetch('/api/state?set=HOME').then(()=>location.reload())\">HOME (Blau)</button>";
  s += "<button onclick=\"fetch('/api/state?set=RUN').then(()=>location.reload())\">RUN (Gr&uuml;n)</button>";
  s += "<button onclick=\"fetch('/api/state?set=WAIT').then(()=>location.reload())\">WAIT (Gelb)</button>";
  s += "<button onclick=\"fetch('/api/state?set=ERROR').then(()=>location.reload())\">ERROR (Rot)</button>";
  s += "</div>";

  s += "<div class='card'><b>Ampel Helligkeit:</b> <span id='tb'>" + String(towerBrightness) + "</span>";
  s += "<input type='range' min='0' max='255' value='" + String(towerBrightness) + "' ";
  s += "oninput=\"document.getElementById('tb').innerText=this.value; fetch('/api/tower?b='+this.value)\">";
  s += "</div>";

  s += "<div class='card'><b>Schaltschrank Licht (10x wei&szlig;):</b><br>";
  s += "<button onclick=\"fetch('/api/cab?toggle=1').then(()=>location.reload())\">" + String(cabOn ? "AUS" : "AN") + "</button>";
  s += "<b>Helligkeit:</b> <span id='cb'>" + String(cabBrightness) + "</span>";
  s += "<input type='range' min='0' max='255' value='" + String(cabBrightness) + "' ";
  s += "oninput=\"document.getElementById('cb').innerText=this.value; fetch('/api/cab?b='+this.value)\">";
  s += "</div>";

  s += "<div class='card'><b>Web OTA:</b> <a href='/update'>/update</a></div>";
  s += "</body></html>";
  return s;
}

String htmlUpdate() {
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
void handleRoot() { server.send(200, "text/html", htmlIndex()); }

void handleApiState() {
  if (server.hasArg("set")) {
    String v = server.arg("set");
    v.toUpperCase();
    if (v == "HOME")  setState(STATE_HOME);
    if (v == "RUN")   setState(STATE_RUN);
    if (v == "WAIT")  setState(STATE_WAIT);
    if (v == "ERROR") setState(STATE_ERR);
  }
  server.send(200, "text/plain", "OK");
}

void handleApiTower() {
  if (server.hasArg("b")) {
    towerBrightness = clampU8(server.arg("b").toInt());
    renderAll();
  }
  server.send(200, "text/plain", "OK");
}

void handleApiCab() {
  if (server.hasArg("toggle")) cabOn = !cabOn;
  if (server.hasArg("b")) cabBrightness = clampU8(server.arg("b").toInt());
  renderAll();
  server.send(200, "text/plain", "OK");
}

// OTA
void handleUpdateGet() { server.send(200, "text/html", htmlUpdate()); }

void handleUpdatePost() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK - Rebooting");
  delay(300);
  ESP.restart();
}

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

// ===== Setup / Loop =====
void setup() {
  // FastLED init: zwei Strips
  FastLED.addLeds<WS2812B, PIN_TOWER, GRB>(towerLeds, NUM_TOWER);
  FastLED.addLeds<WS2812B, PIN_CAB,   GRB>(cabLeds,   NUM_CAB);
  FastLED.clear(true);

  // AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Web
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/tower", HTTP_GET, handleApiTower);
  server.on("/api/cab",   HTTP_GET, handleApiCab);

  // OTA
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);

  server.begin();

  // initial render
  renderAll();
}

void loop() {
  server.handleClient();
}
