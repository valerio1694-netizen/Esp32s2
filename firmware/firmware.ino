#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

// ------------ Access Point Einstellungen ------------
const char* AP_SSID     = "SK120-Tool";
const char* AP_PASS     = "12345678";

// ------------ Webserver ------------
WebServer server(80);

// ------------ HTML Startseite ------------
const char MAIN_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>SK120 Control</title>
<style>
body { background:#111; color:#eee; font-family:Arial; text-align:center; }
h1 { color:#00bfff; }
button { padding:10px 20px; margin:10px; font-size:20px; }
</style>
</head>
<body>
<h1>SK120 Web Interface</h1>

<p>ESP läuft im Access Point Modus.<br>
OTA Firmware Update → <a href="/update">hier</a></p>

</body>
</html>
)rawliteral";


// ----------------------------------------------------
// ------------ OTA UPDATE WEB PAGE -------------------
// ----------------------------------------------------
const char UPDATE_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>OTA Update</title>
<style>
body { background:#222; color:#fff; font-family:Arial; text-align:center; }
input { margin:20px; }
</style>
</head>
<body>
<h2>ESP32 Firmware Update</h2>
<form method='POST' action='/update' enctype='multipart/form-data'>
  <input type='file' name='update'>
  <input type='submit' value='Upload Firmware'>
</form>
</body>
</html>
)rawliteral";


// ----------------------------------------------------
// ------------ OTA Update Handler ---------------------
// ----------------------------------------------------
void handleUpdatePage() {
  server.send(200, "text/html", UPDATE_page);
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA: Update Start: %s\n", upload.filename.c_str());
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } 
  else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
  } 
  else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA: Update Success! Size: %u bytes\n", upload.totalSize);
      server.send(200, "text/plain", "Update complete. Rebooting...");
      delay(300);
      ESP.restart();
    } else {
      server.send(200, "text/plain", "Update error!");
    }
  }
}


// ----------------------------------------------------
// ------------ ROOT PAGE HANDLER ---------------------
// ----------------------------------------------------
void handleRoot() {
  server.send(200, "text/html", MAIN_page);
}


// ----------------------------------------------------
// -------------------- SETUP --------------------------
// ----------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting...");

  // --------- Access Point Starten ---------
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  Serial.print("AP gestartet → IP: ");
  Serial.println(WiFi.softAPIP());

  // --------- Webserver Routen ---------
  server.on("/", handleRoot);
  server.on("/update", HTTP_GET, handleUpdatePage);

  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", "OK");
  }, handleUpdateUpload);

  // Server Start
  server.begin();
  Serial.println("Webserver läuft auf Port 80");
}


// ----------------------------------------------------
// -------------------- LOOP --------------------------
// ----------------------------------------------------
void loop() {
  server.handleClient();
}
