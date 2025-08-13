/***** ESP32-S2 mini: Scope + Uhr + OTA (AsyncWebServer, ohne ElegantOTA) *****/
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

const int PIN_ADC = 2;

const float ADC_REF    = 3.3f;
const float ADC_COUNTS = 4095.0f;
const float DIV_GAIN   = (110000.0f + 10000.0f) / 10000.0f; // 12.0

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT";  // leer lassen => AP-Fallback
AsyncWebServer server(80);

const int N = 128;
uint16_t buf[N], plotBuf[N];
uint16_t trigLevel = 2048;
uint32_t lastDraw = 0;

uint32_t tStart=0, tClock=0;

void drawHeader() {
  tft.fillRect(0,0,160,16,ST77XX_BLACK);
  tft.setCursor(2,2);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  tft.print("Mode: SCOPE");
}
void drawClock() {
  uint32_t sec=(millis()-tStart)/1000;
  int h=(sec/3600)%24, m=(sec/60)%60, s=sec%60;
  char txt[9]; sprintf(txt,"%02d:%02d:%02d",h,m,s);
  tft.fillRect(100,0,60,16,ST77XX_BLACK);
  tft.setCursor(102,2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); tft.setTextSize(1);
  tft.print(txt);
}

void scopeLoop(){
  for(int i=0;i<N;i++) buf[i]=analogRead(PIN_ADC);

  int idx=-1;
  for(int i=1;i<N;i++) if(buf[i-1]<trigLevel && buf[i]>=trigLevel){ idx=i; break; }
  if(idx>0){
    int k=0; for(int i=idx;i<N && k<N;i++) plotBuf[k++]=buf[i];
             for(int i=0;  i<idx && k<N;i++) plotBuf[k++]=buf[i];
  } else memcpy(plotBuf, buf, sizeof(plotBuf));

  if(millis()-lastDraw>50){
    lastDraw=millis();
    tft.fillRect(0,16,160,80,ST77XX_BLACK);
    for(int x=0;x<min(N-1,159);x++){
      int y1=map(plotBuf[x],0,4095,95,16);
      int y2=map(plotBuf[x+1],0,4095,95,16);
      tft.drawLine(x,y1,x+1,y2,ST77XX_GREEN);
    }
    uint32_t acc=0; for(int i=0;i<N;i++) acc+=plotBuf[i];
    float vIn = (acc/(float)N) * (ADC_REF/ADC_COUNTS) * DIV_GAIN;

    tft.fillRect(0,96,160,32,ST77XX_BLACK);
    tft.setCursor(2,100); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
    tft.print("Trig "); tft.print((int)(trigLevel*100/4095)); tft.print("%");
    tft.setCursor(90,100); tft.setTextColor(ST77XX_CYAN);
    tft.print("Vin "); tft.print(vIn,2); tft.print("V");
  }

  if(Serial.available() && Serial.read()=='t')
    trigLevel=(trigLevel+256>4095)?1024:trigLevel+256;
}

void setup(){
  pinMode(TFT_LED, OUTPUT); digitalWrite(TFT_LED, HIGH);
  Serial.begin(115200);
  analogReadResolution(12);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);
  drawHeader();
  tStart=millis(); drawClock();

  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0=millis();
  while(strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(200);
  if(WiFi.status()!=WL_CONNECTED){ WiFi.mode(WIFI_AP); WiFi.softAP("Scope_OTA","12345678"); }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    String ip = (WiFi.getMode()==WIFI_AP)? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    r->send(200,"text/html",
      "<h3>ESP32-S2 Scope</h3>"
      "<p>IP: "+ip+"</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware'>"
      "<input type='submit' value='Flash'>"
      "</form>"
      "<p>Seriell 't' = Triggerlevel +</p>");
  });

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

void loop(){
  if(millis()-tClock>1000){ tClock=millis(); drawClock(); }
  scopeLoop();
}
