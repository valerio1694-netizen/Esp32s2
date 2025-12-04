#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>

Preferences prefs;

const char* AP_SSID = "IR-Messgeraet";
const char* AP_PASSWORD = "";

WebServer server(80);

// --- Pins ---
const int PIN_ADC  = 34;    // ADC (Spannungsteiler)
const int PIN_GATE = 25;    // MOSFET Gate (HIGH = Last EIN)

// --- Teilerwerte ---
const float R1 = 100000.0;  // oben (BAT+ -> ADC)
const float R2 = 33000.0;   // unten (ADC -> GND)

// --- Last ----
const float Rload = 1.10;   // 3x3R3 parallel ~1.1 Ohm

// --- Konfiguration (NVS) ---
float calib = 1.10f;        // ADC-Kalibrierfaktor
String theme = "dark";      // "dark" | "light"

// --- Messgrößen ---
float Uopen=0, Uload=0, Iload=0, Ri=0;

// ---------- Hilfsfunktionen ----------
float readBattVoltageOnce() {
  int raw = analogRead(PIN_ADC);
  float v_adc = (raw / 4095.0f) * 3.3f;      // ADC -> Spannung am Knoten
  float v_bat = v_adc * ((R1 + R2) / R2);    // zurückrechnen
  return v_bat * calib;                      // kalibrieren
}

float readUopenAveraged(uint8_t n=8) {
  float sum=0;
  for(uint8_t i=0;i<n;i++){ sum += readBattVoltageOnce(); delay(5); }
  return sum / n;
}

String css() {
  // CSS-Variablen je nach Theme
  bool dark = (theme=="dark");
  String s;
  s += "<style>:root{";
  s += String("--bg:")   + (dark?"#000":"#f7f7f7") + ";";
  s += String("--fg:")   + (dark?"#eee":"#111")    + ";";
  s += String("--accent:")+ (dark?"#0f0":"#0a0")   + ";";
  s += String("--card:") + (dark?"#111":"#fff")    + ";";
  s += "}";
  s += "body{background:var(--bg);color:var(--fg);font-family:Arial,Helvetica,sans-serif;margin:20px}";
  s += "h1{color:var(--accent)}button,input{padding:10px;margin:6px;font-size:16px}";
  s += "table{border-collapse:collapse;background:var(--card)}td,th{border:1px solid #555;padding:8px}";
  s += ".row{margin:8px 0}";
  s += ".ok{color:#0f0}.bad{color:#f33}";
  s += "</style>";
  return s;
}

// ---------- HTML ----------
String page() {
  String s;
  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += css();
  s += "</head><body>";

  s += "<h1>Innenwiderstands-Messgeraet</h1>";
  s += "<div class=row>";
  s += "<button onclick=\"location.href='/measure'\">Messung starten</button>";
  s += "<button onclick=\"location.href='/update'\">Firmware-Update</button>";
  s += "<button onclick=\"location.href='/toggleTheme'\">Theme: " + theme + "</button>";
  s += "</div>";

  // Kalibrierung
  s += "<h2>Kalibrierfaktor</h2>";
  s += "<form action='/setFactor'><input name='factor' value='" + String(calib,4) + "'>";
  s += "<button type=submit>Speichern</button></form>";

  // Live Uopen
  s += "<h2>Live U<sub>open</sub></h2>";
  s += "<p>Leerlaufspannung: <b id='uopen'>-.-</b> V</p>";
  s += "<small>Aktualisiert jede Sekunde, sobald U &gt; 2.0 V.</small>";

  // Aktuelle Messung
  s += "<h2>Aktuelle Messung</h2>";
  s += "<table>";
  s += "<tr><th>Parameter</th><th>Wert</th></tr>";
  s += "<tr><td>U<sub>open</sub> (Leerlauf)</td><td>" + String(Uopen,3) + " V</td></tr>";
  s += "<tr><td>U<sub>load</sub> (unter Last)</td><td>" + String(Uload,3) + " V</td></tr>";
  s += "<tr><td>I (Laststrom)</td><td>" + String(Iload,3) + " A</td></tr>";
  s += "<tr><td>R<sub>i</sub></td><td>" + String(Ri*1000.0,1) + " mΩ</td></tr>";
  s += "</table>";

  // JS: Polling für Live-Uopen
  s += R"JS(
<script>
async function poll(){
  try{
    const r = await fetch('/api/uopen');
    const j = await r.json();
    document.getElementById('uopen').textContent = j.u.toFixed(3);
  }catch(e){}
}
setInterval(poll,1000);  // jede Sekunde
poll();
</script>
)JS";

  s += "</body></html>";
  return s;
}

// ---------- Web Handlers ----------
void handleRoot(){ server.send(200,"text/html",page()); }

void handleMeasure(){
  // Last aus → Uopen
  digitalWrite(PIN_GATE, LOW);
  delay(120);
  Uopen = readUopenAveraged(16);

  // Last ein → Uload
  digitalWrite(PIN_GATE, HIGH);
  delay(220);
  Uload = readUopenAveraged(16);

  // Strom & Ri
  Iload = Uload / Rload;
  if (Iload < 0.01) Iload = 0;
  Ri = (Iload>0) ? (Uopen - Uload)/Iload : NAN;

  // Last wieder aus
  digitalWrite(PIN_GATE, LOW);

  server.send(200,"text/html",page());
}

void handleSetFactor(){
  if (server.hasArg("factor")){
    calib = server.arg("factor").toFloat();
    if (calib < 0.5) calib = 0.5;
    if (calib > 1.5) calib = 1.5;
    prefs.putFloat("adc_calib", calib);
  }
  server.send(200,"text/html",page());
}

void handleToggleTheme(){
  theme = (theme=="dark") ? "light" : "dark";
  prefs.putString("theme", theme);
  server.send(200,"text/html",page());
}

// JSON-API: Live-Uopen jede Sekunde
void handleApiUopen(){
  float u = readUopenAveraged(8);
  if (u < 2.0) u = 0.0; // keine Zelle erkannt
  String j = String("{\"u\":") + String(u,3) + "}";
  server.send(200,"application/json",j);
}

// OTA-Seite
const char* updatePage PROGMEM = R"HTML(
<html><body style="background:#000;color:#eee;font-family:Arial">
<h2>Firmware-Update (OTA)</h2>
<form method='POST' action='/update' enctype='multipart/form-data'>
<input type='file' name='firmware'>
<button>Flashen</button>
</form></body></html>
)HTML";

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_GATE, OUTPUT);
  digitalWrite(PIN_GATE, LOW);
  pinMode(PIN_ADC, INPUT);

  // NVS laden
  prefs.begin("config", false);
  calib = prefs.getFloat("adc_calib", 1.10f);
  theme = prefs.getString("theme", "dark");

  // Access-Point
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.println("AP: IR-Messgeraet gestartet");

  // Routen
  server.on("/",            handleRoot);
  server.on("/measure",     handleMeasure);
  server.on("/setFactor",   handleSetFactor);
  server.on("/toggleTheme", handleToggleTheme);
  server.on("/api/uopen",   handleApiUopen);

  server.on("/update", HTTP_GET, [](){ server.send(200,"text/html",updatePage); });
  server.on("/update", HTTP_POST, [](){
    server.send(200,"text/html", Update.hasError()?"Fehler":"Update OK, Neustart…");
    delay(800); ESP.restart();
  }, [](){
    HTTPUpload& up = server.upload();
    if (up.status==UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (up.status==UPLOAD_FILE_WRITE) { Update.write(up.buf, up.currentSize); }
    else if (up.status==UPLOAD_FILE_END) { Update.end(true); }
  });

  server.begin();
}

void loop() {
  server.handleClient();
}
