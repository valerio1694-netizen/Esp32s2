#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ModbusMaster.h>

// ========== WLAN / AP ==========
const char* AP_SSID = "SK120-Tool";
const char* AP_PASS = "12345678";

WebServer server(80);

// ========== SK120 / Modbus ==========
HardwareSerial SKSerial(2);      // UART2
ModbusMaster sk120;

// Pins: an dein SK120-Kabel anpassen
constexpr int SK120_RX_PIN = 16; // ESP empfängt hier (an SK120-TX)
constexpr int SK120_TX_PIN = 17; // ESP sendet hier (an SK120-RX)
constexpr uint32_t SK120_BAUD   = 9600;

// Livewerte
float u_set = 0.0f;
float i_set = 0.0f;
float u_meas = 0.0f;
float i_meas = 0.0f;
float p_meas = 0.0f;
bool  out_on = false;

// ========== HTML UI ==========
const char MAIN_page[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>XY-SK120 Control</title>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:0;padding:10px;}
h1{font-size:22px;margin:10px 0;color:#00bfff;}
.card{display:inline-block;margin:8px;padding:12px 18px;border-radius:10px;background:#222;min-width:140px;}
.label{font-size:12px;color:#aaa;}
.value{font-size:22px;margin-top:4px;}
.row{margin-top:10px;}
button{padding:8px 16px;margin:6px;border-radius:6px;border:none;cursor:pointer;font-size:14px;}
button.on{background:#2e7d32;color:#fff;}
button.off{background:#c62828;color:#fff;}
input{width:80px;padding:4px;border-radius:4px;border:1px solid #555;background:#000;color:#fff;text-align:right;}
a{color:#00bfff;}
</style>
</head>
<body>
<h1>XY-SK120 Control</h1>
<p>OTA-Update: <a href="/update">hier</a></p>

<div class="row">
  <div class="card"><div class="label">U Ist</div><div class="value" id="u_meas">-.- V</div></div>
  <div class="card"><div class="label">I Ist</div><div class="value" id="i_meas">-.--- A</div></div>
  <div class="card"><div class="label">P Ist</div><div class="value" id="p_meas">-.- W</div></div>
</div>

<div class="row">
  <div class="card">
    <div class="label">U Soll</div>
    <input type="number" id="u_set" step="0.1"> V
  </div>
  <div class="card">
    <div class="label">I Soll</div>
    <input type="number" id="i_set" step="0.01"> A
  </div>
</div>

<div class="row">
  <button class="on"  id="btn_on"  onclick="setOut(1)">Ausgang EIN</button>
  <button class="off" id="btn_off" onclick="setOut(0)">Ausgang AUS</button>
</div>

<script>
async function loadData(){
  try{
    const res = await fetch('/data');
    if(!res.ok) return;
    const d = await res.json();
    document.getElementById('u_meas').innerText = d.u_meas.toFixed(2)+" V";
    document.getElementById('i_meas').innerText = d.i_meas.toFixed(3)+" A";
    document.getElementById('p_meas').innerText = d.p_meas.toFixed(1)+" W";
    document.getElementById('u_set').value = d.u_set.toFixed(2);
    document.getElementById('i_set').value = d.i_set.toFixed(3);

    const on  = document.getElementById('btn_on');
    const off = document.getElementById('btn_off');
    if(d.out_on){
      on.style.opacity = "1.0";
      off.style.opacity = "0.4";
    }else{
      on.style.opacity = "0.4";
      off.style.opacity = "1.0";
    }
  }catch(e){}
}
async function applySetpoints(){
  const u = document.getElementById('u_set').value;
  const i = document.getElementById('i_set').value;
  try{
    await fetch('/set?u='+encodeURIComponent(u)+'&i='+encodeURIComponent(i));
    loadData();
  }catch(e){}
}
async function setOut(on){
  try{
    await fetch('/out?on='+on);
    loadData();
  }catch(e){}
}
document.getElementById('u_set').addEventListener('change', applySetpoints);
document.getElementById('i_set').addEventListener('change', applySetpoints);
setInterval(loadData, 500);
loadData();
</script>
</body>
</html>
)HTML";

// ========== OTA HTML ==========
const char UPDATE_page[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head><meta charset="UTF-8"><title>OTA Update</title></head>
<body style="background:#222;color:#fff;font-family:Arial;text-align:center;">
<h2>ESP32 Firmware Update</h2>
<form method='POST' action='/update' enctype='multipart/form-data'>
  <input type='file' name='update'>
  <input type='submit' value='Upload'>
</form>
</body>
</html>
)HTML";

// ========== Modbus Helper (Register laut XY-SK120-Lib) ==========
// 0x0000: Voltage set (/100 V)
// 0x0001: Current set (/1000 A)
// 0x0002: U out (/100 V)
// 0x0003: I out (/1000 A)
// 0x0004: P out (/100 W)
// 0x0012: Output on/off (0/1)

void sk_readAll() {
  // Sollwerte
  uint8_t res1 = sk120.readHoldingRegisters(0x0000, 2);
  if (res1 == sk120.ku8MBSuccess) {
    u_set = sk120.getResponseBuffer(0) / 100.0f;
    i_set = sk120.getResponseBuffer(1) / 1000.0f;
  }

  // Istwerte
  uint8_t res2 = sk120.readHoldingRegisters(0x0002, 3);
  if (res2 == sk120.ku8MBSuccess) {
    u_meas = sk120.getResponseBuffer(0) / 100.0f;
    i_meas = sk120.getResponseBuffer(1) / 1000.0f;
    p_meas = sk120.getResponseBuffer(2) / 100.0f;
  }

  // Output state
  uint8_t res3 = sk120.readHoldingRegisters(0x0012, 1);
  if (res3 == sk120.ku8MBSuccess) {
    out_on = (sk120.getResponseBuffer(0) == 1);
  }
}

void sk_setVoltage(float v) {
  uint16_t reg = (uint16_t)(v * 100.0f);
  sk120.writeSingleRegister(0x0000, reg);
}

void sk_setCurrent(float i) {
  uint16_t reg = (uint16_t)(i * 1000.0f);
  sk120.writeSingleRegister(0x0001, reg);
}

void sk_setOutput(bool on) {
  sk120.writeSingleRegister(0x0012, on ? 1 : 0);
  out_on = on;
}

// ========== OTA Handler ==========
void handleUpdatePage() {
  server.send(200, "text/html", UPDATE_page);
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      server.send(200, "text/plain", "Update OK, reboot...");
      delay(300);
      ESP.restart();
    } else {
      server.send(200, "text/plain", "Update failed");
    }
  }
}

// ========== Web Handler ==========
void handleRoot() {
  server.send(200, "text/html", MAIN_page);
}

void handleData() {
  sk_readAll();
  String json = "{";
  json += "\"u_set\":"  + String(u_set, 3)   + ",";
  json += "\"i_set\":"  + String(i_set, 3)   + ",";
  json += "\"u_meas\":" + String(u_meas, 3)  + ",";
  json += "\"i_meas\":" + String(i_meas, 3)  + ",";
  json += "\"p_meas\":" + String(p_meas, 3)  + ",";
  json += "\"out_on\":" + String(out_on ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleSet() {
  if (server.hasArg("u")) {
    float v = server.arg("u").toFloat();
    if (v >= 0.0f && v <= 36.0f) sk_setVoltage(v);
  }
  if (server.hasArg("i")) {
    float i = server.arg("i").toFloat();
    if (i >= 0.0f && i <= 6.0f) sk_setCurrent(i);
  }
  server.send(200, "text/plain", "OK");
}

void handleOut() {
  if (!server.hasArg("on")) {
    server.send(400, "text/plain", "missing param");
    return;
  }
  bool on = server.arg("on") == "1";
  sk_setOutput(on);
  server.send(200, "text/plain", "OK");
}

// ========== Setup / Loop ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nBoot...");

  SKSerial.begin(SK120_BAUD, SERIAL_8N1, SK120_RX_PIN, SK120_TX_PIN);
  sk120.begin(1, SKSerial);   // Slave ID 1

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/set", handleSet);
  server.on("/out", handleOut);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, [](){}, handleUpdateUpload);

  server.begin();
  Serial.println("Webserver ready");
}

void loop() {
  server.handleClient();
}
