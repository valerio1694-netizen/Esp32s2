/************************************************************
 * ESP32-S2 Slideshow + Web-UI + Thumbs + Sort + PWM + OTA
 * + JPG/PNG (on-device)  + RAW/BMP
 * + "echtes" Crossfade mit 2 Framebuffers
 *
 * Abhängigkeiten:
 *  Adafruit GFX + ST7735, AsyncTCP, ESPAsyncWebServer,
 *  TJpg_Decoder (Bodmer), PNGdec (bitbank2)
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

#include <TJpg_Decoder.h>   // JPG
#include <PNGdec.h>         // PNG

// ---------- TFT Pins ----------
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
static const int W=160, H=128;
static uint16_t lineBuf[160];     // 1-Zeilen-Puffer fürs Zeichnen

// ---------- WLAN ----------
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT"; // leer => AP

AsyncWebServer server(80);

// ---------- Slideshow ----------
bool     playing = false;
uint32_t intervalMs = 3000;
uint32_t lastSwitch = 0;
uint8_t  transitionMode = 1; // 0:none 1:slide 2:fade 3:wipe 4:zoom 5:crossfade(NEU)

#define MAX_FILES 256
struct Entry {
  String name;   // "/bild1.raw"/".bmp"/".jpg"/".png"
  size_t size;
  uint32_t ts;   // Upload-Zeit (aus /meta/*.txt)
  String thumb;  // "/thumbs/bild1.traw" (40x32 RAW565 LE)
};
Entry files[MAX_FILES];
int fileCount=0;
int currentIndex=0;
String lastShown="";

// ---------- PNG/JPG Decoder + Framebuffer für Crossfade ----------
static PNG png;
static uint16_t fbCur[160*128];   // aktuelles Bild
static uint16_t fbNext[160*128];  // nächstes Bild

// ---------- Helper ----------
static inline uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline void rgb565_to_parts(uint16_t c, uint8_t& r5, uint8_t& g6, uint8_t& b5){
  r5 = (c >> 11) & 0x1F; g6 = (c >> 5) & 0x3F; b5 = c & 0x1F;
}
static inline uint16_t rgb565_from_parts(uint8_t r5,uint8_t g6,uint8_t b5){
  return (r5<<11) | (g6<<5) | b5;
}
void setBacklightPct(uint8_t pct){
  static bool inited=false;
  if(!inited){ ledcSetup(0, 5000, 8); ledcAttachPin(TFT_LED, 0); inited=true; }
  pct = (pct>100)?100:pct;
  ledcWrite(0, (uint8_t)(pct*255/100));
}
void writeLine(int16_t x0,int16_t y,uint16_t* buf,int w){
  tft.setAddrWindow(x0,y,w,1);
  tft.startWrite();
  tft.writePixels(buf,(uint32_t)w);
  tft.endWrite();
}
String baseName(const String& path){
  int s=path.lastIndexOf('/'); String b=(s<0)?path:path.substring(s+1);
  int d=b.lastIndexOf('.'); if(d>0) b=b.substring(0,d);
  return b;
}
bool endsWithCI(const String& s, const char* suf){
  String a=s; a.toLowerCase(); String t=suf; t.toLowerCase(); return a.endsWith(t);
}
bool isImage(const String& n){
  String s=n; s.toLowerCase();
  return s.endsWith(".raw") || s.endsWith(".bmp") || s.endsWith(".jpg") || s.endsWith(".jpeg") || s.endsWith(".png");
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
  int w=abs(h.biWidth), rowSize=((w*3+3)&~3); bool flip=(h.biHeight>0);
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

// ---------- JPG → Framebuffer ----------
struct JpgToFB { uint16_t* fb; int dx; int dy; } jpgCtx;
static int16_t jpgToFbCB(JPEGDRAW *p){
  int x0 = jpgCtx.dx + p->x;
  int y0 = jpgCtx.dy + p->y;
  int w  = p->iWidth;
  int h  = p->iHeight;
  for (int yy=0; yy<h; ++yy){
    int y = y0 + yy; if (y<0 || y>=H) continue;
    int cx = max(0, -x0);
    int nx = min(w, W - x0);
    if (nx<=cx) continue;
    uint16_t* src = &p->pPixels[yy*w + cx];
    uint16_t* dst = &jpgCtx.fb[y*W + (x0+cx)];
    memcpy(dst, src, (nx-cx)*2);
  }
  return 1;
}
bool loadJPGtoFB(const String& path, uint16_t* fb){
  if (!SPIFFS.exists(path)) return false;
  uint16_t iw=0, ih=0;
  if (TJpgDec.getJpgSize(&iw,&ih, path.c_str()) != TJPG_OK) return false;
  memset(fb, 0, W*H*2);
  int scale=1; while ((iw/scale>W)||(ih/scale>H)){ scale*=2; if(scale>=8)break; }
  int dw=iw/scale, dh=ih/scale;
  jpgCtx = { fb, (W-dw)/2, (H-dh)/2 };
  TJpgDec.setCallback(jpgToFbCB);
  TJpgDec.setJpgScale(scale);
  return (TJpgDec.drawFsJpg(0,0, path.c_str())==0);
}

// ---------- PNG → Framebuffer (zentriert, ohne Skalierung) ----------
struct PngToFB { uint16_t* fb; int dx; int dy; int iw; int ih; } pngCtxFB;
static int pngToFbDraw(PNGDRAW *pDraw){
  static uint8_t rgb[PNG_MAX_BUFFERED_PIXELS*3];
  png.getLineAsRGB(pDraw, rgb, PNG_RGB_TRUECOLOR);
  int y = pngCtxFB.dy + pDraw->y;
  if (y < 0 || y >= H) return 1;
  int x0 = pngCtxFB.dx;
  int w  = pDraw->iWidth;
  int cx = max(0, -x0);
  int nx = min(w, W - x0);
  if (nx<=cx) return 1;
  uint16_t* dst = &pngCtxFB.fb[y*W + (x0+cx)];
  for (int x=cx, j=cx*3; x<nx; ++x, j+=3)
    *dst++ = rgb565(rgb[j], rgb[j+1], rgb[j+2]);
  return 1;
}
bool loadPNGtoFB(const String& path, uint16_t* fb){
  if (!SPIFFS.exists(path)) return false;
  memset(fb, 0, W*H*2);
  auto openFS = [](const char* fn)->File { return SPIFFS.open(fn, "r"); };
  if (png.open(path.c_str(), openFS) != PNG_SUCCESS) return false;
  int iw = png.getWidth(), ih = png.getHeight();
  pngCtxFB = { fb, (W-iw)/2, (H-ih)/2, iw, ih };
  png.setDrawCallback(pngToFbDraw);
  int rc = png.decode(NULL, 0);
  png.close();
  return (rc==PNG_SUCCESS);
}

// ---------- Bild in Framebuffer laden (RAW/BMP/JPG/PNG) ----------
bool loadImageToFB(const String& path, uint16_t* fb){
  String s=path; s.toLowerCase();
  if (s.endsWith(".raw")){
    File f=SPIFFS.open(path,"r"); if(!f) return false;
    if (f.read((uint8_t*)fb, W*H*2)!=(W*H*2)){ f.close(); return false; }
    f.close(); return true;
  }
  if (s.endsWith(".bmp")){
    // in FB kopieren
    File f=SPIFFS.open(path,"r"); if(!f) return false;
    BMPHeader h{}; if(f.read((uint8_t*)&h,sizeof(h))!=sizeof(h)){ f.close(); return false; }
    if(h.bfType!=0x4D42 || h.biBitCount!=24 || h.biCompression!=0){ f.close(); return false; }
    if(abs(h.biWidth)!=W || abs(h.biHeight)!=H){ f.close(); return false; }
    int w=abs(h.biWidth), rowSize=((w*3+3)&~3); bool flip=(h.biHeight>0);
    uint8_t row[160*3];
    for(int y=0;y<H;y++){
      int ry=flip?(H-1-y):y;
      f.seek(h.offBits + (uint32_t)ry*rowSize, SeekSet);
      f.read(row,w*3);
      uint16_t* dst=&fb[y*W];
      for(int x=0,j=0;x<w;x++){ uint8_t B=row[j++],G=row[j++],R=row[j++]; dst[x]=rgb565(R,G,B); }
    }
    f.close(); return true;
  }
  if (s.endsWith(".jpg") || s.endsWith(".jpeg")) return loadJPGtoFB(path, fb);
  if (s.endsWith(".png"))                      return loadPNGtoFB(path, fb);
  return false;
}

// ---------- Effekte ----------
void fxFade(const String& p){
  for(int i=0;i<4;i++){ tft.fillScreen(ST77XX_BLACK); delay(20); }
  if(endsWithCI(p,".raw")) drawRAW(p.c_str());
  else if(endsWithCI(p,".bmp")) drawBMP(p.c_str());
  else { loadImageToFB(p, fbCur); // direkt aus FB anzeigen
         for(int y=0;y<H;y++){ writeLine(0,y,&fbCur[y*W],W); } lastShown=p; }
}
void fxSlide(const String& p){
  const int steps=8;
  bool isRaw = endsWithCI(p,".raw");
  bool isBmp = endsWithCI(p,".bmp");
  if(!(isRaw||isBmp)){
    // für JPG/PNG: vorab in fbNext laden, dann schieben
    if(!loadImageToFB(p, fbNext)) return;
    for(int s=0;s<=steps;s++){
      int xstart = W - (W*s)/steps;
      tft.fillRect(0,0,xstart,H,ST77XX_BLACK);
      for(int y=0;y<H;y++){
        writeLine(xstart,y,&fbNext[y*W]+xstart, W-xstart);
      }
      delay(20);
    }
    memcpy(fbCur, fbNext, W*H*2); lastShown=p;
    return;
  }
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
    }else{ // BMP
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
  loadImageToFB(p, fbCur); lastShown=p;
}
void fxWipe(const String& p){
  if(!loadImageToFB(p, fbNext)) return;
  for(int w=1; w<=W; w+=16){
    for(int y=0;y<H;y++){ writeLine(0,y,&fbNext[y*W], w); }
    delay(10);
  }
  memcpy(fbCur, fbNext, W*H*2); lastShown=p;
}
void fxZoom(const String& p){
  if(!loadImageToFB(p, fbNext)) return;
  const float s0=0.5f, s1=1.0f; const int steps=8;
  for(int st=0; st<=steps; ++st){
    float s = s0 + (s1-s0)*(st/(float)steps);
    int dstW = (int)(W*s), dstH=(int)(H*s);
    int offX=(W-dstW)/2, offY=(H-dstH)/2;
    tft.fillScreen(ST77XX_BLACK);
    for(int y=0;y<dstH;y++){
      int sy=(int)((float)y/s); if(sy<0)sy=0; if(sy>=H)sy=H-1;
      // nearest scale in X
      for(int x=0;x<dstW;x++){ int sx=(int)((float)x/s); lineBuf[x]=fbNext[sy*W + min(max(sx,0),W-1)]; }
      writeLine(offX, offY+y, lineBuf, dstW);
    }
    delay(20);
  }
  memcpy(fbCur, fbNext, W*H*2); lastShown=p;
}
// NEU: Echtes Crossfade
void fxCrossfade(const String& nextPath, uint8_t steps=12){
  if(lastShown=="" || !loadImageToFB(lastShown, fbCur))
    memset(fbCur, 0, W*H*2);
  if(!loadImageToFB(nextPath, fbNext)) return;
  for(int s=0; s<=steps; ++s){
    for(int y=0;y<H;y++){
      for(int x=0;x<W;x++){
        uint8_t r5a,g6a,b5a, r5b,g6b,b5b;
        rgb565_to_parts(fbCur[y*W+x], r5a,g6a,b5a);
        rgb565_to_parts(fbNext[y*W+x], r5b,g6b,b5b);
        uint8_t r5=(r5a*(steps-s)+r5b*s)/steps;
        uint8_t g6=(g6a*(steps-s)+g6b*s)/steps;
        uint8_t b5=(b5a*(steps-s)+b5b*s)/steps;
        lineBuf[x]=rgb565_from_parts(r5,g6,b5);
      }
      writeLine(0,y,lineBuf,W);
    }
    delay(15);
  }
  memcpy(fbCur, fbNext, W*H*2); lastShown=nextPath;
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
      case 5: fxCrossfade(p); break; // NEU
      default:
        if(endsWithCI(p,".raw")) drawRAW(p.c_str());
        else if(endsWithCI(p,".bmp")) drawBMP(p.c_str());
        else { loadImageToFB(p, fbCur); for(int y=0;y<H;y++) writeLine(0,y,&fbCur[y*W],W); lastShown=p; }
        break;
    }
  }else{
    if(endsWithCI(p,".raw")) drawRAW(p.c_str());
    else if(endsWithCI(p,".bmp")) drawBMP(p.c_str());
    else { loadImageToFB(p, fbCur); for(int y=0;y<H;y++) writeLine(0,y,&fbCur[y*W],W); lastShown=p; }
  }
  currentIndex=idx; return true;
}
void nextImage(){ if(fileCount){ int n=(currentIndex+1)%fileCount; showIndex(n,true); lastSwitch=millis(); } }
void prevImage(){ if(fileCount){ int n=(currentIndex-1+fileCount)%fileCount; showIndex(n,true); lastSwitch=millis(); } }

// ---------- Meta/Thumbs ----------
uint32_t nowMillis(){ return millis(); }
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
bool makeThumbFromFB(uint16_t* fb, const String& tp){
  const int TW=40, TH=32;
  if(SPIFFS.exists(tp)) SPIFFS.remove(tp);
  if(!SPIFFS.exists("/thumbs")) SPIFFS.mkdir("/thumbs");
  File t=SPIFFS.open(tp,"w"); if(!t) return false;
  static uint16_t tline[TW];
  for(int ty=0; ty<TH; ++ty){
    int sy = ty*4;
    for(int tx=0; tx<TW; ++tx){
      int sx = tx*4;
      tline[tx] = fb[sy*W + sx];
    }
    t.write((uint8_t*)tline, TW*2);
  }
  t.close(); return true;
}
bool makeThumbFor(const String& imagePath){
  String b=baseName(imagePath); String tp="/thumbs/"+b+".traw";
  // Lade beliebiges Bild in fbNext und downsample
  if(!loadImageToFB(imagePath, fbNext)) return false;
  return makeThumbFromFB(fbNext, tp);
}

// ---------- Dateiliste + Sort ----------
void buildList(){
  fileCount=0;
  File root=SPIFFS.open("/");
  for(File f=root.openNextFile(); f && fileCount<MAX_FILES; f=root.openNextFile()){
    String n=String(f.name());
    if(isImage(n)){
      files[fileCount].name=n;
      files[fileCount].size=f.size();
      files[fileCount].ts=loadMetaTS(n);
      files[fileCount].thumb="/thumbs/"+baseName(n)+".traw";
      ++fileCount;
    }
  }
  if(currentIndex>=fileCount) currentIndex=max(0,fileCount-1);
}
void sortBy(int mode){
  for(int i=0;i<fileCount;i++){
    for(int j=i+1;j<fileCount;j++){
      bool sw=false;
      if(mode==0){ if(files[j].name < files[i].name) sw=true; }
      else if(mode==1){ if(files[j].size < files[i].size) sw=true; }
      else { if(files[j].ts   < files[i].ts)   sw=true; }
      if(sw){ Entry t=files[i]; files[i]=files[j]; files[j]=t; }
    }
  }
}

// ---------- HTML (wie gehabt, + Crossfade in Auswahl) ----------
const char* PAGE = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP Slideshow+</title>
<style>
 body{font-family:system-ui,Arial;margin:14px;background:#111;color:#eee}
 card{display:block;max-width:900px;margin:auto;padding:14px;border:1px solid #333;border-radius:12px;background:#1b1b1b}
 h1{font-size:18px;margin:0 0 10px}
 button,select,input{padding:8px 10px;border:0;border-radius:8px;background:#2c6ef2;color:#fff;cursor:pointer}
 .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
 .muted{color:#aaa} a{color:#9cf}
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
        <option value=5>Crossfade</option>
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
    let lo=buf[i], hi=buf[i+1], c=(hi<<8)|lo;
    let r5=(c>>11)&0x1F, g6=(c>>5)&0x3F, b5=c&0x1F;
    let R=(r5*255/31)|0, G=(g6*255/63)|0, B=(b5*255/31)|0;
    imgData.data[j++]=R; imgData.data[j++]=G; imgData.data[j++]=B; imgData.data[j++]=255;
  }
  ctx.putImageData(imgData,0,0);
}
async function show(name){ await fetch('/api/show?name='+encodeURIComponent(name)); refresh(); }
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
<p>PNG/JPG können auch direkt angezeigt werden. Für schnelle Thumbs/Slideshow bleiben RAW/Thumbs optimal.</p>
<form method="POST" action="/img/upload" enctype="multipart/form-data">
  <input type="file" name="file" accept="image/*" required><br><br>
  Dateiname (ohne Endung): <input name="name" placeholder="bild1" required>
  <button>Upload & (falls 160×128) RAW+Thumb erzeugen</button>
</form>
<p><a href="/">Zurück</a> · <a href="/fs">Dateimanager</a></p></card>
)HTML";

// ---------- Webserver JSON Builders ----------
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

// ---------- Webserver ----------
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
    if(r->hasParam("mode")){ int m=r->getParam("mode")->value().toInt(); transitionMode=constrain(m,0,5); }
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
  server.on("/fs", HTTP_GET, [FS_PAGE](AsyncWebServerRequest* r){ r->send(200,"text/html", FS_PAGE); });
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

  // Bild-Upload Seite (wie gehabt)
  server.on("/img", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200,"text/html", IMG_PAGE); });

  // Upload → wenn exakt 160×128: zusätzlich RAW+Thumb erzeugen (wie vorher)
  server.on("/img/upload", HTTP_POST,
    [](AsyncWebServerRequest* r){ r->send(200,"text/plain","OK"); buildList(); },
    [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final){
      static File tmp; static String tmpPath; static String base; static String targetRaw;
      if(!index){
        base = r->hasParam("name",true) ? r->getParam("name",true)->value() : "bild";
        if(base.indexOf('.')>=0) base=base.substring(0,base.indexOf('.'));
        tmpPath="/__up.bin"; if(SPIFFS.exists(tmpPath)) SPIFFS.remove(tmpPath);
        tmp=SPIFFS.open(tmpPath,"w");
        targetRaw="/"+base+".raw";
      }
      if(tmp) tmp.write(data,len);
      if(final){
        if(tmp) tmp.close();
        // Wenn 160x128 → RAW+Thumb erzeugen (egal ob PNG/JPG)
        bool ok=false;
        // Lade in FB und prüfe Größe anhand png/jpg header, sonst skip RAW
        String low=filename; low.toLowerCase();
        bool fbOK=false;
        if(low.endsWith(".jpg")||low.endsWith(".jpeg")) fbOK=loadJPGtoFB(tmpPath, fbNext);
        else if(low.endsWith(".png")) fbOK=loadPNGtoFB(tmpPath, fbNext);
        if(fbOK){
          // Ist das Bild exakt 160x128? (prüfen auf nicht-schwarze Ränder ist aufwändig; wir nehmen „exakt FB gefüllt“ an)
          // -> Schreibe immer RAW als 160x128
          File f=SPIFFS.open(targetRaw,"w");
          if(f){ f.write((uint8_t*)fbNext, W*H*2); f.close(); ok=true; saveMeta(targetRaw); makeThumbFromFB(fbNext, "/thumbs/"+base+".traw"); }
        }
        // Ursprungsdatei ablegen (jpg/png) unter originalem Namen:
        String ext = low.substring(low.lastIndexOf('.'));
        String dst = "/"+base+ext;
        if(SPIFFS.exists(dst)) SPIFFS.remove(dst);
        SPIFFS.rename(tmpPath, dst);
        if(!ok) SPIFFS.remove(targetRaw); // kein RAW erzeugt
      }
    }
  );

  // Direkte Datei-Auslieferung (inkl. /thumbs/*.traw)
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
