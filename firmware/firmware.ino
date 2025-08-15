/***** ESP32-S2 – Projekt 8 (Slideshow + OTA + PNG/JPG→RAW im Browser + BMP→RAW am ESP)
 *  - Demo-Animation bis Einstellungen gespeichert
 *  - Übergänge: None/Fade/Wipe
 *  - Display Rotation = 1
 *****/
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
#include <vector>

/* ==== TFT Pins ==== */
#define TFT_CS    5
#define TFT_RST   6
#define TFT_DC    7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

/* Farben */
#define RGB565(r,g,b) ( ((r&0xF8)<<8) | ((g&0xFC)<<3) | ((b)>>3) )
#define COL_BG   RGB565(0,0,0)
#define COL_FG   RGB565(255,255,255)
#define COL_GREY RGB565(120,120,120)
#define COL_ACC  RGB565(0,180,255)
#define COL_YEL  RGB565(255,210,0)

/* ==== WLAN / Web ==== */
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT";     // leer => AP-Fallback
AsyncWebServer server(80);

/* ==== Datei-Orte ==== */
const char* RAW_DIR = "/img";   // hier landen RAWs (Browser & Konvertierungen)
const char* BMP_DIR = "/pics";  // optional: hier dürfen BMPs liegen

/* ==== Slideshow ==== */
uint32_t slideMs = 3000;            // Intervall
bool     slideRun = false;          // Start: AUS → erst nach Speichern EIN
bool     settingsReady = false;     // wird true, wenn /set gespeichert wurde
uint8_t  effectMode = 1;            // 0=None, 1=Fade, 2=Wipe
uint8_t  targetBrightness = 230;    // 0..255

uint32_t lastSlide = 0;
String   currentPath = "";          // kompletter Pfad
std::vector<String> items;          // Liste: .raw + .bmp

/* ==== Backlight via LEDC ==== */
void setBacklight(uint8_t lvl) {
#if defined(ARDUINO_ARCH_ESP32)
  static bool init=false;
  if(!init){ ledcSetup(0, 5000, 8); ledcAttachPin(TFT_LED, 0); init=true; }
  ledcWrite(0, lvl);
#else
  pinMode(TFT_LED, OUTPUT);
  analogWrite(TFT_LED, lvl);
#endif
}

/* ==== SPIFFS Hilfen ==== */
void ensureDirs(){
  if(!SPIFFS.exists(RAW_DIR)) SPIFFS.mkdir(RAW_DIR);
  if(!SPIFFS.exists(BMP_DIR)) SPIFFS.mkdir(BMP_DIR);
}

void refreshList() {
  items.clear();
  ensureDirs();

  // RAWs
  {
    File dir = SPIFFS.open(RAW_DIR);
    for(File f = dir.openNextFile(); f; f = dir.openNextFile()){
      String n = f.name(); String l=n; l.toLowerCase();
      if(!f.isDirectory() && (l.endsWith(".raw"))) items.push_back(n);
    }
  }
  // BMPs (optional anzeigen – werden direkt gezeigt oder zu RAW konvertiert)
  {
    File dir = SPIFFS.open(BMP_DIR);
    for(File f = dir.openNextFile(); f; f = dir.openNextFile()){
      String n = f.name(); String l=n; l.toLowerCase();
      if(!f.isDirectory() && (l.endsWith(".bmp"))) items.push_back(n);
    }
  }
  std::sort(items.begin(), items.end());
}

String nextItem(const String& cur){
  if(items.empty()) return "";
  if(cur=="") return items.front();
  for(size_t i=0;i<items.size();++i)
    if(items[i]==cur) return items[(i+1)%items.size()];
  return items.front();
}

/* ==== RAW zeichnen ==== */
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

/* ==== RAW (partial) – für Wipe-Effekt ==== */
bool drawRAWPartial(const char* path, int w){
  if(w<=0) return true;
  if(w>160) w=160;
  File f = SPIFFS.open(path, "r");
  if(!f) return false;
  const uint16_t W=160, H=128;
  static uint16_t line[W];
  for(uint16_t y=0;y<H;y++){
    size_t got = f.read((uint8_t*)line, W*2);
    if(got != W*2){ f.close(); return false; }
    tft.startWrite();
    tft.setAddrWindow(0,y,w,1);
    tft.pushColors(line, w, true);
    tft.endWrite();
  }
  f.close();
  return true;
}

/* ==== BMP Loader (24-bit, unkomprimiert) ==== */
#define BUFFPIXEL 60
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
  tft.startWrite(); tft.setAddrWindow(x, y, biWidth, biHeight);

  uint32_t rowSize = (biWidth * 3 + 3) & ~3;
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
        tft.writePixel(c);
      }
      col -= nb;
    }
  }
  tft.endWrite(); bmpFS.close(); return true;
}

/* ==== BMP→RAW (Server-seitig, 24-bit, unkomprimiert → 160×128 Letterbox) ==== */
static uint16_t rd16(File &f){ uint16_t r; f.read((uint8_t*)&r,2); return r; }
static uint32_t rd32(File &f){ uint32_t r; f.read((uint8_t*)&r,4); return r; }
bool bmp24_to_raw565(const String& bmpPath, const String& rawPath){
  File bmp = SPIFFS.open(bmpPath, "r");
  if(!bmp) return false;
  if(rd16(bmp)!=0x4D42){ bmp.close(); return false; }
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

  const int DSTW=160, DSTH=128;
  float sx = (float)DSTW/srcW, sy=(float)DSTH/srcH, s=min(sx,sy);
  int dstW=max(1,(int)(srcW*s)), dstH=max(1,(int)(srcH*s));
  int x0=(DSTW-dstW)/2, y0=(DSTH-dstH)/2;

  File raw = SPIFFS.open(rawPath, "w");
  if(!raw){ bmp.close(); return false; }

  std::unique_ptr<uint8_t[]> row(new uint8_t[rowSize]);
  std::unique_ptr<uint16_t[]> out(new uint16_t[DSTW]);

  static uint16_t xmap[160], ymap[128];
  for(int x=0;x<dstW;x++) xmap[x]=(uint32_t)x*srcW/max(1,dstW);
  for(int y=0;y<dstH;y++) ymap[y]=(uint32_t)y*srcH/max(1,dstH);

  for(int y=0;y<DSTH;y++){
    for(int x=0;x<DSTW;x++) out[x]=0x0000;
    if(y>=y0 && y<y0+dstH){
      int syy=ymap[y-y0];
      uint32_t pos = startPos + (flip?(srcH-1-syy):syy)*rowSize;
      if(bmp.position()!=pos) bmp.seek(pos);
      bmp.read(row.get(), rowSize);
      for(int x=0;x<dstW;x++){
        int sxx=xmap[x]; int p=sxx*3;
        uint8_t B=row[p+0], G=row[p+1], R=row[p+2];
        out[x0+x] = ((R&0xF8)<<8) | ((G&0xFC)<<3) | (B>>3);
      }
    }
    raw.write((uint8_t*)out.get(), DSTW*2);
  }
  bmp.close(); raw.close(); return true;
}

/* ==== Rahmen / Info ==== */
void drawFrameInfo(const String& name){
  tft.fillRect(0,0,160,10, COL_BG);
  tft.drawFastHLine(0,10,160,COL_GREY);
  tft.setCursor(2,1); tft.setTextSize(1); tft.setTextColor(COL_YEL);
  tft.print(name);
}

/* ==== Übergänge + Anzeige ==== */
void showWithEffect(const String& path){
  // Fade-Out
  if(effectMode==1){
    for(int b=targetBrightness; b>=0; b-=12){ setBacklight(b); delay(6); }
  }

  // zeichnen (RAW oder BMP)
  bool ok=false;
  if(path.endsWith(".raw") || path.endsWith(".RAW")) ok = drawRAW(path.c_str());
  else                                              ok = drawBMP(SPIFFS, path.c_str(), 0, 11); // BMP unterhalb Rahmen
  if(!ok){
    tft.fillRect(0,11,160,117,COL_BG);
    tft.setCursor(6, 40); tft.setTextColor(COL_FG);
    tft.println("Fehler beim Laden:"); tft.println(path);
  }

  // Rahmen/Name
  String base = path.substring(path.lastIndexOf('/')+1);
  drawFrameInfo(base);

  // Wipe (links->rechts über RAW; bei BMP: fallback = kein wipe)
  if(effectMode==2 && (path.endsWith(".raw") || path.endsWith(".RAW"))){
    // Neu zeichnen als progressive Spalten
    for(int w=8; w<=160; w+=8){
      drawRAWPartial(path.c_str(), w);
      // Rahmen darüber nochmal
      drawFrameInfo(base);
      delay(5);
    }
  }

  // Fade-In
  if(effectMode==1){
    for(int b=0; b<=targetBrightness; b+=12){ setBacklight(b); delay(6); }
  } else {
    setBacklight(targetBrightness);
  }
}

void showNext(){
  if(items.empty()){
    tft.fillScreen(COL_BG);
    tft.setCursor(10,48); tft.setTextSize(1); tft.setTextColor(COL_FG);
    tft.println("Keine Bilder gefunden.");
    tft.setCursor(10,60); tft.println("Upload auf /gallery");
    return;
  }
  currentPath = nextItem(currentPath);
  tft.fillRect(0,11,160,117,COL_BG);
  showWithEffect(currentPath);
}

/* ==== Demo-Animation (bis Settings gespeichert) ==== */
void demoFrame() {
  static float ph = 0.0f;
  static int   bx=20, by=30, vx=2, vy=1;

  tft.fillRect(0,0,160,128,COL_BG);

  int prevY = -1;
  for(int x=0;x<160;x++){
    float v = sinf(ph + (x/160.0f)*2.0f*PI*2.0f);
    int y = 68 + (int)(v*26);
    if(prevY>=0) tft.drawLine(x-1, prevY, x, y, COL_ACC);
    prevY = y;
  }
  tft.fillCircle(bx, by, 4, COL_YEL);
  bx += vx; by += vy;
  if(bx<=4 || bx>=156) vx = -vx;
  if(by<=14 || by>=124) vy = -vy; // Kopfzeile frei lassen

  tft.fillRect(0,0,160,10, COL_BG);
  tft.drawFastHLine(0,10,160,COL_GREY);
  tft.setCursor(2,1); tft.setTextSize(1); tft.setTextColor(COL_FG);
  tft.print("Demo – /gallery: Autoplay speichern");

  ph += 0.12f;
}

/* ==== HTML (ohne F() in Lambdas) ==== */
String htmlHeader(const String& title){
  return String("<!doctype html><html><head><meta charset='utf-8'>")
    + "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    + "<title>"+title+"</title>"
    + "<style>body{font-family:system-ui,Arial;margin:18px;background:#111;color:#eee}"
      "a,button,input[type=submit]{padding:.5rem .7rem;border:0;border-radius:8px;background:#0a84ff;color:#fff;text-decoration:none}"
      "input,select{background:#1b1b1b;color:#eee;border:1px solid #333;border-radius:8px;padding:.4rem}"
      "table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #333;padding:.35rem .5rem}"
      "code{background:#000;padding:.15rem .3rem;border-radius:4px}</style></head><body>";
}
String htmlFooter(){ return "</body></html>"; }

/* ==== Webserver ==== */
void setupWeb(){

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    String ip = (WiFi.getMode()==WIFI_AP)? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    String s = htmlHeader("ESP32-S2 – Slideshow");
    s += "<h2>ESP32-S2 – Slideshow</h2>";
    s += "<p>IP: <b>"+ip+"</b></p>";
    s += "<p><a href='/gallery'>Galerie</a> &nbsp; <a href='/status'>Status</a> &nbsp; <a href='/ota'>OTA</a></p>";
    s += "<hr><p>Autoplay: "+String(slideRun?"ON":"OFF")
         +" &nbsp; Intervall: "+String((unsigned long)slideMs)+" ms"
         +" &nbsp; Effekt: "+String(effectMode==0?"None":(effectMode==1?"Fade":"Wipe"))
         +" &nbsp; Settings: "+String(settingsReady?"gesetzt":"offen")+"</p>";
    s += htmlFooter();
    r->send(200,"text/html",s);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r){
    String ip = (WiFi.getMode()==WIFI_AP)? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    String s = htmlHeader("Status");
    s += "<h3>Status</h3><ul>";
    s += "<li>IP: "+ip+"</li>";
    s += "<li>Uptime: "+String(millis()/1000)+" s</li>";
    s += "<li>Dateien: "+String(items.size())+" (.raw/.bmp)</li>";
    s += "<li>Autoplay: "+String(slideRun?"ON":"OFF")+"</li>";
    s += "<li>Intervall: "+String((unsigned long)slideMs)+" ms</li>";
    s += "<li>Effekt: "+String(effectMode==0?"None":(effectMode==1?"Fade":"Wipe"))+"</li>";
    s += "</ul><p><a href='/'>Start</a></p>";
    s += htmlFooter();
    r->send(200,"text/html",s);
  });

  // Galerie mit Uploads (inkl. Client-Konvertierung PNG/JPG → RAW)
  server.on("/gallery", HTTP_GET, [](AsyncWebServerRequest *r){
    refreshList();
    String rows="";
    for(auto &p: items){
      String base = p.substring(p.lastIndexOf('/')+1);
      rows += "<tr><td><code>"+base+"</code></td>"
              "<td><a href='/del?f="+base+"'>Löschen</a></td></tr>";
    }
    if(rows=="") rows = "<tr><td colspan=2><i>Noch keine Bilder</i></td></tr>";

    String s = htmlHeader("Galerie");
    s += "<h3>Galerie</h3>";

    // Settings-Form – aktiviert Slideshow
    s += "<form method='POST' action='/set' style='margin-bottom:1rem'>"
         "Autoplay: <select name='run'>"
         "<option value='1'"+String(slideRun?" selected":"")+">ON</option>"
         "<option value='0'"+String(!slideRun?" selected":"")+">OFF</option>"
         "</select> &nbsp; "
         "Intervall (ms): <input type='number' min='500' step='100' name='ms' value='"+String((unsigned long)slideMs)+"'> &nbsp; "
         "Effekt: <select name='eff'>"
         "<option value='0'"+String(effectMode==0?" selected":"")+">None</option>"
         "<option value='1'"+String(effectMode==1?" selected":"")+">Fade</option>"
         "<option value='2'"+String(effectMode==2?" selected":"")+">Wipe</option>"
         "</select> &nbsp; "
         "Helligkeit: <input type='number' name='bl' min='0' max='255' value='"+String(targetBrightness)+"'> "
         "<input type='submit' value='Speichern'></form>";

    // Upload: PNG/JPG → RAW per Canvas, BMP → ESP-Konvertierung
    s += "<div style='display:flex;gap:16px;flex-wrap:wrap'>"
         "<div style='flex:1 1 300px'><h4>PNG/JPG → RAW (client)</h4>"
         "<input id='img' type='file' accept='image/*'>"
         "<input id='name' type='text' placeholder='dateiname (ohne Endung)' style='width:100%;margin-top:6px'>"
         "<button onclick='uploadAuto()'>Hochladen</button>"
         "<p style='color:#aaa'>Wird auf 160×128 zentriert und als RGB565 .raw gespeichert.</p></div>"
         "<div style='flex:1 1 300px'><h4>BMP 24-bit → RAW (ESP)</h4>"
         "<form id='fbmp' method='POST' action='/uploadbmp' enctype='multipart/form-data'>"
         "<input type='file' name='file' accept='.bmp'>"
         "<input type='text' name='name' placeholder='dateiname.raw' style='width:100%;margin-top:6px'>"
         "<input type='submit' value='Upload & Konvertieren' style='margin-top:6px'>"
         "</form>"
         "<p style='color:#aaa'>Auch andere Größen werden proportional nach 160×128 eingepasst.</p></div>"
         "</div>";

    s += "<h4 style='margin-top:1rem'>Dateien (.raw/.bmp)</h4>"
         "<table><tr><th>Name</th><th>Aktion</th></tr>"+rows+"</table>";
    s += "<p style='margin-top:1rem'><a href='/'>Zurück</a></p>";

    // Canvas-Konvertierung
    s += "<script>"
         "async function uploadAuto(){"
         "const f=document.getElementById('img').files[0]; if(!f){alert('Bitte Bild wählen');return;}"
         "let base=(document.getElementById('name').value||f.name).replace(/\\.[^.]+$/,'');"
         "if(/\\.bmp$/i.test(f.name)||f.type==='image/bmp'){"
         " const fd=new FormData(); fd.append('file',f,f.name); fd.append('name',base+'.raw');"
         " let r=await fetch('/uploadbmp',{method:'POST',body:fd}); alert(await r.text()); location.reload(); return;"
         "}"
         "const img=new Image(); img.onload=async()=>{"
         " const W=160,H=128; const c=document.createElement('canvas'); c.width=W; c.height=H;"
         " const ctx=c.getContext('2d'); ctx.fillStyle='#000'; ctx.fillRect(0,0,W,H);"
         " const sx=W/img.width, sy=H/img.height, s=Math.min(sx,sy);"
         " const dw=Math.max(1,Math.floor(img.width*s)), dh=Math.max(1,Math.floor(img.height*s));"
         " const dx=(W-dw)>>1, dy=(H-dh)>>1; ctx.imageSmoothingEnabled=true; ctx.drawImage(img,dx,dy,dw,dh);"
         " const d=ctx.getImageData(0,0,W,H).data; const raw=new Uint8Array(W*H*2); let j=0;"
         " for(let i=0;i<d.length;i+=4){ const r=d[i],g=d[i+1],b=d[i+2]; const c=((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3); raw[j++]=c&255; raw[j++]=c>>8; }"
         " const fd=new FormData(); fd.append('file', new Blob([raw],{type:'application/octet-stream'}), base+'.raw');"
         " fd.append('name', base+'.raw'); let r=await fetch('/uploadraw',{method:'POST',body:fd});"
         " alert(await r.text()); location.reload();"
         "}; img.onerror=()=>alert('Bild konnte nicht geladen werden'); img.src=URL.createObjectURL(f); }"
         "</script>";

    s += htmlFooter();
    r->send(200,"text/html",s);
  });

  // Settings übernehmen
  server.on("/set", HTTP_POST, [](AsyncWebServerRequest *r){
    if(r->hasParam("run", true))  slideRun = (r->getParam("run", true)->value()=="1");
    if(r->hasParam("ms",  true)) { long v=r->getParam("ms",true)->value().toInt(); if(v<500) v=500; slideMs=(uint32_t)v; }
    if(r->hasParam("eff", true))  effectMode = (uint8_t) constrain(r->getParam("eff",true)->value().toInt(),0,2);
    if(r->hasParam("bl",  true))  { int v=r->getParam("bl",true)->value().toInt(); targetBrightness=constrain(v,0,255); setBacklight(targetBrightness); }
    settingsReady = true;
    r->redirect("/gallery");
  });

  // Dateien löschen
  server.on("/del", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("f")){
      String base = r->getParam("f")->value();
      String rawP = String(RAW_DIR) + "/" + base;
      String bmpP = String(BMP_DIR) + "/" + base;
      if(SPIFFS.exists(rawP)) SPIFFS.remove(rawP);
      if(SPIFFS.exists(bmpP)) SPIFFS.remove(bmpP);
      refreshList();
    }
    r->redirect("/gallery");
  });

  // RAW Upload (vom Browser konvertiert)
  server.on("/uploadraw", HTTP_POST,
    [](AsyncWebServerRequest *r){ r->send(200,"text/plain","RAW gespeichert"); },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t* data, size_t len, bool final){
      static File uf; static String target;
      if(!index){
        ensureDirs();
        String name = r->hasParam("name",true)? r->getParam("name",true)->value() : filename;
        if(!name.endsWith(".raw")) name += ".raw";
        target = String(RAW_DIR) + "/" + name;
        uf = SPIFFS.open(target, "w");
      }
      if(uf) uf.write(data, len);
      if(final && uf){ uf.close(); refreshList(); }
    }
  );

  // BMP Upload (ESP konvertiert)
  server.on("/uploadbmp", HTTP_POST,
    [](AsyncWebServerRequest *r){ r->send(200,"text/plain","BMP empfangen – konvertiert"); },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t* data, size_t len, bool final){
      static File tmp; static String tmpPath; static String rawTarget;
      if(!index){
        ensureDirs();
        String base = r->hasParam("name",true)? r->getParam("name",true)->value() : filename;
        if(!base.endsWith(".raw")) base += ".raw";
        rawTarget = String(RAW_DIR) + "/" + base;
        tmpPath = String(BMP_DIR) + "/__up.bmp";
        tmp = SPIFFS.open(tmpPath, "w");
      }
      if(tmp) tmp.write(data, len);
      if(final){
        if(tmp) tmp.close();
        if(bmp24_to_raw565(tmpPath, rawTarget)){
          SPIFFS.remove(tmpPath);
          refreshList();
        } else {
          SPIFFS.remove(tmpPath);
        }
      }
    }
  );

  // OTA (identisch zu OTA-only)
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest *r){
    String ip = (WiFi.getMode()==WIFI_AP)? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    String s = htmlHeader("OTA Update");
    s += "<h3>OTA Update</h3>"
         "<p>IP: "+ip+"</p>"
         "<form method='POST' action='/update' enctype='multipart/form-data'>"
         "<input type='file' name='firmware'> <input type='submit' value='Flash'></form>"
         "<p><a href='/status'>Status</a> &nbsp; <a href='/'>Start</a></p>";
    s += htmlFooter();
    r->send(200,"text/html",s);
  });

  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *r){
      bool ok = !Update.hasError();
      r->send(200, "text/plain", ok ? "Update OK, reboot…" : "Update FAIL");
      if(ok){ delay(300); ESP.restart(); }
    },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
      if(!index) Update.begin(UPDATE_SIZE_UNKNOWN);
      if(Update.write(data, len) != len) { /* optional log */ }
      if(final){ Update.end(true); }
    }
  );

  server.begin();
}

/* ==== Setup / Loop ==== */
void setup() {
  pinMode(TFT_LED, OUTPUT);
  setBacklight(targetBrightness);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);        // <— wie gewünscht (Rotation = 1)
  tft.fillScreen(COL_BG);
  tft.setTextSize(1); tft.setTextColor(COL_FG);
  tft.setCursor(6, 58); tft.print("Web: /gallery /ota");

  SPIFFS.begin(true);
  ensureDirs();
  refreshList();

  // WLAN
  WiFi.mode(WIFI_STA);
  if(strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while(strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(200);
  if(WiFi.status()!=WL_CONNECTED){ WiFi.mode(WIFI_AP); WiFi.softAP("Slideshow_AP","12345678"); }

  setupWeb();

  lastSlide = millis();
}

void loop() {
  if(settingsReady && slideRun && (millis()-lastSlide >= slideMs)){
    lastSlide = millis();
    showNext();
  }
  if(!settingsReady){
    static uint32_t last=0;
    if(millis()-last > 33){ last=millis(); demoFrame(); } // ~30 FPS
  }
}
