/***** ESP32-S2 mini – Projekt 8: TFT-Slideshow + Web-UI + OTA (AsyncWebServer) *****/
// Rotation = 1, Demo-Animation, RAW/BMP-Renderer ohne pushColors()

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <FS.h>
#include <SPIFFS.h>
#include <Preferences.h>

// ---------- TFT-Pins ----------
#define TFT_CS    5
#define TFT_RST   6
#define TFT_DC    7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// ---------- WLAN ----------
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT"; // leer lassen => AP

AsyncWebServer server(80);

// ---------- Konfig ----------
Preferences prefs;                 // NVS
bool   autoplay = false;
long   slideMs  = 2000;            // Intervall
uint32_t lastSlide = 0;

// ---------- Demo-State ----------
uint8_t demoPhase = 0;             // 0..2 für Sinus/Gradient/Balken
uint16_t demoX = 0;

// ---------- Utils ----------
void setBacklight(uint8_t pwm) {
  static bool init = false;
  if (!init) {
    ledcSetup(0, 5000, 8);     // Kanal 0, 5kHz, 8 Bit
    ledcAttachPin(TFT_LED, 0);
    init = true;
  }
  ledcWrite(0, pwm);
}

// ===== RAW (160x128, RGB565) – kompatibel ohne pushColors() =====
bool drawRAW(const char* path){
  File f = SPIFFS.open(path, "r");
  if(!f) return false;
  const uint16_t W=160, H=128;
  static uint16_t line[W];

  tft.startWrite();
  tft.setAddrWindow(0,0,W,H);
  for(uint16_t y=0; y<H; y++){
    size_t got = f.read((uint8_t*)line, W*2);
    if(got != W*2){ tft.endWrite(); f.close(); return false; }
    for(uint16_t x=0; x<W; x++) tft.pushColor(line[x]);
  }
  tft.endWrite();
  f.close();
  return true;
}

bool drawRAWPartial(const char* path, int w){
  if(w<=0) return true;
  if(w>160) w=160;
  File f = SPIFFS.open(path, "r");
  if(!f) return false;
  const uint16_t W=160, H=128;
  static uint16_t line[W];

  for(uint16_t y=0; y<H; y++){
    size_t got = f.read((uint8_t*)line, W*2);
    if(got != W*2){ f.close(); return false; }
    tft.startWrite();
    tft.setAddrWindow(0, y, w, 1);
    for(int x=0; x<w; x++) tft.pushColor(line[x]);
    tft.endWrite();
  }
  f.close();
  return true;
}

// ===== 24-bit BMP – kompatibel ohne pushColors() =====
bool drawBMP(fs::FS &fs, const char *filename, int16_t x, int16_t y) {
  File bmpFS = fs.open(filename, "r");
  if(!bmpFS) return false;

  uint16_t bfType; uint32_t bfOffBits, biSize;
  int32_t biWidth, biHeight; uint16_t biPlanes, biBitCount; uint32_t biCompression;

  bmpFS.read((uint8_t*)&bfType, 2);
  if(bfType != 0x4D42){ bmpFS.close(); return false; }
  bmpFS.seek(10); bmpFS.read((uint8_t*)&bfOffBits,4);
  bmpFS.read((uint8_t*)&biSize,4);
  bmpFS.read((uint8_t*)&biWidth,4);
  bmpFS.read((uint8_t*)&biHeight,4);
  bmpFS.read((uint8_t*)&biPlanes,2);
  bmpFS.read((uint8_t*)&biBitCount,2);
  bmpFS.read((uint8_t*)&biCompression,4);
  if(biBitCount != 24 || biCompression != 0){ bmpFS.close(); return false; }

  bool flip = true; if(biHeight < 0){ biHeight = -biHeight; flip = false; }
  uint32_t rowSize = (biWidth * 3 + 3) & ~3;

  tft.startWrite();
  tft.setAddrWindow(x, y, biWidth, biHeight);

  const int BUFFPIXEL = 60;
  uint8_t sdbuffer[3*BUFFPIXEL];

  for(int row=0; row<biHeight; row++){
    uint32_t pos = bfOffBits + (flip ? (biHeight - 1 - row) : row) * rowSize;
    bmpFS.seek(pos);
    int col = biWidth;
    while(col > 0){
      int nb = (col > BUFFPIXEL) ? BUFFPIXEL : col;
      bmpFS.read(sdbuffer, nb*3);
      for(int i=0; i<nb; i++){
        uint8_t b = sdbuffer[i*3 + 0];
        uint8_t g = sdbuffer[i*3 + 1];
        uint8_t r = sdbuffer[i*3 + 2];
        uint16_t c = ((r&0xF8)<<8) | ((g&0xFC)<<3) | (b>>3);
        tft.pushColor(c);
      }
      col -= nb;
    }
  }
  tft.endWrite();
  bmpFS.close();
  return true;
}

// ---------- Demo-Animation (läuft bis Autoplay aktiv) ----------
void drawDemoFrame(){
  // 3 einfache Demos, weicher Übergang
  switch(demoPhase){
    case 0: { // Sinus
      if(demoX==0) tft.fillScreen(ST77XX_BLACK);
      uint16_t x = demoX % 160;
      for(uint16_t y=0; y<128; y++){
        tft.drawPixel(x, y, ST77XX_BLACK);
      }
      float s = sinf((demoX/160.0f)*2*PI);
      int y = 64 + (int)(s * 40.0f);
      tft.drawFastVLine(x, 0, 128, ST77XX_BLACK);
      tft.drawPixel(x, y, ST77XX_YELLOW);
      if(++demoX >= 320){ demoX=0; demoPhase=1; }
    } break;

    case 1: { // Gradient wipe
      for(int x=0; x<160; x++){
        uint8_t r = (x*255)/159;
        uint8_t g = 255 - r;
        uint16_t c = ((r&0xF8)<<8) | ((g&0xFC)<<3);
        tft.drawFastVLine(x,0,128,c);
      }
      demoPhase=2; demoX=0;
    } break;

    case 2: { // Balken-Slide
      tft.fillRect(0,0,160,128, ST77XX_BLACK);
      for(int i=0;i<8;i++){
        int w = 10 + ((i*7 + demoX)%90);
        int x = (i*18) % 160;
        uint16_t col = tft.color565(30*i, 255-30*i, 80+(i*20));
        tft.fillRect(x, 10+i*14, w, 10, col);
      }
      if(++demoX>60){ demoX=0; demoPhase=0; }
    } break;
  }
}

// ---------- Slideshow-Logik ----------
int currentIndex = 0;
String files[32];
int fileCount = 0;

bool isRAW(const String& s){ return s.endsWith(".raw") || s.endsWith(".RAW"); }
bool isBMP(const String& s){ return s.endsWith(".bmp") || s.endsWith(".BMP"); }

void scanFiles(){
  fileCount = 0;
  File root = SPIFFS.open("/");
  File f = root.openNextFile();
  while(f && fileCount < 32){
    String name = f.name();
    if(isRAW(name) || isBMP(name)){
      files[fileCount++] = name;
    }
    f = root.openNextFile();
  }
}

void showFile(int idx, bool transition=true){
  if(fileCount==0) return;
  idx = (idx%fileCount + fileCount) % fileCount;
  const String &name = files[idx];

  if(isRAW(name)){
    if(transition){
      for(int w=1; w<=160; w+=16){ drawRAWPartial(name.c_str(), w); delay(10); }
    }else{
      drawRAW(name.c_str());
    }
  }else if(isBMP(name)){
    drawBMP(SPIFFS, name.c_str(), 0, 0);
  }
  currentIndex = idx;
}

void next(){ showFile(currentIndex+1); }
void prev(){ showFile(currentIndex-1); }

// ---------- Web UI (Status + Steuerung + OTA) ----------
const char* PAGE = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP Slideshow + OTA</title>
<style>
 body{font-family:system-ui,Arial;margin:14px;background:#111;color:#eee}
 card{display:block;max-width:520px;margin:auto;padding:14px;border:1px solid #333;border-radius:12px;background:#1b1b1b}
 h1{font-size:18px;margin:0 0 10px}
 label{display:block;margin:8px 0}
 input[type=number]{width:110px}
 button{padding:8px 12px;border:0;border-radius:8px;background:#2c6ef2;color:#fff;cursor:pointer}
 a.btn{display:inline-block;padding:8px 12px;border-radius:8px;background:#444;color:#fff;text-decoration:none}
 .row>*{margin-right:8px}
</style>
<card>
  <h1>Status</h1>
  <div id=stat>lade…</div>
</card>
<br>
<card>
  <h1>Steuerung</h1>
  <div class=row>
    <button onclick="cmd('prev')">◀︎ Zurück</button>
    <button onclick="cmd('next')">Weiter ▶︎</button>
    <button onclick="cmd('scan')">Dateien scannen</button>
  </div>
  <label><input id=auto type=checkbox onchange="save()">
    Autoplay</label>
  <label>Intervall (ms):
    <input id=ms type=number min=500 step=100 value=2000 onchange="save()">
  </label>
</card>
<br>
<card>
  <h1>OTA Update</h1>
  <form id=f method=POST action="/update" enctype="multipart/form-data" onsubmit="log('Sende Firmware…')">
    <input type=file name=firmware required>
    <button type=submit>Flash</button>
  </form>
  <div id=log style="margin-top:8px;color:#8fd">Bereit.</div>
</card>
<script>
function log(s){document.getElementById('log').textContent=s}
async function refresh(){
  let j = await (await fetch('/api/state')).json();
  document.getElementById('auto').checked = j.autoplay;
  document.getElementById('ms').value = j.ms;
  document.getElementById('stat').textContent =
    `WiFi ${j.wifi}  |  Files ${j.files}  |  Aktuell ${j.current}`;
}
async function cmd(c){
  await fetch('/api/'+c); refresh();
}
async function save(){
  let a=document.getElementById('auto').checked;
  let ms=parseInt(document.getElementById('ms').value||2000);
  await fetch(`/api/config?autoplay=${a?'1':'0'}&ms=${ms}`);
  refresh();
}
setInterval(refresh, 2000); refresh();
</script>
)HTML";

void setupWeb(){
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", PAGE); });

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* r){
    String ip = (WiFi.getMode()==WIFI_AP)? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    String j = "{";
    j += "\"wifi\":\"" + ip + "\",";
    j += "\"autoplay\":" + String(autoplay ? "true":"false") + ",";
    j += "\"ms\":" + String(slideMs) + ",";
    j += "\"files\":" + String(fileCount) + ",";
    j += "\"current\":\"" + (fileCount?files[currentIndex]:"-") + "\"";
    j += "}";
    r->send(200,"application/json", j);
  });

  server.on("/api/next", HTTP_GET, [](AsyncWebServerRequest* r){ next(); r->send(200,"text/plain","OK"); });
  server.on("/api/prev", HTTP_GET, [](AsyncWebServerRequest* r){ prev(); r->send(200,"text/plain","OK"); });
  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* r){ scanFiles(); r->send(200,"text/plain","OK"); });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("autoplay")) autoplay = (r->getParam("autoplay")->value() == "1");
    if (r->hasParam("ms")) {
      long v = r->getParam("ms")->value().toInt();
      slideMs = (long)max(500L, v);   // Typ sicher
    }
    prefs.putBool("autoplay", autoplay);
    prefs.putInt("slideMs", (int)slideMs);
    r->send(200,"text/plain","OK");
  });

  // ---- OTA (eigene Seite, wie OTA-only) ----
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *req){
      bool ok = !Update.hasError();
      req->send(200,"text/plain", ok ? "Update OK. Reboot…" : "Update FEHLGESCHLAGEN!");
      if(ok){ delay(400); ESP.restart(); }
    },
    [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final){
      if (!index) { Update.begin(UPDATE_SIZE_UNKNOWN); }
      if (Update.write(data, len) != len) { Update.printError(Serial); }
      if (final) { if (!Update.end(true)) Update.printError(Serial); }
    }
  );

  server.begin();
}

// ---------- Setup/Loop ----------
void setup(){
  pinMode(TFT_LED, OUTPUT);
  setBacklight(200);

  Serial.begin(115200);
  SPIFFS.begin(true);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);                 // <== Rotation 1
  tft.fillScreen(ST77XX_BLACK);

  // Konfig laden
  prefs.begin("cfg", false);
  autoplay = prefs.getBool("autoplay", false);
  slideMs  = prefs.getInt("slideMs", 2000);
  lastSlide = millis();

  // kleine Startmeldung
  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(4,4); tft.setTextSize(1);
  tft.print("Projekt 8 – Slideshow + OTA");
  tft.setCursor(4,18); tft.print("WiFi verbinden…");

  // WiFi
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0=millis();
  while(strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(200);
  if(WiFi.status()!=WL_CONNECTED){
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP-Slideshow","12345678");
  }

  // UI-Info
  tft.fillRect(0,18,160,14,ST77XX_BLACK);
  tft.setCursor(4,18);
  tft.print("IP: ");
  tft.print((WiFi.getMode()==WIFI_AP)? WiFi.softAPIP(): WiFi.localIP());

  scanFiles();
  if(fileCount>0) showFile(0,false);

  setupWeb();
}

void loop(){
  // Demo abspielen, solange Autoplay aus & keine Dateien
  if(!autoplay && fileCount==0){
    drawDemoFrame();
    delay(16);
    return;
  }

  // Autoplay
  if(autoplay && (millis()-lastSlide) >= (uint32_t)slideMs){
    lastSlide = millis();
    next();
  }
}
