/*
  ESP2 MASTER – Spotify Look (auf Basis 1.4.0)
  FW: 1.4.2-M

  Änderungen NUR Optik:
   - Headerleiste mit kleinem Spotify-Icon + MQTT/FW
   - Titel groß (Wrap), Artist klein
   - State-Badge (PLAYING/PAUSED/STOPPED) in grün/gelb/rot
   - Keine Prozent-/Lautstärkeanzeige mehr
   - Partielle Redraws (keine Vollbild-Refreshes)

  Unverändert:
   - OTA Web (Root) + dauerhafter SoftAP 192.168.4.1
   - Buttons (kurz/lang/doppelt) senden JSON auf esp2panel/event (src="A")
   - MQTT-Subscribe: esp2panel/A/line/1  ("Artist - Title")
                     esp2panel/A/line/2  ("STATE XX%") -> wir ignorieren die Zahl
   - Pins, WLAN, Broker-IP/User/Pass
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// ---------- Version ----------
static const char* FW_VERSION = "1.4.2-M";

// ---------- Pins ----------
#define TFT_CS    5
#define TFT_DC    7
#define TFT_RST   6
#define TFT_SCLK 12
#define TFT_MOSI 11
#define BTN_L     8
#define BTN_R     9
#define PIN_BL   13

// ---------- Display ----------
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// Farben
#define COL_BG      ST77XX_BLACK
#define COL_TXT     ST77XX_WHITE
#define COL_SUB     0xC618      // hellgrau
#define COL_ACC     0x07E0      // Spotify-Grün
#define COL_BADGE_P ST77XX_GREEN
#define COL_BADGE_PA ST77XX_YELLOW
#define COL_BADGE_S ST77XX_RED
#define COL_HEADBG  0x0841      // dunkle Kopfzeile

// ---------- WLAN / MQTT ----------
const char* WIFI_SSID = "Peng";
const char* WIFI_PSK  = "Keineahnung123";

const char* MQTT_HOST = "192.168.178.65";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "firstclass55555";
const char* MQTT_PASS = "Zehn+551996";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ---------- Webserver (OTA) ----------
WebServer server(80);

static const char OTA_INDEX[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP2 MASTER OTA</title>
<style>
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px}
h3{margin:0 0 12px}
form{display:flex;gap:12px;align-items:center}
button{padding:.6em 1.2em}
</style></head><body>
<h3>ESP2 MASTER OTA</h3>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="update" accept=".bin" required>
<button type="submit">Upload & Flash</button>
</form>
<p>SoftAP: 192.168.4.1 · Nach Erfolg Neustart.</p>
</body></html>
)HTML";

// ---------- Buttons ----------
struct Btn {
  uint8_t  pin;
  bool     last;          // HIGH = released
  uint32_t lastChange;
  uint32_t lastShortRel;
  bool     pendingShort;
  Btn(uint8_t p): pin(p), last(true), lastChange(0), lastShortRel(0), pendingShort(false) {}
  Btn(): pin(0), last(true), lastChange(0), lastShortRel(0), pendingShort(false) {}
};
enum BtnEvent { EV_NONE, EV_SHORT, EV_LONG, EV_DOUBLE };
static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONG_MIN_MS = 700;
static const uint32_t SHORT_MAX_MS = 300;
static const uint32_t DBL_WIN_MS  = 250;

Btn btnL(BTN_L), btnR(BTN_R);

// ---------- Anzeige-Status ----------
String gTitle  = "-";
String gArtist = "-";
String gState  = "IDLE";

String pTitle, pArtist, pState;
bool   mqttOk = false;

// ---------- Hilfen ----------
inline void clearBox(int x,int y,int w,int h,uint16_t col=COL_BG){ tft.fillRect(x,y,w,h,col); }
void wrapPrint(const String& text, int16_t x, int16_t y, int16_t maxW, uint8_t size, uint16_t col);

// ---------- Vorwärts ----------
void drawHeader(bool force=false);
void drawFooter(bool force=false);
void drawMain(bool force=false);
void drawBadge(const String& state);
void drawSpotifyGlyph(int cx, int cy, int r, uint16_t col);

void mqttReconnect();
void mqttCallback(char* topic, byte* payload, unsigned int len);
void publishEvent(const char* btn, const char* type);

// ---------- OTA-Handler ----------
void handleRootGet() { server.send_P(200, "text/html", OTA_INDEX); }
void handleUpdatePost() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); }
  else if (up.status == UPLOAD_FILE_WRITE) { Update.write(up.buf, up.currentSize); }
  else if (up.status == UPLOAD_FILE_END) { Update.end(true); }
  if (up.status == UPLOAD_FILE_END) {
    server.sendHeader("Connection","close");
    server.send(200,"text/plain","OK");
    delay(200);
    ESP.restart();
  }
}

// ---------- Button-Logik ----------
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
    if (level){ // Release
      uint32_t dur = t - b.lastChange;
      if (dur <= SHORT_MAX_MS){
        if (b.pendingShort && (t - b.lastShortRel) <= DBL_WIN_MS){
          ev = EV_DOUBLE; b.pendingShort=false; b.lastShortRel=0;
        } else { b.pendingShort=true; b.lastShortRel=t; }
      } else if (dur >= LONG_MIN_MS){
        ev = EV_LONG; b.pendingShort=false; b.lastShortRel=0;
      }
    }
  }
  if (b.pendingShort && (millis() - b.lastShortRel) > DBL_WIN_MS){
    ev = EV_SHORT; b.pendingShort=false; b.lastShortRel=0;
  }
  return ev;
}

// ---------- Layout ----------
// Headerleiste mit Icon + MQTT/FW
void drawHeader(bool force){
  static bool pOk = !mqttOk;
  if(!force && pOk==mqttOk) return;

  tft.fillRect(0,0,160,18, COL_HEADBG);
  // Spotify-Glyph links
  drawSpotifyGlyph(9,9,6, COL_ACC);

  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setCursor(18,4);
  tft.setTextColor(COL_TXT); tft.print("ESP2 MASTER  ");
  tft.setTextColor(COL_SUB); tft.print("FW ");
  tft.setTextColor(COL_TXT); tft.print(FW_VERSION);

  tft.setCursor(112,4);
  tft.setTextColor(COL_SUB); tft.print("MQTT ");
  tft.setTextColor(mqttOk ? COL_TXT : COL_BADGE_S);
  tft.print(mqttOk ? "OK" : "...");

  pOk = mqttOk;
}

void drawFooter(bool force){
  static String pIp = "";
  String ip = WiFi.localIP().toString();
  if(!force && pIp == ip) return;

  clearBox(0,118,160,30, COL_BG);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(COL_SUB);
  tft.setCursor(2,132);
  tft.print("STA: "); tft.print(ip);
  tft.setCursor(2,142);
  tft.print("AP : 192.168.4.1");
  pIp = ip;
}

// Hauptfeld (Titel/Artist + State-Badge)
void drawMain(bool force){
  if(!force && gTitle==pTitle && gArtist==pArtist && gState==pState) return;

  clearBox(0,20,160,96, COL_BG);

  // Titel groß, 2 Zeilen Wrap
  tft.setTextWrap(false);
  tft.setTextColor(COL_TXT);
  wrapPrint(gTitle, 2, 24, 156, 2, COL_TXT);

  // Artist klein, grau
  tft.setTextSize(1);
  tft.setTextColor(COL_SUB);
  // Artist unter dem (möglicherweise zweizeiligen) Titel:
  int artistY = 24 + 2*8 + 8; // grob: zwei Zeilen a 16px + Padding
  tft.setCursor(2, artistY);
  tft.print(gArtist);

  // State-Badge oben rechts
  drawBadge(gState);

  pTitle = gTitle;
  pArtist = gArtist;
  pState = gState;
}

// kleines Spotify-Icon (3 Bögen)
void drawSpotifyGlyph(int cx, int cy, int r, uint16_t col){
  tft.drawCircle(cx,cy,r,col);
  // einfache "Wellen":
  tft.drawFastHLine(cx-r+2, cy, r+2, col);
  tft.drawFastHLine(cx-r+3, cy+2, r-1, col);
  tft.drawFastHLine(cx-r+4, cy-2, r-3, col);
}

// Status-Badge
void drawBadge(const String& state){
  uint16_t bg = COL_BADGE_S;
  if(state == "PLAYING") bg = COL_BADGE_P;
  else if(state == "PAUSED") bg = COL_BADGE_PA;

  // Badge-Box
  int w = 64, h = 14, x = 160 - w - 2, y = 22;
  tft.fillRoundRect(x,y,w,h,3,bg);
  tft.drawRoundRect(x,y,w,h,3, COL_BG);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(COL_BG);
  tft.setCursor(x+6, y+4);
  tft.print(state);
}

// Hilfsfunktion: weicher Zeilenumbruch (max. 2 Zeilen) für große Schrift
void wrapPrint(const String& text, int16_t x, int16_t y, int16_t maxW, uint8_t size, uint16_t col){
  tft.setTextSize(size);
  tft.setTextColor(col);

  String s = text;
  s.trim();

  // erste Zeile
  int cut = s.length();
  while(cut>0){
    int16_t bx,by; uint16_t bw,bh;
    String candidate = s.substring(0,cut);
    tft.getTextBounds(candidate, x,y, &bx,&by,&bw,&bh);
    if(bw <= maxW) break;
    int prevSpace = s.lastIndexOf(' ', cut-1);
    cut = prevSpace > 0 ? prevSpace : cut-1;
  }
  String line1 = s.substring(0,cut);
  tft.setCursor(x,y);
  tft.print(line1);

  // zweite Zeile (falls Text übrig)
  if(cut < (int)s.length()){
    String rest = s.substring(cut);
    rest.trim();
    // ggf. mit Ellipsis kürzen
    int16_t bx,by; uint16_t bw,bh;
    String ell = rest;
    tft.getTextBounds(ell, x, y+16, &bx,&by,&bw,&bh);
    while(bw > maxW && ell.length()>1){
      ell.remove(ell.length()-1);
      tft.getTextBounds(ell + "...", x, y+16, &bx,&by,&bw,&bh);
      if(bw <= maxW){ ell += "..."; break; }
    }
    tft.setCursor(x, y+16);
    tft.print(ell);
  }
}

// ---------- MQTT ----------
void mqttCallback(char* topic, byte* payload, unsigned int len){
  String t = String(topic);
  String msg; msg.reserve(len+1);
  for (unsigned int i=0;i<len;i++) msg += (char)payload[i];

  if (t == "esp2panel/A/line/1"){
    int sep = msg.indexOf(" - ");
    if (sep >= 0){
      gArtist = msg.substring(0, sep);
      gTitle  = msg.substring(sep+3);
    } else { gArtist = ""; gTitle = msg; }
    drawMain(false);
  }
  else if (t == "esp2panel/A/line/2"){
    // z.B. "PLAYING 38%" -> wir verwenden NUR den State
    String s = msg; s.trim();
    int sp = s.indexOf(' ');
    gState = (sp > 0) ? s.substring(0, sp) : s;
    drawMain(false);
  }
}

void mqttReconnect(){
  while (!mqtt.connected()){
    String cid = String("esp2panel-A-") + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)){
      mqtt.subscribe("esp2panel/A/line/1");
      mqtt.subscribe("esp2panel/A/line/2");
      mqttOk = true;
      drawHeader(true);
    } else {
      mqttOk = false;
      drawHeader(true);
      delay(1000);
    }
  }
}

// ---------- Events ----------
void publishEvent(const char* btn, const char* type){
  String payload = String("{\"src\":\"A\",\"btn\":\"") + btn + "\",\"type\":\"" + type + "\"}";
  mqtt.publish("esp2panel/event", payload.c_str());
  // kleines Textfeedback oben unter Header
  clearBox(2, 104, 156, 10, COL_BG);
  tft.setTextSize(1);
  tft.setTextColor(COL_ACC);
  tft.setCursor(2,104);
  tft.print("A "); tft.print(btn); tft.print(" "); tft.print(type);
}

// ---------- Setup / Loop ----------
void setup(){
  pinMode(BTN_L, INPUT_PULLUP);
  pinMode(BTN_R, INPUT_PULLUP);

  // Backlight PWM
  ledcAttachPin(PIN_BL, 0);
  ledcSetup(0, 1000, 8);
  ledcWrite(0, 200); // 0..255

  // Display
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(COL_BG);
  drawHeader(true);
  drawMain(true);
  drawFooter(true);

  // WLAN + SoftAP für OTA
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP2-MASTER-OTA");
  WiFi.begin(WIFI_SSID, WIFI_PSK);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 6000){ delay(150); }
  drawFooter(true);

  // OTA Web
  server.on("/", HTTP_GET, handleRootGet);
  server.on("/update", HTTP_POST,
    [](){ server.sendHeader("Connection","close"); server.send(200,"text/plain","OK"); },
    handleUpdatePost
  );
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
