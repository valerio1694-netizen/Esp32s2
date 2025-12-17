/************************************************************
 * ESP32 RobotArm – Web UI + Web OTA + FastLED
 *
 * STATUS-AMPEL (4x WS2812B): GPIO16
 *  - LED0 = Blau  (HOME)
 *  - LED1 = Grün  (RUN)
 *  - LED2 = Gelb  (WAIT)
 *  - LED3 = Rot   (FAULT)
 *
 * Schaltschrank Licht (10x WS2812B): GPIO17 (weiß, dimmbar)
 *
 * Web:
 * - /        -> UI
 * - /update  -> Web OTA Upload
 ************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <FastLED.h>

// ===== WiFi AP =====
static const char* AP_SSID = "RobotArm";
static const char* AP_PASS = "12345678"; // >= 8 chars
WebServer server(80);

// ===== LEDs =====
#define PIN_STATUS    16
#define PIN_CAB       17

#define NUM_STATUS    4
#define NUM_CAB       10

CRGB statusLeds[NUM_STATUS];
CRGB cabLeds[NUM_CAB];

uint8_t statusBrightness = 120; // 0..255
uint8_t cabBrightness    = 110; // 0..255
bool cabOn = true;

// ===== Status =====
enum StatusMode : uint8_t { HOME=0, RUN=1, WAIT=2, FAULT=3 };
volatile StatusMode mode = HOME;

const char* modeName(StatusMode m) {
  switch (m) {
    case HOME:  return "HOME";
    case RUN:   return "RUN";
    case WAIT:  return "WAIT";
    case FAULT: return "FAULT";
    default:    return "?";
  }
}

// ===== Rendering =====
void renderStatusAmpel() {
  // Alles aus
  fill_solid(statusLeds, NUM_STATUS, CRGB::Black);

  // Nur eine LED an: Reihenfolge von Eingang aus:
  // 0=blau, 1=grün, 2=gelb, 3=rot
  switch (mode) {
    case HOME:  statusLeds[0] = CRGB(0, 0, 255); break;
    case RUN:   statusLeds[1] = CRGB(0, 255, 0); break;
    case WAIT:  statusLeds[2] = CRGB(255, 180, 0); break;
    case FAULT: statusLeds[3] = CRGB(255, 0, 0); break;
  }
}

void renderCabinet() {
  if (!cabOn) {
    fill_solid(cabLeds, NUM_CAB, CRGB::Black);
  } else {
    CRGB w = CRGB::White;
    w.nscale8_video(cabBrightness);   // “Dimmung” über Farbe
    fill_solid(cabLeds, NUM_CAB, w);
  }
}

// Ein FastLED.show() für beide Strips
void renderAll() {
  renderStatusAmpel();
  renderCabinet();
  FastLED.setBrightness(statusBrightness); // gilt global; Cabinet ist bereits skaliert
  FastLED.show();
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

  s += "<div class='card'><b>Status:</b> " + String(modeName(mode)) + "<br>";
  s += "<button onclick=\"fetch('/api/mode?m=0').then(()=>location.reload())\">HOME (Blau)</button>";
  s += "<button onclick=\"fetch('/api/mode?m=1').then(()=>location.reload())\">RUN (Gr&uuml;n)</button>";
  s += "<button onclick=\"fetch('/api/mode?m=2').then(()=>location.reload())\">WAIT (Gelb)</button>";
  s += "<button onclick=\"fetch('/api/mode?m=3').then(()=>location.reload())\">FAULT (Rot)</button>";
  s += "</div>";

  s += "<div class='card'><b>Ampel Helligkeit:</b> <span id='sb'>" + String(statusBrightness) + "</span>";
  s += "<input type='range' min='0' max='255' value='" + String(statusBrightness) + "' ";
  s += "oninput=\"document.getElementById('sb').innerText=this.value; fetch('/api/status?b='+this.value)\">";
  s += "</div>";

  s += "<div class='card'><b>Schaltschrank Licht (10 LEDs, wei&szlig;):</b><br>";
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

void handleApiMode() {
  if (server.hasArg("m")) {
    int m = server.arg("m").toInt();
    if (m >= 0 && m <= 3) mode = (StatusMode)m;
    renderAll();
  }
  server.send(200, "text/plain", "OK");
}

void handleApiStatus() {
  if (server.hasArg("b")) {
    int b = server.arg("b").toInt();
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    statusBrightness = (uint8_t)b;
    renderAll();
  }
  server.send(200, "text/plain", "OK");
}

void handleApiCab() {
  if (server.hasArg("toggle")) cabOn = !cabOn;
  if (server.hasArg("b")) {
    int b = server.arg("b").toInt();
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    cabBrightness = (uint8_t)b;
  }
  renderAll();
  server.send(200, "text/plain", "OK");
}

// OTA handlers
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

void setup() {
  // FastLED init
  FastLED.addLeds<WS2812B, PIN_STATUS, GRB>(statusLeds, NUM_STATUS);
  FastLED.addLeds<WS2812B, PIN_CAB,    GRB>(cabLeds,    NUM_CAB);
  FastLED.clear(true);

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/mode", HTTP_GET, handleApiMode);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/cab", HTTP_GET, handleApiCab);

  // OTA
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);

  server.begin();

  // initial
  renderAll();
}

void loop() {
  server.handleClient();
}
