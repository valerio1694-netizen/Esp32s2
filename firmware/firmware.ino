/*
  ESP32-S2 MASTER — MQTT + OTA + 2 Buttons + B-Empfang (FW v1.3.0)
  - SoftAP (OTA):      SSID ESP2_MASTER / PW flashme123
  - STA (WLAN):        SSID "Peng" / PW "Keineahnung123"
  - MQTT:              192.168.178.65:1883, User "firstclass55555", PW "Zehn+551996"
  - Publish:           esp2panel/online (LWT), esp2panel/test ("hallo" beim Boot),
                       esp2panel/event → {"src":"A","btn":"L|R","type":"short|double|long"}
  - Subscribe:         esp2panel/event (nur Anzeige von SLAVE-Events mit src:"B")
  - TFT ST7735 1.8":   CS=5, DC=7, RST=6, SCK=12, MOSI=11; Backlight PWM: GPIO13
  - Buttons:           GPIO8 = L, GPIO9 = R (gegen GND, Pullup)
  - Short erst nach Double-Timeout; Long sofort. Kein Auto-Repeat.
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
static const char* FW_VERSION = "1.3.0";

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

// ---------- Buttons ----------
static const int BTN_L_PIN = 8;  // links, gegen GND
static const int BTN_R_PIN = 9;  // rechts, gegen GND

// Zeiten (ms)
static const uint32_t BTN_DEBOUNCE_MS   = 30;
static const uint32_t BTN_SHORT_MAX_MS  = 300;
static const uint32_t BTN_LONG_MIN_MS   = 700;
static const uint32_t BTN_DBL_WIN_MS    = 250;

// Event-Codes
static const uint8_t EV_NONE=0, EV_SHORT=1, EV_DOUBLE=2, EV_LONG=3;

// Zustand Button L
static bool     L_last=true,  L_state=false,  L_pressed=false,  L_pending=false;
static uint32_t L_tchg=0,     L_tpress=0,     L_deadline=0;

// Zustand Button R
static bool     R_last=true,  R_state=false,  R_pressed=false,  R_pending=false;
static uint32_t R_tchg=0,     R_tpress=0,     R_deadline=0;

// ---------- OTA HTTP ----------
WebServer server(80);
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>MASTER OTA</title><style>
body{font-family:system-ui;margin:20px} .card{max-width:520px;padding:16px;border:1px solid #ccc;border-radius:12px}
progress{width:100%;height:16px}
</style></head><body><div class=card>
<h2>MASTER OTA (FW v1.3.0)</h2>
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

String lastEvent = "—";      // zeigt eigenes letztes Event
String lastPeer  = "—";      // zeigt letztes Event vom SLAVE (src:"B")

static void drawStatus(const String& wifi, const String& mq){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.setCursor(4,6);  tft.print("ESP2 MASTER");
  tft.setTextSize(1);
  tft.setCursor(4,26); tft.setTextColor(ST77XX_CYAN); tft.print("FW: "); tft.setTextColor(ST77XX_WHITE); tft.print(FW_VERSION);
  tft.setCursor(4,38); tft.setTextColor(ST77XX_CYAN); tft.print("WiFi: "); tft.setTextColor(ST77XX_WHITE); tft.print(wifi);
  tft.setCursor(4,50); tft.setTextColor(ST77XX_CYAN); tft.print("MQTT: "); tft.setTextColor(ST77XX_WHITE); tft.print(mq);

  // eigene Events
  tft.fillRect(0, 64, TFT_W, 16, ST77XX_DARKGREY);
  tft.setCursor(4, 68); tft.setTextColor(ST77XX_WHITE); tft.print("A: "); tft.print(lastEvent);

  // Peer-Events (vom SLAVE)
  tft.fillRect(0, 84, TFT_W, 16, ST77XX_DARKGREY);
  tft.setCursor(4, 88); tft.setTextColor(ST77XX_WHITE); tft.print("B: "); tft.print(lastPeer);

  // Fußzeile
  tft.fillRect(0, TFT_H-16, TFT_W, 16, ST77XX_DARKGREY);
  tft.setCursor(4, TFT_H-12); tft.setTextColor(ST77XX_WHITE); tft.print("BL: "); tft.print((int)bl_level);
}

static void publishEvent(const char* btn, const char* type){
  char payload[64];
  snprintf(payload, sizeof(payload), "{\"src\":\"A\",\"btn\":\"%s\",\"type\":\"%s\"}", btn, type);
  mqtt.publish(TOP_EVENT, payload, true);
  lastEvent = payload;
  drawStatus(WiFi.localIP().toString(), mqtt.connected()?"OK":"...");
}

// sehr einfache Parser-Helfer (wir vermeiden JSON-Libs hier bewusst)
static bool contains(const String& s, const char* pat){ return s.indexOf(pat) >= 0; }
static String extractValue(const String& s, const char* key){
  // erwartet ... "key":"VALUE" ...
  String mark="\""; mark += key; mark += "\":\"";
  int p = s.indexOf(mark);
  if(p<0) return "";
  p += mark.length();
  int q = s.indexOf("\"", p);
  if(q<0) return "";
  return s.substring(p, q);
}

static void mqttCallback(char* topic, byte* payload, unsigned int len){
  String t = topic;
  if(t != TOP_EVENT) return; // nur Events interessieren

  // Payload in String wandeln (klein, <64B)
  String msg; msg.reserve(len);
  for(unsigned int i=0;i<len;i++) msg += (char)payload[i];

  // nur auf SLAVE reagieren
  if(!contains(msg, "\"src\":\"B\"")) return;

  // btn/type grob herausziehen
  String btn  = extractValue(msg, "btn");   // "L" oder "R"
  String type = extractValue(msg, "type");  // "short" | "double" | "long"
  if(btn.length()==0 || type.length()==0) return;

  lastPeer = btn + " " + type;   // z.B. "L double"
  drawStatus(WiFi.localIP().toString(), mqtt.connected()?"OK":"...");
}

static void mqttConnect(){
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  while(!mqtt.connected()){
    String cid = "esp2master-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    bool ok = MQTT_USER && *MQTT_USER ?
      mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASSW, TOP_LWT, 0, true, "0") :
      mqtt.connect(cid.c_str(), TOP_LWT, 0, true, "0");
    if(ok){
      mqtt.publish(TOP_LWT, "1", true);
      mqtt.publish(TOP_TEST, "hallo");
      mqtt.subscribe(TOP_EVENT);  // <<< jetzt hören wir auf Events (A & B)
      break;
    }
    delay(1000);
  }
}

// ---------- Button-Helfer (Builtin-Typen in Signaturen) ----------
static void btnInit(int pin, bool &last, bool &state, bool &pressed, bool &pending, uint32_t &tchg, uint32_t &tpress, uint32_t &deadline){
  pinMode(pin, INPUT_PULLUP);
  last = digitalRead(pin);
  state = (last==LOW);
  tchg = millis();
  pressed=false; tpress=0;
  pending=false; deadline=0;
}

static uint8_t btnPoll(int pin, bool &last, bool &state, bool &pressed, bool &pending, uint32_t &tchg, uint32_t &tpress, uint32_t &deadline){
  uint8_t ev=EV_NONE;
  bool raw=digitalRead(pin);
  if(raw!=last){ last=raw; tchg=millis(); }
  if(millis()-tchg < BTN_DEBOUNCE_MS) return EV_NONE;

  bool ns = (raw==LOW);
  if(ns!=state){
    state=ns;
    if(state){ // Press
      pressed=true; tpress=millis();
    }else{     // Release
      if(pressed){
        uint32_t dt = millis()-tpress;
        if(dt >= BTN_LONG_MIN_MS){
          pending=false;
          ev = EV_LONG;
        }else if(dt < BTN_SHORT_MAX_MS){
          if(pending){
            pending=false;
            ev = EV_DOUBLE;
          }else{
            pending = true;
            deadline = millis() + BTN_DBL_WIN_MS;
          }
        }
      }
      pressed=false;
    }
  }
  return ev;
}

static uint8_t btnCheckPending(bool &pending, uint32_t &deadline){
  if(pending && (int32_t)(millis() - deadline) >= 0){
    pending=false;
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

  // Buttons initialisieren
  btnInit(BTN_L_PIN, L_last, L_state, L_pressed, L_pending, L_tchg, L_tpress, L_deadline);
  btnInit(BTN_R_PIN, R_last, R_state, R_pressed, R_pending, R_tchg, R_tpress, R_deadline);

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

  // Button L
  uint8_t eL = btnPoll(BTN_L_PIN, L_last, L_state, L_pressed, L_pending, L_tchg, L_tpress, L_deadline);
  if(eL==EV_DOUBLE) publishEvent("L","double");
  else if(eL==EV_LONG) publishEvent("L","long");
  uint8_t psL = btnCheckPending(L_pending, L_deadline);
  if(psL==EV_SHORT) publishEvent("L","short");

  // Button R
  uint8_t eR = btnPoll(BTN_R_PIN, R_last, R_state, R_pressed, R_pending, R_tchg, R_tpress, R_deadline);
  if(eR==EV_DOUBLE) publishEvent("R","double");
  else if(eR==EV_LONG) publishEvent("R","long");
  uint8_t psR = btnCheckPending(R_pending, R_deadline);
  if(psR==EV_SHORT) publishEvent("R","short");
}
