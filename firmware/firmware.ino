/***** ESP32-S2 mini – Menü + Splash + Scope + Uhr + Settings + Web-OTA (Browser) *****/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>
#include <WiFi.h>
#include <AsyncTCP.h>                 // v1.1.1
#include <ESPAsyncWebServer.h>        // v3.6.0
#include <Update.h>

// ---------- Pins ----------
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13
#define PIN_ADC  2

// ---------- Display ----------
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// Zusätzliche Farben, falls die Lib sie nicht definiert hat
#ifndef ST77XX_DARKGREY
  #define ST77XX_DARKGREY 0x7BEF
#endif
#ifndef ST77XX_NAVY
  #define ST77XX_NAVY 0x000F
#endif
#ifndef ST77XX_ORANGE
  #define ST77XX_ORANGE 0xFD20
#endif
#ifndef ST77XX_OLIVE
  #define ST77XX_OLIVE 0x7BE0
#endif

// ---------- WLAN / Web-OTA ----------
const char* WIFI_SSID = "DEINE_SSID";
const char* WIFI_PASS = "DEIN_PASSWORT";   // leer lassen => AP-Fallback
const char* AP_SSID   = "Scope_OTA";
const char* AP_PASS   = "12345678";
AsyncWebServer server(80);

// ---------- ADC / Berechnung ----------
const float ADC_REF    = 3.3f;
const float ADC_COUNTS = 4095.0f;
const float DIV_GAIN   = 12.0f;

// ---------- States ----------
enum State { SPLASH, MENU, MODE_SCOPE, MODE_CLOCK, MODE_SETTINGS, MODE_ABOUT, MODE_OTA };
State state = SPLASH;

// ---------- Menü ----------
const char* MENU_ITEMS[] = {
  "Oszilloskop",
  "Uhr",
  "Einstellungen",
  "OTA Update",
  "Info"
};
const uint8_t MENU_COUNT = sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0]);
int8_t menuIndex = 0;

// ---------- Settings ----------
Preferences prefs;
uint8_t  backlight = 200;      // 0..255
uint16_t trigLevel = 2048;     // 0..4095

// ---------- Daten ----------
uint32_t tStart = 0;
uint16_t scopeBuf[160];

// ---------- Sinus-LUT fuer Splash ----------
const uint8_t SIN_W = 160;
uint8_t sinY[SIN_W];
void buildSinusLUT() {
  for (int x = 0; x < SIN_W; x++) {
    float a = (2.0f * PI) * (float)x / (float)SIN_W;
    float s = (sinf(a) * 0.45f + 0.5f);
    sinY[x] = (uint8_t)(24 + s * (103 - 24));  // in Plotfenster 24..103
  }
}

// ---------- Backlight ----------
void setBacklight(uint8_t v) {
  static bool init = false;
  if (!init) { ledcSetup(0, 5000, 8); ledcAttachPin(TFT_LED, 0); init = true; }
  ledcWrite(0, v);
}

// ---------- WLAN + Web-OTA ----------
void startWiFi() {
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID)) WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (strlen(WIFI_SSID) && WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
  }
}

String otaIndexHtml() {
  IPAddress ip = (WiFi.getMode()==WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP();
  String html =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:20px}"
    "h2{color:#0ff} .box{background:#222;padding:16px;border-radius:8px}"
    "input[type=file]{margin:8px 0}button{padding:8px 14px;border:0;border-radius:6px;background:#0aa;color:#fff}"
    "#p{height:8px;background:#333;border-radius:4px;margin-top:10px;overflow:hidden}"
    "#bar{height:8px;background:#0f0;width:0%}"
    "</style></head><body>"
    "<h2>Robin's Oszilloskop – OTA</h2>"
    "<div class='box'>"
    "<p>Ger&auml;te-IP: <b>" + ip.toString() + "</b></p>"
    "<form id='f' method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin,.bin.gz' required>"
    "<br><button type='submit'>Flashen</button>"
    "</form>"
    "<div id='p'><div id='bar'></div></div>"
    "<p id='msg'></p>"
    "<script>"
    "const f=document.getElementById('f');const bar=document.getElementById('bar');const msg=document.getElementById('msg');"
    "f.addEventListener('submit', e=>{e.preventDefault();const x=new XMLHttpRequest();"
    "x.upload.onprogress=v=>{if(v.lengthComputable){bar.style.width=(100*v.loaded/v.total).toFixed(0)+'%';}};"
    "x.onload=()=>{msg.textContent=x.responseText;};"
    "x.open('POST','/update');x.send(new FormData(f));});"
    "</script></div></body></html>";
  return html;
}

void setupWebOTA() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/html", otaIndexHtml()); });

  // Upload-Handler
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest* r){
      bool ok = !Update.hasError();
      r->send(200, "text/plain", ok ? "Update OK – Neustart..." : "Update fehlgeschlagen");
      if (ok) { delay(500); ESP.restart(); }
    },
    [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t *data, size_t len, bool final){
      if (!index) {
        // Start
        Update.begin(UPDATE_SIZE_UNKNOWN);
      }
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
      }
      if (final) {
        if (!Update.end(true)) Update.printError(Serial);
      }
    }
  );

  server.begin();
}

// ---------- UI ----------
void header(const char* title) {
  tft.fillRect(0, 0, 160, 16, ST77XX_NAVY);
  tft.setCursor(4, 3);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(title);
}

void showMenu() {
  header("Menue");
  tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);
  for (uint8_t i = 0; i < MENU_COUNT; i++) {
    int y = 22 + i * 18;
    if (i == menuIndex) {
      tft.fillRect(4, y - 2, 152, 14, ST77XX_DARKGREY);
      tft.setTextColor(ST77XX_BLACK);
    } else tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(8, y);
    tft.print(MENU_ITEMS[i]);
  }
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 112);
  tft.print("w/s=wahl, e=OK, b=zurueck");
}

void showSplash() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(10, 6);
  tft.print("Robin's Oszilloskop");
  // Sinus
  for (int x = 0; x < SIN_W - 1; x++) tft.drawLine(x, sinY[x], x + 1, sinY[x + 1], ST77XX_GREEN);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 20); tft.print("wird geladen...");

  // Ladebalken 5s
  int x0 = 10, y0 = 110, w = 140, h = 10;
  tft.drawRect(x0 - 1, y0 - 1, w + 2, h + 2, ST77XX_DARKGREY);
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    float p = (millis() - t0) / 5000.0f;
    int fillW = (int)(w * p);
    tft.fillRect(x0, y0, fillW, h, ST77XX_ORANGE);
    delay(20);
  }
}

void drawClock() {
  header("Uhr");
  tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);
}

void runClock() {
  static uint32_t last = 0;
  if (millis() - last >= 200) {
    last = millis();
    uint32_t sec = (millis() - tStart) / 1000;
    int h = (sec / 3600) % 24, m = (sec / 60) % 60, s = sec % 60;
    char buf[16]; sprintf(buf, "%02d:%02d:%02d", h, m, s);
    tft.fillRect(0, 40, 160, 40, ST77XX_BLACK);
    tft.setTextSize(2); tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(20, 48); tft.print(buf);
    tft.setTextSize(1);
  }
}

void drawScopeFrame() {
  header("Oszilloskop");
  tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);
  tft.drawFastHLine(0, 64, 160, ST77XX_DARKGREY);
  tft.drawFastVLine(0, 16, 112, ST77XX_DARKGREY);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 18); tft.print("Trig:");
  tft.setCursor(34, 18); tft.print((int)(trigLevel * 100 / 4095)); tft.print("%  (t=+  m=menu)");
}

void runScope() {
  for (int i = 0; i < 160; i++) scopeBuf[i] = analogRead(PIN_ADC);
  tft.fillRect(1, 17, 158, 110, ST77XX_BLACK);
  int yTrig = map(trigLevel, 0, 4095, 127, 16);
  tft.drawFastHLine(1, yTrig, 158, ST77XX_ORANGE);
  for (int x = 0; x < 159; x++) {
    int y1 = map(scopeBuf[x],     0, 4095, 127, 16);
    int y2 = map(scopeBuf[x + 1], 0, 4095, 127, 16);
    tft.drawLine(x + 1, y1, x + 2, y2, ST77XX_GREEN);
  }
  uint32_t acc = 0; for (int i = 0; i < 160; i++) acc += scopeBuf[i];
  float vIn = (acc / 160.0f) * (ADC_REF / ADC_COUNTS) * DIV_GAIN;
  tft.setTextColor(ST77XX_CYAN); tft.setCursor(4, 110);
  tft.print("Vin: "); tft.print(vIn, 2); tft.print(" V");
}

void drawSettings() {
  header("Einstellungen");
  tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);
  tft.setCursor(6, 28); tft.setTextColor(ST77XX_WHITE); tft.print("Helligkeit: "); tft.print(backlight);
  tft.setCursor(6, 46); tft.print("Trigger:    "); tft.print((int)(trigLevel * 100 / 4095)); tft.print("%");
  tft.setCursor(6, 86); tft.setTextColor(ST77XX_YELLOW); tft.print("a/d = Hell -, +");
  tft.setCursor(6, 100); tft.print("j/l = Trig -, +");
  tft.setCursor(6, 114); tft.print("b = zurueck (speichern)");
}
void applySettings(){ setBacklight(backlight); }

// Info
void drawAbout() {
  header("Info");
  tft.fillRect(0,16,160,112,ST77XX_BLACK);
  tft.setCursor(6,28); tft.setTextColor(ST77XX_WHITE); tft.print("Robin's Oszilloskop");
  tft.setCursor(6,44); tft.print("ESP32-S2 + ST7735");
  tft.setCursor(6,60); tft.print("Web-OTA: / (Browser)");
  tft.setCursor(6,92); tft.setTextColor(ST77XX_YELLOW); tft.print("b = zurueck");
}

// OTA-Seite (nur Info auf dem Display)
void drawOTA() {
  header("OTA Update");
  tft.fillRect(0,16,160,112,ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  IPAddress ip = (WiFi.getMode()==WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP();
  tft.setCursor(6,28); tft.print("IP im Browser oeffnen:");
  tft.setCursor(6,44); tft.print(ip);
  tft.setCursor(6,60); tft.print("Datei hochladen -> Flash");
  tft.setCursor(6,112); tft.setTextColor(ST77XX_CYAN); tft.print("b = zurueck");
}

// ---------- Eingabe (Serielle Tasten) ----------
void pollInput() {
  if (!Serial.available()) return;
  int c = Serial.read();
  switch (state) {
    case MENU:
      if (c=='w'){ menuIndex=(menuIndex-1+MENU_COUNT)%MENU_COUNT; showMenu(); }
      if (c=='s'){ menuIndex=(menuIndex+1)%MENU_COUNT; showMenu(); }
      if (c=='e'){
        if (menuIndex==0){ state=MODE_SCOPE;    drawScopeFrame(); }
        if (menuIndex==1){ state=MODE_CLOCK;    drawClock(); }
        if (menuIndex==2){ state=MODE_SETTINGS; drawSettings(); }
        if (menuIndex==3){ state=MODE_OTA;      drawOTA(); }
        if (menuIndex==4){ state=MODE_ABOUT;    drawAbout(); }
      }
      break;

    case MODE_SCOPE:
      if (c=='m'||c=='b'){ state=MENU; showMenu(); }
      if (c=='t'){ trigLevel = (trigLevel+128>4095)?1024:trigLevel+128; drawScopeFrame(); }
      break;

    case MODE_CLOCK:
      if (c=='b'||c=='m'){ state=MENU; showMenu(); }
      break;

    case MODE_SETTINGS:
      if (c=='a'&&backlight>0){ backlight--; applySettings(); drawSettings(); }
      if (c=='d'&&backlight<255){ backlight++; applySettings(); drawSettings(); }
      if (c=='j'&&trigLevel>=16){ trigLevel-=16; drawSettings(); }
      if (c=='l'&&trigLevel<=4095-16){ trigLevel+=16; drawSettings(); }
      if (c=='b'){
        prefs.begin("scope", false);
        prefs.putUChar("backlight", backlight);
        prefs.putUShort("trig", trigLevel);
        prefs.end();
        state=MENU; showMenu();
      }
      break;

    case MODE_OTA:
      if (c=='b'){ state=MENU; showMenu(); }
      break;

    case MODE_ABOUT:
      if (c=='b'){ state=MENU; showMenu(); }
      break;

    default: break;
  }
}

// ---------- Setup / Loop ----------
void setup() {
  pinMode(TFT_LED, OUTPUT);
  Serial.begin(115200);
  delay(50);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  buildSinusLUT();

  prefs.begin("scope", true);
  backlight = prefs.getUChar("backlight", backlight);
  trigLevel = prefs.getUShort("trig", trigLevel);
  prefs.end();
  setBacklight(backlight);

  analogReadResolution(12);

  showSplash();           // 5s Ladebild

  startWiFi();            // WLAN (AP-Fallback)
  setupWebOTA();          // Web-OTA per Browser

  tStart = millis();

  state = MENU;
  showMenu();
}

void loop() {
  pollInput();

  switch (state) {
    case MODE_SCOPE: {
      static uint32_t last=0;
      if (millis()-last > 40) { last=millis(); runScope(); }
    } break;
    case MODE_CLOCK:
      runClock();
      break;
    default:
      delay(10);
      break;
  }
}
