/*
  ESP32 + PCA9685 Servo Calibration Web UI + Web OTA (stays forever)

  AP:
    SSID: XY-SK120
    PASS: 12345678
    IP:   192.168.4.1

  Web:
    /      -> calibration UI
    /ota   -> OTA upload page
    /update (POST) -> firmware upload handler
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>
#include <Preferences.h>

#include <Adafruit_PWMServoDriver.h>

static const char* AP_SSID = "XY-SK120";
static const char* AP_PASS = "12345678";

WebServer server(80);
Preferences prefs;

// PCA9685
Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(0x40);

static const uint8_t SERVO_COUNT = 6;          // Base, Schulter, Ellenbogen, Drehen, Kippen, Greifer
static const uint16_t SERVO_FREQ = 50;         // Standard analog servo 50Hz
static const uint16_t DEFAULT_MIN_US  = 500;
static const uint16_t DEFAULT_MAX_US  = 2500;
static const uint16_t DEFAULT_HOME_US = 1500;

// Channels on PCA9685 for each servo (0..15)
uint8_t servoCh[SERVO_COUNT] = {0, 1, 2, 3, 4, 5};

// Calibration data
uint16_t calMin[SERVO_COUNT];
uint16_t calMax[SERVO_COUNT];
uint16_t calHome[SERVO_COUNT];

// Current position per servo
uint16_t curUs[SERVO_COUNT];

// UI state
uint8_t selectedServo = 0;
uint16_t stepUs = 5;     // step size for +/-

// ---------- helpers ----------
uint16_t usToPwmTicks(uint16_t us) {
  // ticks = us * freq * 4096 / 1e6
  // for 50Hz: period 20ms -> 20000us, 4096 ticks
  // ticks = us * 4096 / 20000
  // use general formula:
  uint32_t ticks = (uint32_t)us * SERVO_FREQ * 4096UL / 1000000UL;
  if (ticks > 4095) ticks = 4095;
  return (uint16_t)ticks;
}

void writeServoUs(uint8_t idx, uint16_t us) {
  if (idx >= SERVO_COUNT) return;

  // clamp to something sane to avoid killing servos in case of bad input
  if (us < 300) us = 300;
  if (us > 3000) us = 3000;

  curUs[idx] = us;
  uint16_t ticks = usToPwmTicks(us);
  pca.setPWM(servoCh[idx], 0, ticks);
}

void loadDefaults() {
  for (uint8_t i = 0; i < SERVO_COUNT; i++) {
    calMin[i]  = DEFAULT_MIN_US;
    calMax[i]  = DEFAULT_MAX_US;
    calHome[i] = DEFAULT_HOME_US;
    curUs[i]   = DEFAULT_HOME_US;
  }
}

void loadPrefs() {
  loadDefaults();
  prefs.begin("servoCal", true);
  for (uint8_t i = 0; i < SERVO_COUNT; i++) {
    char kmin[16], kmax[16], khome[16];
    snprintf(kmin,  sizeof(kmin),  "m%d", i);
    snprintf(kmax,  sizeof(kmax),  "x%d", i);
    snprintf(khome, sizeof(khome), "h%d", i);

    uint16_t vmin  = prefs.getUShort(kmin,  calMin[i]);
    uint16_t vmax  = prefs.getUShort(kmax,  calMax[i]);
    uint16_t vhome = prefs.getUShort(khome, calHome[i]);

    // sanity
    if (vmin < 300) vmin = 300;
    if (vmax > 3000) vmax = 3000;
    if (vmin >= vmax) { vmin = DEFAULT_MIN_US; vmax = DEFAULT_MAX_US; }
    if (vhome < vmin) vhome = vmin;
    if (vhome > vmax) vhome = vmax;

    calMin[i]  = vmin;
    calMax[i]  = vmax;
    calHome[i] = vhome;
    curUs[i]   = vhome;
  }
  prefs.end();
}

void savePrefs() {
  prefs.begin("servoCal", false);
  for (uint8_t i = 0; i < SERVO_COUNT; i++) {
    char kmin[16], kmax[16], khome[16];
    snprintf(kmin,  sizeof(kmin),  "m%d", i);
    snprintf(kmax,  sizeof(kmax),  "x%d", i);
    snprintf(khome, sizeof(khome), "h%d", i);

    prefs.putUShort(kmin,  calMin[i]);
    prefs.putUShort(kmax,  calMax[i]);
    prefs.putUShort(khome, calHome[i]);
  }
  prefs.end();
}

String jsonState() {
  String s = "{";
  s += "\"selected\":" + String(selectedServo) + ",";
  s += "\"step\":" + String(stepUs) + ",";
  s += "\"servos\":[";
  for (uint8_t i = 0; i < SERVO_COUNT; i++) {
    if (i) s += ",";
    s += "{";
    s += "\"i\":" + String(i) + ",";
    s += "\"min\":" + String(calMin[i]) + ",";
    s += "\"max\":" + String(calMax[i]) + ",";
    s += "\"home\":" + String(calHome[i]) + ",";
    s += "\"cur\":" + String(curUs[i]) + ",";
    s += "\"ch\":" + String(servoCh[i]);
    s += "}";
  }
  s += "]}";
  return s;
}

// ---------- web pages ----------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>Servo Kalibrierung</title>
  <style>
    body{font-family:system-ui,Segoe UI,Arial;margin:0;background:#0b0f14;color:#e8eef6}
    header{padding:14px 16px;border-bottom:1px solid #1b2533}
    a{color:#7cc4ff}
    .wrap{padding:16px;max-width:900px;margin:0 auto}
    .card{background:#0f1620;border:1px solid #1b2533;border-radius:14px;padding:14px;margin:12px 0}
    .row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
    select,input,button{font-size:16px;border-radius:10px;border:1px solid #243245;background:#0b0f14;color:#e8eef6;padding:10px}
    button{cursor:pointer}
    button.primary{background:#123b63;border-color:#1d5a95}
    button.good{background:#123f2a;border-color:#1f7a4c}
    button.bad{background:#4a1b1b;border-color:#8a2c2c}
    .grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
    @media(max-width:720px){.grid{grid-template-columns:1fr}}
    .mono{font-family:ui-monospace,Consolas,monospace;font-size:13px;white-space:pre-wrap;background:#0b0f14;border:1px solid #243245;border-radius:10px;padding:10px}
    .hint{opacity:.85}
  </style>
</head>
<body>
<header>
  <div class="wrap">
    <div style="display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap">
      <div>
        <div style="font-size:18px;font-weight:700">ESP32 Servo Kalibrierung (PCA9685)</div>
        <div class="hint">Web-OTA: <a href="/ota">/ota</a></div>
      </div>
      <button class="primary" onclick="refresh()">Refresh</button>
    </div>
  </div>
</header>

<div class="wrap">
  <div class="card">
    <div class="row">
      <label>Servo:</label>
      <select id="servoSel" onchange="setServo()"></select>

      <label>Step (µs):</label>
      <input id="step" type="number" min="1" max="200" value="5" style="width:110px" onchange="setStep()"/>

      <button onclick="move(-1)">-</button>
      <button onclick="move(+1)">+</button>
      <button class="primary" onclick="gotoHome()">Goto HOME</button>
    </div>

    <div class="row" style="margin-top:10px">
      <button class="good" onclick="setMin()">SET MIN</button>
      <button class="good" onclick="setMax()">SET MAX</button>
      <button class="good" onclick="setHome()">SET HOME</button>
      <button class="primary" onclick="save()">SAVE</button>
      <button class="bad" onclick="defaults()">Defaults</button>
    </div>
  </div>

  <div class="grid">
    <div class="card">
      <div style="font-weight:700;margin-bottom:8px">Aktueller Status</div>
      <div id="status" class="mono">...</div>
    </div>

    <div class="card">
      <div style="font-weight:700;margin-bottom:8px">So benutzt du das</div>
      <div class="hint">
        1) Hörner erst grob mechanisch setzen (Home-Pose).<br/>
        2) Servo wählen → mit -/+ langsam fahren.<br/>
        3) Kurz vor Anschlag: SET MIN / SET MAX.<br/>
        4) Wunsch-Home anfahren: SET HOME.<br/>
        5) SAVE → bleibt dauerhaft gespeichert.<br/><br/>
        <b>Wichtig:</b> Servos extern versorgen, GND gemeinsam.
      </div>
    </div>
  </div>
</div>

<script>
async function api(q){
  const r = await fetch('/api?'+q);
  if(!r.ok) throw new Error(await r.text());
  return await r.json();
}
function pretty(o){ return JSON.stringify(o,null,2); }

async function refresh(){
  const st = await api('cmd=state');
  document.getElementById('status').textContent = pretty(st);

  const sel = document.getElementById('servoSel');
  sel.innerHTML = '';
  st.servos.forEach(s=>{
    const opt = document.createElement('option');
    opt.value = s.i;
    opt.textContent = `Servo ${s.i+1} (CH ${s.ch})`;
    sel.appendChild(opt);
  });
  sel.value = st.selected;
  document.getElementById('step').value = st.step;
}

async function setServo(){
  const v = document.getElementById('servoSel').value;
  await api('cmd=select&servo='+encodeURIComponent(v));
  await refresh();
}
async function setStep(){
  const v = document.getElementById('step').value;
  await api('cmd=step&value='+encodeURIComponent(v));
  await refresh();
}
async function move(dir){
  await api('cmd=move&dir='+encodeURIComponent(dir));
  await refresh();
}
async function setMin(){ await api('cmd=setmin'); await refresh(); }
async function setMax(){ await api('cmd=setmax'); await refresh(); }
async function setHome(){ await api('cmd=sethome'); await refresh(); }
async function gotoHome(){ await api('cmd=gotohome'); await refresh(); }
async function save(){ await api('cmd=save'); await refresh(); alert('Gespeichert.'); }
async function defaults(){
  if(!confirm('Defaults laden? (Überschreibt Kalibrierung im RAM. SAVE schreibt es dauerhaft.)')) return;
  await api('cmd=defaults'); await refresh();
}

refresh().catch(e=>{ document.getElementById('status').textContent = e.toString(); });
</script>
</body>
</html>
)HTML";

const char OTA_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>Web OTA</title>
  <style>
    body{font-family:system-ui,Segoe UI,Arial;background:#0b0f14;color:#e8eef6;margin:0}
    .wrap{max-width:700px;margin:0 auto;padding:16px}
    .card{background:#0f1620;border:1px solid #1b2533;border-radius:14px;padding:14px;margin:12px 0}
    input,button{font-size:16px;border-radius:10px;border:1px solid #243245;background:#0b0f14;color:#e8eef6;padding:10px}
    button{cursor:pointer}
    a{color:#7cc4ff}
    .hint{opacity:.85}
  </style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <div style="font-size:18px;font-weight:700">ESP32 Web OTA</div>
    <div class="hint">Zurück: <a href="/">/</a></div>
  </div>

  <div class="card">
    <form method="POST" action="/update" enctype="multipart/form-data">
      <div class="hint">Wähle deine <b>.bin</b> (Arduino/CI Build) und lade hoch.</div><br/>
      <input type="file" name="update" accept=".bin" required/>
      <button type="submit" style="margin-left:10px">Upload</button>
    </form>
  </div>

  <div class="card hint">
    Nach erfolgreichem Upload startet der ESP neu. Kalibrierwerte bleiben erhalten (NVS).
  </div>
</div>
</body>
</html>
)HTML";

// ---------- API handler ----------
void handleApi() {
  String cmd = server.arg("cmd");
  cmd.toLowerCase();

  if (cmd == "state" || cmd == "") {
    server.send(200, "application/json", jsonState());
    return;
  }

  if (cmd == "select") {
    int s = server.arg("servo").toInt();
    if (s < 0) s = 0;
    if (s >= SERVO_COUNT) s = SERVO_COUNT - 1;
    selectedServo = (uint8_t)s;
    server.send(200, "application/json", jsonState());
    return;
  }

  if (cmd == "step") {
    int v = server.arg("value").toInt();
    if (v < 1) v = 1;
    if (v > 200) v = 200;
    stepUs = (uint16_t)v;
    server.send(200, "application/json", jsonState());
    return;
  }

  if (cmd == "move") {
    int dir = server.arg("dir").toInt();
    int32_t next = (int32_t)curUs[selectedServo] + (dir >= 0 ? (int32_t)stepUs : -(int32_t)stepUs);

    // Keep within a wide safety range, NOT clamped to min/max while calibrating (because min/max may be wrong)
    if (next < 300) next = 300;
    if (next > 3000) next = 3000;

    writeServoUs(selectedServo, (uint16_t)next);
    server.send(200, "application/json", jsonState());
    return;
  }

  if (cmd == "setmin") {
    calMin[selectedServo] = curUs[selectedServo];
    if (calMin[selectedServo] >= calMax[selectedServo]) {
      calMax[selectedServo] = calMin[selectedServo] + 200;
      if (calMax[selectedServo] > 3000) calMax[selectedServo] = 3000;
    }
    server.send(200, "application/json", jsonState());
    return;
  }

  if (cmd == "setmax") {
    calMax[selectedServo] = curUs[selectedServo];
    if (calMin[selectedServo] >= calMax[selectedServo]) {
      calMin[selectedServo] = (calMax[selectedServo] > 200) ? (calMax[selectedServo] - 200) : 300;
      if (calMin[selectedServo] < 300) calMin[selectedServo] = 300;
    }
    server.send(200, "application/json", jsonState());
    return;
  }

  if (cmd == "sethome") {
    // keep home within min/max if those are set sane
    uint16_t h = curUs[selectedServo];
    if (h < calMin[selectedServo]) h = calMin[selectedServo];
    if (h > calMax[selectedServo]) h = calMax[selectedServo];
    calHome[selectedServo] = h;
    server.send(200, "application/json", jsonState());
    return;
  }

  if (cmd == "gotohome") {
    writeServoUs(selectedServo, calHome[selectedServo]);
    server.send(200, "application/json", jsonState());
    return;
  }

  if (cmd == "save") {
    savePrefs();
    server.send(200, "application/json", jsonState());
    return;
  }

  if (cmd == "defaults") {
    loadDefaults();
    // move all to home defaults
    for (uint8_t i = 0; i < SERVO_COUNT; i++) writeServoUs(i, calHome[i]);
    server.send(200, "application/json", jsonState());
    return;
  }

  server.send(400, "application/json", "{\"error\":\"unknown cmd\"}");
}

// ---------- OTA handlers ----------
void handleOtaPage() {
  server.send_P(200, "text/html", OTA_HTML);
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    // start update
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      // begin failed
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      // write failed
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      // success
    } else {
      // fail
    }
  }
}

void handleUpdateResult() {
  bool ok = !Update.hasError();
  server.send(200, "text/plain", ok ? "OK - Rebooting" : "FAIL");
  delay(400);
  ESP.restart();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);

  // I2C for PCA9685
  Wire.begin(); // default SDA=21, SCL=22 on ESP32 Dev Module

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(150);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP: ");
  Serial.println(ip);

  // PCA init
  pca.begin();
  pca.setPWMFreq(SERVO_FREQ);
  delay(20);

  // load cal + move to home
  loadPrefs();
  for (uint8_t i = 0; i < SERVO_COUNT; i++) {
    writeServoUs(i, calHome[i]);
  }

  // routes
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api", HTTP_GET, handleApi);

  server.on("/ota", HTTP_GET, handleOtaPage);

  server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);

  server.begin();
}

void loop() {
  server.handleClient();
}
