/*
 * ESP32 18650-Tester (AP-Modus)
 * - Low-Side-Shunt 0,02 Ω
 * - VBatt über Teiler (98,1k / 33k) an ADC34
 * - Shunt oben (GND-Knoten) an ADC35
 * - MOSFET-Gate an GPIO25 über 220 Ω
 * - Webinterface:
 *      - Live-Leerlaufspannung (1 Hz)
 *      - IR-Messung (Mehrfachmessung, Mittelwert)
 *      - Web-OTA (.bin Upload)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

// ======== AP-KONFIG =========
const char* AP_SSID     = "18650-Tester";
const char* AP_PASSWORD = "12345678";   // nach Wunsch ändern

// ======== PINS ========
const int PIN_BAT_ADC    = 34;  // VBatt-Teiler
const int PIN_SHUNT_ADC  = 35;  // Shunt oben (GND-Knoten)
const int PIN_LOAD_FET   = 25;  // MOSFET-Gate

// ======== WIDERSTÄNDE ========
const float R_DIV_TOP    = 98100.0f;   // 98,1k
const float R_DIV_BOTTOM = 33000.0f;   // 33k
const float R_SHUNT      = 0.020f;     // 0,02 Ω

// ======== ADC ========
const float ADC_REF_V    = 1.10f;
const int   ADC_MAX      = 4095;

// ======== KALIBRIERUNG ========
// Spannung war im Web zu hoch (4,37 V statt 4,07 V):
// Faktor = 4,07 / 4,37 ≈ 0,93
float CAL_VOLTAGE_FACTOR = 0.93f;
float CAL_CURRENT_FACTOR = 1.0f;   // Strom noch unkalibriert

// ======== IR-MESSUNG ========
const int   IR_SAMPLES        = 5;
const int   IR_DELAY_OPEN_MS  = 200;
const int   IR_DELAY_LOAD_MS  = 200;
const float IR_MIN_CURRENT_A  = 0.05f;

// ======== GLOBALE WERTE ========
float g_v_idle   = 0.0f;
float g_v_open   = 0.0f;
float g_v_load   = 0.0f;
float g_i_load   = 0.0f;
float g_r_int    = 0.0f;
bool  g_validIR  = false;
bool  g_isMeasuring = false;

WebServer server(80);


// ==========================================
//                ADC-FUNKTIONEN
// ==========================================
int readAdcAveraged(int pin, int samples = 16) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  return (int)(sum / samples);
}

float readBatteryVoltage() {
  int raw     = readAdcAveraged(PIN_BAT_ADC);
  float v_adc = (float)raw * ADC_REF_V / (float)ADC_MAX;
  float v_bat = v_adc * (R_DIV_TOP + R_DIV_BOTTOM) / R_DIV_BOTTOM;
  return v_bat * CAL_VOLTAGE_FACTOR;
}

float readShuntCurrent() {
  int raw      = readAdcAveraged(PIN_SHUNT_ADC);
  float v_shunt = (float)raw * ADC_REF_V / (float)ADC_MAX;
  float current = v_shunt / R_SHUNT;
  return current * CAL_CURRENT_FACTOR;
}


// ==========================================
//            IR-MESSUNG MIT MITTELWERT
// ==========================================
void performIRMeasurement() {
  g_isMeasuring = true;

  float sum_v_open = 0.0f;
  float sum_v_load = 0.0f;
  float sum_i_load = 0.0f;

  for (int i = 0; i < IR_SAMPLES; i++) {
    // Leerlauf
    digitalWrite(PIN_LOAD_FET, LOW);
    delay(IR_DELAY_OPEN_MS);
    float v_open = readBatteryVoltage();

    // Unter Last
    digitalWrite(PIN_LOAD_FET, HIGH);
    delay(IR_DELAY_LOAD_MS);
    float v_load = readBatteryVoltage();
    float i_load = readShuntCurrent();

    // Last aus
    digitalWrite(PIN_LOAD_FET, LOW);

    sum_v_open += v_open;
    sum_v_load += v_load;
    sum_i_load += i_load;
  }

  g_v_open = sum_v_open / IR_SAMPLES;
  g_v_load = sum_v_load / IR_SAMPLES;
  g_i_load = sum_i_load / IR_SAMPLES;

  if (g_i_load > IR_MIN_CURRENT_A) {
    g_r_int   = (g_v_open - g_v_load) / g_i_load;
    g_validIR = true;
  } else {
    g_r_int   = 0;
    g_validIR = false;
  }

  g_isMeasuring = false;
}


// ==========================================
//              WEB-INTERFACE
// ==========================================

String formatFloat(float v, uint8_t d = 3) {
  char b[32];
  dtostrf(v, 0, d, b);
  return String(b);
}

void handleRoot() {
  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>18650 Tester</title>"
    "<style>"
    "body{font-family:Arial;background:#111;color:#eee;padding:10px;}"
    ".card{background:#222;padding:10px;margin-bottom:10px;border-radius:6px;}"
    "button{padding:8px 16px;border:none;background:#28a745;color:#fff;border-radius:4px;font-size:1rem;cursor:pointer;}"
    "button:hover{background:#218838;}"
    "a.btn{padding:8px 16px;background:#007bff;color:#fff;border-radius:4px;text-decoration:none;}"
    "</style>"
    "</head><body>";

  html += "<h1>18650 Tester (AP)</h1>";

  html += "<div class='card'><h2>Leerlaufspannung</h2>";
  html += "<p>U<sub>batt</sub>: <span id='idle'>-</span> V</p>";
  html += "</div>";

  html += "<div class='card'><h2>Innenwiderstand</h2>";
  html += "<p>V<sub>open</sub>: <span id='v_open'>-</span> V</p>";
  html += "<p>V<sub>load</sub>: <span id='v_load'>-</span> V</p>";
  html += "<p>I<sub>load</sub>: <span id='i_load'>-</span> A</p>";
  html += "<p>R<sub>int</sub>: <span id='rint'>-</span> mΩ</p>";
  html += "<form action='/measure'><button>IR-Messung starten</button></form>";
  html += "</div>";

  html += "<div class='card'><h2>Firmware</h2>";
  html += "<a class='btn' href='/update'>Firmware-Update</a>";
  html += "</div>";

  html +=
    "<script>"
    "function upd(){"
    "fetch('/data').then(r=>r.json()).then(j=>{"
    "document.getElementById('idle').textContent  = j.ubatt_idle.toFixed(3);"
    "document.getElementById('v_open').textContent= j.v_open.toFixed(3);"
    "document.getElementById('v_load').textContent= j.v_load.toFixed(3);"
    "document.getElementById('i_load').textContent= j.i_load.toFixed(3);"
    "document.getElementById('rint').textContent  = j.valid_ir ? (j.r_int_milliohm.toFixed(1)) : '-';"
    "});}"
    "setInterval(upd,1000);"
    "upd();"
    "</script>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleData() {
  String json = "{";
  json += "\"ubatt_idle\":"     + String(g_v_idle, 4) + ",";
  json += "\"v_open\":"         + String(g_v_open, 4) + ",";
  json += "\"v_load\":"         + String(g_v_load, 4) + ",";
  json += "\"i_load\":"         + String(g_i_load, 4) + ",";
  json += "\"r_int_milliohm\":" + String(g_r_int * 1000.0f, 3) + ",";
  json += "\"valid_ir\":"       + String(g_validIR ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleMeasure() {
  if (!g_isMeasuring) performIRMeasurement();
  server.sendHeader("Location", "/");
  server.send(303);
}


// ==========================================
//                WEB-OTA
// ==========================================

const char* updateForm = R"(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Firmware Update</title>
<style>
body{font-family:Arial;background:#111;color:#eee;padding:10px;}
.card{background:#222;padding:10px;border-radius:6px;}
button{padding:8px 16px;background:#007bff;color:#fff;border:none;border-radius:4px;}
</style>
</head><body>
<h1>Firmware-Update</h1>
<div class='card'>
<form method='POST' action='/update' enctype='multipart/form-data'>
<input type='file' name='update'><br><br>
<button>Flash .bin</button>
</form>
</div>
<p><a href='/'>Zurück</a></p>
</body></html>
)";

void handleUpdatePage() {
  server.send(200, "text/html", updateForm);
}

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

void handleUpdateFinish() {
  if (Update.hasError())
    server.send(200, "text/plain", "Update fehlgeschlagen");
  else {
    server.send(200, "text/plain", "Update OK – Neustart...");
    delay(500);
    ESP.restart();
  }
}


// ==========================================
//               AP-MODUS
// ==========================================
void setupWifi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.println("Access-Point gestartet:");
  Serial.print("SSID: "); Serial.println(AP_SSID);
  Serial.print("Passwort: "); Serial.println(AP_PASSWORD);
  Serial.print("AP-IP: "); Serial.println(WiFi.softAPIP());
}


// ==========================================
//                 SETUP / LOOP
// ==========================================
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/measure", handleMeasure);

  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateFinish, handleUpdateUpload);

  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(12);
  analogSetAttenuation(ADC_0db);

  pinMode(PIN_LOAD_FET, OUTPUT);
  digitalWrite(PIN_LOAD_FET, LOW);

  setupWifi();
  setupWebServer();
}

void loop() {
  server.handleClient();

  static unsigned long last = 0;
  if (millis() - last >= 1000 && !g_isMeasuring) {
    last = millis();
    g_v_idle = readBatteryVoltage();
  }
}
