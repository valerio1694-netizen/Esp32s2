/*
  ESP32-S2 MASTER — MQTT + OTA + 1 Button (FW v1.1.3)
  - SoftAP (OTA):      SSID ESP2_MASTER / PW flashme123
  - STA (WLAN):        SSID "Peng" / PW "Keineahnung123"
  - MQTT:              192.168.178.65:1883, User "firstclass55555", PW "Zehn+551996"
  - Publish:           esp2panel/online (LWT), esp2panel/test ("hallo" beim Boot),
                       esp2panel/event → {"src":"A","btn":"L","type":"short|double|long"}
  - TFT ST7735 1.8":   CS=5, DC=7, RST=6, SCK=12, MOSI=11; Backlight PWM: GPIO13
  - Button:            GPIO8 gegen GND, Pullup, kurz/doppelt/lang, **short erst nach Double-Timeout**
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <PubSubClient.h>

// ---------- WLAN ----------
static const char* WIFI_SSID   = "Peng";
static const char* WIFI_PASS   = "Keineahnung123";

// ---------- MQTT ----------
static const char* MQTT_HOST   = "192.168.178.65";
static const uint16_t MQTT_PORT= 1883;
static const char* MQTT_USER   = "firstclass55555";
static const char* MQTT_PASSW  = "Zehn+551996";

// ---------- OTA (AP) ----------
static const char* AP_SSID = "ESP2_MASTER";
static const char* AP_PASS = "flashme123";

// ---------- Version ----------
static const char* FW_VERSION = "1.1.3";

// ---------- TFT / Pins ----------
static const int TFT_CS=5, TFT_DC=7, TFT_RST=6, TFT_SCK=12, TFT_MOSI=11;
static const int TFT_BL_PIN=13;
#define CALIB_TAB INITR_BLACKTAB
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
#ifndef ST77XX_DARKGREY
  #define ST77XX_DARKGREY 0x7BEF
#endif
static const int TFT_W=160, TFT_H=128;

// ---------- Backlight ----------
static const int BL_CH=0, BL_FREQ=5000, BL_RES=8;
static uint8_t bl_level=200;
static inline void setBL(uint8_t v){ bl_level=v; ledcWrite(BL_CH,v); }

// ---------- Button (GPIO8) ----------
static const int BTN1_PIN = 8; // gegen GND, interner Pullup

// Zeiten (ms)
static const uint32_t BTN_DEBOUNCE_MS   = 30;
static const uint32_t BTN_SHORT_MAX_MS  = 300;
static const uint32_t BTN_LONG_MIN_MS   = 700;
static const uint32_t BTN_DBL_WIN_MS    = 250;

enum BtnEvent : uint8_t { EV_NONE, EV_SHORT, EV_DOUBLE, EV_LONG };

struct Btn {
  int pin; bool pullup;

  // Entprellen
  bool state; bool last; uint32_t tchg;

  // Press/Release
  bool pressed; uint32_t tpress;

  // Short-Decision
  bool pendingShort; uint32_t pendingDeadline;
};

static Btn btn1{BTN1_PIN, true, false, true, 0, false, 0, false, 0};

// ---------- OTA HTTP ----------
WebServer server(80);
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>MASTER OTA</title><style>
body{font-family:system-ui;margin:20px} .card{max-width:520px;padding:16px;border:1px solid #ccc;border-radius:12px}
progress{width:100%;height:16px}
</style></head><body><div class=card>
<h2>MASTER OTA (FW v1.1.3)</h2>
<input id=f type=file accept=".bin,application/octet-stream"><br><br>
<button id=b>Upload</button><br><br>
<progress id=p max=100 value=0 hidden></progress>
<div id=m></div>
<script>
b.onclick=()=>{
 if(!f.files.length){m.textContent="Datei fehlt";return;}
 let x=new XMLHttpRequest(); p.hidden=false; p.value=0; m.textContent="Lade hoch...";
 x.upload.onprogress=e=>{if(e.lengthComputable)p.value=Math.round(e.loaded/e.total*100);};
 x.onload=()=>{m.textContent=(x.status==200?"OK – Reboot":"Fehler: "+x.responseText); if(x.status==200)setTimeout(()=>location.reload(),5000);};
 let fd=new FormData(); fd.append("firmware",f.files[0]); x.open("POST","/update",true); x.send(fd);
};
</script></div></body></html>
)HTML";
static void handleRoot(){ server.send_P(200,"text/html; charset=utf-8",INDEX_HTML); }
static void handleUpdate(){
  HTTPUpload& u=server.upload(); static bool ok=false;
  if(u.status==UPLOAD_FILE_START){ ok=Update.begin(UPDATE_SIZE_UNKNOWN); }
  else if(u.status==UPLOAD_FILE_WRITE){ if(ok) Update.write(u.buf,u.currentSize); }
  else if(u.status==UPLOAD_FILE_END){
    if(ok && Update.end(true)){ server.send(200,"text/plain","OK"); delay(300); ESP.restart(); }
    else server.send(500,"text/plain","FAIL");
  }
}
static void handleNotFound(){ server.send(404,"text/plain","Not found"); }

// ---------- MQTT ----------
WiFiClient net;
PubSubClient mqtt(net);
static const char* TOP_LWT   = "esp2panel/online";
static const char* TOP_TEST  = "esp2panel/test";
static const char* TOP_EVENT = "esp2panel/event";

String lastEvent = "—";

static void drawStatus(const String& wifi, const String& mq){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.setCursor(4,6);  tft.print("ESP2 MASTER");
  tft.setTextSize(1);
  tft.setCursor(4,30); tft.setTextColor(ST77XX_CYAN); tft.print("FW: "); tft.setTextColor(ST77XX_WHITE); tft.print(FW_VERSION);
  tft.setCursor(4,42); tft.setTextColor(ST77XX_CYAN); tft.print("WiFi: "); tft.setTextColor(ST77XX_WHITE); tft.print(wifi);
  tft.setCursor(4,54); tft.setTextColor(ST77XX_CYAN); tft.print("MQTT: "); tft.setTextColor(ST77XX_WHITE); tft.print(mq);
  tft.setCursor(4,66); tft.setTextColor(ST77XX_CYAN); tft.print("BL: "); tft.setTextColor(ST77XX_WHITE); tft.print((int)bl_level);
  tft.fillRect(0, TFT_H-22, TFT_W, 22, ST77XX_DARKGREY);
  tft.setCursor(4, TFT_H-18); tft.setTextColor(ST77XX_WHITE); tft.print("Event: "); tft.print(lastEvent);
}

static void mqttConnect(){
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  while(!mqtt.connected()){
    String cid = "esp2master-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    bool ok = MQTT_USER && *MQTT_USER ?
      mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASSW, TOP_LWT, 0, true, "0") :
      mqtt.connect(cid.c_str(), TOP_LWT, 0, true, "0");
    if(ok){
      mqtt.publish(TOP_LWT, "1", true);
      mqtt.publish(TOP_TEST, "hallo");
      break;
    }
    delay(1000);
  }
}

static void publishEvent(const char* btn, const char* type){
  char payload[64];
  snprintf(payload, sizeof(payload), "{\"src\":\"A\",\"btn\":\"%s\",\"type\":\"%s\"}", btn, type);
  mqtt.publish(TOP_EVENT, payload, true);
  lastEvent = payload;
  drawStatus(WiFi.localIP().toString(), mqtt.connected()?"OK":"...");
}

// ---------- Button-Helpers ----------
static void initBtn(Btn& b){
  pinMode(b.pin, INPUT_PULLUP);
  b.last = digitalRead(b.pin);
  b.state = (b.last==LOW); // Pullup → LOW = gedrückt
  b.tchg=millis(); b.pressed=false; b.tpress=0;
  b.pendingShort=false; b.pendingDeadline=0;
}

// liefert EV_DOUBLE oder EV_LONG sofort; EV_SHORT wird NICHT sofort geliefert,
// sondern nur über checkPendingShort(), falls kein 2. Klick kommt.
static BtnEvent pollBtn(Btn& b){
  BtnEvent ev=EV_NONE;
  bool raw=digitalRead(b.pin);
  if(raw!=b.last){ b.last=raw; b.tchg=millis(); }
  if(millis()-b.tchg < BTN_DEBOUNCE_MS) return EV_NONE;

  bool ns = (raw==LOW);
  if(ns!=b.state){
    b.state=ns;
    if(b.state){ // Press
      b.pressed=true; b.tpress=millis();
      // Falls Short noch aussteht, und direkt wieder gedrückt wird: das wird dann zum Double in Release
    }else{       // Release
      if(b.pressed){
        uint32_t dt = millis()-b.tpress;
        if(dt >= BTN_LONG_MIN_MS){
          // langer Druck → sofort LONG, ausstehendes Short verwerfen
          b.pendingShort=false;
          ev = EV_LONG;
        }else if(dt < BTN_SHORT_MAX_MS){
          if(b.pendingShort){
            // 2. kurzer Klick in Zeit → DOUBLE
            b.pendingShort=false;
            ev = EV_DOUBLE;
          }else{
            // 1. kurzer Klick → Short vormerken (nicht sofort senden!)
            b.pendingShort = true;
            b.pendingDeadline = millis() + BTN_DBL_WIN_MS;
          }
        }
      }
      b.pressed=false;
    }
  }
  return ev;
}

// Prüft, ob ein vorgemerktes Short nun „fällig“ ist
static BtnEvent checkPendingShort(Btn& b){
  if(b.pendingShort && (int32_t)(millis() - b.pendingDeadline) >= 0){
    b.pendingShort=false;
    return EV_SHORT;
  }
  return EV_NONE;
}

// ---------- Setup ----------
void setup(){
  Serial.begin(115200);

  // Backlight
  ledcSetup(BL_CH, BL_FREQ, BL_RES);
  ledcAttachPin(TFT_BL_PIN, BL_CH);
  setBL(bl_level);

  // TFT
  tft.initR(CALIB_TAB);
  tft.setRotation(1);
  drawStatus("...", "...");

  // Button
  initBtn(btn1);

  // Dual Mode: AP (OTA) + STA (MQTT)
  WiFi.mode(WIFI_MODE_APSTA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // auf STA warten
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000){ delay(250); }
  drawStatus(WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString() : "no WiFi", "...");

  // OTA HTTP
  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST, [](){}, handleUpdate);
  server.onNotFound(handleNotFound);
  server.begin();

  // MQTT
  if(WiFi.status()==WL_CONNECTED) mqttConnect();
  drawStatus(WiFi.localIP().toString(), mqtt.connected()?"OK":"...");
}

// ---------- Loop ----------
void loop(){
  server.handleClient();

  if(WiFi.status()==WL_CONNECTED){
    if(!mqtt.connected()) mqttConnect();
    mqtt.loop();
  }

  // Button: zuerst „harte“ Events (DOUBLE, LONG) sofort,
  // dann ggf. fälliges SHORT nach Ablauf des Fensters.
  BtnEvent e = pollBtn(btn1);
  if(e==EV_DOUBLE) publishEvent("L","double");
  else if(e==EV_LONG) publishEvent("L","long");

  BtnEvent ps = checkPendingShort(btn1);
  if(ps==EV_SHORT) publishEvent("L","short");
}
