/*******************************************************
 * ESP32(-S2) Slideshow + Web UI + SPIFFS Dateimanager + OTA
 * Display: Adafruit ST7735 160x128
 * Rotation: 1 (wie gewünscht)
 *
 * Abhängigkeiten (Arduino IDE/CLI):
 *  - ESP32 Core 2.0.17
 *  - Adafruit GFX Library
 *  - Adafruit ST7735 and ST7789 Library
 *  - AsyncTCP (ESP32)
 *  - ESPAsyncWebServer
 *  - SPIFFS
 *******************************************************/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <FS.h>
#include <SPIFFS.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

// -------------------- TFT Pins (dein Layout) --------------------
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// -------------------- WLAN --------------------
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT";   // leer => AP-Fallback
AsyncWebServer server(80);

// -------------------- Slideshow State --------------------
volatile bool playing = false;
uint32_t intervalMs = 3000;   // Autoplay-Intervall
uint32_t lastSwitch = 0;
int currentIndex = 0;

// Übergang: 0=keiner, 1=Slide, 2=Fade
uint8_t transitionMode = 1;

// Liste unterstützt .raw und .bmp
#define MAX_FILES 128
String files[MAX_FILES];
int fileCount = 0;

// Framebuffer-Zeile
static uint16_t line[160];

// -------------- HTML Seiten --------------
const char* MAIN_PAGE = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP Slideshow</title>
<style>
 body{font-family:system-ui,Arial;margin:14px;background:#111;color:#eee}
 card{display:block;max-width:720px;margin:auto;padding:14px;border:1px solid #333;border-radius:12px;background:#1b1b1b}
 h1{font-size:18px;margin:0 0 10px}
 button,select,input{padding:8px 10px;border:0;border-radius:8px;background:#2c6ef2;color:#fff;cursor:pointer}
 .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
 .muted{color:#aaa}
 a{color:#9cf}
</style>
<card>
  <h1>Slideshow Steuerung</h1>
  <div class=row>
    <button onclick="fetch('/api/prev')">◀︎ Zurück</button>
    <button onclick="fetch('/api/next')">Weiter ▶︎</button>
    <button id=playBtn onclick="toggle()"></button>
    <label>Intervall:
      <input id=ms type=number min=500 step=100 value=3000 style="width:110px;color:#000">
    </label>
    <button onclick="setMs()">Setzen</button>
    <label>Übergang:
      <select id=tr style="color:#000">
        <option value=0>keiner</option>
        <option value=1 selected>Slide</option>
        <option value=2>Fade</option>
      </select>
    </label>
    <button onclick="setTr()">OK</button>
  </div>
  <p class=muted id=stat>…</p>
  <div class=row>
    <a href="/fs">Dateimanager (SPIFFS)</a>
    <a href="/ota">OTA Update</a>
  </div>
</card>
<script>
async function get(){ return await (await fetch('/api/status')).json(); }
async function setMs(){
  let v=parseInt(document.getElementById('ms').value||'3000',10);
  if(v<500)v=500;
  await fetch('/api/play?ms='+v);
  refresh();
}
async function setTr(){
  let v=document.getElementById('tr').value;
  await fetch('/api/transition?mode='+v); refresh();
}
async function toggle(){
  let s = await get();
  await fetch(s.playing?'/api/stop':'/api/play?ms='+s.interval);
  refresh();
}
async function refresh(){
  let s = await get();
  document.getElementById('ms').value = s.interval;
  document.getElementById('tr').value = s.transition;
  document.getElementById('stat').textContent =
    `Status: ${s.playing?'spielt':'gestoppt'} | Datei ${s.index+1}/${s.total}: ${s.name}`;
  document.getElementById('playBtn').textContent = s.playing?'Stop':'Play';
}
setInterval(refresh,1500); refresh();
</script>
)HTML";

const char* FS_PAGE = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP Dateimanager</title>
<style>
 body{font-family:system-ui,Arial;margin:14px;background:#111;color:#eee}
 card{display:block;max-width:720px;margin:auto;padding:14px;border:1px solid #333;border-radius:12px;background:#1b1b1b}
 h1{font-size:18px;margin:0 0 10px}
 table{width:100%;border-collapse:collapse}
 th,td{padding:6px;border-bottom:1px solid #2a2a2a}
 tr:hover{background:#222}
 button{padding:6px 10px;border:0;border-radius:8px;background:#2c6ef2;color:#fff;cursor:pointer}
 .danger{background:#d33}
 .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
 a{color:#9cf}
</style>
<card>
  <h1>SPIFFS – Dateien</h1>
  <div class=row>
    <form id=up enctype="multipart/form-data" method=POST action="/fs/upload">
      <input type=file name=file required>
      <button>Upload</button>
    </form>
    <a href="/" class=btn>Zur&uuml;ck</a>
  </div>
  <div id=log style="margin:8px 0;color:#8fd">Bereit.</div>
  <table id=tbl>
    <thead><tr><th>Name</th><th style="width:120px">Gr&ouml;&szlig;e</th><th style="width:210px">Aktion</th></tr></thead>
    <tbody></tbody>
  </table>
</card>
<script>
function fmt(n){if(n>1048576)return (n/1048576).toFixed(2)+' MB'; if(n>1024)return (n/1024).toFixed(1)+' kB'; return n+' B'}
function log(s){document.getElementById('log').textContent=s}
async function refresh(){
  let j = await (await fetch('/fs/list')).json();
  let tb = document.querySelector('#tbl tbody'); tb.innerHTML='';
  j.files.forEach(f=>{
    let tr = document.createElement('tr');
    tr.innerHTML = `<td>${f.name}</td>
      <td>${fmt(f.size)}</td>
      <td>
        <a href="${f.url}" download>Download</a>
        &nbsp;|&nbsp;
        <button class="danger" onclick="delFile('${encodeURIComponent(f.name)}')">L&ouml;schen</button>
      </td>`;
    tb.appendChild(tr);
  });
  log('Belegt: '+fmt(j.used)+' / '+fmt(j.total));
}
async function delFile(n){
  if(!confirm('Datei l&ouml;schen?')) return;
  let r = await fetch('/fs/delete?name='+n);
  log(await r.text()); refresh();
}
refresh();
</script>
)HTML";

const char* OTA_PAGE = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>OTA Update</title>
<style>
 body{font-family:system-ui,Arial;margin:14px;background:#111;color:#eee}
 card{display:block;max-width:720px;margin:auto;padding:14px;border:1px solid #333;border-radius:12px;background:#1b1b1b}
 h1{font-size:18px;margin:0 0 10px}
 button,input{padding:8px 10px;border:0;border-radius:8px;background:#2c6ef2;color:#fff;cursor:pointer}
 .muted{color:#aaa}
</style>
<card>
  <h1>OTA Firmware Update</h1>
  <form method='POST' action='/update' enctype='multipart/form-data' onsubmit="log('Lade hoch…')">
    <input type='file' name='firmware' required>
    <button>Flash</button>
  </form>
  <p id=log class=muted>Bereit.</p>
  <p><a href="/">Zur&uuml;ck</a></p>
</card>
<script>function log(s){document.getElementById('log').textContent=s}</script>
)HTML";

// -------------------- SPIFFS Utils --------------------
String jsonListFiles(){
  String j = "{";
  j += "\"total\":" + String(SPIFFS.totalBytes()) + ",";
  j += "\"used\":"  + String(SPIFFS.usedBytes())  + ",";
  j += "\"files\":[";
  bool first=true;

  File root = SPIFFS.open("/");
  if(root){
    File f = root.openNextFile();
    while(f){
      if(!first) j += ",";
      first=false;
      j += "{\"name\":\"" + String(f.name()) + "\",";
      j +=  "\"size\":" + String(f.size()) + ",";
      j +=  "\"url\":\"" + String(f.name()) + "\"}";
      f = root.openNextFile();
    }
  }
  j += "]}";
  return j;
}

static File uploadFile; // für multipart Upload-Handler

// -------------------- Dateiliste (nur .raw/.bmp) --------------------
bool isImageName(const String& n) {
  String s = n; s.toLowerCase();
  return s.endsWith(".raw") || s.endsWith(".bmp");
}

void buildList() {
  fileCount = 0;
  File root = SPIFFS.open("/");
  if(!root) return;
  File f = root.openNextFile();
  while(f && fileCount < MAX_FILES) {
    String n = String(f.name());
    if (isImageName(n)) files[fileCount++] = n;
    f = root.openNextFile();
  }
  if (currentIndex >= fileCount) currentIndex = max(0, fileCount-1);
}

// -------------------- Zeichen-Helfer --------------------
void setAddrWindowFull() {
  tft.setAddrWindow(0, 0, 160, 128);
}

// schreibt eine Zeile (y) an die X-Position x0 (Breite w)
void writeLine(int16_t x0, int16_t y, uint16_t* buf, int w) {
  tft.setAddrWindow(x0, y, w, 1);
  tft.startWrite();
  tft.writePixels(buf, (uint32_t)w);
  tft.endWrite();
}

// -------------------- RAW (160x128 RGB565 LE) --------------------
bool drawRAW(const char* path) {
  File f = SPIFFS.open(path, "r");
  if(!f) return false;

  setAddrWindowFull();
  for (int y=0; y<128; ++y) {
    int need = 160*2;
    int got = f.readBytes((char*)line, need);
    if (got != need) { f.close(); return false; }
    // little-endian -> line ist schon korrekt für writePixels
    writeLine(0, y, line, 160);
  }
  f.close();
  return true;
}

// -------------------- BMP (24-bit unkomprimiert) --------------------
#pragma pack(push,1)
struct BMPHeader {
  uint16_t bfType;
  uint32_t bfSize;
  uint16_t r1, r2;
  uint32_t offBits;
  uint32_t biSize;
  int32_t  biWidth;
  int32_t  biHeight;
  uint16_t biPlanes;
  uint16_t biBitCount;
  uint32_t biCompression;
  uint32_t biSizeImage;
  int32_t  biXPelsPerMeter;
  int32_t  biYPelsPerMeter;
  uint32_t biClrUsed;
  uint32_t biClrImportant;
};
#pragma pack(pop)

static inline uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

bool drawBMP(const char* path) {
  File f = SPIFFS.open(path, "r");
  if(!f) return false;
  BMPHeader h{};
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h)) { f.close(); return false; }
  if (h.bfType != 0x4D42 || h.biBitCount != 24 || h.biCompression != 0) { f.close(); return false; }

  int w = h.biWidth;
  int hgt = abs(h.biHeight);
  bool flip = (h.biHeight > 0); // BMP bottom-up

  if (w != 160 || hgt != 128) { f.close(); return false; }

  int rowSize = ((w*3 + 3) & ~3);
  uint8_t row[160*3];

  setAddrWindowFull();
  for (int y=0; y<128; ++y) {
    int ry = flip ? (127 - y) : y;
    uint32_t pos = h.offBits + (uint32_t)ry * rowSize;
    f.seek(pos, SeekSet);
    if (f.read(row, w*3) != w*3) { f.close(); return false; }

    for (int x=0, j=0; x<w; ++x) {
      uint8_t b=row[j++], g=row[j++], r=row[j++];
      line[x] = rgb565(r,g,b);
    }
    writeLine(0, y, line, w);
  }
  f.close();
  return true;
}

// -------------------- Übergänge --------------------
void transitionSlide(const String& nextPath) {
  // Render next Bild einmal in Puffer (Zeile für Zeile) und schiebe seitlich rein
  // Wir zeichnen in 8 Schritten von rechts rein
  const int steps = 8;
  // Vorab vollständiges Bild in RAM? Zu groß. Daher für jeden Schritt Linie aus Datei lesen.
  // Lösung: Für RAW machen wir es effizient, für BMP laden wir jede Zeile pro Schritt erneut.
  bool isRaw = nextPath.endsWith(".raw") || nextPath.endsWith(".RAW");

  File f = SPIFFS.open(nextPath, "r");
  if(!f){ return; }

  for (int s=0; s<=steps; ++s) {
    int xstart = 160 - (160*s)/steps; // sichtbarer Start der neuen Grafik
    // zeichne Hintergrund (schwarz) links vom neuen Bild
    if (xstart>0) tft.fillRect(0,0,xstart,128,ST77XX_BLACK);

    if (isRaw) {
      // RAW: Zeilen streamen und versetzt zeichnen
      for (int y=0; y<128; ++y) {
        int got = f.readBytes((char*)line, 160*2);
        if (got != 160*2) break;
        writeLine(xstart, y, line, 160 - xstart);
      }
      f.seek(0); // für nächsten Schritt wieder von vorn
    } else {
      // BMP: pro Schritt/Zeile konvertieren
      BMPHeader h{};
      f.seek(0); f.read((uint8_t*)&h, sizeof(h));
      int w = 160, hgt = 128, rowSize=((w*3+3)&~3);
      bool flip = (h.biHeight>0);
      uint8_t rowB[160*3];

      for (int y=0; y<128; ++y) {
        int ry = flip ? (127-y) : y;
        uint32_t pos = h.offBits + (uint32_t)ry * rowSize;
        f.seek(pos, SeekSet);
        f.read(rowB, 160*3);
        for (int x=0, j=0; x<w; ++x){ uint8_t b=rowB[j++],g=rowB[j++],r=rowB[j++]; line[x]=rgb565(r,g,b); }
        writeLine(xstart, y, line+xstart, 160 - xstart);
      }
    }
    delay(20);
  }
  f.close();
}

void transitionFade(const String& nextPath) {
  // Fade: zeichne neues Bild und lege halbtransparente schwarze Ebenen drüber (einfacher Fake)
  // 1) dunkle aktuelles Bild stufenweise -> 2) blende neues Bild ein (ohne True-Alpha)
  for (int i=0;i<5;i++){ tft.fillRect(0,0,160,128,ST77XX_BLACK); delay(20); }
  // finale Grafik:
  if (nextPath.endsWith(".raw")) drawRAW(nextPath.c_str()); else drawBMP(nextPath.c_str());
}

// -------------------- Bild anzeigen --------------------
bool showIndex(int idx, bool withTransition=true) {
  if (fileCount==0) { tft.fillScreen(ST77XX_BLACK); return false; }
  if (idx<0 || idx>=fileCount) return false;
  String p = files[idx];

  if (withTransition && transitionMode==1) transitionSlide(p);
  else if (withTransition && transitionMode==2) transitionFade(p);
  else {
    if (p.endsWith(".raw")) { if(!drawRAW(p.c_str())) return false; }
    else if (p.endsWith(".bmp")) { if(!drawBMP(p.c_str())) return false; }
    else return false;
  }

  currentIndex = idx;
  return true;
}

void nextImage(){ if(fileCount==0) return; int n=(currentIndex+1)%fileCount; showIndex(n,true); lastSwitch=millis(); }
void prevImage(){ if(fileCount==0) return; int n=(currentIndex-1+fileCount)%fileCount; showIndex(n,true); lastSwitch=millis(); }

// -------------------- Web-Setup --------------------
void setupWeb(){
  // Hauptseite
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", MAIN_PAGE); });

  // Status JSON
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r){
    String name = (fileCount? files[currentIndex] : String("-"));
    String j = "{";
    j += "\"playing\":" + String(playing?"true":"false") + ",";
    j += "\"interval\":" + String((unsigned long)intervalMs) + ",";
    j += "\"index\":" + String(currentIndex) + ",";
    j += "\"total\":" + String(fileCount) + ",";
    j += "\"transition\":" + String(transitionMode) + ",";
    j += "\"name\":\"" + name + "\"}";
    r->send(200, "application/json", j);
  });

  server.on("/api/next", HTTP_GET, [](AsyncWebServerRequest* r){ nextImage(); r->send(200,"text/plain","OK"); });
  server.on("/api/prev", HTTP_GET, [](AsyncWebServerRequest* r){ prevImage(); r->send(200,"text/plain","OK"); });

  server.on("/api/play", HTTP_GET, [](AsyncWebServerRequest* r){
    if(r->hasParam("ms")){
      unsigned long v = strtoul(r->getParam("ms")->value().c_str(),nullptr,10);
      if (v<500) v=500;
      intervalMs = v;
    }
    playing = true; lastSwitch = millis();
    r->send(200,"text/plain","PLAY");
  });
  server.on("/api/stop", HTTP_GET, [](AsyncWebServerRequest* r){ playing=false; r->send(200,"text/plain","STOP"); });

  server.on("/api/transition", HTTP_GET, [](AsyncWebServerRequest* r){
    if(r->hasParam("mode")){
      int m = r->getParam("mode")->value().toInt();
      transitionMode = constrain(m,0,2);
    }
    r->send(200,"text/plain","OK");
  });

  // ----------- SPIFFS Dateimanager -----------
  server.on("/fs", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", FS_PAGE); });
  server.on("/fs/list", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"application/json", jsonListFiles()); });

  // Dateien direkt aus SPIFFS ausliefern
  server.onNotFound([](AsyncWebServerRequest* r){
    if (r->method()==HTTP_GET) {
      String path = r->url();
      if (SPIFFS.exists(path)) { r->send(SPIFFS, path, String()); return; }
    }
    r->send(404,"text/plain","Not found");
  });

  // Delete
  server.on("/fs/delete", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!r->hasParam("name")) { r->send(400,"text/plain","name fehlt"); return; }
    String name = r->getParam("name")->value();
    if(!name.startsWith("/")) name = "/" + name;
    if(!SPIFFS.exists(name)){ r->send(404,"text/plain","gibt es nicht"); return; }
    SPIFFS.remove(name);
    buildList(); // Liste aktualisieren
    r->send(200,"text/plain","OK");
  });

  // Upload (multipart)
  server.on("/fs/upload", HTTP_POST,
    [](AsyncWebServerRequest *req){ req->send(200,"text/plain","Upload OK"); buildList(); },
    [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final){
      if (!index) {
        if(!filename.startsWith("/")) filename = "/" + filename;
        uploadFile = SPIFFS.open(filename, "w");
      }
      if (uploadFile) uploadFile.write(data, len);
      if (final && uploadFile) { uploadFile.close(); }
    }
  );

  // ----------- OTA ----------
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", OTA_PAGE); });

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

// -------------------- Setup/Loop --------------------
void setup(){
  pinMode(TFT_LED, OUTPUT); digitalWrite(TFT_LED, HIGH);
  Serial.begin(115200);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);   // wie gewünscht
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);

  // SPIFFS
  if(!SPIFFS.begin(true)){
    tft.setCursor(0,0); tft.setTextColor(ST77XX_RED); tft.print("SPIFFS FAIL");
  }

  buildList();
  tft.setCursor(0,0); tft.setTextColor(ST77XX_YELLOW);
  tft.print("Files: "); tft.print(fileCount);

  // WLAN
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0=millis();
  while(strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(200);
  if(WiFi.status()!=WL_CONNECTED){
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP_Slideshow","12345678");
  }

  // Erstes Bild (falls vorhanden)
  if (fileCount) showIndex(currentIndex,false);

  setupWeb();
}

void loop(){
  if (playing && millis() - lastSwitch >= intervalMs) {
    nextImage();
  }
}
