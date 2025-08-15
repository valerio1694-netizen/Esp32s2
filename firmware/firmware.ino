/*
  ESP32-S2 Bildrahmen / Slideshow (Projekt 8)
  - ST7735 160x128, Rotation = 1
  - Pins: CS=5, RST=6, DC=7, MOSI=11, SCLK=12, LED=13 (PWM-Dimmen)
  - JPG + PNG aus SPIFFS(/img)
  - Web-UI: Upload, Liste, Autoplay/Intervall/Fade, Helligkeits-Slider, OTA
  - Echtes Crossfade: zeilenweise Blend (kein 3. Vollpuffer nötig)
  - Große Puffer dynamisch (heap_caps_malloc) → kein .bss Overflow
  - AP läuft dauerhaft (AP+STA), SSID: ESP-Frame
  - Getestet mit ESP32 Core 2.0.17 + Adafruit/Async/TJpg/PNGdec
*/

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include <TJpg_Decoder.h>
#include <PNGdec.h>
#include <algorithm>
#include <esp_heap_caps.h>   // heap_caps_malloc

// -------------------- TFT-Pins --------------------
#define TFT_CS    5
#define TFT_RST   6
#define TFT_DC    7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13   // Backlight (PWM via LEDC)

static const int TFT_W = 160;
static const int TFT_H = 128;

// Software-SPI (MOSI/SCLK wie oben)
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// -------------------- Backlight / LEDC -------------------------
static void setBacklight(uint8_t level) {
  static bool init = false;
  if (!init) {
    ledcSetup(0 /*channel*/, 5000 /*Hz*/, 8 /*bits*/);
    ledcAttachPin(TFT_LED, 0);
    init = true;
  }
  ledcWrite(0, level); // 0..255
}

// -------------------- Netzwerk -------------------------------
const char* STA_SSID = "Peng";
const char* STA_PASS = "";           // leer => es wird trotzdem AP gestartet (AP+STA)
const char* AP_SSID  = "ESP-Frame";
const char* AP_PASS  = "12345678";

AsyncWebServer server(80);

// -------------------- Slideshow/Buffer ------------------------
static uint16_t* fbCurr = nullptr;
static uint16_t* fbNext = nullptr;

bool autoplay = false;       // true, wenn intervalMs > 0
long intervalMs = 0;         // 0 = Demo
long fadeMs     = 600;       // Crossfade ms
unsigned long lastSwitch = 0;
int currentIndex = -1;

String images[128];
int imageCount = 0;

uint8_t brightness = 255;    // 0..255

// -------------------- Decoder -----------------------
PNG png;

// Ziel-Kontext für JPG-Callback
struct BlitCtx {
  uint16_t* dst;
  int dstW, dstH;
  int ox, oy;
} jpgCtx;

// -------------------- Helfer -------------------------------
static void blitFull(const uint16_t* buf) {
  tft.drawRGBBitmap(0, 0, buf, TFT_W, TFT_H);
}

static void clearFB(uint16_t* fb, uint16_t color = 0x0000) {
  for (int i = 0; i < TFT_W * TFT_H; ++i) fb[i] = color;
}

// Zeilenweises Crossfade -> nur kleiner Line-Buffer nötig
static void crossfade(uint8_t alpha /*0..255*/) {
  static uint16_t mixLine[TFT_W];
  for (int y = 0; y < TFT_H; ++y) {
    uint16_t* row0 = fbCurr + y * TFT_W;
    uint16_t* row1 = fbNext + y * TFT_W;
    for (int x = 0; x < TFT_W; ++x) {
      uint16_t c0 = row0[x];
      uint16_t c1 = row1[x];

      uint16_t r0 = (c0 >> 11) & 0x1F;
      uint16_t g0 = (c0 >> 5)  & 0x3F;
      uint16_t b0 =  c0        & 0x1F;

      uint16_t r1 = (c1 >> 11) & 0x1F;
      uint16_t g1 = (c1 >> 5)  & 0x3F;
      uint16_t b1 =  c1        & 0x1F;

      uint16_t r = ( ((r0 * (255 - alpha)) + (r1 * alpha)) >> 8 );
      uint16_t g = ( ((g0 * (255 - alpha)) + (g1 * alpha)) >> 8 );
      uint16_t b = ( ((b0 * (255 - alpha)) + (b1 * alpha)) >> 8 );

      mixLine[x] = (r << 11) | (g << 5) | b;
    }
    tft.drawRGBBitmap(0, y, mixLine, TFT_W, 1);
  }
}

// ================= JPG (TJpg_Decoder) =========================
// Korrekte Signatur für v1.1.0 (w/h als uint16_t)
static bool jpgToBuffer(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  int dstW = jpgCtx.dstW, dstH = jpgCtx.dstH;
  int ox = jpgCtx.ox, oy = jpgCtx.oy;

  for (uint16_t row = 0; row < h; ++row) {
    int dy = y + oy + row;
    if (dy < 0 || dy >= dstH) continue;
    uint16_t* dline = jpgCtx.dst + dy * dstW;

    int sx = 0, dx = x + ox;
    if (dx < 0) { sx = -dx; dx = 0; }
    int copy = std::min<int>(w - sx, dstW - dx);
    if (copy > 0) {
      memcpy(dline + dx, bmp + row * w + sx, copy * sizeof(uint16_t));
    }
  }
  return true;
}

static bool renderJPGtoNext(const String& path) {
  if (!SPIFFS.exists(path)) return false;
  clearFB(fbNext, 0);

  jpgCtx = { fbNext, TFT_W, TFT_H, 0, 0 };
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(jpgToBuffer);

  JRESULT rc = TJpgDec.drawFsJpg(0, 0, path);
  return (rc == JDR_OK);
}

// ================= PNG (PNGdec 1.1.4) ========================
// FS-Handle für PNGdec
typedef struct { File f; } PNGFILEX;

static void* pngOpen(const char* fname, int32_t* size) {
  PNGFILEX* p = (PNGFILEX*)malloc(sizeof(PNGFILEX));
  if (!p) return nullptr;
  p->f = SPIFFS.open(fname, "r");
  if (!p->f) { free(p); return nullptr; }
  *size = p->f.size();
  return p;
}
static void pngClose(void* handle) {
  PNGFILEX* p = (PNGFILEX*)handle;
  if (p) { if (p->f) p->f.close(); free(p); }
}
static int32_t pngRead(PNGFILE* file, uint8_t* buf, int32_t len) {
  PNGFILEX* p = (PNGFILEX*)file->fHandle;
  return p->f.read(buf, len);
}
static int32_t pngSeek(PNGFILE* file, int32_t pos) {
  PNGFILEX* p = (PNGFILEX*)file->fHandle;
  p->f.seek(pos);
  return pos;
}

// Draw-Callback: liefert eine Zeile als RGB565.
// PNGdec 1.1.4 hat kein pDraw->x -> wir beginnen bei x=0.
static int pngDraw(PNGDRAW* pDraw) {
  static uint16_t line[TFT_W];
  png.getLineAsRGB565(pDraw, line, PNG_RGB565_BIG_ENDIAN, 0xFFFF);

  int y = pDraw->y;
  if (y < 0 || y >= TFT_H) return 1;

  int w = pDraw->iWidth;
  if (w > TFT_W) w = TFT_W;

  memcpy(&fbNext[y * TFT_W], line, w * 2);
  return 1;
}

static bool renderPNGtoNext(const String& path) {
  if (!SPIFFS.exists(path)) return false;
  clearFB(fbNext, 0);

  int rc = png.open(path.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDraw);
  if (rc != PNG_SUCCESS) return false;
  rc = png.decode(NULL, 0);
  png.close();
  return (rc == PNG_SUCCESS);
}

// ================= High-Level Loader ==========================
static bool renderImageToNext(const String& path) {
  String p = path; p.toLowerCase();
  if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return renderJPGtoNext(path);
  if (p.endsWith(".png"))                       return renderPNGtoNext(path);
  return false;
}

// ================= Datei-Liste / SPIFFS =======================
static void refreshList() {
  imageCount = 0;
  File dir = SPIFFS.open("/img");
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f && imageCount < (int)(sizeof(images)/sizeof(images[0]))) {
    String name = String("/img/") + String(f.name()).substring(5);
    String low = name; low.toLowerCase();
    if (low.endsWith(".jpg") || low.endsWith(".jpeg") || low.endsWith(".png")) {
      images[imageCount++] = name;
    }
    f = dir.openNextFile();
  }
}

static String listJSON() {
  String out = F("[");
  for (int i = 0; i < imageCount; ++i) {
    if (i) out += ',';
    out += '\"'; out += images[i]; out += '\"';
  }
  out += ']';
  return out;
}

// ================= Demo-Animation =============================
static void renderDemoFrame(float phase) {
  // einfache Sinus-Linie
  memset(fbNext, 0, TFT_W * TFT_H * 2);
  for (int x = 0; x < TFT_W; ++x) {
    float t = (x / 20.0f) + phase;
    int y = (int)((sinf(t) * 0.4f + 0.5f) * (TFT_H - 1));
    if (y < 0) y = 0; if (y >= TFT_H) y = TFT_H - 1;
    fbNext[y * TFT_W + x] = 0xFFE0; // Gelb
  }
}

// ================= Web-UI (inkl. OTA + Brightness) ============
static const char* INDEX_HTML = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP Frame</title>
<style>body{font-family:system-ui;margin:16px}label{display:block;margin:.5em 0}input,button{font-size:1rem} .row{display:flex;gap:12px;align-items:center}</style>
<h1>ESP32-S2 Bildrahmen</h1>
<section>
<h2>Upload (JPG/PNG)</h2>
<form id="up" method="POST" action="/upload" enctype="multipart/form-data">
<input type="file" name="file" accept=".jpg,.jpeg,.png" required>
<button>Hochladen</button>
</form>
</section>
<section>
<h2>Steuerung</h2>
<label>Intervall (ms): <input id="ival" type="number" min="0" step="100" value="0"></label>
<label>Fade (ms): <input id="fade" type="number" min="0" step="50" value="600"></label>
<label class="row"><input id="auto" type="checkbox"> <span>Autoplay</span></label>
<div class="row">
  <label for="br">Helligkeit:</label>
  <input id="br" type="range" min="0" max="255" value="255" oninput="brv.value=this.value" onchange="setBright(this.value)">
  <output id="brv">255</output>
</div>
<button onclick="apply()">Übernehmen</button>
</section>
<section>
<h2>Dateien</h2>
<pre id="list">(laden…)</pre>
</section>
<hr>
<section>
<h2>OTA Update</h2>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin" required>
<button>Firmware flashen</button>
</form>
</section>
<script>
async function loadList(){
  let r = await fetch('/api/list'); let j = await r.json();
  list.textContent = j.join('\\n');
  try {
    let s = await fetch('/api/status'); let js = await s.json();
    if (js && typeof js.brightness === 'number') {
      br.value = js.brightness; brv.value = js.brightness;
      ival.value = js.interval; fade.value = js.fade; auto.checked = !!js.autoplay;
    }
  } catch(e){}
}
async function apply(){
  let ms=+ival.value||0, fd=+fade.value||0, au=auto.checked?1:0;
  await fetch(`/api/set?interval=${ms}&fade=${fd}&auto=${au}`);
  alert('OK');
}
async function setBright(v){
  await fetch(`/api/bright?lvl=${v}`);
}
loadList();
</script>
)HTML";

static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/html", INDEX_HTML); });

  server.on("/api/list", HTTP_GET, [](AsyncWebServerRequest* r){
    refreshList();
    r->send(200, "application/json", listJSON());
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r){
    String j = "{\"interval\":" + String(intervalMs)
             + ",\"fade\":" + String(fadeMs)
             + ",\"autoplay\":" + String(autoplay ? 1 : 0)
             + ",\"brightness\":" + String(brightness) + "}";
    r->send(200, "application/json", j);
  });

  server.on("/api/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("interval")) {
      long v = r->getParam("interval")->value().toInt();
      intervalMs = std::max<long>(0, (long)v);
      autoplay = intervalMs > 0;
    }
    if (r->hasParam("fade")) {
      long v = r->getParam("fade")->value().toInt();
      fadeMs = std::max<long>(0, (long)v);
    }
    if (r->hasParam("auto")) {
      autoplay = r->getParam("auto")->value() == "1";
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/api/bright", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("lvl")) {
      int v = r->getParam("lvl")->value().toInt();
      if (v < 0) v = 0; if (v > 255) v = 255;
      brightness = (uint8_t)v;
      setBacklight(brightness);
    }
    r->send(200, "text/plain", "OK");
  });

  // Upload -> /img/
  server.on(
    "/upload", HTTP_POST,
    [](AsyncWebServerRequest* r){ r->send(200, "text/plain", "OK"); },
    [](AsyncWebServerRequest* r, String fn, size_t idx, uint8_t* data, size_t len, bool fin){
      static File up;
      if (idx == 0) {
        SPIFFS.mkdir("/img");
        String path = "/img/" + fn;
        up = SPIFFS.open(path, "w");
      }
      if (up) up.write(data, len);
      if (fin && up) { up.close(); }
    }
  );

  // OTA Update
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest* r){
      bool ok = !Update.hasError();
      r->send(200, "text/plain", ok ? "Update OK. Rebooting…" : "Update FAILED!");
      delay(500);
      if (ok) ESP.restart();
    },
    [](AsyncWebServerRequest* r, String fn, size_t idx, uint8_t* data, size_t len, bool fin){
      if (idx == 0) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); }
      }
      if (len) {
        if (Update.write(data, len) != len) { Update.printError(Serial); }
      }
      if (fin) { if (!Update.end(true)) Update.printError(Serial); }
    }
  );

  server.begin();
}

// -------------------- Setup / Loop ---------------------------
// Neuer Start: AP läuft IMMER, STA wird parallel versucht.
// AP bleibt aktiv – auch wenn STA verbindet.
static void wifiStart() {
  WiFi.persistent(false);       // nicht ins NVS schreiben
  WiFi.disconnect(true, true);  // alte Verbindungen + Config aus RAM löschen
  delay(100);

  WiFi.mode(WIFI_AP_STA);

  // AP immer starten (2.4 GHz, Kanal 1)
  bool apOK = WiFi.softAP(AP_SSID, AP_PASS, 1 /*channel*/, 0 /*hidden*/, 4 /*max conn*/);
  Serial.printf("AP start: %s | AP-IP: %s\n", apOK ? "OK" : "FAIL", WiFi.softAPIP().toString().c_str());

  // STA optional verbinden, falls Credentials vorhanden
  if (strlen(STA_SSID) > 0 && strlen(STA_PASS) > 0) {
    WiFi.begin(STA_SSID, STA_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
      delay(200);
    }
    Serial.printf("STA status: %d, IP: %s\n", WiFi.status(), WiFi.localIP().toString().c_str());
  } else {
    Serial.println("STA skipped (keine Credentials gesetzt).");
  }
}

void setup() {
  Serial.begin(115200);

  // Display
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  setBacklight(brightness);

  // FS
  SPIFFS.begin(true);
  SPIFFS.mkdir("/img");

  // Framebuffer dynamisch anfordern (8-Bit Heap)
  fbCurr = (uint16_t*) heap_caps_malloc(TFT_W * TFT_H * 2, MALLOC_CAP_8BIT);
  fbNext = (uint16_t*) heap_caps_malloc(TFT_W * TFT_H * 2, MALLOC_CAP_8BIT);
  if (!fbCurr || !fbNext) {
    if (!fbCurr) fbCurr = (uint16_t*) heap_caps_malloc(TFT_W * TFT_H * 2, MALLOC_CAP_8BIT);
    if (!fbCurr) { while (1) { Serial.println("RAM-Allocation failed"); delay(1000);} }
    fbNext = fbCurr; // Crossfade später überspringen, falls nur ein Buffer
  }

  // Decoder-Init
  TJpgDec.setSwapBytes(true);

  // Netzwerk + Web
  wifiStart();
  setupWeb();
  refreshList();

  clearFB(fbCurr, 0);
  clearFB(fbNext, 0);
  blitFull(fbCurr);
}

void loop() {
  static float demoPhase = 0.0f;

  // Demo-Mode, solange kein Intervall/kein Bild
  if (!autoplay || intervalMs <= 0 || imageCount == 0) {
    renderDemoFrame(demoPhase);
    demoPhase += 0.08f;

    if (fbNext != fbCurr) {
      unsigned long t0 = millis();
      while (millis() - t0 < 100) {
        uint8_t a = (uint8_t)(((millis() - t0) * 255) / 100);
        if (a > 255) a = 255;
        crossfade(a);
        delay(10);
      }
    }
    blitFull(fbNext);
    if (fbNext != fbCurr) std::swap(fbCurr, fbNext);
    delay(60);
    return;
  }

  // Autoplay
  unsigned long now = millis();
  if (lastSwitch == 0 || now - lastSwitch >= (unsigned long)intervalMs) {
    lastSwitch = now;
    currentIndex = (currentIndex + 1) % imageCount;
    String path = images[currentIndex];
    bool ok = renderImageToNext(path);
    if (!ok) return;

    if (fbNext != fbCurr) {
      unsigned long t0 = millis();
      unsigned long dur = (unsigned long)std::max<long>(0, fadeMs);
      if (dur == 0) {
        blitFull(fbNext);
      } else {
        while (millis() - t0 < dur) {
          uint8_t a = (uint8_t)(((millis() - t0) * 255) / dur);
          if (a > 255) a = 255;
          crossfade(a);
          delay(10);
        }
        blitFull(fbNext);
      }
      std::swap(fbCurr, fbNext);
    } else {
      blitFull(fbCurr);
    }
  }

  delay(5);
}
