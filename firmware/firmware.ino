#include <WiFi.h>
#include <WebServer.h>
#include <ModbusMaster.h>

// ---------- SK120 / Modbus ----------
HardwareSerial SKSerial(2);      // UART2
ModbusMaster sk120;

// Pins zum SK120
constexpr int SK120_RX_PIN = 16; // ESP empfängt hier (an SK120-TX)
constexpr int SK120_TX_PIN = 17; // ESP sendet hier (an SK120-RX)

// Letzte gelesene Werte
float u_set = 0.0f;
float i_set = 0.0f;
float u_meas = 0.0f;
float i_meas = 0.0f;
float p_meas = 0.0f;
bool  out_on = false;

// ---------- WiFi / Web ----------
const char* AP_SSID     = "LabPSU";
const char* AP_PASSWORD = "12345678";

WebServer server(80);

// ---------- Hilfsfunktionen SK120 ----------

// Sollspannung in Volt
void setVoltage(float volts) {
  uint16_t reg = (uint16_t)(volts * 100.0f);   // /100 laut Registermap
  sk120.writeSingleRegister(0x0000, reg);
}

// Sollstrom in Ampere
void setCurrent(float amps) {
  uint16_t reg = (uint16_t)(amps * 1000.0f);   // /1000 laut Registermap
  sk120.writeSingleRegister(0x0001, reg);
}

// Ausgang ein/aus
void setOutput(bool on) {
  sk120.writeSingleRegister(0x0012, on ? 1 : 0);
  out_on = on;
}

// Werte holen
void readValues() {
  // 0x0000/0x0001 = Sollwerte
  uint8_t res1 = sk120.readHoldingRegisters(0x0000, 2);
  if (res1 == sk120.ku8MBSuccess) {
    u_set = sk120.getResponseBuffer(0) / 100.0f;
    i_set = sk120.getResponseBuffer(1) / 1000.0f;
  }

  // 0x0002,0x0003,0x0004 = U/I/P Ist
  uint8_t res2 = sk120.readHoldingRegisters(0x0002, 3);
  if (res2 == sk120.ku8MBSuccess) {
    u_meas = sk120.getResponseBuffer(0) / 100.0f;
    i_meas = sk120.getResponseBuffer(1) / 1000.0f;
    p_meas = sk120.getResponseBuffer(2) / 100.0f;
  }

  // 0x0012 = Output state
  uint8_t res3 = sk120.readHoldingRegisters(0x0012, 1);
  if (res3 == sk120.ku8MBSuccess) {
    out_on = sk120.getResponseBuffer(0) == 1;
  }
}

// ---------- Webserver Handler ----------

String htmlPage() {
  String html = F(
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<style>"
    "body{font-family:sans-serif;background:#111;color:#eee;text-align:center;}"
    ".card{display:inline-block;margin:10px;padding:15px;border-radius:10px;background:#222;}"
    "h1{font-size:22px;margin-bottom:10px;}"
    "table{margin:0 auto;}"
    "td{padding:4px 10px;}"
    "a.btn{display:inline-block;margin:8px;padding:8px 16px;border-radius:6px;"
    "text-decoration:none;color:#000;background:#4caf50;}"
    "a.btn.off{background:#f44336;color:#fff;}"
    "input{width:80px;padding:4px;border-radius:4px;border:1px solid #555;background:#000;color:#fff;text-align:right;}"
    "</style></head><body>"
    "<h1>XY-SK120 Control</h1>"
    "<div class='card'><table>"
  );

  html += "<tr><td>U Ist:</td><td>" + String(u_meas, 2) + " V</td></tr>";
  html += "<tr><td>I Ist:</td><td>" + String(i_meas, 3) + " A</td></tr>";
  html += "<tr><td>P Ist:</td><td>" + String(p_meas, 1) + " W</td></tr>";
  html += "<tr><td>U Soll:</td><td>" + String(u_set, 2) + " V</td></tr>";
  html += "<tr><td>I Soll:</td><td>" + String(i_set, 3) + " A</td></tr>";
  html += "<tr><td>Output:</td><td>" + String(out_on ? "ON" : "OFF") + "</td></tr>";

  html += F("</table></div><br/>");

  // einfache Form zum Setzen von U/I
  html += F(
    "<div class='card'>"
    "<form action='/set' method='get'>"
    "U[V]: <input name='u' type='number' step='0.01'/><br/>"
    "I[A]: <input name='i' type='number' step='0.001'/><br/><br/>"
    "<input type='submit' value='Set' />"
    "</form></div><br/>"
  );

  // Buttons Output an/aus
  if (out_on) {
    html += "<a class='btn off' href='/out?state=0'>Output OFF</a>";
  } else {
    html += "<a class='btn' href='/out?state=1'>Output ON</a>";
  }

  html += F("<p style='margin-top:20px;font-size:12px;color:#888;'>Reload zum Aktualisieren.</p>");

  html += F("</body></html>");
  return html;
}

void handleRoot() {
  readValues();
  server.send(200, "text/html", htmlPage());
}

void handleSet() {
  if (server.hasArg("u")) {
    float u = server.arg("u").toFloat();
    if (u > 0 && u <= 36.0) {
      setVoltage(u);
    }
  }
  if (server.hasArg("i")) {
    float i = server.arg("i").toFloat();
    if (i > 0 && i <= 6.0) {
      setCurrent(i);
    }
  }
  server.sendHeader("Location", "/");
  server.send(303); // redirect
}

void handleOut() {
  if (server.hasArg("state")) {
    String s = server.arg("state");
    if (s == "1") setOutput(true);
    else         setOutput(false);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// ---------- Setup / Loop ----------

void setup() {
  Serial.begin(115200);
  delay(500);

  // UART2 für SK120
  SKSerial.begin(9600, SERIAL_8N1, SK120_RX_PIN, SK120_TX_PIN);
  sk120.begin(1, SKSerial); // Slave-ID 1

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP: ");
  Serial.println(ip);

  // Webserver Routen
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/out", handleOut);
  server.begin();
  Serial.println("HTTP server gestartet");
}

void loop() {
  server.handleClient();
}
