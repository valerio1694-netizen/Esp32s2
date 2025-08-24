/*
  ESP32-S2 Mini – 1.8" TFT (ST7735) + 2 Buttons + OTA (AP)
  Navigation: Schema B+ (Softkeys) mit Kurz / Lang / Doppel-Klick + Auto-Repeat
  SPI-TFT: ST7735 (160x128), PWM-Backlight auf GPIO13, Taster auf 8/9

  Seiten:
    - HOME (Menü: Info / Spiele / Settings / Calib)
    - INFO (SSID/IP/BL%)
    - GAMES (Menü: Snake)
    - SNAKE (2-Tasten, Wrap, Highscore im NVS, Turbo-Pille)
    - SETTINGS (Helligkeit)
    - CALIB

  OTA: AP "ESP32S2-OTA" / Passwort "flashme123"
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>

// ---------- Forward-Decls, um Arduino-Autoprototyping zu befriedigen ----------
enum Page    : uint8_t;   // Definition folgt später
enum PillType: uint8_t;   // Definition folgt später

// =================== Button & Events ===================
struct Btn {
  int pin;
  bool pullup;
  bool state; bool lastRead; uint32_t lastChange;
  bool pressedEdge; uint32_t pressTs;
  uint32_t lastShortReleaseTs; bool pendingShort;
  bool repeatArmed; uint32_t repeatStartTs; uint32_t nextRepeatTs;
};
enum BtnEvent { EV_NONE, EV_SHORT, EV_LONG, EV_DOUBLE, EV_REPEAT };

// =================== OTA / AP ===================
static const char* AP_SSID     = "ESP32S2-OTA";
static const char* AP_PASSWORD = "flashme123";
static const char* HTTP_USER   = "admin";
static const char* HTTP_PASS   = "esp32s2";
WebServer server(80);

// =================== Pins ===================
static const int TFT_CS=5, TFT_DC=7, TFT_RST=6, TFT_SCK=12, TFT_MOSI=11;
static const int BTN1_PIN=8, BTN2_PIN=9;
static const int TFT_BL_PIN=13;

// ===== Backlight (PWM) =====
static const int BL_PWM_CHANNEL=0, BL_PWM_FREQ=5000, BL_PWM_RES=8;
static uint8_t bl_level=200, bl_prev_level=200;
static inline void setBacklight(uint8_t lvl){ bl_level=lvl; ledcWrite(BL_PWM_CHANNEL,lvl); }
static inline void toggleBacklight(){ if(bl_level>0){ bl_prev_level=bl_level; setBacklight(0);} else setBacklight(bl_prev_level?bl_prev_level:128); }

// =================== Display ===================
#define CALIB_TAB INITR_BLACKTAB
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
static const int TFT_W=160, TFT_H=128;
#ifndef ST77XX_DARKGREY
  #define ST77XX_DARKGREY 0x7BEF
#endif

// =================== Button-Parameter ===================
static const uint32_t DEBOUNCE_MS=30, SHORT_MS_MAX=300, LONG_MS_MIN=700;
static const uint32_t DOUBLE_WIN_MS=250, REPEAT_START_MS=400, REPEAT_STEP_MS=120;
static Btn btn1{BTN1_PIN,true,false,true,0,false,0,0,false,false,0,0};
static Btn btn2{BTN2_PIN,true,false,true,0,false,0,0,false,false,0,0};

// =================== Seiten/Status ===================
static String apIP="0.0.0.0";
Preferences prefs; // NVS für Highscore

// ---------- Prototypen ----------
static void initButton(Btn& b); static bool debouncedUpdate(Btn& b);
static void armRepeatOnPress(Btn& b); static void disarmRepeat(Btn& b);
static BtnEvent pollBtnEvent(Btn& b, bool repeatEnabled);

static void drawSoftkeys(const char* left,const char* right);
static void showMessage(const char* text,uint16_t color=ST77XX_WHITE,uint16_t bg=ST77XX_BLACK);
static void printKV(int16_t x,int16_t y,const char* k,const String& v,uint16_t kc=ST77XX_YELLOW,uint16_t vc=ST77XX_WHITE);
static void drawProgressBar(int16_t x,int16_t y,int16_t w,int16_t h,int percent);

static void renderHome(bool full=true); static void renderInfo(bool full=true);
static void renderGames(bool full=true); static void renderSnakeHUD();
static void renderSettings(bool full=true); static void renderCalib(bool full=true);

static bool isAuthenticated(); static void handleRoot(); static void handleNotFound(); static void handleUpdateUpload();
static void goPage(Page p);

// =================== Button-Funktionen ===================
static void initButton(Btn& b){
  pinMode(b.pin,b.pullup?INPUT_PULLUP:INPUT);
  b.lastRead=digitalRead(b.pin);
  b.state=(b.pullup? (b.lastRead==LOW):(b.lastRead==HIGH));
  b.lastChange=millis(); b.pressedEdge=false; b.pressTs=0;
  b.lastShortReleaseTs=0; b.pendingShort=false; b.repeatArmed=false; b.repeatStartTs=0; b.nextRepeatTs=0;
}
static bool debouncedUpdate(Btn& b){
  bool raw=digitalRead(b.pin);
  if(raw!=b.lastRead){ b.lastChange=millis(); b.lastRead=raw; }
  if((millis()-b.lastChange)>=DEBOUNCE_MS){
    bool ns=b.pullup?(raw==LOW):(raw==HIGH);
    if(ns!=b.state){ b.state=ns; return true; }
  }
  return false;
}
static void armRepeatOnPress(Btn& b){ b.repeatArmed=true; b.repeatStartTs=millis()+REPEAT_START_MS; b.nextRepeatTs=b.repeatStartTs; }
static void disarmRepeat(Btn& b){ b.repeatArmed=false; b.repeatStartTs=0; b.nextRepeatTs=0; }
static BtnEvent pollBtnEvent(Btn& b,bool repeatEnabled){
  BtnEvent ev=EV_NONE; uint32_t now=millis();
  if(debouncedUpdate(b)){
    if(b.state){ b.pressedEdge=true; b.pressTs=now; b.pendingShort=false; if(repeatEnabled)armRepeatOnPress(b); else disarmRepeat(b); }
    else{
      disarmRepeat(b);
      if(b.pressedEdge){
        uint32_t dt=now-b.pressTs;
        if(dt<SHORT_MS_MAX){
          if(b.lastShortReleaseTs && (now-b.lastShortReleaseTs)<=DOUBLE_WIN_MS){ ev=EV_DOUBLE; b.lastShortReleaseTs=0; }
          else { ev=EV_SHORT; b.lastShortReleaseTs=now; }
        } else if(dt>=LONG_MS_MIN){ ev=EV_LONG; }
      }
      b.pressedEdge=false;
    }
  } else if(b.state && b.pressedEdge){
    if(repeatEnabled && b.repeatArmed && now>=b.repeatStartTs){
      if(now>=b.nextRepeatTs){ ev=EV_REPEAT; b.nextRepeatTs+=REPEAT_STEP_MS; }
    }
  }
  return ev;
}

// =================== UI-Bausteine ===================
static void drawSoftkeys(const char* left,const char* right){
  tft.fillRect(0,TFT_H-16,TFT_W,16,ST77XX_DARKGREY);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  tft.setCursor(2,TFT_H-13); tft.print(left);
  int16_t x,y; uint16_t w,h; tft.getTextBounds(right,0,0,&x,&y,&w,&h);
  tft.setCursor(TFT_W-2-w,TFT_H-13); tft.print(right);
}
static void showMessage(const char* text,uint16_t color,uint16_t bg){
  tft.fillScreen(bg); tft.setTextSize(2); tft.setTextColor(color); tft.setCursor(6,8); tft.print(text);
}
static void printKV(int16_t x,int16_t y,const char* k,const String& v,uint16_t kc,uint16_t vc){
  tft.setTextSize(1); tft.setCursor(x,y); tft.setTextColor(kc); tft.print(k); tft.setTextColor(vc); tft.print(v);
}
static void drawProgressBar(int16_t x,int16_t y,int16_t w,int16_t h,int percent){
  if(percent<0)percent=0; if(percent>100)percent=100;
  tft.drawRect(x,y,w,h,ST77XX_WHITE);
  int fillw=(w-2)*percent/100; tft.fillRect(x+1,y+1,fillw,h-2,ST77XX_GREEN);
}

// =================== OTA Website ===================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S2 OTA Upload</title>
<style>
body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:20px}
.card{max-width:520px;padding:16px;border:1px solid #ccc;border-radius:12px}
progress{width:100%;height:16px}.ok{color:green}.err{color:#b00}
</style></head><body>
<div class="card">
<h2>ESP32‑S2 OTA Firmware Upload</h2>
<input id="file" type="file" accept=".bin,application/octet-stream"><br><br>
<button id="btn">Upload starten</button><br><br>
<progress id="pb" max="100" value="0" hidden></progress>
<div id="msg"></div>
</div>
<script>
const b=document.getElementById("btn"),f=document.getElementById("file"),pb=document.getElementById("pb"),m=document.getElementById("msg");
b.onclick=()=>{
 if(!f.files.length){m.textContent="Bitte .bin auswählen";m.className="err";return;}
 const x=new XMLHttpRequest();pb.hidden=false;pb.value=0;m.textContent="Lade hoch...";
 x.upload.onprogress=e=>{if(e.lengthComputable)pb.value=Math.round(e.loaded/e.total*100);};
 x.onload=()=>{if(x.status==200){m.textContent="OK – Reboot...";m.className="ok";pb.value=100;setTimeout(()=>location.reload(),6000);}else{m.textContent="Fehler: "+x.responseText;m.className="err";}};
 const form=new FormData();form.append("firmware",f.files[0]);x.open("POST","/update",true);x.send(form);
};
</script></body></html>
)HTML";

static bool isAuthenticated(){ if(!HTTP_USER||!*HTTP_USER) return true; if(server.authenticate(HTTP_USER,HTTP_PASS)) return true; server.requestAuthentication(); return false; }
static void handleRoot(){ if(!isAuthenticated()) return; server.send_P(200,"text/html; charset=utf-8",INDEX_HTML); }
static void handleNotFound(){ if(!isAuthenticated()) return; server.send(404,"text/plain","Not Found"); }
static void handleUpdateUpload(){
  if(!isAuthenticated()) return;
  HTTPUpload& upload=server.upload(); static bool beginOk=false;
  if(upload.status==UPLOAD_FILE_START){ beginOk=Update.begin(UPDATE_SIZE_UNKNOWN); }
  else if(upload.status==UPLOAD_FILE_WRITE){ if(beginOk) Update.write(upload.buf,upload.currentSize); }
  else if(upload.status==UPLOAD_FILE_END){ if(beginOk && Update.end(true)){ server.send(200,"text/plain","OK"); delay(200); ESP.restart(); } else { server.send(500,"text/plain","Update failed"); } }
  else if(upload.status==UPLOAD_FILE_ABORTED){ Update.abort(); server.send(500,"text/plain","Upload abgebrochen"); }
}

// =================== enum Page & UI-State ===================
enum Page: uint8_t { PAGE_HOME=0, PAGE_INFO, PAGE_GAMES, PAGE_SNAKE, PAGE_SETTINGS, PAGE_CALIB, PAGE_COUNT };
static Page currentPage=PAGE_HOME;

static const char* HOME_ITEMS[]={"Info","Spiele","Settings","Calib"};
static const uint8_t HOME_LEN=sizeof(HOME_ITEMS)/sizeof(HOME_ITEMS[0]);
static int8_t homeSel=0;

static const char* GAME_ITEMS[]={"Snake"};
static const uint8_t GAME_LEN=sizeof(GAME_ITEMS)/sizeof(GAME_ITEMS[0]);
static int8_t gameSel=0;

static uint8_t pendingBL=bl_level;

// =================== Snake: Spiellogik ===================
struct Cell{ int8_t x,y; };

// Raster / Spielfeld
static const uint8_t  SNAKE_CS=5;
static const int16_t  HUD_H=10, SOFTKEY_H=16;
static const int16_t  PLAY_TOP=HUD_H;
static const int16_t  PLAY_BOTTOM=TFT_H-SOFTKEY_H;
static const int8_t   GRID_W=TFT_W/SNAKE_CS;                      // 32
static const int8_t   GRID_H=(PLAY_BOTTOM-PLAY_TOP)/SNAKE_CS;     // 20

static Cell snake[GRID_W*GRID_H]; static int16_t snakeLen=0;

enum Dir{ D_RIGHT=0, D_DOWN=1, D_LEFT=2, D_UP=3 };
static Dir dirCur=D_RIGHT;

static Cell food{0,0};

// ---- Power-Ups ----
enum PillType: uint8_t { PILL_NONE=0, PILL_TURBO=1 };
static Cell pillPos{-1,-1}; static PillType pillType=PILL_NONE;
static uint32_t turboUntilMs=0;

static bool snakeAlive=false, snakePaused=false, snakeTurboManual=false;
static uint16_t snakeScore=0;
static uint32_t snakeTickMs=160, lastTick=0;

static inline int rndInt(int a,int b){ uint32_t r=esp_random(); return a+(int)(r%((uint32_t)(b-a+1))); }

static void placeFood(){
  while(true){
    food.x=rndInt(0,GRID_W-1); food.y=rndInt(0,GRID_H-1);
    bool coll=(pillType!=PILL_NONE && pillPos.x==food.x && pillPos.y==food.y);
    for(int i=0; i<snakeLen && !coll; i++) if(snake[i].x==food.x && snake[i].y==food.y) coll=true;
    if(!coll) break;
  }
}
static void placePill(PillType type){
  if(rndInt(0,4)!=0){ pillType=PILL_NONE; return; } // ~20% Chance
  while(true){
    Cell p{(int8_t)rndInt(0,GRID_W-1),(int8_t)rndInt(0,GRID_H-1)};
    bool coll=(p.x==food.x && p.y==food.y);
    for(int i=0;i<snakeLen && !coll;i++) if(snake[i].x==p.x && snake[i].y==p.y) coll=true;
    if(!coll){ pillPos=p; pillType=type; break; }
  }
}

static void renderSnakeHUD();
static inline void drawCell(int8_t cx,int8_t cy,uint16_t col){
  int16_t x=cx*SNAKE_CS; int16_t y=PLAY_TOP+cy*SNAKE_CS; if(y+SNAKE_CS>PLAY_BOTTOM) return;
  tft.fillRect(x,y,SNAKE_CS,SNAKE_CS,col);
}
static inline void drawFood(){ drawCell(food.x,food.y,ST77XX_RED); }
static inline void clearCell(int8_t cx,int8_t cy){ drawCell(cx,cy,ST77XX_BLACK); }
static inline void drawPill(){
  if(pillType==PILL_NONE) return;
  int16_t x=pillPos.x*SNAKE_CS, y=PLAY_TOP+pillPos.y*SNAKE_CS;
  tft.fillRect(x,y,SNAKE_CS,SNAKE_CS,ST77XX_YELLOW); tft.drawRect(x,y,SNAKE_CS,SNAKE_CS,ST77XX_WHITE);
}

static void snakeInit(){
  snakeLen=4; int8_t sx=GRID_W/2-2, sy=GRID_H/2;
  for(int i=0;i<snakeLen;i++) snake[i]={ (int8_t)(sx+i), sy };
  dirCur=D_RIGHT; snakeAlive=true; snakePaused=false; snakeTurboManual=false;
  snakeScore=0; snakeTickMs=160; lastTick=millis(); turboUntilMs=0;
  pillType=PILL_NONE; pillPos={-1,-1}; placeFood();

  tft.fillRect(0,0,TFT_W,PLAY_BOTTOM,ST77XX_BLACK);
  renderSnakeHUD(); drawSoftkeys("Pause/Back","L/R/Turbo");

  // Start zeichnen
  for(int i=0;i<snakeLen;i++){
    int16_t x=snake[i].x*SNAKE_CS, y=PLAY_TOP+snake[i].y*SNAKE_CS;
    tft.fillRect(x,y,SNAKE_CS,SNAKE_CS,(i==snakeLen-1)?ST77XX_GREEN:ST77XX_BLUE);
  }
  drawFood();
}

static bool snakeStep(){
  Cell head=snake[snakeLen-1];
  switch(dirCur){ case D_RIGHT: head.x++; break; case D_LEFT: head.x--; break; case D_DOWN: head.y++; break; case D_UP: head.y--; break; }
  // Wrap
  if(head.x<0) head.x=GRID_W-1; if(head.x>=GRID_W) head.x=0;
  if(head.y<0) head.y=GRID_H-1; if(head.y>=GRID_H) head.y=0;
  // Eigenkollision
  for(int i=0;i<snakeLen;i++) if(snake[i].x==head.x && snake[i].y==head.y) return false;

  bool grow=(head.x==food.x && head.y==food.y);
  bool gotPill=(pillType!=PILL_NONE && head.x==pillPos.x && head.y==pillPos.y);

  if(!grow){
    clearCell(snake[0].x,snake[0].y);
    for(int i=0;i<snakeLen-1;i++) snake[i]=snake[i+1];
    snake[snakeLen-1]=head;
  } else {
    snake[snakeLen]=head; snakeLen++; snakeScore+=10; renderSnakeHUD(); placeFood(); placePill(PILL_TURBO);
  }

  drawCell(head.x,head.y,ST77XX_GREEN); drawFood();

  if(gotPill){
    if(pillType==PILL_TURBO){ turboUntilMs=millis()+5000; renderSnakeHUD(); }
    pillType=PILL_NONE; pillPos={-1,-1};
  } else if(pillType!=PILL_NONE){ drawPill(); }

  return true;
}
static void snakeRotateLeft(){ dirCur=(Dir)((dirCur+3)%4); }
static void snakeRotateRight(){ dirCur=(Dir)((dirCur+1)%4); }

// =================== Render-Seiten ===================
static void renderHome(bool full){
  if(full){ tft.fillScreen(ST77XX_BLACK); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.setCursor(6,6); tft.print("HOME"); }
  tft.setTextSize(1);
  for(int i=0;i<HOME_LEN;i++){
    int y=28+i*12; tft.fillRect(6,y-1,TFT_W-12,10,(i==homeSel)?ST77XX_BLUE:ST77XX_BLACK);
    tft.setCursor(10,y); tft.setTextColor((i==homeSel)?ST77XX_WHITE:ST77XX_CYAN); tft.print(HOME_ITEMS[i]);
  }
  drawSoftkeys("Weiter/Zurueck","OK/Info");
}
static void renderInfo(bool){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN); tft.setTextSize(2); tft.setCursor(6,6); tft.print("INFO");
  tft.setTextSize(1);
  printKV(6,30,"SSID: ",String(AP_SSID),ST77XX_YELLOW,ST77XX_WHITE);
  printKV(6,42,"IP  : ",apIP,ST77XX_YELLOW,ST77XX_WHITE);
  int pct=(int)((bl_level*100)/255); printKV(6,54,"BL  : ",String(pct)+"%",ST77XX_YELLOW,ST77XX_WHITE);
  drawSoftkeys("Zurueck","OK");
}
static void renderGames(bool full){
  if(full){ tft.fillScreen(ST77XX_BLACK); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2); tft.setCursor(6,6); tft.print("GAMES"); }
  tft.setTextSize(1);
  for(int i=0;i<GAME_LEN;i++){
    int y=28+i*12; tft.fillRect(6,y-1,TFT_W-12,10,(i==gameSel)?ST77XX_BLUE:ST77XX_BLACK);
    tft.setCursor(10,y); tft.setTextColor((i==gameSel)?ST77XX_WHITE:ST77XX_CYAN); tft.print(GAME_ITEMS[i]);
  }
  drawSoftkeys("Weiter/Zurueck","OK");
}
static void renderSettings(bool){
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(2); tft.setCursor(6,6); tft.print("SETTINGS");
  tft.setTextSize(1); tft.setCursor(6,30); tft.setTextColor(ST77XX_WHITE);
  tft.print("Helligkeit: "); int pct=(int)((pendingBL*100)/255); tft.print(pct); tft.print("%");
  tft.drawRect(6,46,TFT_W-12,12,ST77XX_WHITE); int fillw=(TFT_W-14)*pendingBL/255; tft.fillRect(7,47,fillw,10,ST77XX_GREEN);
  drawSoftkeys("- / Std.","+ / Save");
}
static void renderCalib(bool){
  tft.fillScreen(ST77XX_BLACK);
  tft.drawRect(0,0,TFT_W,TFT_H,ST77XX_WHITE); tft.drawRect(1,1,TFT_W-2,TFT_H-2,ST77XX_RED); tft.drawRect(2,2,TFT_W-4,TFT_H-4,ST77XX_GREEN);
  tft.fillRect(0,0,5,5,ST77XX_WHITE); tft.fillRect(TFT_W-5,0,5,5,ST77XX_WHITE); tft.fillRect(0,TFT_H-5,5,5,ST77XX_WHITE); tft.fillRect(TFT_W-5,TFT_H-5,5,5,ST77XX_WHITE);
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE); tft.setCursor(6,10); tft.print("CALIB: Rander pruefen."); tft.setCursor(6,24); tft.print("TAB: BLACKTAB");
  drawSoftkeys("Zurueck","OK");
}
static void renderSnakeHUD(){
  tft.fillRect(0,0,TFT_W,10,ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  tft.setCursor(2,1); tft.print("Score: "); tft.print(snakeScore);
  uint32_t his=prefs.getULong("hiscore",0); tft.setCursor(80,1); tft.print("Hi: "); tft.print(his);
  if(snakePaused){ tft.setCursor(TFT_W-50,1); tft.print("PAUSE"); }
  else {
    bool turboActive=snakeTurboManual || (millis()<turboUntilMs);
    if(turboActive){ tft.setCursor(TFT_W-52,1); tft.print("TURBO");
      if(millis()<turboUntilMs){ uint32_t msLeft=turboUntilMs-millis(); uint8_t sLeft=(msLeft+500)/1000; tft.setCursor(TFT_W-20,1); tft.print(sLeft); tft.print("s"); }
    }
  }
}

// =================== Seitenwechsel ===================
static void goPage(Page p){
  currentPage=p;
  switch(p){
    case PAGE_HOME:     renderHome(true); break;
    case PAGE_INFO:     renderInfo(true); break;
    case PAGE_GAMES:    renderGames(true); break;
    case PAGE_SNAKE:    snakeInit(); break;
    case PAGE_SETTINGS: renderSettings(true); break;
    case PAGE_CALIB:    renderCalib(true); break;
    default: break;
  }
}

// =================== Setup ===================
void setup(){
  Serial.begin(115200); delay(150);
  ledcSetup(BL_PWM_CHANNEL,BL_PWM_FREQ,BL_PWM_RES); ledcAttachPin(TFT_BL_PIN,BL_PWM_CHANNEL); setBacklight(bl_level);
  initButton(btn1); initButton(btn2);

  tft.initR(CALIB_TAB); tft.setRotation(1); tft.fillScreen(ST77XX_BLACK); showMessage("Boot...",ST77XX_WHITE,ST77XX_BLACK);

  prefs.begin("snake",false); // NVS für Highscore

  WiFi.mode(WIFI_MODE_AP); WiFi.softAP(AP_SSID,AP_PASSWORD); apIP=WiFi.softAPIP().toString();
  server.on("/",HTTP_GET,handleRoot);
  server.on("/update",HTTP_POST, [](){ if(!Update.isFinished()) server.send(500,"text/plain","Update nicht abgeschlossen"); }, handleUpdateUpload);
  server.onNotFound(handleNotFound); server.begin();

  goPage(PAGE_HOME);
}

// =================== Loop ===================
void loop(){
  server.handleClient();

  bool repBtn1=false, repBtn2=false;
  switch(currentPage){
    case PAGE_HOME:     repBtn1=true;  repBtn2=false; break;
    case PAGE_GAMES:    repBtn1=true;  repBtn2=false; break;
    case PAGE_SETTINGS: repBtn1=true;  repBtn2=true;  break;
    default:            repBtn1=false; repBtn2=false; break;
  }
  BtnEvent e1=pollBtnEvent(btn1,repBtn1);
  BtnEvent e2=pollBtnEvent(btn2,repBtn2);

  if(currentPage==PAGE_HOME){
    if(e1==EV_SHORT || e1==EV_REPEAT){ homeSel=(homeSel+1)%HOME_LEN; renderHome(false); }
    if(e1==EV_DOUBLE){ homeSel=(homeSel-1+HOME_LEN)%HOME_LEN; renderHome(false); }
    if(e2==EV_SHORT){
      if(homeSel==0) goPage(PAGE_INFO);
      else if(homeSel==1){ gameSel=0; goPage(PAGE_GAMES); }
      else if(homeSel==2){ pendingBL=bl_level; goPage(PAGE_SETTINGS); }
      else if(homeSel==3) goPage(PAGE_CALIB);
    }
    if(e2==EV_DOUBLE){
      tft.fillRect(90,6,64,16,ST77XX_BLACK);
      tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
      tft.setCursor(90,8); tft.print("Preview:"); tft.setCursor(90,16); tft.print(HOME_ITEMS[homeSel]);
    }
    if(e2==EV_LONG){ goPage(PAGE_HOME); }
  }
  else if(currentPage==PAGE_INFO){
    if(e1==EV_SHORT || e1==EV_DOUBLE || e1==EV_LONG || e2==EV_SHORT || e2==EV_LONG) goPage(PAGE_HOME);
  }
  else if(currentPage==PAGE_GAMES){
    if(e1==EV_SHORT || e1==EV_REPEAT){ gameSel=(gameSel+1)%GAME_LEN; renderGames(false); }
    if(e1==EV_DOUBLE){ gameSel=(gameSel-1+GAME_LEN)%GAME_LEN; renderGames(false); }
    if(e2==EV_SHORT){ if(gameSel==0) goPage(PAGE_SNAKE); }
    if(e2==EV_LONG){ goPage(PAGE_HOME); }
  }
  else if(currentPage==PAGE_SETTINGS){
    if(e1==EV_SHORT || e1==EV_REPEAT){ pendingBL=(pendingBL>=5)?(pendingBL-5):0; renderSettings(false); setBacklight(pendingBL); }
    if(e1==EV_DOUBLE){ pendingBL=128; setBacklight(pendingBL); renderSettings(false); }
    if(e2==EV_SHORT || e2==EV_REPEAT){ pendingBL=(pendingBL<=250)?(pendingBL+5):255; renderSettings(false); setBacklight(pendingBL); }
    if(e2==EV_DOUBLE){ setBacklight(pendingBL); goPage(PAGE_HOME); }
    if(e2==EV_LONG){ goPage(PAGE_HOME); }
  }
  else if(currentPage==PAGE_CALIB){
    if(e1==EV_SHORT || e1==EV_DOUBLE || e1==EV_LONG || e2==EV_SHORT || e2==EV_DOUBLE || e2==EV_LONG) goPage(PAGE_HOME);
  }
  else if(currentPage==PAGE_SNAKE){
    if(e1==EV_SHORT) snakeRotateLeft();
    if(e2==EV_SHORT) snakeRotateRight();
    if(e1==EV_LONG){ snakePaused=!snakePaused; renderSnakeHUD(); }
    if(e2==EV_DOUBLE){ snakeTurboManual=!snakeTurboManual; renderSnakeHUD(); }
    if(e2==EV_LONG){
      uint32_t his=prefs.getULong("hiscore",0); if(snakeScore>his) prefs.putULong("hiscore",snakeScore);
      goPage(PAGE_HOME);
    }

    uint32_t now=millis();
    bool turboActive=snakeTurboManual || (now<turboUntilMs);
    uint32_t stepMs=turboActive ? (snakeTickMs>40?snakeTickMs-40:30) : snakeTickMs;

    if(!snakePaused && snakeAlive && (now-lastTick)>=stepMs){
      lastTick=now;
      if(!snakeStep()){
        snakeAlive=false;
        uint32_t his=prefs.getULong("hiscore",0); if(snakeScore>his){ prefs.putULong("hiscore",snakeScore); his=snakeScore; }
        renderSnakeHUD();
        tft.setTextColor(ST77XX_WHITE); tft.setTextSize(2);
        tft.setCursor(20,(PLAY_TOP+(PLAY_BOTTOM-PLAY_TOP)/2)-8); tft.print("GAME OVER");
        tft.setTextSize(1); tft.setCursor(28,(PLAY_TOP+(PLAY_BOTTOM-PLAY_TOP)/2)+12);
        tft.print("Hi: "); tft.print(his);
        tft.setCursor(36,(PLAY_TOP+(PLAY_BOTTOM-PLAY_TOP)/2)+24); tft.print("BTN2 lang: Home");
      }
    }
    if(pillType!=PILL_NONE) drawPill();
  }
}
