/***** ESP32/ESP32-S2 Slideshow mit PNG+JPG, Crossfade, OTA, AsyncWebServer *****
 * Display: ST7735 160x128 (Adafruit_ST7735)
 * Rotation: 1 (quer)
 * Web: Upload (JPG/PNG), Liste, Start/Stop, Intervall (ms), Crossfade (ms), Löschen, OTA
 * Speicher: SPIFFS  /images  (Dateinamen ohne Leerzeichen empfohlen)
 *******************************************************************************/

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// ---- TFT Pins (wie bei dir) ----
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// -------------- WiFi / Web ----------------
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

const char* WIFI_SSID = "Peng";            // STA-Zugang – anpassen
const char* WIFI_PASS = "DEIN_PASSWORT";   // "" => AP-Fallback
AsyncWebServer server(80);

// -------------- Bild Decoder --------------
#include <TJpg_Decoder.h>   // JPG
#include <PNGdec.h>         // PNG
PNG png;

// -------------- Anzeigeparameter ----------
static const int TFT_W = 160;
static const int TFT_H = 128;
static const uint16_t COL_BG   = ST77XX_BLACK;
static const uint16_t COL_TEXT = ST77XX_WHITE;

// Frame-Puffer für echtes Crossfade (RGB565)
static uint16_t* framePrev = nullptr;  // TFT_W * TFT_H * 2 Bytes
static uint16_t* frameNext = nullptr;

// Player-Status
bool autoplay = false;
uint32_t intervalMs = 3000;   // Default Intervall
uint32_t fadeMs     = 600;    // Crossfade-Dauer
uint32_t lastSwitch = 0;
int currentIndex    = -1;     // Index in images[]

#define MAX_IMAGES 256
String images[MAX_IMAGES];
int imageCount = 0;

// Hilfsfunktionen --------------------------------------------------------------

static inline uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b) >> 3);
}

void fillScreenFromBuffer(const uint16_t* src) {
  // Komplettbild aus Puffer auf TFT schieben (schnell)
  tft.startWrite();
  tft.setAddrWindow(0, 0, TFT_W, TFT_H);
  // Adafruit_ST7735 erwartet 16-Bit RGB565 als HighByte/LowByte
  for (int i = 0; i < TFT_W * TFT_H; i++) {
    uint16_t c = src[i];
    tft.writePixel(c);
  }
  tft.endWrite();
}

void crossfadeToNext() {
  if (!framePrev || !frameNext) return;
  if (fadeMs == 0) { fillScreenFromBuffer(frameNext); memcpy(framePrev, frameNext, TFT_W*TFT_H*2); return; }

  uint32_t steps = constrain(fadeMs / 16, 1u, 60u);  // ~60..1 Schritte
  static uint16_t* mix = nullptr;
  if (!mix) mix = (uint16_t*)heap_caps_malloc(TFT_W * TFT_H * 2, MALLOC_CAP_8BIT);

  for (uint32_t s = 1; s <= steps; s++) {
    float a = (float)s / (float)steps;  // 0..1
    // Mischen (565 -> 888 -> LERP -> 565)
    for (int i = 0; i < TFT_W * TFT_H; i++) {
      uint16_t p = framePrev[i], n = frameNext[i];
      // aus 565 extrahieren
      uint8_t pr = (p >> 8) & 0xF8, pg = (p >> 3) & 0xFC, pb = (p << 3) & 0xF8;
      uint8_t nr = (n >> 8) & 0xF8, ng = (n >> 3) & 0xFC, nb = (n << 3) & 0xF8;
      uint8_t r = pr + (nr - pr) * a;
      uint8_t g = pg + (ng - pg) * a;
      uint8_t b = pb + (nb - pb) * a;
      mix[i] = rgb888_to_565(r, g, b);
    }
    fillScreenFromBuffer(mix);
    delay(16);
  }
  // Übernahme als neues „prev“
  memcpy(framePrev, frameNext, TFT_W * TFT_H * 2);
}

// --------- Bild in frameNext decodieren (Fit + Zentrieren) ----------
// Ziel: gesamte Fläche füllen (letterbox/pillarbox mit schwarz)

void clearBuffer(uint16_t* dst, uint16_t color) {
  for (int i = 0; i < TFT_W * TFT_H; i++) dst[i] = color;
}

// ---- JPG Callback: schreibt Kachel in frameNext an offset+scale
struct JPGContext {
  uint16_t* buf;
  int x0, y0; // Offset
};
bool jpgDrawToBuffer(JPEGDRAW *pDraw) {
  JPGContext* ctx = (JPGContext*)TJpgDec.getUserPointer();
  int x = ctx->x0 + pDraw->x;
  int y = ctx->y0 + pDraw->y;
  if (x >= TFT_W || y >= TFT_H) return true;

  // begrenzen
  int w = pDraw->iWidth;
  int h = pDraw->iHeight;
  if (x + w > TFT_W) w = TFT_W - x;
  if (y + h > TFT_H) h = TFT_H - y;

  // pDraw->pPixels ist RGB565 (Little Endian)
  // in frameNext kopieren, Zeile für Zeile
  uint16_t* src = (uint16_t*)pDraw->pPixels;
  for (int row = 0; row < h; row++) {
    memcpy(&ctx->buf[(y + row) * TFT_W + x], &src[row * pDraw->iWidth], w * 2);
  }
  return true;
}

// ---- PNG Callback: je Zeile RGBA8888 in frameNext malen
struct PNGContext {
  uint16_t* buf;
  int x0, y0, outW;
};

void pngDraw(PNGDRAW *pDraw) {
  PNGContext* ctx = (PNGContext*)pDraw->pUser;
  uint8_t *line = png.getLineAsRGBA8(pDraw, 0);  // RGBA8888
  int y = ctx->y0 + pDraw->y;
  if (y < 0 || y >= TFT_H) return;

  int maxW = min(pDraw->iWidth, ctx->outW);
  for (int x = 0; x < maxW; x++) {
    int xx = ctx->x0 + x;
    if (xx < 0 || xx >= TFT_W) continue;
    uint8_t r = line[x * 4 + 0];
    uint8_t g = line[x * 4 + 1];
    uint8_t b = line[x * 4 + 2];
    uint8_t a = line[x * 4 + 3]; // (optional für Alpha)
    if (a < 8) {
      // transparente Pixel -> Hintergrund
      continue;
    }
    ctx->buf[y * TFT_W + xx] = rgb888_to_565(r, g, b);
  }
}

// ---- Helfer: Bilddatei (JPG/PNG) in frameNext decodieren ----
bool renderImageToNext(const String& path) {
  File f = SPIFFS.open(path, "r");
  if (!f) return false;

  clearBuffer(frameNext, COL_BG);

  // Größe (ohne vollständiges Dekodieren) ermitteln
  bool isPNG = path.endsWith(".png") || path.endsWith(".PNG");
  bool isJPG = path.endsWith(".jpg") || path.endsWith(".jpeg") || path.endsWith(".JPG") || path.endsWith(".JPEG");

  int srcW = 0, srcH = 0;
  if (isJPG) {
    TJpgDec.getJpgSize(&srcW, &srcH, f);
  } else if (isPNG) {
    int16_t rc = png.open(path.c_str(), SPIFFS);
    if (rc == PNG_SUCCESS) {
      srcW = png.getWidth();
      srcH = png.getHeight();
      png.close();
    }
  } else {
    f.close();
    return false;
  }

  if (srcW <= 0 || srcH <= 0) { f.close(); return false; }

  // Fit berechnen (Aspect-Fit)
  float scale = min((float)TFT_W / srcW, (float)TFT_H / srcH);
  int outW = max(1, (int)(srcW * scale));
  int outH = max(1, (int)(srcH * scale));
  int x0 = (TFT_W - outW) / 2;
  int y0 = (TFT_H - outH) / 2;

  bool ok = false;

  if (isJPG) {
    // JPG: Decoder unterstützt nur 1/1,1/2,1/4,1/8 – wir dekodieren nativ und lassen TJpg die Kacheln liefern
    f.close(); // TJpg arbeitet selbst über FS
    JPGContext ctx{ frameNext, x0, y0 };
    TJpgDec.setUserPointer(&ctx);
    TJpgDec.setJpgScale(1);     // beste Qualität, wir zentrieren
    TJpgDec.setCallback(jpgDrawToBuffer);
    ok = (TJpgDec.drawFsJpg(0, 0, path) == 1);
  } else {
    // PNG: PNGdec kann frei skalieren? – wir lesen 1:1 und lassen bei Bedarf clippen (Fit kleiner als TFT)
    int16_t rc = png.open(path.c_str(), SPIFFS);
    if (rc == PNG_SUCCESS) {
      PNGContext ctx{ frameNext, x0, y0, outW };
      png.setUserPointer(&ctx);
      // PNGdec kann leider nicht „on-the-fly“ skalieren – wir clippen falls größer
      ok = (png.decode(NULL, 0) == PNG_SUCCESS); // ruft pngDraw für jede Zeile
      png.close();
    } else ok = false;
    f.close();
  }

  return ok;
}

// ----------- Dateiverwaltung -------------
void refreshList() {
  imageCount = 0;
  File dir = SPIFFS.open("/images");
  if (!dir || !dir.isDirectory()) return;
  File f;
  while ((f = dir.openNextFile())) {
    String n = String(f.name());
    f.close();
    n.toLowerCase();
    if (n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".png")) {
      if (imageCount < MAX_IMAGES) images[imageCount++] = String(f.name());
    }
  }
  // sortieren (einfach)
  for (int i=0;i<imageCount-1;i++) {
    for (int j=i+1;j<imageCount;j++) {
      if (images[j] < images[i]) { String t=images[i]; images[i]=images[j]; images[j]=t; }
    }
  }
  if (currentIndex >= imageCount) currentIndex = imageCount - 1;
}

// ----------- Steuerung --------------------
bool showByIndex(int idx, bool fade=true) {
  if (idx < 0 || idx >= imageCount) return false;
  String path = images[idx];
  if (!renderImageToNext(path)) return false;

  if (fade && framePrev) crossfadeToNext();
  else { fillScreenFromBuffer(frameNext); memcpy(framePrev, frameNext, TFT_W*TFT_H*2); }

  currentIndex = idx;
  lastSwitch = millis();
  return true;
}

bool showNext(bool fade=true) {
  if (imageCount == 0) return false;
  int n = (currentIndex + 1) % imageCount;
  return showByIndex(n, fade);
}
bool showPrev(bool fade=true) {
  if (imageCount == 0) return false;
  int p = (currentIndex - 1 + imageCount) % imageCount;
  return showByIndex(p, fade);
}

// ---------------- Web UI -----------------
const char* PAGE_INDEX = R"HTML(
<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP Slideshow</title>
<style>
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:14px;background:#111;color:#eee}
h1{font-size:18px;margin:8px 0}
.card{background:#1b1b1b;border-radius:12px;padding:12px;margin:10px 0;border:1px solid #333}
label{display:block;margin:6px 0 2px}
input[type=number]{width:120px}
.btn{display:inline-block;padding:8px 12px;margin:4px;border-radius:8px;border:1px solid #444;background:#222;color:#eee;cursor:pointer}
.btn:hover{background:#2a2a2a}
ul{padding-left:18px}
li{margin:4px 0}
small{color:#bbb}
</style></head><body>
<h1>ESP Slideshow</h1>

<div class=card>
  <form id="up" method="POST" action="/upload" enctype="multipart/form-data">
    <label>Datei hochladen (JPG/PNG)</label>
    <input type="file" name="file">
    <button class=btn>Upload</button>
  </form>
  <form id="ota" method="POST" action="/update" enctype="multipart/form-data" style="margin-top:10px">
    <label>OTA Firmware (.bin)</label>
    <input type="file" name="firmware">
    <button class=btn>Flash</button>
  </form>
</div>

<div class=card>
  <div>
    <button class=btn onclick="fetch('/prev')">◀️ Zurück</button>
    <button class=btn onclick="fetch('/next')">▶️ Weiter</button>
    <button class=btn onclick="fetch('/play?on=1')">▶️ Auto</button>
    <button class=btn onclick="fetch('/play?on=0')">⏸️ Stop</button>
  </div>
  <div style="margin-top:8px">
    <label>Intervall (ms)</label>
    <input id=i type=number min=500 step=100>
    <button class=btn onclick="setInt()">Speichern</button>
  </div>
  <div style="margin-top:8px">
    <label>Crossfade (ms)</label>
    <input id=f type=number min=0 step=50>
    <button class=btn onclick="setFade()">Speichern</button>
  </div>
  <small id=st></small>
</div>

<div class=card>
  <b>Bilder</b>
  <ul id="list"></ul>
</div>

<script>
async function refresh(){
  let r = await fetch('/status'); let s = await r.json();
  document.getElementById('i').value = s.interval;
  document.getElementById('f').value = s.fade;
  document.getElementById('st').innerText = `Autoplay: ${s.autoplay?'AN':'AUS'} | Bilder: ${s.count} | Aktuell: ${s.current}`;
  let ul = document.getElementById('list');
  ul.innerHTML = '';
  s.files.forEach((name,idx)=>{
    let li = document.createElement('li');
    li.innerHTML = `${idx==s.current?'👉 ':''}${name} 
      <button class=btn onclick="fetch('/show?i=${idx}')">Anzeigen</button>
      <button class=btn onclick="fetch('/delete?i=${idx}').then(()=>refresh())">Löschen</button>`;
    ul.appendChild(li);
  });
}
function setInt(){ fetch('/interval?ms='+document.getElementById('i').value).then(refresh); }
function setFade(){ fetch('/fade?ms='+document.getElementById('f').value).then(refresh); }
refresh(); setInterval(refresh, 3000);
</script>
</body></html>
)HTML";

void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/html", PAGE_INDEX); });

  // Upload Bild
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest* r){ r->send(200,"text/plain","OK"); refreshList(); },
    [](AsyncWebServerRequest* r, String fn, size_t index, uint8_t *data, size_t len, bool final){
      if (index==0) {
        if (!SPIFFS.exists("/images")) SPIFFS.mkdir("/images");
        // Normalisiere Endung
        fn.toLowerCase();
        if (!fn.endsWith(".jpg") && !fn.endsWith(".jpeg") && !fn.endsWith(".png")) fn += ".jpg";
        r->_tempFile = SPIFFS.open("/images/"+fn, "w");
      }
      if (r->_tempFile) r->_tempFile.write(data, len);
      if (final && r->_tempFile) r->_tempFile.close();
    });

  // OTA (klassisch, wie bei OTA-only)
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *request){
      bool ok = !Update.hasError();
      request->send(200, "text/plain", ok ? "Update OK, rebooting..." : "Update FAILED");
      if (ok) { delay(300); ESP.restart(); }
    },
    [](AsyncWebServerRequest *request, String fn, size_t index, uint8_t *data, size_t len, bool final){
      if (!index) { Update.begin(UPDATE_SIZE_UNKNOWN); }
      if (Update.write(data, len) != len) { Update.printError(Serial); }
      if (final) { if (!Update.end(true)) Update.printError(Serial); }
    }
  );

  // Steuerung / Status
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* r){
    String json = "{";
    json += "\"autoplay\":"; json += autoplay?"true":"false"; json += ",";
    json += "\"interval\":" + String(intervalMs) + ",";
    json += "\"fade\":" + String(fadeMs) + ",";
    json += "\"count\":" + String(imageCount) + ",";
    json += "\"current\":" + String(currentIndex<0?0:currentIndex) + ",";
    json += "\"files\":[";
    for (int i=0;i<imageCount;i++){ if (i) json+=','; json += "\""+images[i]+"\""; }
    json += "]}";
    r->send(200,"application/json",json);
  });

  server.on("/play", HTTP_GET, [](AsyncWebServerRequest* r){
    bool on = r->hasParam("on") ? (r->getParam("on")->value().toInt()!=0) : true;
    autoplay = on; r->send(200,"text/plain","OK");
  });
  server.on("/interval", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("ms")) intervalMs = max(500, r->getParam("ms")->value().toInt());
    r->send(200,"text/plain","OK");
  });
  server.on("/fade", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("ms")) fadeMs = max(0, r->getParam("ms")->value().toInt());
    r->send(200,"text/plain","OK");
  });
  server.on("/next", HTTP_GET, [](AsyncWebServerRequest* r){ showNext(); r->send(200,"text/plain","OK"); });
  server.on("/prev", HTTP_GET, [](AsyncWebServerRequest* r){ showPrev(); r->send(200,"text/plain","OK"); });
  server.on("/show", HTTP_GET, [](AsyncWebServerRequest* r){
    int i = r->hasParam("i") ? r->getParam("i")->value().toInt() : 0;
    showByIndex(constrain(i,0,max(0,imageCount-1))); r->send(200,"text/plain","OK");
  });
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("i")) {
      int i = r->getParam("i")->value().toInt();
      if (i>=0 && i<imageCount) SPIFFS.remove(images[i]);
      refreshList();
    }
    r->send(200,"text/plain","OK");
  });

  server.begin();
}

// ---------------- Setup / Loop ----------------
void setBacklight(uint8_t duty /*0..255*/) {
  static bool init=false;
  if (!init) { ledcSetup(0, 5000, 8); ledcAttachPin(TFT_LED, 0); init=true; }
  ledcWrite(0, duty);
}

void setup() {
  Serial.begin(115200);
  // Display
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // gewünscht
  tft.fillScreen(COL_BG);
  setBacklight(255);

  // Speicher
  SPIFFS.begin(true);
  if (!SPIFFS.exists("/images")) SPIFFS.mkdir("/images");

  // Framebuffer
  framePrev = (uint16_t*)heap_caps_malloc(TFT_W*TFT_H*2, MALLOC_CAP_8BIT);
  frameNext = (uint16_t*)heap_caps_malloc(TFT_W*TFT_H*2, MALLOC_CAP_8BIT);
  clearBuffer(framePrev, COL_BG);
  clearBuffer(frameNext, COL_BG);
  fillScreenFromBuffer(framePrev);

  // JPG-Decoder Basis
  TJpgDec.setSwapBytes(true); // RGB565 Endian richtig
  // (Callback setzen wir dynamisch in renderImageToNext)

  // WiFi
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(200);
  if (WiFi.status()!=WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Slideshow_ESP","12345678");
  }

  refreshList();
  setupWeb();

  // Start: erstes Bild (falls vorhanden)
  if (imageCount>0) showByIndex(0, false);
}

void loop() {
  if (autoplay && imageCount>0 && millis()-lastSwitch >= intervalMs) {
    showNext(true);
  }
}
