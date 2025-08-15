/***** ESP32-S2 mini – Splash + Menü + Scope + Uhr + Einstellungen + Web-OTA (Browser) *****/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

/* ---------- Pins ---------- */
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13
#define PIN_ADC  2

#define BTN_NEXT 8
#define BTN_OK   9

/* ---------- Display ---------- */
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
#ifndef ST77XX_DARKGREY
  #define ST77XX_DARKGREY 0x7BEF
#endif
#ifndef ST77XX_NAVY
  #define ST77XX_NAVY 0x000F
#endif
#ifndef ST77XX_ORANGE
  #define ST77XX_ORANGE 0xFD20
#endif

/* ---------- WLAN ---------- */
const char* WIFI_SSID = "DEINE_SSID";
const char* WIFI_PASS = "DEIN_PASSWORT";
const char* AP_SSID   = "Scope_OTA";
const char* AP_PASS   = "12345678";

WebServer server(80);

/* ---------- ADC ---------- */
const float ADC_REF    = 3.3f;
const float ADC_COUNTS = 4095.0f;
const float DIV_GAIN   = 12.0f;

/* ---------- States ---------- */
enum State { SPLASH, MENU, MODE_SCOPE, MODE_CLOCK, MODE_SETTINGS, MODE_OTA, MODE_ABOUT };
State state = SPLASH;

/* ---------- Menü ---------- */
const char* MENU_ITEMS[] = { "Oszilloskop", "Uhr", "Einstellungen", "OTA Update", "Info" };
const uint8_t MENU_COUNT = sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0]);
int8_t menuIndex = 0;

/* ---------- Settings ---------- */
uint8_t  backlight = 200;
uint16_t trigLevel = 2048;

/* ---------- Daten ---------- */
uint32_t tStart = 0;
uint16_t scopeBuf[160];

/* ---------- Sinus-LUT ---------- */
const uint8_t SIN_W = 160;
uint8_t sinY[SIN_W];
void buildSinusLUT() {
  for (int x = 0; x < SIN_W; x++) {
    float a = (2.0f * PI) * (float)x / (float)SIN_W;
    float s = (sinf(a) * 0.45f + 0.5f);
    sinY[x] = (uint8_t)(24 + s * (103 - 24));
  }
}

/* ---------- Backlight ---------- */
void setBacklight(uint8_t v) {
  pinMode(TFT_LED, OUTPUT);
  analogWrite(TFT_LED, v);
}

/* ---------- WLAN ---------- */
void startWiFi() {
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (strlen(WIFI_SSID) && WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
  if (WiFi.status() != WL_CONNECTED) { WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS); }
}

/* ---------- Web-OTA ---------- */
const char* HTML_INDEX =
  "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<style>body{font-family:sans-serif;margin:20px}button,input{font-size:16px}"
  "#p{width:100%;height:10px;background:#eee;border-radius:6px;overflow:hidden}"
  "#b{height:100%;width:0;background:#2aa198;transition:width .2s}</style></head><body>"
  "<h3>ESP32-S2 Web-OTA</h3><p>WLAN-IP: %IP%</p>"
  "<form id='f' method='POST' action='/update' enctype='multipart/form-data'>"
  "<input type='file' name='firmware' required> <button>Flash</button></form>"
  "<div id='p'><div id='b'></div></div>"
  "<script>const f=document.getElementById('f');f.addEventListener('submit',e=>{e.preventDefault();"
  "let x=new XMLHttpRequest();x.upload.onprogress=e2=>{if(e2.lengthComputable)"
  "document.getElementById('b').style.width=(100*e2.loaded/e2.total)+'%';};"
  "x.onreadystatechange=()=>{if(x.readyState==4)alert(x.responseText)};"
  "x.open('POST','/update');x.send(new FormData(f));});</script></body></html>";

String pageWithIP() {
  IPAddress ip = (WiFi.getMode()==WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP();
  String s = HTML_INDEX; s.replace("%IP%", ip.toString()); return s;
}
void beginWebOTA() {
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", pageWithIP()); });
  server.on("/update", HTTP_POST,
    [](){ bool ok=!Update.hasError(); server.send(200,"text/plain", ok?"Update OK, reboot...":"Update FAILED");
          if(ok){ delay(300); ESP.restart(); } },
    [](){ HTTPUpload& up=server.upload();
          if(up.status==UPLOAD_FILE_START){ Update.begin(UPDATE_SIZE_UNKNOWN); }
          else if(up.status==UPLOAD_FILE_WRITE){ if(Update.write(up.buf, up.currentSize)!=up.currentSize) Update.printError(Serial); }
          else if(up.status==UPLOAD_FILE_END){ if(!Update.end(true)) Update.printError(Serial); } }
  );
  server.begin();
}

/* ---------- UI ---------- */
void header(const char* title) {
  tft.fillRect(0,0,160,16,ST77XX_NAVY);
  tft.setCursor(4,3); tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.print(title);
}
void showMenu() {
  header("Menue");
  tft.fillRect(0,16,160,112,ST77XX_BLACK);
  for (uint8_t i=0;i<MENU_COUNT;i++){
    int y=22+i*18;
    if(i==menuIndex){ tft.fillRect(4,y-2,152,14,ST77XX_DARKGREY); tft.setTextColor(ST77XX_BLACK); }
    else tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(8,y); tft.print(MENU_ITEMS[i]);
  }
  tft.setTextColor(ST77XX_YELLOW); tft.setCursor(4,112);
  tft.print("NEXT/OK oder w/s/e, b=zurueck");
}
void showSplash() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1); tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(10,6); tft.print("Robin's Oszilloskop");
  for(int x=0;x<SIN_W-1;x++) tft.drawLine(x, sinY[x], x+1, sinY[x+1], ST77XX_GREEN);
  tft.setTextColor(ST77XX_WHITE); tft.setCursor(10,20); tft.print("wird geladen...");
  int x0=10,y0=110,w=140,h=10; tft.drawRect(x0-1,y0-1,w+2,h+2,ST77XX_DARKGREY);
  uint32_t t0=millis();
  while(millis()-t0<5000){
    float p=(millis()-t0)/5000.0f; if(p>1)p=1;
    tft.fillRect(x0,y0,(int)(w*p),h,ST77XX_YELLOW);
    delay(20);
  }
}

/* ---------- Screens ---------- */
void drawClock() {
  header("Uhr"); tft.fillRect(0,16,160,112,ST77XX_BLACK);
}
void runClock() {
  static uint32_t last=0;
  if(millis()-last>=200){
    last=millis();
    uint32_t sec=(millis()-tStart)/1000;
    int h=(sec/3600)%24, m=(sec/60)%60, s=sec%60;
    char buf[16]; sprintf(buf,"%02d:%02d:%02d",h,m,s);
    tft.fillRect(0,40,160,40,ST77XX_BLACK);
    tft.setTextSize(2); tft.setTextColor(ST77XX_WHITE); tft.setCursor(20,48); tft.print(buf);
    tft.setTextSize(1);
  }
}

void drawScopeFrame() {
  header("Oszilloskop");
  tft.fillRect(0,16,160,112,ST77XX_BLACK);
  tft.drawFastHLine(0,64,160,ST77XX_DARKGREY);
  tft.drawFastVLine(0,16,112,ST77XX_DARKGREY);
  tft.setTextColor(ST77XX_YELLOW); tft.setCursor(4,18); tft.print("Trig:");
  tft.setCursor(34,18); tft.print((int)(trigLevel*100/4095)); tft.print("% (t=+  b=zurueck)");
}
void runScope() {
  for(int i=0;i<160;i++) scopeBuf[i]=analogRead(PIN_ADC);
  tft.fillRect(1,17,158,110,ST77XX_BLACK);
  int yTrig = map(trigLevel,0,4095,127,16);
  tft.drawFastHLine(1,yTrig,158,ST77XX_RED);
  for(int x=0;x<159;x++){
    int y1=map(scopeBuf[x],0,4095,127,16);
    int y2=map(scopeBuf[x+1],0,4095,127,16);
    tft.drawLine(x+1,y1,x+2,y2,ST77XX_GREEN);
  }
  uint32_t acc=0; for(int i=0;i<160;i++) acc+=scopeBuf[i];
  float vIn=(acc/160.0f)*(ADC_REF/ADC_COUNTS)*DIV_GAIN;
  tft.setTextColor(ST77XX_CYAN); tft.setCursor(4,110);
  tft.print("Vin: "); tft.print(vIn,2); tft.print(" V");
}

/* ---------- Buttons ---------- */
bool btnNextEdge=false, btnOkEdge=false;
void pollButtons(){
  static uint32_t last=0; if(millis()-last<8) return; last=millis();
  static bool ln=true, lo=true;
  bool n=digitalRead(BTN_NEXT); bool o=digitalRead(BTN_OK);
  if(n==LOW && ln==HIGH) btnNextEdge=true;
  if(o==LOW && lo==HIGH) btnOkEdge=true;
  ln=n; lo=o;
}

/* ---------- Setup / Loop ---------- */
void setup() {
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_OK,   INPUT_PULLUP);
  Serial.begin(115200);
  tft.initR(INITR_BLACKTAB); 
  tft.setRotation(1); // 180° gedreht
  tft.fillScreen(ST77XX_BLACK);
  analogReadResolution(12);
  setBacklight(backlight);
  buildSinusLUT();
  showSplash();
  startWiFi();
  beginWebOTA();
  tStart=millis();
  state=MENU;
  showMenu();
}

void loop() {
  server.handleClient();
  pollButtons();
  switch(state){
    case MENU:
      if(btnNextEdge){ btnNextEdge=false; menuIndex=(menuIndex+1)%MENU_COUNT; showMenu(); }
      if(btnOkEdge){
        btnOkEdge=false;
        if(menuIndex==0){ state=MODE_SCOPE; drawScopeFrame(); }
        if(menuIndex==1){ state=MODE_CLOCK; drawClock(); }
        if(menuIndex==3){ state=MODE_OTA; }
      }
      break;
    case MODE_SCOPE: runScope(); break;
    case MODE_CLOCK: runClock(); break;
    default: break;
  }
}
