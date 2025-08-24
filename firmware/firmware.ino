/*  ESP2 MASTER Panel – Layout-Update
    FW: 1.5.0  (nur Anzeige überarbeitet, Logik/Topics unverändert)
    - Großer Titel + kleiner Artist
    - Farbiger STATE + Prozent + Volumen-Balken
    - Nur betroffene Bereiche neu zeichnen (weniger Flackern)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// ===== Version / Rolle =====
static const char* FW_VERSION = "1.5.0";   // MASTER

// ===== Pins (deine Belegung) =====
#define TFT_CS    5
#define TFT_DC    7
#define TFT_RST   6
#define TFT_SCLK 12
#define TFT_MOSI 11
#define BTN_L     8
#define BTN_R     9
#define PIN_BL   13   // Backlight (PWM)

// ===== Display =====
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
#define TFT_W 128
#define TFT_H 160

#define COL_BG    ST77XX_BLACK
#define COL_TXT   ST77XX_WHITE
#define COL_SUB   0xC618
#define COL_OK    ST77XX_CYAN
#define COL_PLAY  ST77XX_GREEN
#define COL_PAUSE ST77XX_YELLOW
#define COL_ERR   ST77XX_RED
#define COL_BAR   ST77XX_CYAN
#define COL_BOX   0x18C3

// ===== WLAN / MQTT (wie besprochen) =====
const char* WIFI_SSID = "Peng";
const char* WIFI_PSK  = "Keineahnung123";

const char* MQTT_HOST = "core-mosquitto";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "firstclass55555";
const char* MQTT_PASS = "Zehn+551996";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ===== OTA Webserver (simpler HTTP Upload) =====
WebServer server(80);

// ===== Button-Engine (unverändert vom Verhalten) =====
struct Btn {
  uint8_t pin;
  bool last = true;            // Pullup → HIGH = released
  uint32_t lastChange = 0;
  uint32_t lastShortRel = 0;
  bool pendingShort = false;
};
enum BtnEvent { EV_NONE, EV_SHORT, EV_LONG, EV_DOUBLE };

static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONG_MIN_MS = 700;
static const uint32_t SHORT_MAX_MS = 300;
static const uint32_t DBL_WIN_MS  = 250;

Btn btnL{BTN_L}, btnR{BTN_R};

// ===== Anzeige-Status =====
String gTitle  = "";
String gArtist = "";
String gState  = "IDLE";
int    gVol    = 0;

String pTitle, pArtist, pState;
int    pVol = -1;
bool   headerDrawn = false;

// ===== Prototypen =====
void drawHeader();
void drawFooter();
void drawTitleArtist(bool force=false);
void drawStateVolume(bool force=false);
void clearBox(int x,int y,int w,int h,uint16_t col=COL_BG);
void mqttReconnect();
void mqttCallback(char* topic, byte* payload, unsigned int len);
void publishEvent(const char* btn, const char* type);

// ===== OTA Handler (einfach) =====
void handleRoot(){
  server.send(200,"text/plain",String("ESP2 MASTER FW ")+FW_VERSION);
}
void handleUpdate(){
  HTTPUpload &up = server.upload();
  if (server.uri() != "/update") return;
  if (up.status == UPLOAD_FILE_START) {
    Update.begin();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    Update.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
  server.sendHeader("Connection","close");
  server.send(200,"text/plain","OK");
  delay(200);
  ESP.restart();
}

// ===== Button-Helfer =====
static bool btnDebounce(Btn& b, bool now){
  uint32_t t = millis();
  if (now != b.last){
    if (t - b.lastChange >= DEBOUNCE_MS){
      b.last = now;
      b.lastChange = t;
      return true;
    }
  }
  return false;
}

static BtnEvent pollBtn(Btn& b){
  bool level = digitalRead(b.pin); // HIGH = released
  BtnEvent ev = EV_NONE;

  if (btnDebounce(b, level)){
    uint32_t t = millis();
    if (!level){
      // pressed
    } else {
      // released
      uint32_t dur = t - b.lastChange;
      if (dur <= SHORT_MAX_MS){
        if (b.pendingShort && (t - b.lastShortRel) <= DBL_WIN_MS){
          ev = EV_DOUBLE;
          b.pendingShort = false;
          b.lastShortRel = 0;
        } else {
          b.pendingShort = true;
          b.lastShortRel = t;
        }
      } else if (dur >= LONG_MIN_MS){
        ev = EV_LONG;
        b.pendingShort = false;
        b.lastShortRel = 0;
      }
    }
  }
  if (b.pendingShort && (millis() - b.lastShortRel) > DBL_WIN_MS){
    ev = EV_SHORT;
    b.pendingShort = false;
    b.lastShortRel = 0;
  }
  return ev;
}

// ===== Layout =====
void drawHeader(){
  if (headerDrawn) return;
  tft.fillScreen(COL_BG);
  clearBox(0,0,TFT_W,20,COL_BG);

  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setCursor(2,3);
  tft.setTextColor(COL_OK); tft.print("FW:");
  tft.setTextColor(COL_TXT); tft.print(" "); tft.print(FW_VERSION);

  tft.setCursor(72,3);
  tft.setTextColor(COL_OK); tft.print("MQTT:");
  tft.setTextColor(COL_TXT); tft.print(" OK");

  headerDrawn = true;
}

void drawFooter(){
  clearBox(0,130,TFT_W,30,COL_BG);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(COL_SUB);
  tft.setCursor(2,142);
  tft.print("ESP2 MASTER  IP: ");
  tft.print(WiFi.localIP());
}

void drawTitleArtist(bool force){
  if (!force && gTitle==pTitle && gArtist==pArtist) return;

  clearBox(0,24,TFT_W,48,COL_BG);

  // Title groß
  tft.setTextWrap(false);
  tft.setTextColor(COL_TXT);
  tft.setTextSize(2);
  tft.setCursor(2,28);
  tft.print(gTitle);

  // Artist klein
  tft.setTextColor(COL_SUB);
  tft.setTextSize(1);
  tft.setCursor(2,56);
  tft.print(gArtist);

  pTitle = gTitle;
  pArtist = gArtist;
}

void drawStateVolume(bool force){
  if (!force && gState==pState && gVol==pVol) return;

  clearBox(0,80,TFT_W,40,COL_BG);

  // State farbig
  uint16_t col = COL_SUB;
  if (gState=="PLAYING") col = COL_PLAY;
  else if (gState=="PAUSED") col = COL_PAUSE;
  else if (gState=="STOPPED") col = COL_ERR;

  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(col);
  tft.setCursor(2,84);
  tft.print(gState);

  // Prozent rechts
  String vs = String(gVol) + "%";
  int16_t x,y; uint16_t w,h;
  tft.getTextBounds(vs.c_str(), 0,0, &x,&y,&w,&h);
  tft.setTextColor(COL_TXT);
  tft.setCursor(TFT_W - w - 4, 84);
  tft.print(vs);

  // Vol-Balken
  int barX=4, barY=104, barW=TFT_W-8, barH=6;
  tft.drawRect(barX-1, barY-1, barW+2, barH+2, COL_SUB);
  int fillW = map(gVol, 0, 100, 0, barW);
  tft.fillRect(barX, barY, barW, barH, COL_BOX);
  tft.fillRect(barX, barY, fillW, barH, COL_BAR);

  pState = gState;
  pVol   = gVol;
}

void clearBox(int x,int y,int w,int h,uint16_t col){
  tft.fillRect(x,y,w,h,col);
}

// ===== MQTT =====
void mqttCallback(char* topic, byte* payload, unsigned int len){
  String t = String(topic);
  String msg; msg.reserve(len+1);
  for (unsigned int i=0;i<len;i++) msg += (char)payload[i];

  // A/line/1  -> "Artist - Title"
  // A/line/2  -> "STATE XX%"
  if (t == "esp2panel/A/line/1"){
    int sep = msg.indexOf(" - ");
    if (sep >= 0){
      gArtist = msg.substring(0, sep);
      gTitle  = msg.substring(sep+3);
    } else {
      gArtist = "";
      gTitle  = msg;
    }
    drawTitleArtist(false);
  }
  else if (t == "esp2panel/A/line/2"){
    String s = msg; s.trim();
    int sp = s.indexOf(' ');
    if (sp > 0){
      gState = s.substring(0, sp);
      String rest = s.substring(sp+1);
      rest.replace("%","");
      gVol = constrain(rest.toInt(), 0, 100);
    } else {
      gState = s;
    }
    drawStateVolume(false);
  }
}

void mqttReconnect(){
  while (!mqtt.connected()){
    String cid = String("esp2panel-A-") + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)){
      mqtt.subscribe("esp2panel/A/line/1");
      mqtt.subscribe("esp2panel/A/line/2");
      headerDrawn = false; drawHeader();   // zeigt "MQTT: OK"
    } else {
      delay(1000);
    }
  }
}

// ===== Event Publish =====
void publishEvent(const char* btn, const char* type){
  String payload = String("{\"src\":\"A\",\"btn\":\"") + btn + "\",\"type\":\"" + type + "\"}";
  mqtt.publish("esp2panel/event", payload.c_str());

  // kurze Bestätigung im unteren Bereich
  clearBox(0,120,TFT_W,12,COL_BG);
  tft.setTextSize(1);
  tft.setTextColor(COL_OK);
  tft.setCursor(2,120);
  tft.print("Event: A ");
  tft.print(btn);
  tft.print(" ");
  tft.print(type);
}

// ===== Setup / Loop =====
void setup(){
  pinMode(BTN_L, INPUT_PULLUP);
  pinMode(BTN_R, INPUT_PULLUP);

  // Backlight PWM
  ledcAttachPin(PIN_BL, 0);
  ledcSetup(0, 1000, 8);
  ledcWrite(0, 200);    // 0..255

  // Display
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(COL_BG);
  drawHeader();
  drawFooter();

  // WLAN
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PSK);
  while (WiFi.status() != WL_CONNECTED) delay(150);
  drawFooter();

  // OTA Web
  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST,
            [](){ server.sendHeader("Connection","close"); server.send(200,"text/plain","OK"); },
            handleUpdate);
  server.begin();

  // MQTT
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
}

void loop(){
  if (!mqtt.connected()) mqttReconnect();
  mqtt.loop();
  server.handleClient();

  BtnEvent eL = pollBtn(btnL);
  BtnEvent eR = pollBtn(btnR);

  if (eL==EV_SHORT)  publishEvent("L","short");
  else if (eL==EV_LONG)   publishEvent("L","long");
  else if (eL==EV_DOUBLE) publishEvent("L","double");

  if (eR==EV_SHORT)  publishEvent("R","short");
  else if (eR==EV_LONG)   publishEvent("R","long");
  else if (eR==EV_DOUBLE) publishEvent("R","double");
}
