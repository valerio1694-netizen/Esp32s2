#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>
#include <Preferences.h>

#include <Adafruit_PWMServoDriver.h>

// ========= Config =========
static const char* AP_SSID = "RobotArm-Calib";
static const char* AP_PASS = "12345678"; // mind. 8 Zeichen

// PCA9685
Adafruit_PWMServoDriver pca(0x40);

// Servo settings (typisch)
static const uint16_t SERVO_FREQ = 50;      // 50Hz
static const uint16_t PULSE_MIN_US = 500;   // meist 500-600us
static const uint16_t PULSE_MAX_US = 2500;  // meist 2400-2500us

// 6 Achsen: Base, Schulter, Ellenbogen, Drehen, Kippen, Greifen
static const int SERVO_COUNT = 6;
static const char* SERVO_NAME[SERVO_COUNT] = {
  "Base", "Schulter", "Ellenbogen", "Drehen", "Kippen", "Greifen"
};

// PCA-Kanäle 0..5
static const uint8_t SERVO_CH[SERVO_COUNT] = {0,1,2,3,4,5};

Preferences prefs;
WebServer server(80);

// gespeicherte Home-Positionen (Grad)
int homeDeg[SERVO_COUNT] = {90,90,90,90,90,90};
int curDeg[SERVO_COUNT]  = {90,90,90,90,90,90};

static uint16_t usToPcaTicks(uint16_t microseconds) {
  // PCA9685 hat 4096 steps pro Periode
  // Periodendauer bei 50Hz = 20ms = 20000us
  // ticks = us * 4096 / 20000
  const uint32_t ticks = (uint32_t)microseconds * 4096UL / 20000UL;
  return (uint16_t)ticks;
}

static uint16_t degToUs(int deg) {
  if (deg < 0) deg = 0;
  if (deg > 180) deg = 180;
  const uint32_t us = PULSE_MIN_US + (uint32_t)(PULSE_MAX_US - PULSE_MIN_US) * (uint32_t)deg / 180UL;
  return (uint16_t)us;
}

static void setServoDeg(int idx, int deg) {
  if (idx < 0 || idx >= SERVO_COUNT) return;
  if (deg < 0) deg = 0;
  if (deg > 180) deg = 180;

  curDeg[idx] = deg;
  uint16_t us = degToUs(deg);
  uint16_t ticks = usToPcaTicks(us);

  // setPWM(channel, on, off)
  pca.setPWM(SERVO_CH[idx], 0, ticks);
}

static void loadHomes() {
  prefs.begin("robotarm", true);
  for (int i=0;i<SERVO_COUNT;i++) {
    String key = String("h") + i;
    homeDeg[i] = prefs.getInt(key.c_str(), 90);
    curDeg[i] = homeDeg[i];
  }
  prefs.end();
}

static void saveHomes() {
  prefs.begin("robotarm", false);
  for (int i=0;i<SERVO_COUNT;i++) {
    String key = String("h") + i;
    prefs.putInt(key.c_str(), homeDeg[i]);
  }
  prefs.end();
}

static String htmlPage() {
  String s;
  s.reserve(6000);
  s += "<!doctype html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  s += "<title>RobotArm Kalibrierung</title>";
  s += "<style>body{font-family:Arial;margin:20px;background:#0b0b0b;color:#eaeaea}"
       ".card{background:#161616;padding:14px;border-radius:12px;margin:10px 0}"
       "input[type=range]{width:100%} .row{display:flex;gap:10px;align-items:center}"
       "button{padding:10px 14px;border:0;border-radius:10px;margin:6px 6px 0 0}"
       ".ok{background:#2e7d32;color:#fff} .warn{background:#b71c1c;color:#fff} a{color:#66aaff}"
       "</style></head><body>";
  s += "<h2>RobotArm Kalibrierung</h2>";
  s += "<div class='card'><b>Web OTA:</b> <a href='/ota'>hier</a></div>";

  for (int i=0;i<SERVO_COUNT;i++) {
    s += "<div class='card'>";
    s += "<div class='row'><b>";
    s += SERVO_NAME[i];
    s += "</b><span id='v";
    s += i;
    s += "'>";
    s += String(curDeg[i]);
    s += "</span>°</div>";
    s += "<input type='range' min='0' max='180' value='";
    s += String(curDeg[i]);
    s += "' oninput='setS(";
    s += i;
    s += ", this.value)'>";
    s += "<div class='row'>";
    s += "<button class='ok' onclick='setHome(";
    s += i;
    s += ")'>Als HOME speichern</button>";
    s += "</div></div>";
  }

  s += "<div class='card'>";
  s += "<button class='ok' onclick='goHome()'>Alle auf HOME</button>";
  s += "<button class='warn' onclick='saveAll()'>HOME Werte speichern</button>";
  s += "</div>";

  s += "<script>"
       "async function setS(i,val){document.getElementById('v'+i).innerText=val;"
       "await fetch(`/api/set?id=${i}&deg=${val}`);}"
       "async function setHome(i){await fetch(`/api/sethome?id=${i}`);}"
       "async function goHome(){await fetch('/api/gohome'); location.reload();}"
       "async function saveAll(){await fetch('/api/save'); alert('gespeichert');}"
       "</script>";

  s += "</body></html>";
  return s;
}

// --- OTA page ---
static String otaPage() {
  return String(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OTA Update</title>"
    "<style>body{font-family:Arial;margin:20px;background:#0b0b0b;color:#eaeaea}"
    ".card{background:#161616;padding:14px;border-radius:12px;margin:10px 0}"
    "button{padding:10px 14px;border:0;border-radius:10px;background:#2e7d32;color:#fff}"
    "a{color:#66aaff}</style></head><body>"
    "<h2>Web OTA</h2>"
    "<div class='card'>Firmware .bin auswählen und hochladen. Danach reboot.</div>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='update' accept='.bin' required>"
    "<button type='submit'>Upload</button>"
    "</form>"
    "<div class='card'><a href='/'>zurück</a></div>"
    "</body></html>"
  );
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // I2C + PCA
  Wire.begin(); // ESP32 default SDA=21 SCL=22
  pca.begin();
  pca.setOscillatorFrequency(27000000);
  pca.setPWMFreq(SERVO_FREQ);

  loadHomes();
  for (int i=0;i<SERVO_COUNT;i++) setServoDeg(i, curDeg[i]);

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  // Routes
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", htmlPage()); });

  server.on("/api/set", HTTP_GET, [](){
    if (!server.hasArg("id") || !server.hasArg("deg")) { server.send(400, "text/plain", "bad args"); return; }
    int id  = server.arg("id").toInt();
    int deg = server.arg("deg").toInt();
    setServoDeg(id, deg);
    server.send(200, "text/plain", "ok");
  });

  server.on("/api/sethome", HTTP_GET, [](){
    if (!server.hasArg("id")) { server.send(400, "text/plain", "bad args"); return; }
    int id = server.arg("id").toInt();
    if (id < 0 || id >= SERVO_COUNT) { server.send(400, "text/plain", "bad id"); return; }
    homeDeg[id] = curDeg[id];
    server.send(200, "text/plain", "ok");
  });

  server.on("/api/gohome", HTTP_GET, [](){
    for (int i=0;i<SERVO_COUNT;i++) setServoDeg(i, homeDeg[i]);
    server.send(200, "text/plain", "ok");
  });

  server.on("/api/save", HTTP_GET, [](){
    saveHomes();
    server.send(200, "text/plain", "ok");
  });

  // OTA endpoints
  server.on("/ota", HTTP_GET, [](){ server.send(200, "text/html", otaPage()); });

  server.on("/update", HTTP_POST,
    []() {
      // finished
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK - reboot");
      delay(200);
      ESP.restart();
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (!Update.end(true)) {
          Update.printError(Serial);
        }
      }
    }
  );

  server.begin();

  Serial.println("AP ready:");
  Serial.print("SSID: "); Serial.println(AP_SSID);
  Serial.print("IP:   "); Serial.println(WiFi.softAPIP());
}

void loop() {
  server.handleClient();
}
