/************************************************************
 * ESP32-S2 Slideshow + Web-UI + Thumbs + Sort + PWM + OTA
 * + JPG/PNG -> RAW (160x128 RGB565 LE) + Thumb(40x32)
 *
 * Abhängigkeiten:
 *  Adafruit GFX + ST7735, AsyncTCP, ESPAsyncWebServer,
 *  TJpg_Decoder (Bodmer), pngle
 ************************************************************/

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

#include <TJpg_Decoder.h>
#include <pngle.h>

// ---------- TFT Pins ----------
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
static const int W=160, H=128;
static uint16_t lineBuf[160];

// ---------- WLAN ----------
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT"; // leer => AP

AsyncWebServer server(80);

// ---------- Slideshow ----------
bool     playing = false;
uint32_t intervalMs = 3000;
uint32_t lastSwitch = 0;
uint8_t  transitionMode = 1; // 0:none 1:slide 2:fade 3:wipe 4:zoom

#define MAX_FILES 256
struct Entry {
  String name;   // z.B. "/bild1.raw"
  size_t size;
  uint32_t ts;   // Upload-Zeit (aus /meta/*.txt)
  String thumb;  // z.B. "/thumbs/bild1.traw" (40x32 RAW565 LE)
};
Entry files[MAX_FILES];
int fileCount=0;
int currentIndex=0;

// ---------- Helper ----------
static inline uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
void setBacklightPct(uint8_t pct){
  static bool inited=false;
  if(!inited){
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_LED, 0);
    inited=true;
  }
  pct = (pct>100)?100:pct;
  uint8_t duty = (uint8_t)(pct*255/100);
  ledcWrite(0, duty);
}
void writeLine(int16_t x0,int16_t y,uint16_t* buf,int w){
  tft.setAddrWindow(x0,y,w,1);
  tft.startWrite();
  tft.writePixels(buf,(uint32_t)w);
  tft.endWrite();
}

// ---------- RAW/BMP Zeichnen ----------
bool drawRAW(const char* path){
  File f=SPIFFS.open(path,"r"); if(!f) return false;
  for(int y=0;y<H;y++){
    if(f.readBytes((char*)lineBuf,W*2)!=W*2){ f.close(); return false; }
    writeLine(0,y,lineBuf,W);
  }
  f.close(); return true;
}
#pragma pack(push,1)
struct BMPHeader{
  uint16_t bfType; uint32_t bfSize; uint16_t r1,r2; uint32_t offBits;
  uint32_t biSize; int32_t biWidth; int32_t biHeight; uint16_t biPlanes; uint16_t biBitCount;
  uint32_t biCompression; uint32_t biSizeImage; int32_t biXPelsPerMeter; int32_t biYPelsPerMeter;
  uint32_t biClrUsed; uint32_t biClrImportant;
};
#pragma pack(pop)
bool drawBMP(const char* path){
  File f=SPIFFS.open(path,"r"); if(!f) return false;
  BMPHeader h{}; if(f.read((uint8_t*)&h,sizeof(h))!=sizeof(h)){ f.close(); return false; }
  if(h.bfType!=0x4D42 || h.biBitCount!=24 || h.biCompression!=0){ f.close(); return false; }
  if(abs(h.biWidth)!=W || abs(h.biHeight)!=H){ f.close(); return false; }
  int w=abs(h.biWidth), hh=abs(h.biHeight), rowSize=((w*3+3)&~3); bool flip=(h.biHeight>0);
  uint8_t row[160*3];
  for(int y=0;y<H;y++){
    int ry=flip?(H-1-y):y;
    f.seek(h.offBits + (uint32_t)ry*rowSize, SeekSet);
    if(f.read(row,w*3)!=w*3){ f.close(); return false; }
    for(int x=0,j=0;x<w;x++){ uint8_t B=row[j++],G=row[j++],R=row[j++]; lineBuf[x]=rgb565(R,G,B); }
    writeLine(0,y,lineBuf,w);
  }
  f.close(); return true;
}

// ---------- Effekte ----------
void fxFade(const String& p){
  for(int i=0;i<4;i++){ tft.fillScreen(ST77XX_BLACK); delay(20); }
  if(p.endsWith(".raw")) drawRAW(p.c_str()); else drawBMP(p.c_str());
}
void fxSlide(const String& p){
  const int steps=8;
  bool isRaw = p.endsWith(".raw")||p.endsWith(".RAW");
  for(int s=0;s<=steps;s++){
    int xstart=W-(W*s)/steps;
    if(xstart>0) tft.fillRect(0,0,xstart,H,ST77XX_BLACK);
    if(isRaw){
      File f=SPIFFS.open(p,"r"); if(!f) return;
      for(int y=0;y<H;y++){
        f.read((uint8_t*)lineBuf,W*2);
        writeLine(xstart,y,lineBuf+xstart,W-xstart);
      }
      f.close();
    }else{
      File f=SPIFFS.open(p,"r"); if(!f) return;
      BMPHeader h{}; f.read((uint8_t*)&h,sizeof(h));
      int w=abs(h.biWidth), rowSize=((w*3+3)&~3); bool flip=(h.biHeight>0);
      uint8_t row[160*3];
      for(int y=0;y<H;y++){
        int ry=flip?(H-1-y):y; f.seek(h.offBits + (uint32_t)ry*rowSize, SeekSet);
        f.read(row,w*3);
        for(int x=0,j=0;x<w;x++){ uint8_t B=row[j++],G=row[j++],R=row[j++]; lineBuf[x]=rgb565(R,G,B); }
        writeLine(xstart,y,lineBuf+xstart,W-xstart);
      }
      f.close();
    }
    delay(20);
  }
}
void fxWipe(const String& p){
  // von links nach rechts aufziehen
  if(p.endsWith(".raw")){
    for(int w=1; w<=W; w+=16){ // chunkweise
      File f=SPIFFS.open(p,"r"); if(!f) return;
      for(int y=0;y<H;y++){
        f.read((uint8_t*)lineBuf,W*2);
        writeLine(0,y,lineBuf,w);
      }
      f.close(); delay(10);
    }
  }else{
    // BMP: zeilenweise lesen, pro Iteration nur Teilbreite zeichnen
    File f=SPIFFS.open(p,"r"); if(!f) return;
    BMPHeader h{}; f.read((uint8_t*)&h,sizeof(h));
    int ww=abs(h.biWidth), rowSize=((ww*3+3)&~3); bool flip=(h.biHeight>0);
    uint8_t row[160*3];
    for(int w=1; w<=W; w+=16){
      for(int y=0;y<H;y++){
        int ry=flip?(H-1-y):y; f.seek(h.offBits+(uint32_t)ry*rowSize,SeekSet);
        f.read(row,ww*3);
        for(int x=0,j=0;x<ww;x++){ uint8_t B=row[j++],G=row[j++],R=row[j++]; lineBuf[x]=rgb565(R,G,B); }
        writeLine(0,y,lineBuf,w);
      }
      delay(10);
    }
    f.close();
  }
}
void fxZoom(const String& p){
  // einfacher Zoom-in: 0.5 -> 1.0 (nearest)
  // RAW-only schnell; BMP analog, aber wir nutzen RAW-Pfad für Performance:
  bool isRaw = p.endsWith(".raw")||p.endsWith(".RAW");
  const float s0=0.5f, s1=1.0f; const int steps=8;
  for(int st=0; st<=steps; ++st){
    float s = s0 + (s1-s0)*(st/(float)steps);
    int dstW = (int)(W*s), dstH = (int)(H*s);
    int offX = (W-dstW)/2, offY = (H-dstH)/2;
    tft.fillScreen(ST77XX_BLACK);
    if(isRaw){
      File f=SPIFFS.open(p,"r"); if(!f) return;
      // skaliert zeichnen: Zielzeile y -> Quelle floor(y/s)
      for(int y=0;y<dstH;y++){
        int sy = (int)((float)y / s);
        f.seek((uint32_t)sy*W*2, SeekSet);
        f.read((uint8_t*)lineBuf, W*2);
        // Zielzeile zusammenbauen
        static uint16_t z[160];
        for(int x=0;x<dstW;x++){
          int sx = (int)((float)x / s);
          z[x] = lineBuf[sx];
        }
        writeLine(offX, offY+y, z, dstW);
      }
      f.close();
    }else{
      // BMP → vorerst einfacher: normales Bild zeichnen (ohne echten Zoom)
      if(p.endsWith(".bmp")) drawBMP(p.c_str()); else drawRAW(p.c_str());
    }
    delay(20);
  }
}

// ---------- Anzeige ----------
bool showIndex(int idx, bool withTrans=true){
  if(fileCount==0) { tft.fillScreen(ST77XX_BLACK); return false; }
  if(idx<0 || idx>=fileCount) return false;
  String p = files[idx].name;

  if(withTrans){
    switch(transitionMode){
      case 1: fxSlide(p); break;
      case 2: fxFade(p);  break;
      case 3: fxWipe(p);  break;
      case 4: fxZoom(p);  break;
      default: if(p.endsWith(".raw")) drawRAW(p.c_str()); else drawBMP(p.c_str()); break;
    }
  }else{
    if(p.endsWith(".raw")) drawRAW(p.c_str()); else drawBMP(p.c_str());
  }
  currentIndex=idx; return true;
}
void nextImage(){ if(fileCount){ int n=(currentIndex+1)%fileCount; showIndex(n,true); lastSwitch=millis(); } }
void prevImage(){ if(fileCount){ int n=(currentIndex-1+fileCount)%fileCount; showIndex(n,true); lastSwitch=millis(); } }

// ---------- Meta/Thumbs ----------
uint32_t nowMillis(){ return millis(); } // simple Zeitbasis
String baseName(const String& path){
  int s=path.lastIndexOf('/'); if(s<0) return path;
  String b=path.substring(s+1);
  int d=b.lastIndexOf('.'); if(d>0) b=b.substring(0,d);
  return b;
}
void saveMeta(const String& rawPath){
  String b=baseName(rawPath);
  String mp="/meta/"+b+".txt";
  if(!SPIFFS.exists("/meta")) SPIFFS.mkdir("/meta");
  File f=SPIFFS.open(mp,"w");
  if(f){ f.print(String(nowMillis())); f.close(); }
}
uint32_t loadMetaTS(const String& rawPath){
  String b=baseName(rawPath);
  String mp="/meta/"+b+".txt";
  File f=SPIFFS.open(mp,"r");
  if(!f) return 0;
  String s=f.readString(); f.close();
  return (uint32_t)s.toInt();
}
bool makeThumbFromRAW(const String& rawPath, const String& thumbPath){
  // 40x32 -> Faktor 4 in X, 4 in Y (nearest)
  const int TW=40, TH=32;
  File f=SPIFFS.open(rawPath,"r"); if(!f) return false;
  if(SPIFFS.exists(thumbPath)) SPIFFS.remove(thumbPath);
  if(!SPIFFS.exists("/thumbs")) SPIFFS.mkdir("/thumbs");
  File t=SPIFFS.open(thumbPath,"w"); if(!t){ f.close(); return false; }

  static uint16_t row[160];
  static uint16_t tline[TW];
  for(int ty=0; ty<TH; ++ty){
    int sy = ty*4; // 0..124
    f.seek((uint32_t)sy*W*2, SeekSet);
    f.read((uint8_t*)row, W*2);
    for(int tx=0; tx<TW; ++tx){
      int sx = tx*4;
      tline[tx] = row[sx];
    }
    t.write((uint8_t*)tline, TW*2);
  }
  t.close(); f.close(); return true;
}

// ---------- Dateiliste + Sort ----------
bool isImage(const String& n){
  String s=n; s.toLowerCase();
  return s.endsWith(".raw") || s.endsWith(".bmp");
}
void buildList(){
  fileCount=0;
  File root=SPIFFS.open("/");
  for(File f=root.openNextFile(); f && fileCount<MAX_FILES; f=root.openNextFile()){
    String n=String(f.name());
    if(isImage(n)){
      files[fileCount].name=n;
      files[fileCount].size=f.size();
      files[fileCount].ts=loadMetaTS(n);
      String b=baseName(n);
      files[fileCount].thumb="/thumbs/"+b+".traw";
      ++fileCount;
    }
  }
  if(currentIndex>=fileCount) currentIndex=max(0,fileCount-1);
}
void sortBy(int mode){
  // 0: Name, 1: Größe, 2: Datum
  for(int i=0;i<fileCount;i++){
    for(int j=i+1;j<fileCount;j++){
      bool swap=false;
      if(mode==0){ if(files[j].name < files[i].name) swap=true; }
      else if(mode==1){ if(files[j].size < files[i].size) swap=true; }
      else { if(files[j].ts   < files[i].ts)   swap=true; }
      if(swap){ Entry tmp=files[i]; files[i]=files[j]; files[j]=tmp; }
    }
  }
}

// ---------- JPG/PNG -> RAW + Thumb (nur 160x128) ----------
static File rawOut;

static int16_t jpgDrawToRaw(JPEGDRAW *p){
  for(int y=0;y<p->iHeight;y++){
    int dstY=p->y+y; if(dstY<0||dstY>=H) continue;
    for(int x=0;x<p->iWidth;x++){
      int dstX=p->x+x; if(dstX<0||dstX>=W) continue;
      uint16_t c=p->pPixels[y*p->iWidth+x];
      uint32_t off=(dstY*W + dstX)*2;
      rawOut.seek(off, SeekSet);
      uint8_t lo=c&0xFF, hi=c>>8; rawOut.write(&lo,1); rawOut.write(&hi,1);
    }
  }
  return 1;
}
bool convertJPG_Exact(const String& jpgPath, const String& rawPath){
  int16_t jw=0,jh=0; if(!TJpgDec.getJpgSize(&jw,&jh, jpgPath.c_str())) return false;
  if(jw!=W || jh!=H) return false;
  if(SPIFFS.exists(rawPath)) SPIFFS.remove(rawPath);
  rawOut=SPIFFS.open(rawPath,"w+"); if(!rawOut) return false;
  TJpgDec.setJpgScale(0); TJpgDec.setSwapBytes(false); TJpgDec.setCallback(jpgDrawToRaw);
  auto rc=TJpgDec.drawFsJpg(0,0, jpgPath.c_str());
  rawOut.close(); return (rc==0);
}
struct PNGCtx { File raw; };
static PNGCtx pngctx;
static void png_init(pngle_t*, uint32_t, uint32_t){}
static void png_draw(pngle_t*, uint32_t x, uint32_t y, uint32_t, uint32_t, uint8_t rgba[4]){
  if(x>=W||y>=H) return;
  uint16_t c=rgb565(rgba[0],rgba[1],rgba[2]);
  uint32_t off=(y*W + x)*2;
  pngctx.raw.seek(off, SeekSet);
  uint8_t lo=c&0xFF, hi=c>>8; pngctx.raw.write(&lo,1); pngctx.raw.write(&hi,1);
}
bool convertPNG_Exact(const String& pngPath, const String& rawPath){
  // Größe prüfen
  File f=SPIFFS.open(pngPath,"r"); if(!f) return false;
  uint8_t buf[1024]; int n=f.read(buf,sizeof(buf));
  pngle_t* png=pngle_new(); if(!png){ f.close(); return false; }
  pngle_feed(png, buf, n);
  uint32_t pw=pngle_get_width(png), ph=pngle_get_height(png);
  pngle_destroy(png); f.close();
  if(pw!=W || ph!=H) return false;

  // dekodieren
  f=SPIFFS.open(pngPath,"r");
  if(SPIFFS.exists(rawPath)) SPIFFS.remove(rawPath);
  pngctx.raw=SPIFFS.open(rawPath,"w+"); if(!pngctx.raw){ f.close(); return false; }
  png=pngle_new(); pngle_set_init_callback(png,png_init); pngle_set_draw_callback(png,png_draw);
  bool ok=true; while(true){ int rd=f.read(buf,sizeof(buf)); if(rd<=0) break; int fed=pngle_feed(png,buf,rd); if(fed<0){ ok=false; break; } }
  pngle_destroy(png); pngctx.raw.close(); f.close(); return ok;
}

bool makeThumbFor(const String& rawPath){
  String b=baseName(rawPath); String tp="/thumbs/"+b+".traw";
  return makeThumbFromRAW(rawPath, tp);
}

// ---------- HTML (Seiten) ----------
const char* PAGE = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP Slideshow+</title>
<style>
 body{font-family:system-ui,Arial;margin:14px;background:#111;color:#eee}
 card{display:block;max-width:900px;margin:auto;padding:14px;border:1px solid #333;border-radius:12px;background:#1b1b1b}
 h1{font-size:18px;margin:0 0 10px}
 button,select,input{padding:8px 10px;border:0;border-radius:8px;background:#2c6ef2;color:#fff;cursor:pointer}
 .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
 .muted{color:#aaa}
 a{color:#9cf}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:8px}
 .tile{background:#222;border-radius:10px;padding:8px}
 canvas{display:block;width:120px;height:96px;background:#000;border-radius:6px}
 .small{font-size:12px;color:#bbb}
 .bar{display:flex;align-items:center;gap:8px}
 input[type=range]{width:160px}
</style>
<card>
  <h1>Slideshow Steuerung</h1>
  <div class=row>
    <button onclick="fetch('/api/prev')">◀ Zurück</button>
    <button onclick="fetch('/api/next')">Weiter ▶</button>
    <button id=playBtn onclick="toggle()"></button>
    <label>Intervall <input id=ms type=number min=500 step=100 value=3000 style="width:110px;color:#000"></label>
    <button onclick="setMs()">Setzen</button>
    <label>Effekt
      <select id=tr style="color:#000">
        <option value=0>keiner</option>
        <option value=1 selected>Slide</option>
        <option value=2>Fade</option>
        <option value=3>Wipe</option>
        <option value=4>Zoom</option>
      </select>
    </label>
    <button onclick="setTr()">OK</button>
    <div class=bar>
      <label>Backlight</label>
      <input id=bl type=range min=0 max=100 value=100 oninput="blv.innerText=this.value" onchange="setBL(this.value)">
      <span id=blv>100</span>%
    </div>
  </div>
  <p class=muted id=stat>…</p>
  <div class=row>
    <a href="/img">Bild-Upload (JPG/PNG→RAW+Thumb)</a>
    <a href="/fs">Dateimanager</a>
    <a href="/ota">OTA Update</a>
  </div>
</card>
<br>
<card>
  <h1>Dateien <span class=muted>(Sortierung)</span></h1>
  <div class=row>
    <button onclick="loadList(0)">Nach Name</button>
    <button onclick="loadList(1)">Nach Größe</button>
    <button onclick="loadList(2)">Nach Datum</button>
  </div>
  <div class="grid" id=grid></div>
</card>
<script>
async function get(){ return await (await fetch('/api/status')).json(); }
async function setMs(){ let v=parseInt(ms.value||'3000',10); if(v<500)v=500; await fetch('/api/play?ms='+v); refresh(); }
async function setTr(){ await fetch('/api/transition?mode='+tr.value); refresh(); }
async function toggle(){ let s=await get(); await fetch(s.playing?'/api/stop':'/api/play?ms='+s.interval); refresh(); }
async function setBL(v){ await fetch('/api/backlight?pct='+v); }
async function refresh(){
  let s=await get();
  ms.value=s.interval; tr.value=s.transition;
  stat.textContent=`Status: ${s.playing?'spielt':'gestoppt'} | Datei ${s.index+1}/${s.total}: ${s.name}`;
  playBtn.textContent=s.playing?'Stop':'Play';
}
async function loadList(mode){
  let j=await (await fetch('/api/list?sort='+mode)).json();
  let g=document.getElementById('grid'); g.innerHTML='';
  for(let it of j.items){
    let d=document.createElement('div'); d.className='tile';
    d.innerHTML=`<canvas width="40" height="32"></canvas>
      <div class=small>${it.name}</div>
      <div class=small>${(it.size/1024).toFixed(1)} kB · ${it.ts}</div>
      <button onclick="show('${it.name}')">Anzeigen</button>`;
    g.appendChild(d);
    if(it.thumb){ drawThumb(d.querySelector('canvas'), it.thumb); }
  }
}
async function drawThumb(cv, path){
  // .traw = 40x32 RGB565 LE → Canvas
  let r=await fetch(path); if(!r.ok) return;
  let buf=new Uint8Array(await r.arrayBuffer());
  let ctx=cv.getContext('2d');
  let imgData=ctx.createImageData(40,32);
  for(let i=0,j=0;i<buf.length;i+=2){
    let lo=buf[i], hi=buf[i+1];
    let c=(hi<<8)|lo;
    let r5=(c>>11)&0x1F, g6=(c>>5)&0x3F, b5=c&0x1F;
    let R=(r5*255/31)|0, G=(g6*255/63)|0, B=(b5*255/31)|0;
    imgData.data[j++]=R; imgData.data[j++]=G; imgData.data[j++]=B; imgData.data[j++]=255;
  }
  ctx.putImageData(imgData,0,0);
}
async function show(name){
  await fetch('/api/show?name='+encodeURIComponent(name));
  refresh();
}
setInterval(refresh,1500); refresh(); loadList(0);
</script>
)HTML";

const char* OTA_PAGE = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>OTA Update</title>
<style>body{font-family:system-ui;background:#111;color:#eee;margin:14px}card{display:block;max-width:520px;margin:auto;padding:14px;border:1px solid #333;border-radius:12px;background:#1b1b1b}button,input{padding:8px 10px;border:0;border-radius:8px;background:#2c6ef2;color:#fff;cursor:pointer}</style>
<card><h1>OTA Firmware Update</h1>
<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='firmware' required> <button>Flash</button></form>
<p><a href="/">Zurück</a></p></card>
)HTML";

const char* IMG_PAGE = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Bild-Upload</title>
<style>body{font-family:system-ui;background:#111;color:#eee;margin:14px}card{display:block;max-width:520px;margin:auto;padding:14px;border:1px solid #333;border-radius:12px;background:#1b1b1b}button,input{padding:8px 10px;border:0;border-radius:8px;background:#2c6ef2;color:#fff;cursor:pointer}</style>
<card><h1>JPG/PNG → RAW + Thumb</h1>
<p>Bitte Bilder in <b>exakt 160×128 px</b> hochladen.</p>
<form method="POST" action="/img/upload" enctype="multipart/form-data">
  <input type="file" name="file" accept="image/*" required><br><br>
  Dateiname (ohne Endung): <input name="name" placeholder="bild1" required>
  <button>Upload & Konvertieren</button>
</form>
<p><a href="/">Zurück</a> · <a href="/fs">Dateimanager</a></p></card>
)HTML";

const char* FS_PAGE = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Dateimanager</title>
<style>body{font-family:system-ui;background:#111;color:#eee;margin:14px}card{display:block;max-width:720px;margin:auto;padding:14px;border:1px solid #333;border-radius:12px;background:#1b1b1b}table{width:100%;border-collapse:collapse}th,td{padding:6px;border-bottom:1px solid #2a2a2a}button{padding:6px 10px;border:0;border-radius:8px;background:#2c6ef2;color:#fff;cursor:pointer}.danger{background:#d33}a{color:#9cf}</style>
<card><h1>SPIFFS Dateien</h1>
<form id=up enctype="multipart/form-data" method=POST action="/fs/upload">
  <input type=file name=file required> <button>Upload</button>
</form>
<p id=log>Bereit.</p>
<table id=tbl><thead><tr><th>Name</th><th>Größe</th><th>Aktion</th></tr></thead><tbody></tbody></table>
<p><a href="/">Zurück</a></p></card>
<script>
function fmt(n){if(n>1048576)return (n/1048576).toFixed(2)+' MB'; if(n>1024)return (n/1024).toFixed(1)+' kB'; return n+' B'}
async function refresh(){
  let j=await (await fetch('/fs/list')).json();
  let tb=document.querySelector('#tbl tbody'); tb.innerHTML='';
  j.files.forEach(f=>{
    let tr=document.createElement('tr');
    tr.innerHTML=`<td>${f.name}</td><td>${fmt(f.size)}</td>
      <td><a href="${f.url}" download>Download</a> |
      <button class="danger" onclick="delFile('${encodeURIComponent(f.name)}')">Löschen</button></td>`;
    tb.appendChild(tr);
  });
  log.textContent=`Belegt: ${fmt(j.used)} / ${fmt(j.total)}`;
}
async function delFile(n){ if(!confirm('Löschen?')) return; let r=await fetch('/fs/delete?name='+n); alert(await r.text()); refresh(); }
refresh();
</script>
)HTML";

// ---------- Webserver ----------
String statusJson(){
  String name=(fileCount?files[currentIndex].name:String("-"));
  String j="{";
  j+="\"playing\":"+(String)(playing?"true":"false")+",";
  j+="\"interval\":"+(String)((unsigned long)intervalMs)+",";
  j+="\"index\":"+String(currentIndex)+",";
  j+="\"total\":"+String(fileCount)+",";
  j+="\"transition\":"+String(transitionMode)+",";
  j+="\"name\":\""+name+"\"}";
  return j;
}
String listJson(){
  String j="{\"items\":[";
  for(int i=0;i<fileCount;i++){
    if(i) j+=",";
    j+="{\"name\":\""+files[i].name+"\",\"size\":"+String(files[i].size)+",\"ts\":"+String(files[i].ts)+",\"thumb\":\""+files[i].thumb+"\"}";
  }
  j+="]}"; return j;
}
String fsListJson(){
  String j="{";
  j+="\"total\":"+String(SPIFFS.totalBytes())+",";
  j+="\"used\":"+String(SPIFFS.usedBytes())+",";
  j+="\"files\":[";
  bool first=true;
  File root=SPIFFS.open("/");
  for(File f=root.openNextFile(); f; f=root.openNextFile()){
    if(!first) j+=','; first=false;
    j+="{\"name\":\""+String(f.name())+"\",\"size\":"+String(f.size())+",\"url\":\""+String(f.name())+"\"}";
  }
  j+="]}"; return j;
}

void setupWeb(){
  // Hauptseite
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", PAGE); });
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"application/json", statusJson()); });
  server.on("/api/next", HTTP_GET, [](AsyncWebServerRequest* r){ nextImage(); r->send(200,"text/plain","OK"); });
  server.on("/api/prev", HTTP_GET, [](AsyncWebServerRequest* r){ prevImage(); r->send(200,"text/plain","OK"); });
  server.on("/api/play", HTTP_GET, [](AsyncWebServerRequest* r){
    if(r->hasParam("ms")){ unsigned long v=strtoul(r->getParam("ms")->value().c_str(),nullptr,10); if(v<500)v=500; intervalMs=v; }
    playing=true; lastSwitch=millis(); r->send(200,"text/plain","PLAY");
  });
  server.on("/api/stop", HTTP_GET, [](AsyncWebServerRequest* r){ playing=false; r->send(200,"text/plain","STOP"); });
  server.on("/api/transition", HTTP_GET, [](AsyncWebServerRequest* r){
    if(r->hasParam("mode")){ int m=r->getParam("mode")->value().toInt(); transitionMode=constrain(m,0,4); }
    r->send(200,"text/plain","OK");
  });
  server.on("/api/backlight", HTTP_GET, [](AsyncWebServerRequest* r){
    int p = r->hasParam("pct") ? r->getParam("pct")->value().toInt() : 100;
    setBacklightPct(constrain(p,0,100)); r->send(200,"text/plain","OK");
  });
  server.on("/api/list", HTTP_GET, [](AsyncWebServerRequest* r){
    int s= r->hasParam("sort") ? r->getParam("sort")->value().toInt() : 0;
    sortBy(constrain(s,0,2)); r->send(200,"application/json", listJson());
  });
  server.on("/api/show", HTTP_GET, [](AsyncWebServerRequest* r){
    if(!r->hasParam("name")){ r->send(400,"text/plain","name fehlt"); return; }
    String n=r->getParam("name")->value();
    for(int i=0;i<fileCount;i++){ if(files[i].name==n){ showIndex(i,false); currentIndex=i; break; } }
    r->send(200,"text/plain","OK");
  });

  // Datei-Manager (einfach)
  server.on("/fs", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", FS_PAGE); });
  server.on("/fs/list", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"application/json", fsListJson()); });
  static File up;
  server.on("/fs/upload", HTTP_POST,
    [](AsyncWebServerRequest* r){ r->send(200,"text/plain","Upload OK"); buildList(); },
    [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final){
      if(!index){ if(!filename.startsWith("/")) filename="/"+filename; up=SPIFFS.open(filename,"w"); }
      if(up) up.write(data,len);
      if(final && up){ up.close(); }
    });

  // OTA
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", OTA_PAGE); });
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *req){
      bool ok = !Update.hasError();
      req->send(200,"text/plain", ok ? "Update OK, rebooting..." : "Update FAILED");
      if (ok) { delay(300); ESP.restart(); }
    },
    [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final){
      if(!index) Update.begin(UPDATE_SIZE_UNKNOWN);
      if(Update.write(data,len)!=len) Update.printError(Serial);
      if(final){ if(!Update.end(true)) Update.printError(Serial); }
    }
  );

  // Web-Upload (JPG/PNG -> RAW + Thumb)
  server.on("/img", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", IMG_PAGE); });
  server.on("/img/upload", HTTP_POST,
    [](AsyncWebServerRequest* r){ r->send(200,"text/plain","OK"); buildList(); },
    [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final){
      static File tmp; static String tmpPath; static String targetRaw; static String base;
      if(!index){
        base = r->hasParam("name",true) ? r->getParam("name",true)->value() : "bild";
        if(base.indexOf('.')>=0) base=base.substring(0,base.indexOf('.'));
        tmpPath="/__up.bin"; if(SPIFFS.exists(tmpPath)) SPIFFS.remove(tmpPath);
        tmp=SPIFFS.open(tmpPath,"w"); targetRaw="/"+base+".raw";
      }
      if(tmp) tmp.write(data,len);
      if(final){
        if(tmp) tmp.close();
        String low=filename; low.toLowerCase(); bool ok=false;
        if(low.endsWith(".jpg")||low.endsWith(".jpeg")) ok=convertJPG_Exact(tmpPath, targetRaw);
        else if(low.endsWith(".png"))                  ok=convertPNG_Exact(tmpPath, targetRaw);
        SPIFFS.remove(tmpPath);
        if(ok){
          saveMeta(targetRaw);
          makeThumbFor(targetRaw);
        }
      }
    }
  );

  // Direkte Datei-Auslieferung (einschließlich /thumbs/*.traw)
  server.onNotFound([](AsyncWebServerRequest* r){
    if(r->method()==HTTP_GET){
      String p=r->url();
      if(SPIFFS.exists(p)){ r->send(SPIFFS,p,String()); return; }
    }
    r->send(404,"text/plain","Not found");
  });

  server.begin();
}

// ---------- Setup/Loop ----------
void setup(){
  pinMode(TFT_LED, OUTPUT);
  setBacklightPct(100);

  Serial.begin(115200);
  if(!SPIFFS.begin(true)) { Serial.println("SPIFFS FAIL"); }

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_YELLOW); tft.setCursor(2,2); tft.print("Slideshow+ Ready");

  // WLAN
  WiFi.mode(WIFI_STA);
  if(strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0=millis();
  while(strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(200);
  if(WiFi.status()!=WL_CONNECTED){ WiFi.mode(WIFI_AP); WiFi.softAP("ESP_Slideshow","12345678"); }

  tft.setCursor(2,14); tft.print("IP: ");
  tft.print((WiFi.getMode()==WIFI_AP)? WiFi.softAPIP() : WiFi.localIP());

  buildList();
  if(fileCount) showIndex(currentIndex,false);
  setupWeb();
}
void loop(){
  if(playing && millis()-lastSwitch >= intervalMs){ nextImage(); }
}
