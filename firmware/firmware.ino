/*
  Projekt: ESP32-S2 Mini mit 1.8" TFT (ST7735), LED, 2 Buttons, OTA
  Schritt 2 von N: Display aktiviert

  - OTA Weboberfläche (AP-Modus) bleibt enthalten
  - Display: ST7735 128x160 SPI
  - Pins: SCK=12, SDA(MOSI)=11, DC(A0)=7, RST=6, CS=5
  - LED: GPIO13
  - Buttons: GPIO8, GPIO9 gegen GND (mit PullUps)

  Regeln:
  - Keine unbesprochenen Änderungen
  - Immer vollständiger Sketch nach jeder Anpassung
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// =================== OTA / AP-Konfiguration ===================
static const char* AP_SSID     = "ESP32S2-OTA";
static const char* AP_PASSWORD = "flashme123";
static const char* HTTP_USER   = "admin";
static const char* HTTP_PASS   = "esp32s2";

// =================== Pins ===================
static const int TFT_CS   = 5;
static const int TFT_DC   = 7;
static const int TFT_RST  = 6;
static const int TFT_SCK  = 12;
static const int TFT_MOSI = 11;
static const int LED_PIN  = 13;
static const int BTN1_PIN = 8;
static const int BTN2_PIN = 9;

// =================== Display ===================
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);

// =================== Button-Entprellung ===================
static const uint32_t DEBOUNCE_MS = 30;
struct DebouncedButton {
  int pin;
  bool pullup;
  bool state;
  bool lastRead;
  uint32_t lastChange;
};

static DebouncedButton btn1{BTN1_PIN, true, false, true, 0};
static DebouncedButton btn2{BTN2_PIN, true, false, true, 0};

static void initButton(DebouncedButton& b) {
  pinMode(b.pin, b.pullup ? INPUT_PULLUP : INPUT);
  b.lastRead = digitalRead(b.pin);
  b.state = (b.pullup ? (b.lastRead == LOW) : (b.lastRead == HIGH));
  b.lastChange = millis();
}

static bool updateButton(DebouncedButton& b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.lastRead) {
    b.lastChange = millis();
    b.lastRead = raw;
  }
  if ((millis() - b.lastChange) >= DEBOUNCE_MS) {
    bool newState = b.pullup ? (raw == LOW) : (raw == HIGH);
    if (newState != b.state) {
      b.state = newState;
      return true;
    }
  }
  return false;
}

// =================== HTTP Server ===================
WebServer server(80);

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="de">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S2 OTA Upload</title>
<style>
body { font-family: sans-serif; margin:20px; }
.card { max-width: 500px; padding: 15px; border:1px solid #ccc; border-radius:10px;}
progress { width:100%; height:16px;}
.ok { color:green;} .err{color:red;}
</style></head><body>
<div class="card">
<h2>ESP32-S2 OTA Firmware Upload</h2>
<input id="file" type="file" accept=".bin">
<br><br>
<button id="btn">Upload starten</button>
<br><br>
<progress id="pb" max="100" value="0" hidden></progress>
<div id="msg"></div>
</div>
<script>
const b=document.getElementById("btn"),f=document.getElementById("file"),pb=document.getElementById("pb"),m=document.getElementById("msg");
b.onclick=()=>{
 if(!f.files.length){m.textContent="Bitte .bin auswählen";m.className="err";return;}
 const x=new XMLHttpRequest();pb.hidden=false;pb.value=0;m.textContent="Lade hoch...";
 x.upload.onprogress=e=>{if(e.lengthComputable)pb.value=e.loaded/e.total*100;};
 x.onload=()=>{if(x.status==200){m.textContent="OK – Reboot...";m.className="ok";pb.value=100;setTimeout(()=>location.reload(),5000);}else{m.textContent="Fehler:"+x.responseText;m.className="err";}};
 const form=new FormData();form.append("firmware",f.files[0]);x.open("POST","/update",true);x.send(form);
};
</script>
</body></html>
)HTML";

// =================== Auth ===================
static bool isAuthenticated() {
  if (!HTTP_USER || !*HTTP_USER) return true;
  if (server.authenticate(HTTP_USER, HTTP_PASS)) return true;
  server.requestAuthentication();
  return false;
}

// =================== HTTP Routes ===================
static void handleRoot() {
  if (!isAuthenticated()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}
static void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}
static void handleUpdateUpload() {
  if (!isAuthenticated()) return;
  HTTPUpload& upload = server.upload();
  static bool beginOk=false;
  if(upload.status==UPLOAD_FILE_START){
    Serial.printf("Update start: %s\n", upload.filename.c_str());
    beginOk = Update.begin(UPDATE_SIZE_UNKNOWN);
  }else if(upload.status==UPLOAD_FILE_WRITE){
    if(beginOk) Update.write(upload.buf, upload.currentSize);
  }else if(upload.status==UPLOAD_FILE_END){
    if(beginOk && Update.end(true)){
      server.send(200,"text/plain","OK");
      delay(200);ESP.restart();
    }else{
      server.send(500,"text/plain","Update failed");
    }
  }
}

// =================== Setup ===================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Taster
  initButton(btn1);
  initButton(btn2);

  // Display init
  tft.initR(INITR_BLACKTAB); // typisches 1.8" ST7735
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10,10);
  tft.println("ESP32-S2 Ready");

  // WiFi AP + OTA
  WiFi.mode(WIFI_MODE_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST, [](){
    if(!Update.isFinished()) server.send(500,"text/plain","Update nicht abgeschlossen");
  }, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  server.begin();
}

// =================== Loop ===================
void loop() {
  server.handleClient();

  if(updateButton(btn1)){
    Serial.printf("BTN1: %s\n", btn1.state?"GEDRUECKT":"LOS");
    tft.fillScreen(ST77XX_BLUE);
    tft.setCursor(20,70); tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2); tft.println("BTN1!");
    digitalWrite(LED_PIN, btn1.state);
  }
  if(updateButton(btn2)){
    Serial.printf("BTN2: %s\n", btn2.state?"GEDRUECKT":"LOS");
    tft.fillScreen(ST77XX_RED);
    tft.setCursor(20,70); tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2); tft.println("BTN2!");
    digitalWrite(LED_PIN, btn2.state);
  }
}
