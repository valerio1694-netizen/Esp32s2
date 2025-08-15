/***** ESP32-S2 – Slideshow + Web UI + OTA + PNG/JPG->RAW (Client) + BMP->RAW (Server optional) *****/
#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

#include <vector>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

/* ===== Hardware-Pins ===== */
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13
#define TFT_ROTATION 1    // 0..3 (1 = 180° gedreht im Querformat, bei dir meist 1/3)

/* ===== WLAN ===== */
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT";      // leer => AP-Fallback
const char* AP_SSID   = "Slideshow_OTA";
const char* AP_PASS   = "12345678";

AsyncWebServer server(80);

/* ===== Display + Backlight ===== */
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
void setBacklight(uint8_t percent) {
  static bool init=false;
  if(!init){ ledcSetup(0, 5000, 8); ledcAttachPin(TFT_LED, 0); init=true; }
  if(percent>100) percent=100;
  ledcWrite(0, map(percent,0,100,0,255));
}

/* ===== Slideshow-State ===== */
String currentFile = "";
bool slideRunning  = true;
uint32_t slideMs   = 3000;
uint32_t tNext     = 0;

const char* IMG_DIR = "/img";

/* ===== Utils ===== */
String htmlEsc(String s){ s.replace("&","&amp;"); s.replace("<","&lt;"); s.replace(">","&gt;");
  s.replace("\"","&quot;"); s.replace("'","&#39;"); return s; }

std::vector<String> listRaw() {
  std::vector<String> v;
  if(!SPIFFS.exists(IMG_DIR)) SPIFFS.mkdir(IMG_DIR);
  File dir = SPIFFS.open(IMG_DIR);
  if(!dir || !dir.isDirectory()) return v;
  for(File f = dir.openNextFile(); f; f = dir.openNextFile()){
    String n = f.name();
    if(!f.isDirectory() && (n.endsWith(".raw")||n.endsWith(".RAW"))) v.push_back(n);
  }
  return v;
}

String nextFile(const std::vector<String>& v, const String& cur){
  if(v.empty()) return "";
  if(cur=="") return v.front();
  for(size_t i=0;i<v.size();++i) if(v[i]==cur) return v[(i+1)%v.size()];
  return v.front();
}

bool drawRAW(const char* path){
  File f = SPIFFS.open(path, "r");
  if(!f) return false;
  const uint16_t W=160, H=128;
  tft.startWrite();
  tft.setAddrWindow(0,0,W,H);
  static uint16_t line[W];
  for(uint16_t y=0;y<H;y++){
    size_t got = f.read((uint8_t*)line, W*2);
    if(got != W*2){ tft.endWrite(); f.close(); return false; }
    tft.pushColors(line, W, (y==0));
  }
  tft.endWrite();
  f.close();
  return true;
}

/* ===== BMP->RAW (Server-seitig, 24-bit, unkomprimiert) ===== */
static uint16_t rd16(File &f){ uint16_t r; f.read((uint8_t*)&r,2); return r; }
static uint32_t rd32(File &f){ uint32_t r; f.read((uint8_t*)&r,4); return r; }

bool bmp24_to_raw565(const String& bmpPath, const String& rawPath){
  File bmp = SPIFFS.open(bmpPath, "r");
  if(!bmp) return false;
  if(rd16(bmp)!=0x4D42){ bmp.close(); return false; } // 'BM'
  (void)rd32(bmp); (void)rd32(bmp);
  uint32_t startPos = rd32(bmp);
  uint32_t hdrSize  = rd32(bmp);
  if(hdrSize != 40){ bmp.close(); return false; }
  int32_t  srcW = (int32_t)rd32(bmp);
  int32_t  srcH = (int32_t)rd32(bmp);
  if(rd16(bmp)!=1){ bmp.close(); return false; }
  uint16_t depth = rd16(bmp);
  uint32_t comp  = rd32(bmp);
  if(depth!=24 || comp!=0){ bmp.close(); return false; }
  bool flip = (srcH>0); if(srcH<0) srcH = -srcH;
  uint32_t rowSize = (srcW*3 + 3) & ~3;

  // Ziel ist 160x128, letterboxed (schwarz)
  const int DSTW=160, DSTH=128;
  float sx = (float)DSTW/srcW, sy=(float)DSTH/srcH, s=min(sx,sy);
  int dstW=max(1,(int)(srcW*s)), dstH=max(1,(int)(srcH*s));
  int x0=(DSTW-dstW)/2, y0=(DSTH-dstH)/2;

  File raw = SPIFFS.open(rawPath, "w");
  if(!raw){ bmp.close(); return false; }

  // Zwischenspeicher
  std::unique_ptr<uint8_t[]> row(new uint8_t[rowSize]);
  std::unique_ptr<uint16_t[]> out(new uint16_t[DSTW]);

  // vorberechnete Maps
  static uint16_t xmap[160], ymap[128];
  for(int x=0;x<dstW;x++) xmap[x]=(uint32_t)x*srcW/max(1,dstW);
  for(int y=0;y<dstH;y++) ymap[y]=(uint32_t)y*srcH/max(1,dstH);

  for(int y=0;y<DSTH;y++){
    // standard: schwarz
    for(int x=0;x<DSTW;x++) out[x]=0x0000;

    if(y>=y0 && y<y0+dstH){
      int syy=ymap[y-y0];
      uint32_t pos = startPos + (flip?(srcH-1-syy):syy)*rowSize;
      if(bmp.position()!=pos) bmp.seek(pos);
      bmp.read(row.get(), rowSize);

      for(int x=0;x<dstW;x++){
        int sxx=xmap[x]; int p=sxx*3;
        uint8_t B=row[p+0], G=row[p+1], R=row[p+2];
        uint16_t c = ((R&0xF8)<<8) | ((G&0xFC)<<3) | (B>>3);
        out[x0+x] = c;
      }
    }
    raw.write((uint8_t*)out.get(), DSTW*2);
  }
  bmp.close(); raw.close();
  return true;
}

/* ===== OTA-Seiten ===== */
String pageOTA() {
  String ip = (WiFi.getMode()==WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  return String(F(
  "<!doctype html><html><head><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<style>body{font-family:system-ui,Arial;background:#111;color:#eee;margin:24px}"
  "form{background:#1b1b1b;padding:16px;border-radius:12px}"
  "input[type=file]{display:block;margin:12px 0;color:#eee}"
  "button{padding:8px 12px;border:0;border-radius:8px;background:#0a84ff;color:#fff}"
  "a{color:#0a84ff}</style></head><body><h2>OTA-Update</h2>"
  )) + "<p>IP: "+ip+"</p>"
  + "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<label>Firmware (.bin)</label><input type='file' name='firmware'>"
    "<button>Flash</button>"
  "</form><p><a href='/'>Zurück</a></p></body></html>";
}

/* ===== Index (mit Client-Konvertierung PNG/JPG->RAW) ===== */
String pageIndex() {
  auto files = listRaw();
  String list="";
  for(auto &n: files){
    String esc=htmlEsc(n);
    list += "<li>"+esc+" <a href='/show?f="+esc+"'>[anzeigen]</a> <a href='/delete?f="+esc+"' onclick='return confirm(\"Löschen?\")'>[löschen]</a></li>";
  }
  if(list=="") list="<li>(keine .raw in /img)</li>";

  String btn = slideRunning? "<a class=btn href='/slideshow?cmd=stop'>⏸ Stop</a>"
                           : "<a class=btn href='/slideshow?cmd=start'>▶ Start</a>";

  return String(F(
"<!doctype html><html><head><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>"
"body{font-family:system-ui,Segoe UI,Arial;margin:16px;background:#111;color:#eee}"
"h1 small{font-weight:400;color:#aaa}"
".row{display:flex;gap:12px;flex-wrap:wrap}"
".card{background:#1b1b1b;border-radius:12px;padding:14px;box-shadow:0 1px 12px #0007}"
"input[type=file],input[type=text],input[type=number]{display:block;margin:8px 0;background:#222;color:#eee;border:1px solid #333;border-radius:8px;padding:8px}"
".btn{display:inline-block;padding:8px 12px;background:#0a84ff;color:#fff;border-radius:8px;text-decoration:none;border:0}"
"label{display:block;margin-top:6px;color:#bbb}"
"ul{line-height:1.8}"
"</style></head><body>"
"<h1>Bild-Viewer <small>(ESP32-S2)</small></h1>"
"<div class=row>"
  "<div class=card style='flex:1 1 320px'>"
    "<h3>Bilder</h3><ul id='list'>"
  )) + list + String(F(
    "</ul>"
  "</div>"
  "<div class=card style='flex:1 1 320px'>"
    "<h3>Upload</h3>"
    "<p><b>PNG/JPG → RAW (Client)</b></p>"
    "<input id='img' type='file' accept='image/*,.bmp,.raw'>"
    "<label>Dateiname (ohne Pfad, Endung wird automatisch gesetzt):</label>"
    "<input id='name' type='text' placeholder='bild'>"
    "<div style='display:flex;gap:8px;margin-top:8px'>"
      "<button class=btn onclick='uploadAuto()'>Hochladen</button>"
      "<a class=btn href='/ota'>OTA</a>"
    "</div>"
    "<p style='color:#aaa'>BMP 24-bit kann alternativ serverseitig konvertiert werden.</p>"
  "</div>"
  "<div class=card style='flex:1 1 320px'>"
    "<h3>Slideshow</h3>"
    "<form method='GET' action='/slideshow'>"
      "<label>Intervall (ms)</label><input name='ms' type='number' value='3000'>"
      "<input type='hidden' name='cmd' value='set'>"
      "<button class=btn>Übernehmen</button>"
    "</form>"
    "<p style='margin-top:8px'>"
  )) + btn + String(F(
    "</p>"
  "</div>"
"</div>"
"<script>"
"async function uploadAuto(){"
" const f=document.getElementById('img').files[0]; if(!f){alert('Bitte Bild wählen');return;}"
" let base=(document.getElementById('name').value||f.name);"
" base=base.replace(/\\.[^.]+$/,'');"
" // Wenn BMP: direkt an /uploadbmp senden (ESP konvertiert)"
" if(/\\.bmp$/i.test(f.name) || f.type==='image/bmp'){"
"   const fd=new FormData(); fd.append('file',f,f.name); fd.append('name',base+'.raw');"
"   let r=await fetch('/uploadbmp',{method:'POST',body:fd}); alert(await r.text()); location.reload(); return;"
" }"
" // Sonst: im Browser rendern -> RAW565 erzeugen -> /upload"
" const img=new Image();"
" img.onload=async()=>{"
"   const W=160,H=128; const cvs=document.createElement('canvas'); cvs.width=W; cvs.height=H;"
"   const ctx=cvs.getContext('2d');"
"   ctx.fillStyle='#000'; ctx.fillRect(0,0,W,H);"
"   const sx= W/img.width, sy= H/img.height, s=Math.min(sx,sy);"
"   const dw= Math.max(1, Math.floor(img.width*s)), dh=Math.max(1, Math.floor(img.height*s));"
"   const dx= (W-dw)>>1, dy=(H-dh)>>1;"
"   ctx.imageSmoothingEnabled=true; ctx.drawImage(img, dx, dy, dw, dh);"
"   const d = ctx.getImageData(0,0,W,H).data;"
"   const raw = new Uint8Array(W*H*2);"
"   let j=0; for(let i=0;i<d.length;i+=4){"
"     const r=d[i], g=d[i+1], b=d[i+2];"
"     const c = ((r&0xF8)<<8) | ((g&0xFC)<<3) | (b>>3);"
"     raw[j++] = c & 0xFF; raw[j++] = c >> 8;"
"   }"
"   const fd=new FormData();"
"   fd.append('file', new Blob([raw],{type:'application/octet-stream'}), base+'.raw');"
"   fd.append('name', base+'.raw');"
"   let r = await fetch('/upload',{method:'POST',body:fd});"
"   alert(await r.text()); location.reload();"
" };"
" img.onerror=()=>alert('Bild konnte nicht geladen werden');"
" img.src = URL.createObjectURL(f);"
"}"
"</script>"
"</body></html>"
  ));
}

/* ===== WiFi + OTA + Web ===== */
void startWiFi(){
  WiFi.mode(WIFI_STA);
  if(strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0=millis();
  while(strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(100);
  if(WiFi.status()!=WL_CONNECTED){ WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS); }
}

void setup(){
  pinMode(TFT_LED, OUTPUT); digitalWrite(TFT_LED, HIGH);
  Serial.begin(115200);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(ST77XX_BLACK);
  setBacklight(90);

  SPIFFS.begin(true);
  if(!SPIFFS.exists(IMG_DIR)) SPIFFS.mkdir(IMG_DIR);

  // Splash
  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.setCursor(6,6); tft.print("Slideshow + OTA + Convert");
  tft.drawRect(2, 20, 156, 10, ST77XX_DARKGREY);
  for(int i=3;i<155;i++){ tft.drawLine(3,25,i,25,ST77XX_CYAN); delay(4); }

  startWiFi();

  /* --- Seiten --- */
  // Index
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", pageIndex()); });

  // Bild anzeigen
  server.on("/show", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!r->hasParam("f")){ r->redirect("/"); return; }
    currentFile = r->getParam("f")->value();
    drawRAW(currentFile.c_str());
    tNext = millis() + slideMs;
    r->redirect("/");
  });

  // Bild löschen
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest* r){
    if(r->hasParam("f")){
      String f=r->getParam("f")->value();
      if(f.startsWith(IMG_DIR)) SPIFFS.remove(f);
    }
    r->redirect("/");
  });

  // RAW Upload (vom Browser (PNG/JPG) konvertiert)
  server.on(
    "/upload", HTTP_POST,
    [](AsyncWebServerRequest* r){ r->send(200,"text/plain","Upload OK"); },
    [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final){
      static File uf; static String target;
      if(!index){
        String name = (r->hasParam("name", true)) ? r->getParam("name", true)->value() : filename;
        if(!name.endsWith(".raw")) name += ".raw";
        if(!name.startsWith(IMG_DIR)) name = String(IMG_DIR) + "/" + name;
        target = name;
        uf = SPIFFS.open(target, "w");
      }
      if(uf) uf.write(data, len);
      if(final && uf) uf.close();
    }
  );

  // BMP Upload (ESP konvertiert zu RAW)
  server.on(
    "/uploadbmp", HTTP_POST,
    [](AsyncWebServerRequest* r){
      // Konvertierung nach Abschluss in Upload-Handler erledigt
      r->send(200,"text/plain","BMP verarbeitet");
    },
    [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final){
      static File tmp; static String tmpPath; static String rawTarget;
      if(!index){
        String base = (r->hasParam("name", true)) ? r->getParam("name", true)->value() : filename;
        if(!base.endsWith(".raw")) base += ".raw";
        rawTarget = String(IMG_DIR) + "/" + base;
        tmpPath = String(IMG_DIR) + "/__up.bmp";
        tmp = SPIFFS.open(tmpPath, "w");
      }
      if(tmp) tmp.write(data, len);
      if(final){
        if(tmp) tmp.close();
        // konvertieren
        if(bmp24_to_raw565(tmpPath, rawTarget)){
          SPIFFS.remove(tmpPath);
        }else{
          // misslungen, tmp löschen
          SPIFFS.remove(tmpPath);
        }
      }
    }
  );

  // Slideshow Steuerung
  server.on("/slideshow", HTTP_GET, [](AsyncWebServerRequest* r){
    if(r->hasParam("cmd")){
      String cmd=r->getParam("cmd")->value();
      if(cmd=="start"){ slideRunning=true; tNext=millis(); }
      else if(cmd=="stop"){ slideRunning=false; }
      else if(cmd=="set" && r->hasParam("ms")){
        slideMs = max(500, r->getParam("ms")->value().toInt());
        tNext = millis() + slideMs;
      }
    }
    r->redirect("/");
  });

  // OTA UI + Update (identisch wie deine OTA-only Seite)
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", pageOTA()); });

  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest* r){
      bool ok = !Update.hasError();
      r->send(200, "text/plain", ok ? "Update OK, rebooting..." : "Update FAILED");
      if(ok){ delay(300); ESP.restart(); }
    },
    [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final){
      if(!index) Update.begin(UPDATE_SIZE_UNKNOWN);
      if(Update.write(data, len) != len) Update.printError(Serial);
      if(final){ if(!Update.end(true)) Update.printError(Serial); }
    }
  );

  server.begin();

  // erstes Bild
  auto files = listRaw();
  currentFile = files.empty()? "" : files.front();
  if(currentFile!="") drawRAW(currentFile.c_str());
  tNext = millis() + slideMs;
}

void loop(){
  if(slideRunning && millis() >= tNext){
    auto files = listRaw();
    if(!files.empty()){
      currentFile = nextFile(files, currentFile);
      drawRAW(currentFile.c_str());
    }
    tNext = millis() + slideMs;
  }
}
