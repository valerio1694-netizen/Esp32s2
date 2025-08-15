/************  ESP32-S2 JPG/PNG-Viewer mit Web-Upload + Autoplay  ************/
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <SPIFFS.h>
#include <Update.h>

#include <TJpg_Decoder.h>   // JPG
#include <PNGdec.h>         // PNG

/**** TFT-Pins (anpassen, falls nötig) ****/
#define TFT_CS    5
#define TFT_RST   6
#define TFT_DC    7
#define TFT_MOSI  11
#define TFT_SCLK  12
#define TFT_LED   13    // PWM-Backlight (LEDC)

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

/**** Display-Parameter ****/
const int16_t TFT_W = 160;
const int16_t TFT_H = 128;

/**** WLAN ****/
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "";   // leer => AP-Fallback
AsyncWebServer server(80);

/**** Autoplay/Demo ****/
bool demoRunning = true;
uint32_t demoIntervalMs = 3000;   // per Web einstellbar
String lastShown = "";            // zuletzt angezeigte Datei

/**** PNG-Decoder Instanz ****/
PNG png;

/**** Backlight (Fade) ****/
void setBacklight(uint8_t level) {
  static bool init = false;
  if (!init) {
    ledcSetup(0 /*chan*/, 5000 /*Hz*/, 8 /*bits*/);
    ledcAttachPin(TFT_LED, 0);
    init = true;
  }
  ledcWrite(0, level);  // 0..255
}

/**** Hilfen ****/
String contentType(const String& path) {
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".bmp")) return "image/bmp";
  if (path.endsWith(".gif")) return "image/gif";
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  return "application/octet-stream";
}

bool isJpg(const String& p){ String s=p; s.toLowerCase(); return s.endsWith(".jpg")||s.endsWith(".jpeg"); }
bool isPng(const String& p){ String s=p; s.toLowerCase(); return s.endsWith(".png"); }

/**** Zeichen-Hilfen ****/
uint16_t rgb888to565(uint8_t r, uint8_t g, uint8_t b){
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

/**** JPG-Callback (TJpg_Decoder) -> zeichnet einen Block auf das TFT ****/
bool tjpgDrawCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap){
  // Clipping
  if (x >= tft.width() || y >= tft.height()) return 1;
  int16_t cw = (x + w > tft.width())  ? (tft.width()  - x) : w;
  int16_t ch = (y + h > tft.height()) ? (tft.height() - y) : h;
  // Fenster setzen und Bitmap schieben
  tft.startWrite();
  tft.setAddrWindow(x, y, cw, ch);
  // Achtung: Adafruit_ST7735 erwartet 16-bit Farben (RGB565)
  tft.pushColors(bitmap, cw*ch, true);
  tft.endWrite();
  return 1;
}

/**** PNG-Callback (PNGdec) – zeichnet Scanlines ****/
int pngDraw(PNGDRAW *pDraw) {
  // Eine Zeile dekodieren
  uint8_t lineBuf[PNG_MAX_BUFFERED_PIXELS*3]; // RGB888
  png.getLineAsRGB(pDraw, lineBuf, PNG_RGB_TRUECOLOR);
  // Zu RGB565 konvertieren (in kleinen Blöcken) und ausgeben
  int16_t y = pDraw->y;
  int16_t x = 0;
  int16_t npx = pDraw->iWidth;
  // Fenster setzen
  tft.startWrite();
  tft.setAddrWindow( (tft.width()-pDraw->iWidth)/2, (tft.height()-pDraw->pPNG->iHeight)/2 + y, npx, 1 );
  // Zeile in RGB565 umwandeln
  static uint16_t row565[PNG_MAX_BUFFERED_PIXELS];
  for (int i=0, j=0; i<npx; ++i, j+=3){
    row565[i] = rgb888to565(lineBuf[j], lineBuf[j+1], lineBuf[j+2]);
  }
  tft.pushColors(row565, npx, true);
  tft.endWrite();
  return 1;
}

/**** „Fit into screen“: Berechnet TJpg-Skalierung 1/2/4/8 und Zielposition ****/
struct Fit { int scale; int x; int y; };
Fit fitJpg(int imgW, int imgH){
  int s = 1;
  while ((imgW/s > TFT_W) || (imgH/s > TFT_H)) {
    s *= 2;
    if (s >= 8) break;
  }
  int w = imgW / s;
  int h = imgH / s;
  int x = (TFT_W - w) / 2;
  int y = (TFT_H - h) / 2;
  if (x < 0) x = 0; if (y < 0) y = 0;
  return {s, x, y};
}

/**** Übergänge ****/
void fadeIn(uint8_t from=0, uint8_t to=255, uint16_t ms=300){
  uint32_t t0 = millis();
  setBacklight(from);
  while (true){
    uint32_t dt = millis()-t0;
    if (dt >= ms) break;
    uint8_t v = from + (uint32_t)(to-from) * dt / ms;
    setBacklight(v);
    delay(5);
  }
  setBacklight(to);
}

void fadeOut(uint8_t from=255, uint8_t to=0, uint16_t ms=300){
  uint32_t t0 = millis();
  setBacklight(from);
  while (true){
    uint32_t dt = millis()-t0;
    if (dt >= ms) break;
    uint8_t v = from - (uint32_t)(from-to) * dt / ms;
    setBacklight(v);
    delay(5);
  }
  setBacklight(to);
}

/**** Slide-Effect: Zeichnet Bild außerhalb & schiebt rein ****/
void slideInFromRight(std::function<void(int16_t xOff)> drawAt, uint16_t ms=300){
  int steps = 12;
  for (int i=0;i<=steps;i++){
    int16_t xOff = (TFT_W) - (int32_t)TFT_W * i / steps;
    tft.fillScreen(ST77XX_BLACK);
    drawAt(xOff);
    delay(ms/steps);
  }
}

/**** Datei-Anzeige (JPG/PNG) mit Effekt ****/
bool drawFile(const String& path, const String& effect){
  if (!SPIFFS.exists(path)) return false;

  // Vorbereitung
  tft.fillScreen(ST77XX_BLACK);

  if (isJpg(path)) {
    // Größe ermitteln
    uint16_t w,h;
    if (TJpgDec.getJpgSize(&w, &h, path.c_str()) != TJPG_OK) return false;
    Fit f = fitJpg(w, h);
    TJpgDec.setJpgScale(f.scale);
    TJpgDec.setCallback(tjpgDrawCallback);

    auto drawAt = [&](int16_t xOff){
      // Ziel-Fenster durch Offset verschieben
      // TJpg_Decoder zeichnet absolute Koordinaten, also setzen wir einen globalen Offset
      // -> einfacher Trick: Wir verschieben „x“ beim StartDraw
      // Leider bietet die Lib dafür kein Offset – daher setzen wir die Startposition via setSwapBytes + setJpgScale und lassen
      // das Bild direkt an (f.x+xOff, f.y) zeichnen:
      TJpgDec.drawFsJpg(f.x + xOff, f.y, path.c_str());
    };

    if (effect=="fade") {
      setBacklight(0);
      drawAt(0);
      fadeIn();
    } else if (effect=="slide") {
      slideInFromRight(drawAt);
    } else {
      drawAt(0);
    }
    lastShown = path;
    return true;
  }

  if (isPng(path)) {
    int16_t rc = png.open(path.c_str(), [] (const char* fn)->File { return SPIFFS.open(fn, "r"); });
    if (rc != PNG_SUCCESS) return false;

    // Bild wird mittig gezeichnet in pngDraw()
    auto drawAt = [&](int16_t xOff){
      // Für Slide brauchen wir x-Offset:
      // Spart RAM: Wir „simulieren“ den Offset, indem wir die TFT-Rotation auf Standard lassen
      // und vor jedem png.decode() den Bildschirm leeren, dann nach dem Decode nochmal die Zeile
      // kopieren – das ist zu schwergewichtig. Daher unterstützen wir Slide für PNG nicht; nutzen Fade:
      png.decode(NULL, 0);  // ruft pngDraw() für jede Zeile auf
      png.close();
    };

    if (effect=="fade") {
      setBacklight(0);
      drawAt(0);
      fadeIn();
    } else {
      drawAt(0);
    }
    lastShown = path;
    return true;
  }

  return false;
}

/**** HTML: Startseite ****/
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP Image Viewer</title>
<style>
 body{font-family:system-ui,Arial;margin:20px;background:#111;color:#eee}
 h1{font-size:20px;margin:0 0 10px}
 .card{background:#1b1b1b;border-radius:10px;padding:12px;margin:10px 0}
 label{display:block;margin:8px 0 4px;color:#aaa}
 input[type=file]{width:100%}
 button,select{padding:8px 12px;border-radius:8px;border:0;background:#2d6cdf;color:#fff}
 .row{display:flex;gap:10px;flex-wrap:wrap}
 .row>*{flex:1}
 code{color:#9fe}
 .muted{color:#888}
</style>
</head><body>
<h1>ESP32-S2: JPG/PNG hochladen & anzeigen</h1>

<div class=card>
  <form id=up method=POST action="/upload" enctype="multipart/form-data">
    <label>Bild (JPG oder PNG)</label>
    <input type=file name="img" accept=".jpg,.jpeg,.png" required>
    <div class=row>
      <div><label>Effekt</label>
        <select name="effect">
          <option value="none">ohne</option>
          <option value="fade">Fade</option>
          <option value="slide">Slide (nur JPG)</option>
        </select>
      </div>
      <div><label>&nbsp;</label>
        <button>Hochladen & Anzeigen</button>
      </div>
    </div>
  </form>
  <div class=muted>Datei wird in <code>/spiffs</code> gespeichert.</div>
</div>

<div class=card>
  <div class=row>
    <div>
      <label>Autoplay Intervall (ms)</label>
      <input id=ms type=number value="3000" style="width:100%;padding:8px;border-radius:8px;border:0;">
    </div>
    <div style="align-self:end">
      <button onclick="fetch('/demo?cmd=start&ms='+ms.value).then(()=>location.reload())">Autoplay Start</button>
      <button onclick="fetch('/demo?cmd=stop').then(()=>location.reload())">Stop</button>
    </div>
  </div>
</div>

<div class=card>
  <h3>Dateien</h3>
  <div id=list class=muted>Lade…</div>
</div>

<script>
fetch('/list').then(r=>r.json()).then(a=>{
  if(!a.length){list.innerHTML='(keine Dateien)';return}
  list.innerHTML = a.map(f=>`<div>
    <code>${f}</code>
    <button onclick="fetch('/show?file=${encodeURIComponent(f)}&effect=fade')">Zeigen</button>
    <button onclick="fetch('/delete?file=${encodeURIComponent(f)}').then(()=>location.reload())">Löschen</button>
  </div>`).join('');
});
</script>
</body></html>
)HTML";

/**** Datei-Listing ****/
void handleList(AsyncWebServerRequest *r){
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.to<JsonArray>();
  File root = SPIFFS.open("/");
  File f;
  while ( (f = root.openNextFile()) ){
    String name = String(f.name());
    if (isJpg(name) || isPng(name)) arr.add(name);
    f.close();
  }
  String out; serializeJson(arr,out);
  r->send(200,"application/json",out);
}

/**** Setup ****/
void setup(){
  pinMode(TFT_LED, OUTPUT);
  setBacklight(0);  // sanft einschalten nach init

  Serial.begin(115200);

  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS mount failed");
  }

  // TFT
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);        // <== wie gewünscht
  tft.fillScreen(ST77XX_BLACK);
  setBacklight(200);

  // WiFi
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0=millis();
  while(strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(100);
  if(WiFi.status()!=WL_CONNECTED){
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP_Image_Viewer","12345678");
  }
  Serial.print("IP: "); Serial.println( (WiFi.getMode()==WIFI_AP)? WiFi.softAPIP() : WiFi.localIP() );

  // Webserver
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send_P(200,"text/html",INDEX_HTML);
  });

  // Upload (JPG/PNG) + Anzeige mit Effekt
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *r){
      // Abschluss – wurde im Body gespeichert/gezeigt
      r->send(200,"text/plain","OK");
    },
    [](AsyncWebServerRequest *r, String fn, size_t index, uint8_t *data, size_t len, bool final){
      String effect = "none";
      if (r->hasParam("effect", true)) effect = r->getParam("effect", true)->value();

      if (index==0){
        // Zielname: /bild_*.ext
        String ext = fn.substring(fn.lastIndexOf('.')); ext.toLowerCase();
        if (ext!=".jpg" && ext!=".jpeg" && ext!=".png") ext=".jpg";
        fn = "/img_" + String((uint32_t)millis()) + ext;
        r->_tempFile = SPIFFS.open(fn, "w");
        r->addInterestingHeader("filename");
        r->addInterestingHeader("Content-Type");
        r->tempObject = new String(fn); // merken
      }
      if (r->_tempFile) r->_tempFile.write(data, len);
      if (final){
        if (r->_tempFile) r->_tempFile.close();
        String saved = *(String*)r->tempObject;
        delete (String*)r->tempObject;

        // anzeigen
        if (!drawFile(saved, effect)) {
          r->send(500,"text/plain","Decoding error");
          return;
        }
        r->send(200,"text/plain", "Saved & shown: " + saved);
      }
    }
  );

  // Datei anzeigen per GET
  server.on("/show", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!r->hasParam("file")) { r->send(400,"text/plain","missing file"); return; }
    String f = r->getParam("file")->value();
    String effect = r->hasParam("effect") ? r->getParam("effect")->value() : "none";
    bool ok = drawFile(f, effect);
    r->send(ok?200:404, "text/plain", ok?"OK":"Not found/decoding error");
  });

  // Liste
  server.on("/list", HTTP_GET, handleList);

  // Löschen
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!r->hasParam("file")) { r->send(400,"text/plain","missing file"); return; }
    String f = r->getParam("file")->value();
    bool ok = SPIFFS.remove(f);
    r->send(ok?200:404, "text/plain", ok?"Deleted":"Not found");
  });

  // Demo/Autoplay steuern
  server.on("/demo", HTTP_GET, [](AsyncWebServerRequest *r){
    String cmd = r->hasParam("cmd") ? r->getParam("cmd")->value() : "";
    if (cmd=="start"){
      demoRunning = true;
      if (r->hasParam("ms")) demoIntervalMs = max(500, r->getParam("ms")->value().toInt());
      r->send(200,"text/plain","demo started");
    } else if (cmd=="stop"){
      demoRunning = false;
      r->send(200,"text/plain","demo stopped");
    } else {
      r->send(400,"text/plain","use /demo?cmd=start&ms=3000 or /demo?cmd=stop");
    }
  });

  // OTA (gleiches, simples wie in deiner OTA-Only-Version)
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *r){
      bool ok = !Update.hasError();
      r->send(200,"text/plain", ok? "Update OK, rebooting…" : "Update FAILED");
      if (ok) { delay(300); ESP.restart(); }
    },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
      if (!index) { Update.begin(UPDATE_SIZE_UNKNOWN); }
      if (Update.write(data, len) != len) { Update.printError(Serial); }
      if (final) { if (!Update.end(true)) Update.printError(Serial); }
    }
  );

  server.begin();

  // Startbild / Hinweis
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(2,2);
  tft.print("Upload im Browser:");
  tft.setCursor(2,12);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  IPAddress ip = (WiFi.getMode()==WIFI_AP)? WiFi.softAPIP() : WiFi.localIP();
  tft.print(ip);
}

/**** Einfacher Autoplay-Loop über alle Dateien ****/
uint32_t tLast = 0;
void loop(){
  if (!demoRunning) return;

  if (millis() - tLast >= demoIntervalMs){
    tLast = millis();
    // Nächste Bilddatei suchen
    File root = SPIFFS.open("/");
    File f;
    bool showNext = false;
    String first = "", next = "";

    while ( (f = root.openNextFile()) ){
      String name = String(f.name());
      f.close();
      if (!(isJpg(name) || isPng(name))) continue;
      if (first=="") first = name;
      if (showNext){ next = name; break; }
      if (name == lastShown) showNext = true;
    }
    if (next=="") next = (lastShown=="" ? first : first); // roll-over
    if (next!="") drawFile(next, "fade");
  }
}
