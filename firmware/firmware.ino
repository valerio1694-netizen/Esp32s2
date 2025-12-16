/************************************************************
 * ESP32 RobotArm – Web UI + Web OTA + WS2812 Status LEDs
 * Board: ESP32 DevKit (AZ-Delivery ESP32-WROOM-32)
 *
 * LED Ring (44x WS2812B): GPIO16
 * Schaltschrank (10x WS2812B): GPIO17 (weiß, dimmbar)
 * I2C (PCA9685): SDA=GPIO21, SCL=GPIO22
 ************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>

// ====== Pins / Counts ======
#define PIN_RING        16
#define PIN_CABINET     17
#define LEDS_RING       44
#define LEDS_CABINET    10

#define I2C_SDA         21
#define I2C_SCL         22

// ====== WiFi AP ======
const char* AP_SSID = "RobotArm";
const char* AP_PASS = "12345678";  // mind. 8 Zeichen

// ====== Web ======
WebServer server(80);

// ====== LEDs ======
Adafruit_NeoPixel ring(LEDS_RING, PIN_RING, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel cab(LEDS_CABINET, PIN_CABINET, NEO_GRB + NEO_KHZ800);

// ====== State ======
enum StatusMode : uint8_t { IDLE=0, ENABLED=1, AUTO=2, FAULT=3, ESTOP=4 };
StatusMode statusMode = IDLE;

uint8_t cabBrightness = 80; // 0..255 (Software-Dimmung)
bool cabOn = true;

// ====== Helpers ======
uint32_t colorForMode(StatusMode m) {
  switch (m) {
    case IDLE:    return ring.Color(0, 0, 40);      // blau
    case ENABLED: return ring.Color(0, 40, 0);      // grün
    case AUTO:    return ring.Color(40, 20, 0);     // gelb/orange
    case FAULT:   return ring.Color(40, 0, 0);      // rot
    case ESTOP:   return ring.Color(80, 0, 0);      // rot hell
    default:      return ring.Color(0, 0, 0);
  }
}

void applyRing() {
  uint32_t c = colorForMode(statusMode);
  for (int i = 0; i < LEDS_RING; i++) ring.setPixelColor(i, c);
  ring.show();
}

void applyCabinet() {
  cab.setBrightness(cabOn ? cabBrightness : 0);
  // nur weiß:
  for (int i = 0; i < LEDS_CABINET; i++) cab.setPixelColor(i, cab.Color(255, 255, 255));
  cab.show();
}

String modeName(StatusMode m) {
  switch (m) {
    case IDLE: return "IDLE";
    case ENABLED: return "ENABLED";
    case AUTO: return "AUTO";
    case FAULT: return "FAULT";
    case ESTOP: return "E-STOP";
    default: return "?";
  }
}

// ====== Pages ======
String htmlIndex() {
  String s;
  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>RobotArm</title>";
  s += "<style>body{font-family:Arial;margin:20px;background:#111;color:#eee}";
  s += "a{color:#7cf} .card{background:#1b1b1b;padding:16px;border-radius:12px;margin-bottom:12px}";
  s += "button{padding:12px 16px;margin:6px;border-radius:10px;border:0;background:#2b2b2b;color:#eee}";
  s += "input[type=range]{width:100%}</style></head><body>";

  s += "<h2>RobotArm Control</h2>";

  s += "<div class='card'><b>Status Ring:</b> " + modeName(statusMode) + "<br>";
  s += "<button onclick=\"setMode(0)\">IDLE</button>";
  s += "<button onclick=\"setMode(1)\">ENABLED</button>";
  s += "<button onclick=\"setMode(2)\">AUTO</button>";
  s += "<button onclick=\"setMode(3)\">FAULT</button>";
  s += "<button onclick=\"setMode(4)\">E-STOP</button>";
  s += "</div>";

  s += "<div class='card'><b>Schaltschrank Licht (WS2812 wei&szlig;):</b><br>";
  s += "An/Aus: <button onclick=\"cabToggle()\">" + String(cabOn ? "AUS" : "AN") + "</button><br><br>";
  s += "Helligkeit: <span id='bval'>" + String(cabBrightness) + "</span>";
  s += "<input type='range' min='0' max='255' value='" + String(cabBrightness) + "' oninput='setBright(this.value)'>";
  s += "</div>";

  s += "<div class='card'><b>Web OTA:</b> <a href='/update'>Firmware hochladen</a></div>";

  s += "<script>";
  s += "function req(u){fetch(u).then(()=>location.reload());}";
  s += "function setMode(m){req('/set?mode='+m);}";
  s += "function cabToggle(){req('/cab?toggle=1');}";
  s += "function setBright(v){document.getElementById('bval').innerText=v; fetch('/cab?b='+v);}";
  s += "</script>";

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

// ====== Handlers ======
void handleRoot() {
  server.send(200, "text/html", htmlIndex());
}

void handleSet() {
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= 0 && m <= 4) {
      statusMode = (StatusMode)m;
      applyRing();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleCab() {
  if (server.hasArg("toggle")) {
    cabOn = !cabOn;
    applyCabinet();
  }
  if (server.hasArg("b")) {
    int b = server.arg("b").toInt();
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    cabBrightness = (uint8_t)b;
    applyCabinet();
  }
  server.send(200, "text/plain", "OK");
}

void handleUpdateGet() {
  server.send(200, "text/html", htmlUpdate());
}

void handleUpdatePost() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK - Rebooting");
  delay(500);
  ESP.restart();
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Serial.printf("Update: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      // Serial.println("Update.begin failed");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      // Serial.println("Update.write failed");
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      // Serial.printf("Update.end failed: %s\n", Update.errorString());
    }
  }
}

// ====== Setup / Loop ======
void setup() {
  // Serial.begin(115200);

  // I2C vorbereiten (PCA9685 später)
  Wire.begin(I2C_SDA, I2C_SCL);

  // LEDs init
  ring.begin();
  ring.setBrightness(120); // Ring-Helligkeit fix (kannst du später auch per Web machen)
  ring.show();

  cab.begin();
  cab.setBrightness(cabBrightness);
  cab.show();

  // Startwerte
  applyRing();
  applyCabinet();

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/set", HTTP_GET, handleSet);
  server.on("/cab", HTTP_GET, handleCab);

  // OTA
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);

  server.begin();
}

void loop() {
  server.handleClient();
}

