/***** ESP32-S2 mini – Scope Pro All-In *****/
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <SPIFFS.h>

extern "C" {
  #include "esp_task_wdt.h"
}

/* =================== Hardware (anpassen) =================== */
#define TFT_CS    5
#define TFT_RST   6
#define TFT_DC    7
#define TFT_MOSI  11
#define TFT_SCLK  12
#define TFT_LED   13   // Backlight (LEDC)

#define BTN_NEXT  8    // aktiv LOW (INPUT_PULLUP)
#define BTN_OK    9

// Optionaler Encoder (oder -1)
#define ENC_A    -1
#define ENC_B    -1
#define ENC_BTN  -1

// ADC-Kanäle
#define PIN_ADC1  2
#define PIN_ADC2 -1     // zweiten Kanal anschließen oder -1 lassen

#ifndef ST77XX_DARKGREY
  #define ST77XX_DARKGREY 0x7BEF
#endif

/* =================== Netz / OTA =================== */
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT";     // leer => AP-Fallback

// OTA Basic-Auth
const char* OTA_USER = "admin";
const char* OTA_PASS = "esp32s2scope";

/* =================== Globale Objekte =================== */
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
AsyncWebServer server(80);
Preferences prefs;

/* =================== Splash (Sinus, LUT) =================== */
static const uint8_t SINE_Y[160] PROGMEM = {
  56,58,60,62,64,66,68,70,72,73,75,77,78,80,81,82,83,84,85,86,
  86,87,87,88,88,88,88,88,87,87,86,85,84,83,82,81,80,78,77,75,
  73,72,70,68,66,64,62,60,58,56,54,52,50,48,46,44,42,41,39,37,
  36,34,33,32,31,30,29,28,27,26,26,25,25,25,25,25,26,26,27,28,
  29,30,31,32,33,34,36,37,39,41,42,44,46,48,50,52,54,56,58,60,
  62,64,66,68,70,72,73,75,77,78,80,81,82,83,84,85,86,87,87,88,
  88,88,88,88,87,87,86,86,85,84,83,82,81,80,78,77,75,73,72,70,
  68,66,64,62,60,58,56,54,52,50,48,46,44,42,41,39,37,36,34,33
};

/* =================== Scope/Signalkette =================== */
static const int   N = 128;        // Samples/Frame
uint16_t ch1[N], ch2[N], draw1[N], work[N];

uint16_t trigLevel = 2048;
uint32_t holdoffMs = 20;
enum TrigMode { TM_AUTO, TM_NORMAL, TM_SINGLE };
TrigMode trigMode = TM_AUTO;
bool     trigArmed = true;

uint8_t  lcdBrightness = 255;            // 0..255 (LEDC)
uint16_t traceColor   = ST77XX_GREEN;
uint16_t trace2Color  = ST77XX_MAGENTA;
bool     showGrid     = true;

enum SnapMode { SNAP_NONE, SNAP_PEAK, SNAP_ZERO };
SnapMode snapMode = SNAP_NONE;

bool     showCursors  = true;
int      cursorA = 32, cursorB = 96;     // 0..159

uint32_t samplePauseUs = 80;             // „Zusatz“-Pause
float    vGain   = (110000.0f+10000.0f)/10000.0f;  // Teiler ≈12.0 (skalierbar)
float    vOffset = 0.0f;                 // DC-Offset (Kalibrierung)

/* Energiesparen */
uint32_t idleTimeoutMs = 30000;          // 30 s
uint8_t  dimLevel      = 60;             // gedimmt 0..255
uint32_t lastUserMs    = 0;
bool     isDimmed      = false;

/* Anzeige/Timing */
uint32_t lastDraw=0, tStart=0, tClock=0, tHold=0;

/* ============== Buttons / Encoder / Input ============== */
bool readBtn(uint8_t pin){ return (pin>=0)? digitalRead(pin)==LOW : false; }
bool eNext=false, eOk=false;
uint32_t lastBtnPoll=0;
int8_t encStep=0;

void pollEncoder(){
  if (ENC_A<0 || ENC_B<0) return;
  static int lastA=HIGH, lastB=HIGH;
  int A=digitalRead(ENC_A), B=digitalRead(ENC_B);
  if (A!=lastA){
    if (A==HIGH) encStep += (B==LOW)? +1 : -1;
    lastA=A;
  }
  lastB=B;
}
void markUserActivity(){
  lastUserMs = millis();
  if (isDimmed){ isDimmed=false; ledcWrite(0, lcdBrightness); }
}
void pollInput(){
  if (millis()-lastBtnPoll<8) return;
  lastBtnPoll = millis();

  bool n = readBtn(BTN_NEXT);
  bool o = readBtn(BTN_OK);
  static bool ln=false, lo=false;
  if (n && !ln){ eNext=true; markUserActivity(); }
  if (o && !lo){ eOk  =true; markUserActivity(); }
  ln=n; lo=o;

  if (ENC_BTN>=0){
    static bool lb=false; bool b=readBtn(ENC_BTN);
    if (b && !lb){ eOk=true; markUserActivity(); }
    lb=b;
  }

  pollEncoder();
  if (encStep){ markUserActivity(); }

  while (Serial.available()){
    char c=(char)Serial.read();
    if (c=='n'){ eNext=true; }
    if (c=='o'){ eOk=true;   }
    if (c=='b'){ eNext=eOk=false; state = MENU; }
    if (c=='+'){ trigLevel = min<uint16_t>(4095, trigLevel+205); }
    if (c=='-'){ trigLevel = trigLevel>=205? trigLevel-205 : 0; }
    markUserActivity();
  }
}

/* ============== Utility/Anzeige ============== */
inline int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
inline float adcToVolt(uint16_t a){ return (a*(3.3f/4095.0f))*vGain + vOffset; }

void header(const char* t){
  tft.fillRect(0,0,160,16,ST77XX_BLACK);
  tft.setCursor(2,2); tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE);
  tft.print(t);
}
void clockBar(){
  uint32_t s=(millis()-tStart)/1000; int h=(s/3600)%24, m=(s/60)%60, ss=s%60;
  char buf[9]; sprintf(buf,"%02d:%02d:%02d",h,m,ss);
  tft.fillRect(100,0,60,16,ST77XX_BLACK);
  tft.setCursor(102,2); tft.setTextColor(ST77XX_WHITE,ST77XX_BLACK); tft.print(buf);
}

/* ============== Energiesparen ============== */
void powerIdleTick(){
  if (!idleTimeoutMs) return;
  if (!isDimmed && millis()-lastUserMs > idleTimeoutMs){
    isDimmed = true;
    ledcWrite(0, dimLevel);
  }
}

/* ============== Splash ============== */
void splash(){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1); tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(6,8); tft.print("Robin's Oszilloskop wird geladen");

  uint16_t g=tft.color565(64,64,64);
  tft.drawLine(0,64,159,64,g);
  for(int x=0;x<159;x++){
    uint8_t y1=pgm_read_byte(&SINE_Y[x]), y2=pgm_read_byte(&SINE_Y[x+1]);
    tft.drawLine(x,y1,x+1,y2,ST77XX_CYAN);
  }
  int bx=10,by=104,bw=140,bh=12; tft.drawRect(bx-1,by-1,bw+2,bh+2,g);
  uint32_t t0=millis();
  while(millis()-t0<5000){
    pollInput();
    float p=(millis()-t0)/5000.0f; if (p>1)p=1;
    tft.fillRect(bx,by,(int)(bw*p),bh,ST77XX_GREEN);
    delay(16);
    if (eOk){ eOk=false; break; }
  }
}

/* ============== Grid / Scales ============== */
void drawGrid(){
  if(!showGrid) return;
  uint16_t c=tft.color565(32,32,32);
  for(int x=0;x<160;x+=20) tft.drawFastVLine(x,16,80,c);
  for(int y=16;y<=96;y+=10) tft.drawFastHLine(0,y,160,c);
}
void drawScales(){
  float frameTime_ms = (N*(samplePauseUs))/1000.0f;
  float tdiv_ms = frameTime_ms/10.0f;
  tft.setCursor(2,100); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
  tft.print("Trig "); tft.print((int)(trigLevel*100/4095)); tft.print("%  ");
  tft.print("T/div "); (tdiv_ms>=1)? (tft.print(tdiv_ms,1), tft.print("ms")) :
                                   (tft.print(tdiv_ms*1000,0), tft.print("us"));
  float counts_per_px = 4095.0f/80.0f;
  float v_per_px = adcToVolt((uint16_t)(counts_per_px)) - adcToVolt(0);
  float vdiv = v_per_px*8.0f;
  tft.setCursor(2,112); tft.setTextColor(ST77XX_WHITE);
  tft.print("V/div "); tft.print(vdiv,2); tft.print("  ");
}

/* ============== Trigger / Reorder ============== */
bool reorderForTrigger(const uint16_t* in, uint16_t* out){
  int idx=-1;
  for(int i=1;i<N;i++) if (in[i-1]<trigLevel && in[i]>=trigLevel){ idx=i; break; }
  if (idx>0){
    int k=0; for(int i=idx;i<N && k<N;i++) out[k++]=in[i];
            for(int i=0;  i<idx && k<N;i++) out[k++]=in[i];
    return true;
  }
  return false;
}

/* ============== Cursor + Snap ============== */
void applySnapToCursor(int &cx, const uint16_t* s){
  if (snapMode==SNAP_NONE) return;
  int best = cx;
  if (snapMode==SNAP_ZERO){
    // suche Crossing nahe cx (steigend oder fallend)
    int radius=10, bestd=1e9;
    for (int i=max(1,cx-radius); i<min(N,cx+radius); i++){
      bool cross = ( (s[i-1]<2048 && s[i]>=2048) || (s[i-1]>2048 && s[i]<=2048) );
      if (cross){ int d=abs(i-cx); if (d<bestd){ bestd=d; best=i; } }
    }
  } else if (snapMode==SNAP_PEAK){
    int radius=10, bestd=1e9;
    for (int i=max(1,cx-radius); i<min(N-1,cx+radius); i++){
      if (s[i]>=s[i-1] && s[i]>=s[i+1]){
        int d=abs(i-cx); if (d<bestd){ bestd=d; best=i; }
      }
    }
  }
  cx = best;
}
void drawCursors(const uint16_t* s){
  if(!showCursors) return;
  uint16_t cA=ST77XX_ORANGE, cB=ST77XX_RED;
  int xA=clampi(cursorA,0,159), xB=clampi(cursorB,0,159);
  tft.drawFastVLine(xA,16,80,cA);
  tft.drawFastVLine(xB,16,80,cB);

  auto vAt=[&](int xi)->float{ int i=clampi(xi,0,N-1); return adcToVolt(s[i]); };
  float dV=vAt(cursorB)-vAt(cursorA);
  float dT_ms=((cursorB-cursorA)*samplePauseUs)/1000.0f;

  tft.setCursor(90,112); tft.setTextColor(ST77XX_CYAN);
  tft.print("dV "); tft.print(dV,2); tft.print("  dT "); tft.print(dT_ms,2); tft.print("ms");
}

/* ============== FFT (128) + Peak-Hold ============== */
typedef struct { float r,i; } cpx;
void fft128_mag(const uint16_t* s, float* out){ // out: 64 bins
  static cpx x[128];
  for(int n=0;n<128;n++){
    float w=0.5f-0.5f*cosf((2.0f*PI*n)/127.0f);
    float v=(s[n]-2048)*(3.3f/4095.0f)*vGain;
    x[n].r=v*w; x[n].i=0.0f;
  }
  for(int step=1; step<128; step<<=1){
    float theta=-PI/step;
    float wpr=-2.0f*sinf(0.5f*theta)*sinf(0.5f*theta);
    float wpi=sinf(theta);
    for(int m=0;m<step;m++){
      float wr=1.0f, wi=0.0f;
      for(int k=m;k<128;k+=(step<<1)){
        int j=k+step;
        float tr=wr*x[j].r - wi*x[j].i;
        float ti=wr*x[j].i + wi*x[j].r;
        x[j].r=x[k].r - tr; x[j].i=x[k].i - ti;
        x[k].r+=tr;         x[k].i+=ti;
      }
      float tmp=wr; wr=wr+wpr*wr - wpi*wi; wi=wi+wpr*wi + wpi*tmp;
    }
  }
  for(int i=0;i<64;i++){
    out[i]=sqrtf(x[i].r*x[i].r + x[i].i*x[i].i);
  }
}

float peakHold[64]={0};
void drawSpectrum(const float* mag){
  tft.fillRect(0,16,160,112,ST77XX_BLACK);
  if (showGrid){
    uint16_t c=tft.color565(32,32,32);
    for(int x=0;x<160;x+=20) tft.drawFastVLine(x,16,112-16,c);
    for(int y=16;y<=112;y+=16) tft.drawFastHLine(0,y,160,c);
  }
  float m=1e-9f; for(int i=1;i<64;i++) if(mag[i]>m) m=mag[i];
  int bestI=1; float best=0;
  for(int i=1;i<64;i++){
    // Peak-Hold mit leichter Abklingzeit
    peakHold[i] = max(peakHold[i]*0.995f, mag[i]);
    int x=i*(160/64);
    int h=(int)((mag[i]/m)*80.0f); h=clampi(h,0,80);
    int hp=(int)((peakHold[i]/m)*80.0f); hp=clampi(hp,0,80);
    tft.drawFastVLine(x, 96-h, h, ST77XX_YELLOW);
    tft.drawFastVLine(x, 96-hp, 1, ST77XX_RED); // kleine Peak-Markierung
    if (mag[i]>best){ best=mag[i]; bestI=i; }
  }
  // Top-Frequenz marker
  int x=bestI*(160/64);
  tft.drawFastVLine(x,16,96-16, ST77XX_MAGENTA);
  // Frequenz abschätzen:
  float fs_approx = 1e6f / max(1.0f, (float)samplePauseUs + 30.0f); // grob
  float freq = (bestI * fs_approx) / 128.0f;
  tft.setCursor(4,98); tft.setTextColor(ST77XX_CYAN); tft.setTextSize(1);
  tft.print("Top ~"); if (freq>=1000) { tft.print(freq/1000.0f,2); tft.print("kHz"); } else { tft.print(freq,1); tft.print("Hz"); }
}

/* ============== Sampling ============== */
void sampleOnce(){
  for(int i=0;i<N;i++){
    ch1[i]=analogRead(PIN_ADC1);
    if (PIN_ADC2>=0) ch2[i]=analogRead(PIN_ADC2);
    if (samplePauseUs) delayMicroseconds(samplePauseUs);
  }
}

/* ============== Plot-Framebuffer (echter Screenshot) ============== */
// RAM-Framebuffer für Plotbereich (160x80 px RGB565 ≈ 25.6 KB)
uint16_t fbPlot[160*80];

inline void fbClear(){ memset(fbPlot, 0, sizeof(fbPlot)); }
inline void fbPix(int x,int y,uint16_t c){ if(x>=0&&x<160&&y>=0&&y<80) fbPlot[y*160+x]=c; }
void fbLine(int x0,int y0,int x1,int y1,uint16_t c){
  int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy, e2;
  while(true){
    fbPix(x0,y0,c);
    if (x0==x1 && y0==y1) break;
    e2=2*err;
    if (e2>=dy){ err+=dy; x0+=sx; }
    if (e2<=dx){ err+=dx; y0+=sy; }
  }
}

/* Zeichnet Plot auf Display **und** fbPlot */
void drawWaveDual(){
  // Bildschirm
  tft.fillRect(0,16,160,80,ST77XX_BLACK);
  drawGrid();
  // Framebuffer
  fbClear();
  uint16_t gridc = showGrid? tft.color565(32,32,32) : 0;
  if (showGrid){
    for(int x=0;x<160;x+=20) fbLine(x,0,x,79,gridc);
    for(int y=0;y<=80;y+=10) fbLine(0,y,159,y,gridc);
  }
  // CH1
  for(int x=0;x<min(N-1,159);x++){
    int y1=map(draw1[x],0,4095,79,0);
    int y2=map(draw1[x+1],0,4095,79,0);
    tft.drawLine(x,16+y1, x+1,16+y2, traceColor);
    fbLine(x,y1, x+1,y2, traceColor);
  }
  // CH2/MATH
  if (PIN_ADC2>=0){
    for(int i=0;i<N;i++){
      int32_t d=(int32_t)ch1[i] - (int32_t)ch2[i] + 2048;
      work[i]=(uint16_t)clampi(d,0,4095);
    }
    uint16_t tmp[N]; reorderForTrigger(work,tmp);
    for(int x=0;x<min(N-1,159);x++){
      int y1=map(tmp[x],0,4095,79,0);
      int y2=map(tmp[x+1],0,4095,79,0);
      tft.drawLine(x,16+y1, x+1,16+y2, trace2Color);
      fbLine(x,y1, x+1,y2, trace2Color);
    }
  }
  drawScales();
  drawCursors(draw1);
}

/* ============== Dateien: CSV + echter BMP-Screenshot ============== */
bool saveCSV(const char* path, const uint16_t* s){
  File f=SPIFFS.open(path, FILE_WRITE); if(!f) return false;
  f.println("i,ADC,Volt");
  for(int i=0;i<N;i++){ f.printf("%d,%u,%.6f\n", i, s[i], adcToVolt(s[i])); }
  f.close(); return true;
}
bool saveBMPPlot(const char* path){
  // Speichert fbPlot (160x80) als 16-bit RGB565 BMP (keine Kompression)
  File f=SPIFFS.open(path, FILE_WRITE); if(!f) return false;
  const uint32_t W=160, H=80, BPP=2;
  uint32_t fileSize=54 + W*H*BPP;
  uint8_t header[54] = {
    'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0,
    40,0,0,0, 0,0,0,0, 0,0,0,0, 1,0, 16,0,
    0,0,0,0, 0,0,0,0, 0x13,0x0B,0,0, 0x13,0x0B,0,0, 0,0,0,0, 0,0,0,0
  };
  header[2]= fileSize & 0xFF; header[3]=(fileSize>>8)&0xFF; header[4]=(fileSize>>16)&0xFF; header[5]=(fileSize>>24)&0xFF;
  header[18]= W & 0xFF; header[19]=(W>>8)&0xFF;
  header[22]= H & 0xFF; header[23]=(H>>8)&0xFF;
  f.write(header,54);
  // BMP ist bottom-up: Zeilen von unten nach oben
  for(int y=H-1; y>=0; y--){
    const uint16_t* row = &fbPlot[y*W];
    f.write((const uint8_t*)row, W*2);
  }
  f.close(); return true;
}

/* ============== Web (OTA, Live, Files, Cal) ============== */
bool checkAuth(AsyncWebServerRequest *r){
  if (OTA_USER[0]==0) return true;
  if (!r->authenticate(OTA_USER, OTA_PASS)){ r->requestAuthentication(); return false; }
  return true;
}

void setupWeb(){
  // Index (OTA + Links)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    if(!checkAuth(r)) return;
    String ip = (WiFi.getMode()==WIFI_AP)? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    String h;
    h+= "<h3>ESP32-S2 Scope</h3><p>IP: "+ip+"</p>";
    h+= "<form method='POST' action='/update' enctype='multipart/form-data'>";
    h+= "<input type='file' name='firmware'> <input type='submit' value='Flash'></form>";
    h+= "<p><a href='/live'>Live-Trace</a> | <a href='/files'>Dateien</a> | <a href='/cal'>Kalibrieren</a></p>";
    r->send(200,"text/html",h);
  });

  // OTA upload
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *r){ if(!checkAuth(r)) return;
      bool ok=!Update.hasError();
      r->send(200,"text/plain", ok? "Update OK, rebooting..." : "Update FAILED");
      if(ok){ delay(300); ESP.restart(); } },
    [](AsyncWebServerRequest *r, String, size_t idx, uint8_t*data,size_t len, bool final){
      if(!checkAuth(r)) return;
      if(!idx) Update.begin(UPDATE_SIZE_UNKNOWN);
      if(Update.write(data,len)!=len) Update.printError(Serial);
      if(final){ if(!Update.end(true)) Update.printError(Serial); }
    }
  );

  // Live JSON (128 Samples)
  server.on("/trace", HTTP_GET, [](AsyncWebServerRequest *r){
    if(!checkAuth(r)) return;
    String j="{\"n\":128,\"u\":"+String(samplePauseUs)+",\"data\":[";
    for(int i=0;i<N;i++){ if(i) j+=','; j+=String(draw1[i]); }
    j+="]}";
    r->send(200,"application/json",j);
  });

  // Live-Seite (Canvas)
  server.on("/live", HTTP_GET, [](AsyncWebServerRequest *r){
    if(!checkAuth(r)) return;
    String h;
    h+= "<!doctype html><meta name=viewport content='width=device-width, initial-scale=1.0'>";
    h+= "<canvas id=c width=640 height=320 style='width:100%;max-width:640px;border:1px solid #888'></canvas>";
    h+= "<script>const c=document.getElementById('c'),x=c.getContext('2d');";
    h+= "async function tick(){try{let r=await fetch('/trace');let j=await r.json();";
    h+= "x.fillStyle='#000';x.fillRect(0,0,c.width,c.height);x.strokeStyle='#0f0';x.beginPath();";
    h+= "for(let i=0;i<j.n;i++){let px=i*(c.width/(j.n-1));let py=(1-j.data[i]/4095)*(c.height-20)+10; if(i==0)x.moveTo(px,py);else x.lineTo(px,py);} x.stroke();}catch(e){} setTimeout(tick,200);} tick();</script>";
    r->send(200,"text/html",h);
  });

  // Dateien
  server.on("/files", HTTP_GET, [](AsyncWebServerRequest *r){
    if(!checkAuth(r)) return;
    String h="<h3>Files (SPIFFS)</h3><ul>";
    File root=SPIFFS.open("/"); File f=root.openNextFile();
    while(f){ h+="<li><a href='"+String(f.name())+"'>"+String(f.name())+"</a> ("+String((int)f.size())+"B)</li>"; f=root.openNextFile(); }
    h+="</ul><p>CSV: /last.csv, BMP: /last.bmp</p>";
    r->send(200,"text/html",h);
  });
  server.onNotFound([](AsyncWebServerRequest *r){
    if(!checkAuth(r)) return;
    String path=r->url();
    if (SPIFFS.exists(path)){ r->send(SPIFFS, path, "application/octet-stream", true); }
    else r->send(404,"text/plain","Not found");
  });

  // Kalibrierseite (2-Punkt-LinFit auf vCalc)
  server.on("/cal", HTTP_GET, [](AsyncWebServerRequest *r){
    if(!checkAuth(r)) return;
    // aktueller Vin (Mittelwert)
    uint32_t acc=0; for(int i=0;i<N;i++) acc+=draw1[i];
    float vCalc = (acc/(float)N)*(3.3f/4095.0f)*vGain + vOffset;
    String h;
    h+= "<h3>Kalibrieren (2-Punkt)</h3>";
    h+= "<p>Aktuell: vCalc = "+String(vCalc,4)+" V</p>";
    h+= "<form method='GET' action='/calset'>";
    h+= "Punkt1: vShown1=<input name='vs1' value='"+String(vCalc,3)+"'>  vTrue1=<input name='vt1' value='"+String(vCalc,3)+"'><br>";
    h+= "Punkt2: vShown2=<input name='vs2' value='"+String(vCalc+0.5f,3)+"'>  vTrue2=<input name='vt2' value='"+String(vCalc+0.5f,3)+"'><br>";
    h+= "<input type='submit' value='Fit & Save'></form>";
    r->send(200,"text/html",h);
  });
  server.on("/calset", HTTP_GET, [](AsyncWebServerRequest *r){
    if(!checkAuth(r)) return;
    float vs1=r->getParam("vs1")->value().toFloat();
    float vt1=r->getParam("vt1")->value().toFloat();
    float vs2=r->getParam("vs2")->value().toFloat();
    float vt2=r->getParam("vt2")->value().toFloat();
    // vt = a*vs + b
    float a=(vt2-vt1)/max(1e-6f,(vs2-vs1));
    float b=vt1 - a*vs1;
    // Unser Modell: v = (raw*(3.3/4095))*gain + offset
    // => neuer gain = a*alter_gain ; neuer offset = a*offset + b
    vGain   = a * vGain;
    vOffset = a * vOffset + b;
    prefs.begin("osc", false); prefs.putFloat("gain", vGain); prefs.putFloat("offs", vOffset); prefs.end();
    r->send(200,"text/plain","OK saved. gain="+String(vGain,6)+" offset="+String(vOffset,6));
  });

  server.begin();
}

/* ============== States & Menüs ============== */
enum AppState { SPLASH_ST, MENU, SCOPE, FFTMODE, CLOCK, OTA, SETTINGS, FILES };
AppState state = SPLASH_ST;

enum { MI_SCOPE, MI_FFT, MI_CLOCK, MI_OTA, MI_SETTINGS, MI_FILES, MI_COUNT };
const char* MENU_TXT[MI_COUNT]={"Oszilloskop","Spektrum (FFT)","Uhr","OTA-Update","Einstellungen","Dateien"};
int menuIdx=0;

void drawMenu(){
  tft.fillScreen(ST77XX_BLACK); header("Menue");
  for(int i=0;i<MI_COUNT;i++){
    int y=24+i*18;
    if(i==menuIdx){ tft.fillRect(6,y-2,148,14,ST77XX_NAVY); tft.setTextColor(ST77XX_YELLOW); }
    else tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(10,y); tft.setTextSize(1); tft.print(MENU_TXT[i]);
  }
  tft.setTextColor(ST77XX_DARKGREEN);
  tft.setCursor(8, 24+MI_COUNT*18+8); tft.print("NEXT: weiter  OK: auswaehlen");
}
void handleMenu(){
  if(eNext){ eNext=false; menuIdx=(menuIdx+1)%MI_COUNT; drawMenu(); }
  if(eOk){
    eOk=false;
    if(menuIdx==MI_SCOPE){ state=SCOPE; header("Mode: SCOPE"); }
    if(menuIdx==MI_FFT)  { state=FFTMODE; header("Mode: FFT");  }
    if(menuIdx==MI_CLOCK){ state=CLOCK; tft.fillScreen(ST77XX_BLACK); header("Mode: UHR"); }
    if(menuIdx==MI_OTA)  { state=OTA;   tft.fillScreen(ST77XX_BLACK); header("Mode: OTA"); }
    if(menuIdx==MI_SETTINGS){ state=SETTINGS; tft.fillScreen(ST77XX_BLACK); }
    if(menuIdx==MI_FILES){ state=FILES; tft.fillScreen(ST77XX_BLACK); }
  }
}

/* ============== Scope Loop ============== */
void scopeLoop(){
  if(!trigArmed && (millis()-tHold)>=holdoffMs) trigArmed=true;

  sampleOnce();
  bool got = reorderForTrigger(ch1, draw1);
  if (!got){
    if (trigMode==TM_AUTO){ memcpy(draw1,ch1,sizeof(draw1)); }
    else if (trigMode==TM_NORMAL){ if (!trigArmed) return; else memcpy(draw1,ch1,sizeof(draw1)); }
    else if (trigMode==TM_SINGLE){ if (!trigArmed) return; else memcpy(draw1,ch1,sizeof(draw1)); }
  } else {
    tHold=millis(); trigArmed=false; if(trigMode==TM_SINGLE) trigMode=TM_NORMAL;
  }

  // Cursormove via Encoder
  if (encStep){
    // Toggle: A/B über NEXT
    static bool which=false;
    if (eNext){ eNext=false; which=!which; }
    if (!which) cursorA=clampi(cursorA+encStep,0,159);
    else        cursorB=clampi(cursorB+encStep,0,159);
    encStep=0;
    // Snap
    applySnapToCursor(cursorA, draw1);
    applySnapToCursor(cursorB, draw1);
  } else if (eNext){
    // NEXT ohne Encoder: A/B togglen
    eNext=false;
  }

  if (eOk){ eOk=false; state=MENU; drawMenu(); return; }

  if(millis()-lastDraw>30){
    lastDraw=millis();
    drawWaveDual(); // Display + Framebuffer für Screenshot
  }
}

/* ============== FFT Mode ============== */
void fftLoop(){
  sampleOnce();
  float mag[64]; fft128_mag(ch1,mag);
  drawSpectrum(mag);
  if (eOk){ eOk=false; state=MENU; drawMenu(); }
}

/* ============== Uhr ============== */
void clockLoop(){
  static uint32_t last=0;
  if (millis()-last>200){
    last=millis();
    tft.fillRect(0,16,160,112,ST77XX_BLACK);
    uint32_t s=(millis()-tStart)/1000; int h=(s/3600)%24, m=(s/60)%60, ss=s%60;
    char buf[9]; sprintf(buf,"%02d:%02d:%02d",h,m,ss);
    tft.setTextSize(2); tft.setTextColor(ST77XX_CYAN); tft.setCursor(20,60); tft.print(buf);
    tft.setTextSize(1); tft.setTextColor(ST77XX_YELLOW); tft.setCursor(8,100);
    tft.print("OK: zurueck  NEXT: (keine Aktion)");
  }
  if (eOk){ eOk=false; state=MENU; drawMenu(); }
}

/* ============== Einstellungen ============== */
enum SetItem {
  SI_TRIGMODE, SI_HOLDOFF, SI_TRIGLVL, SI_BRIGHT, SI_TS, SI_COLOR,
  SI_SNAP, SI_GRID, SI_CURSORS, SI_GAIN, SI_OFFS, SI_IDLE, SI_DIM, SI_BACK, SI_COUNT
};
int si=0; bool edit=false;

const char* trigName(TrigMode m){ return m==TM_AUTO?"AUTO": m==TM_NORMAL?"NORM":"SINGLE"; }
const char* snapName(SnapMode m){ return m==SNAP_NONE?"NONE": m==SNAP_PEAK?"PEAK":"ZERO"; }

uint16_t nextColor(uint16_t c){
  static const uint16_t t[]={ ST77XX_GREEN, ST77XX_CYAN, ST77XX_YELLOW, ST77XX_MAGENTA, ST77XX_WHITE, ST77XX_RED };
  for (int i=0;i<6;i++) if (t[i]==c) return t[(i+1)%6]; return t[0];
}

void drawSettings(){
  tft.fillScreen(ST77XX_BLACK); header("Einstellungen");
  const char* name[SI_COUNT]={
    "Trigger Mode","Holdoff [ms]","Triggerlevel","Helligkeit","Tpause [us]","Kurvenfarbe",
    "Cursor Snap","Gitter","Cursors","Gain","Offset [V]","Idle [ms]","Dim Level","Zurueck"
  };
  for(int i=0;i<SI_COUNT;i++){
    int y=24+i*14; bool sel=(i==si);
    if(sel){ tft.fillRect(6,y-2,148,12,ST77XX_DARKGREY); tft.setTextColor(ST77XX_BLACK); }
    else    { tft.setTextColor(ST77XX_WHITE); }
    tft.setCursor(10,y); tft.setTextSize(1); tft.print(name[i]);
    tft.setCursor(120,y); tft.setTextColor(sel?ST77XX_BLACK:ST77XX_GREEN);
    switch(i){
      case SI_TRIGMODE: tft.print(trigName(trigMode)); break;
      case SI_HOLDOFF:  tft.print((int)holdoffMs); break;
      case SI_TRIGLVL:  tft.print((int)(trigLevel*100/4095)); tft.print("%"); break;
      case SI_BRIGHT:   tft.print((int)lcdBrightness); break;
      case SI_TS:       tft.print((int)samplePauseUs); break;
      case SI_COLOR:    tft.print("0x"); tft.print(traceColor, HEX); break;
      case SI_SNAP:     tft.print(snapName(snapMode)); break;
      case SI_GRID:     tft.print(showGrid?"AN":"AUS"); break;
      case SI_CURSORS:  tft.print(showCursors?"AN":"AUS"); break;
      case SI_GAIN:     tft.print(vGain,3); break;
      case SI_OFFS:     tft.print(vOffset,3); break;
      case SI_IDLE:     tft.print((int)idleTimeoutMs); break;
      case SI_DIM:      tft.print((int)dimLevel); break;
      default: break;
    }
  }
  tft.setTextColor(ST77XX_YELLOW); tft.setCursor(8, 24+SI_COUNT*14+6);
  tft.print(edit? "NEXT: Wert  OK: Speichern" : "NEXT: weiter  OK: editieren");
}

void settingsLoop(){
  if(!edit){
    if(eNext){ eNext=false; si=(si+1)%SI_COUNT; drawSettings(); }
    if(eOk){ eOk=false;
      if (si==SI_BACK){ // speichern & zurück
        prefs.begin("osc", false);
        prefs.putUShort("trig", trigLevel);
        prefs.putUInt  ("hold", holdoffMs);
        prefs.putUChar ("tmod", trigMode==TM_AUTO?0: trigMode==TM_NORMAL?1:2);
        prefs.putUChar ("bri",  lcdBrightness);
        prefs.putUInt  ("ts",   samplePauseUs);
        prefs.putUShort("col",  traceColor);
        prefs.putUChar ("snap", snapMode==SNAP_NONE?0: snapMode==SNAP_PEAK?1:2);
        prefs.putBool  ("grid", showGrid);
        prefs.putBool  ("cur",  showCursors);
        prefs.putFloat ("gain", vGain);
        prefs.putFloat ("offs", vOffset);
        prefs.putUInt  ("idle", idleTimeoutMs);
        prefs.putUChar ("dim",  dimLevel);
        prefs.end();
        state=MENU; drawMenu(); return;
      }
      edit=true; drawSettings();
    }
  } else {
    if(eNext){ eNext=false;
      switch(si){
        case SI_TRIGMODE: trigMode=(trigMode==TM_AUTO?TM_NORMAL: trigMode==TM_NORMAL?TM_SINGLE:TM_AUTO); break;
        case SI_HOLDOFF:  holdoffMs = min<uint32_t>(1000, holdoffMs+10); break;
        case SI_TRIGLVL:  trigLevel = min<uint16_t>(4095, trigLevel+205); break;
        case SI_BRIGHT:   lcdBrightness = min<uint8_t>(255, (uint8_t)(lcdBrightness+16)); ledcWrite(0,lcdBrightness); break;
        case SI_TS:       samplePauseUs = min<uint32_t>(2000, samplePauseUs+20); break;
        case SI_COLOR:    traceColor = nextColor(traceColor); break;
        case SI_SNAP:     snapMode = (SnapMode)((snapMode+1)%3); break;
        case SI_GRID:     showGrid=!showGrid; break;
        case SI_CURSORS:  showCursors=!showCursors; break;
        case SI_GAIN:     vGain += 0.1f; break;
        case SI_OFFS:     vOffset += 0.01f; break;
        case SI_IDLE:     idleTimeoutMs = min<uint32_t>(120000, idleTimeoutMs+5000); break;
        case SI_DIM:      dimLevel = min<uint8_t>(250, (uint8_t)(dimLevel+10)); break;
        default: break;
      }
      drawSettings();
    }
    if(eOk){ eOk=false; edit=false; drawSettings(); }
  }
}

/* ============== Files-Menü (Speichern) ============== */
void filesScreen(bool okCSV,bool okBMP){
  tft.fillRect(0,16,160,112,ST77XX_BLACK);
  tft.setCursor(8,24); tft.setTextColor(ST77XX_WHITE); tft.print("Dateien:");
  tft.setCursor(8,44); tft.print("OK: CSV -> /last.csv");
  tft.setCursor(8,58); tft.print("NEXT: BMP -> /last.bmp");
  if (okCSV){ tft.setCursor(8,78); tft.setTextColor(ST77XX_GREEN); tft.print("CSV gespeichert"); }
  if (okBMP){ tft.setCursor(8,92); tft.setTextColor(ST77XX_GREEN); tft.print("BMP gespeichert"); }
}
void filesLoop(){
  static bool show=true; if(show){ show=false; filesScreen(false,false); }
  if (eOk){ eOk=false; bool ok=saveCSV("/last.csv", draw1); filesScreen(ok,false); }
  if (eNext){ eNext=false; bool ok=saveBMPPlot("/last.bmp"); filesScreen(false,ok); }
}

/* ============== Setup / Loop ============== */
void loadPrefs(){
  prefs.begin("osc", true);
  trigLevel     = prefs.getUShort("trig", 2048);
  holdoffMs     = prefs.getUInt  ("hold", 20);
  uint8_t tm    = prefs.getUChar ("tmod", 0); trigMode=(tm==0?TM_AUTO: tm==1?TM_NORMAL:TM_SINGLE);
  lcdBrightness = prefs.getUChar ("bri", 255);
  samplePauseUs = prefs.getUInt  ("ts", 80);
  traceColor    = prefs.getUShort("col", ST77XX_GREEN);
  uint8_t sm    = prefs.getUChar ("snap", 0); snapMode=(sm==0?SNAP_NONE: sm==1?SNAP_PEAK:SNAP_ZERO);
  showGrid      = prefs.getBool  ("grid", true);
  showCursors   = prefs.getBool  ("cur",  true);
  vGain         = prefs.getFloat ("gain", vGain);
  vOffset       = prefs.getFloat ("offs", vOffset);
  idleTimeoutMs = prefs.getUInt  ("idle", 30000);
  dimLevel      = prefs.getUChar ("dim", 60);
  prefs.end();
}
void setup(){
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_OK,   INPUT_PULLUP);
  if (ENC_A>=0){ pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP); }
  if (ENC_BTN>=0) pinMode(ENC_BTN, INPUT_PULLUP);

  Serial.begin(115200);
  analogReadResolution(12);

  ledcAttachPin(TFT_LED, 0); ledcSetup(0, 5000, 8);

  tft.initR(INITR_BLACKTAB); tft.setRotation(3); tft.fillScreen(ST77XX_BLACK);

  SPIFFS.begin(true);
  loadPrefs();
  ledcWrite(0, lcdBrightness);
  lastUserMs = millis();

  esp_task_wdt_init(8, true); esp_task_wdt_add(NULL);

  splash();
  header("Menue");
  tStart=millis(); clockBar();

  if (strlen(WIFI_PASS)){
    WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0=millis(); while(WiFi.status()!=WL_CONNECTED && millis()-t0<8000) delay(200);
  }
  if (WiFi.status()!=WL_CONNECTED){ WiFi.mode(WIFI_AP); WiFi.softAP("Scope_OTA","12345678"); }
  setupWeb();

  drawMenu();
  state=MENU;
}

void loop(){
  esp_task_wdt_reset();
  pollInput();
  powerIdleTick();
  if (millis()-tClock>1000){ tClock=millis(); clockBar(); }

  switch(state){
    case MENU:      handleMenu();   break;
    case SCOPE:     scopeLoop();    break;
    case FFTMODE:   fftLoop();      break;
    case CLOCK:     clockLoop();    break;
    case OTA:       // Info-Seite nur als Text; Webserver läuft im Hintergrund
      if (eOk){ eOk=false; state=MENU; drawMenu(); }
      break;
    case SETTINGS:  settingsLoop(); break;
    case FILES:     filesLoop();    break;
    default: break;
  }
}
