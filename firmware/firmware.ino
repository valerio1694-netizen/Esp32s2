/***** ESP32-S2 mini – Scope + Splash + OTA (AsyncWebServer) *****/
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

/* ---------- Display-Pins (SPI) ---------- */
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13

/* Einige Adafruit-Farben fehlen je nach Lib-Version */
#ifndef ST77XX_DARKGREY
  #define ST77XX_DARKGREY 0x7BEF  // 50% Grau in RGB565
#endif

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

/* ---------- ADC / Mess-Setup ---------- */
const int PIN_ADC = 2;           // Mess-Eingang
const float ADC_REF    = 3.3f;   // Referenz (intern ~3.3 V)
const float ADC_COUNTS = 4095.0f; // 12 Bit
// Teiler 110k / 10k => Gesamt ~12x
const float DIV_GAIN   = (110000.0f + 10000.0f) / 10000.0f;

/* ---------- WLAN / OTA ---------- */
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT";         // leer => AP-Fallback
AsyncWebServer server(80);

/* ---------- Scope-Puffer ---------- */
const int N = 128;
uint16_t buf[N], plotBuf[N];
uint16_t trigLevel = 2048;
uint32_t lastDraw = 0;

/* ---------- Uhr ---------- */
uint32_t tStart=0, tClock=0;

/* ---------- Splash: Logo + vorberechnete Sinuskurve ---------- */
/* „Logo“: ganz einfache Pixel-Schrift (OSC) als Beispiellogo */
static const char *LOGO = "OSC";
/* Sinus-LUT über volle Displaybreite (160 px) – wird EINMAL beim Start berechnet */
uint8_t sineY[160]; // y-Positionen (0..127). Wir zeichnen in Höhe ~[24..95]

void buildSineLUT() {
  // Mittelpunkt und Amplitude passend zur sichtbaren Höhe
  const float mid = 64.0f;     // Displaymitte
  const float amp = 30.0f;     // Amplitude
  for (int x = 0; x < 160; x++) {
    float y = mid + amp * sinf(2.0f * PI * (float)x / 160.0f);
    // Begrenzen in 0..127 (ST7735 160x128, Rotation 3 verwenden wir)
    if (y < 0) y = 0;
    if (y > 127) y = 127;
    sineY[x] = (uint8_t)lroundf(y);
  }
}

/* ---------- Header + Uhr ---------- */
void drawHeader() {
  tft.fillRect(0, 0, 160, 16, ST77XX_BLACK);
  tft.setCursor(2, 2);
  tft.setTextColor(ST77XX_WHITE); 
  tft.setTextSize(1);
  tft.print("Mode: SCOPE");
}
void drawClock() {
  uint32_t sec=(millis()-tStart)/1000;
  int h=(sec/3600)%24, m=(sec/60)%60, s=sec%60;
  char txt[9]; sprintf(txt,"%02d:%02d:%02d",h,m,s);
  tft.fillRect(100,0,60,16,ST77XX_BLACK);
  tft.setCursor(102,2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.print(txt);
}

/* ---------- Splash-Page (5 s) ---------- */
void drawSplash() {
  tft.fillScreen(ST77XX_BLACK);

  // Trennlinie
  tft.drawLine(0, 64, 159, 64, ST77XX_DARKGREY);

  // „Logo“ mittig oben
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_YELLOW);
  int16_t x = 80 - (strlen(LOGO) * 6);  // grobe Zentrierung
  tft.setCursor(x, 20);
  tft.print(LOGO);

  // kleine Info
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(10, 44);
  tft.print("Mini Scope + OTA (ESP32-S2)");

  // vorberechnete Sinuskurve zeichnen
  for (int px = 0; px < 159; px++) {
    int y1 = sineY[px];
    int y2 = sineY[px+1];
    tft.drawLine(px, y1, px+1, y2, ST77XX_GREEN);
  }

  // unten ein paar Pixelmarker
  for (int i=0;i<160;i+=10) {
    tft.drawPixel(i, 66, ST77XX_DARKGREY);
    tft.drawPixel(i, 62, ST77XX_DARKGREY);
  }
}

/* ---------- Scope-Logik ---------- */
void scopeLoop(){
  // Samples holen
  for(int i=0;i<N;i++) buf[i]=analogRead(PIN_ADC);

  // einfache Flanken-Triggerung (steigende Flanke)
  int idx=-1;
  for(int i=1;i<N;i++) if(buf[i-1]<trigLevel && buf[i]>=trigLevel){ idx=i; break; }
  if(idx>0){
    int k=0; 
    for(int i=idx;i<N && k<N;i++) plotBuf[k++]=buf[i];
    for(int i=0;  i<idx && k<N;i++) plotBuf[k++]=buf[i];
  } else {
    memcpy(plotBuf, buf, sizeof(plotBuf));
  }

  // Zeichnen (max ~20 fps)
  if(millis()-lastDraw>50){
    lastDraw=millis();

    // Plotbereich
    tft.fillRect(0,16,160,80,ST77XX_BLACK);

    // Wellenform
    for(int x=0;x<min(N-1,159);x++){
      int y1=map(plotBuf[x],0,4095,95,16);
      int y2=map(plotBuf[x+1],0,4095,95,16);
      tft.drawLine(x,y1,x+1,y2,ST77XX_GREEN);
    }

    // Mittelwert -> geschätzte Vin
    uint32_t acc=0; for(int i=0;i<N;i++) acc+=plotBuf[i];
    float vIn = (acc/(float)N) * (ADC_REF/ADC_COUNTS) * DIV_GAIN;

    // Statuszeile
    tft.fillRect(0,96,160,32,ST77XX_BLACK);
    tft.setCursor(2,100); 
    tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
    tft.print("Trig "); tft.print((int)(trigLevel*100/4095)); tft.print("%");
    tft.setCursor(90,100); 
    tft.setTextColor(ST77XX_CYAN);
    tft.print("Vin "); tft.print(vIn,2); tft.print("V");
  }

  // Triggerlevel testweise per Seriell erhöhen
  if(Serial.available() && Serial.read()=='t')
    trigLevel=(trigLevel+256>4095)?1024:trigLevel+256;
}

/* ---------- OTA HTTP-Endpunkte ---------- */
void setupOTA() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    String ip = (WiFi.getMode()==WIFI_AP)? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    r->send(200,"text/html",
      String(F("<h3>ESP32-S2 Scope</h3>"))+
      F("<p>IP: ") + ip + F("</p>") +
      F("<form method='POST' action='/update' enctype='multipart/form-data'>") 
      F("<input type='file' name='firmware'> ")
      F("<input type='submit' value='Flash'>")
      F("</form>")
      F("<p>Seriell 't' = Trigger +</p>"));
  });

  // Upload-Ziel (Bin-Datei)
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *r){
      bool ok = !Update.hasError();
      r->send(200,"text/plain", ok ? "Update OK, rebooting..." : "Update FAILED");
      if (ok) { delay(300); ESP.restart(); }
    },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
      if (!index) { Update.begin(UPDATE_SIZE_UNKNOWN); }
      if (Update.write(data, len) != len) { Update.printError(Serial); }
      if (final) { if (!Update.end(true)) Update.printError(Serial); }
    }
  );

  server.begin();
}

/* ---------- Setup / Loop ---------- */
void setup(){
  pinMode(TFT_LED, OUTPUT); digitalWrite(TFT_LED, HIGH);
  Serial.begin(115200);
  analogReadResolution(12);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  // Splash vorbereiten & anzeigen (5 s)
  buildSineLUT();
  drawSplash();
  delay(5000);

  // Danach normale UI
  tft.fillScreen(ST77XX_BLACK);
  drawHeader();
  tStart=millis(); 
  drawClock();

  // WLAN / OTA
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0=millis();
  while(strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(200);

  if(WiFi.status()!=WL_CONNECTED){
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Scope_OTA","12345678");
  }

  setupOTA();
}

void loop(){
  if(millis()-tClock>1000){ tClock=millis(); drawClock(); }
  scopeLoop();
}
