/*
  ESP32-S2 Mini – 1.8" TFT (ST7735) + 2 Buttons + LED + OTA (AP)
  Pins:
    TFT: CS=5, DC(A0)=7, RST=6, SCK=12, MOSI(SDA)=11, (MISO unbenutzt)
    LED: 13
    BTN1: 8 (gegen GND, PullUp)
    BTN2: 9 (gegen GND, PullUp)

  Schritte 1–5: Startscreen, Kurz/Langdruck, Menü (HOME/INFO/SETTINGS/CALIB),
  Kalibrier-Testbild, UI-Bausteine (Hint-Bar, Progress-Bar, KV, Message).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// =================== OTA / AP ===================
static const char* AP_SSID     = "ESP32S2-OTA";
static const char* AP_PASSWORD = "flashme123";
static const char* HTTP_USER   = "admin";
static const char* HTTP_PASS   = "esp32s2";

WebServer server(80);

// =================== Pins ===================
static const int TFT_CS   = 5;
static const int TFT_DC   = 7;   // A0
static const int TFT_RST  = 6;
static const int TFT_SCK  = 12;  // SCK
static const int TFT_MOSI = 11;  // SDA (MOSI)
static const int LED_PIN  = 13;
static const int BTN1_PIN = 8;
static const int BTN2_PIN = 9;

// =================== Display ===================
// Wähle nötige "TAB"-Variante (üblich: INITR_BLACKTAB)
#define CALIB_TAB INITR_BLACKTAB
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
static const int TFT_W = 160;
static const int TFT_H = 128;

// Eigene Farbe, falls Lib kein DARKGREY hat
#ifndef ST77XX_DARKGREY
  #define ST77XX_DARKGREY 0x7BEF
#endif

// =================== Buttons ===================
static const uint32_t DEBOUNCE_MS   = 30;
static const uint32_t SHORT_MS_MAX  = 300;
static const uint32_t LONG_MS_MIN   = 700;

struct Btn {
  int pin;
  bool pullup;
  bool state;        // debounced (true = gedrückt)
  bool lastRead;     // raw
  uint32_t lastChange;
  bool pressedEdge;
  uint32_t pressTs;
};

static Btn btn1{BTN1_PIN, true, false, true, 0, false, 0};
static Btn btn2{BTN2_PIN, true, false, true, 0, false, 0};

enum BtnEvent { EV_NONE, EV_SHORT, EV_LONG };

// =================== Seiten/Status (enum VOR Prototypen!) ===================
enum Page { PAGE_HOME=0, PAGE_INFO, PAGE_SETTINGS, PAGE_CALIB, PAGE_COUNT };
static Page currentPage = PAGE_HOME;

// Vorwärtsdeklarationen (Verhindert Autoprototyping-Probleme)
static void goPage(Page p);
static void renderHome(bool full=true);
static void renderInfo(bool full=true);
static void renderSettings(bool full=true);
static void renderCalib(bool full=true);

// =================== Button-Funktionen ===================
static void initButton(Btn& b) {
  pinMode(b.pin, b.pullup ? INPUT_PULLUP : INPUT);
  b.lastRead = digitalRead(b.pin);
  b.state    = (b.pullup ? (b.lastRead == LOW) : (b.lastRead == HIGH));
  b.lastChange = millis();
  b.pressedEdge = false;
  b.pressTs = 0;
}

static bool debouncedUpdate(Btn& b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.lastRead) {
    b.lastChange = millis();
    b.lastRead = raw;
  }
  if ((millis() - b.lastChange) >= DEBOUNCE_MS) {
    bool newState = b.pullup ? (raw == LOW) : (raw == HIGH);
    if (newState != b.state) {
      b.state = newState;
      return true;
    }
  }
  return false;
}

static BtnEvent pollBtnEvent(Btn& b) {
  BtnEvent ev = EV_NONE;
  if (debouncedUpdate(b)) {
    if (b.state) {
      b.pressedEdge = true;
      b.pressTs = millis();
    } else {
      if (b.pressedEdge) {
        uint32_t dt = millis() - b.pressTs;
        if (dt < SHORT_MS_MAX) ev = EV_SHORT;
        else if (dt >= LONG_MS_MIN) ev = EV_LONG;
      }
      b.pressedEdge = false;
    }
  } else {
    if (b.state && b.pressedEdge) {
      uint32_t dt = millis() - b.pressTs;
      if (dt >= LONG_MS_MIN) {
        ev = EV_LONG;
        b.pressedEdge = false;
      }
    }
  }
  return ev;
}

// =================== UI-Bausteine ===================
static void drawHintBar(const char* text) {
  tft.fillRect(0, TFT_H-14, TFT_W, 14, ST77XX_DARKGREY);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, TFT_H-12);
  tft.print(text);
}

static void showMessage(const char* text, uint16_t color = ST77XX_WHITE, uint16_t bg = ST77XX_BLACK) {
  tft.fillScreen(bg);
  tft.setTextSize(2);
  tft.setTextColor(color);
  tft.setCursor(6, 8);
  tft.print(text);
}

static void printKV(int16_t x, int16_t y, const char* k, const String& v, uint16_t kc=ST77XX_YELLOW, uint16_t vc=ST77XX_WHITE) {
  tft.setTextSize(1);
  tft.setCursor(x, y);
  tft.setTextColor(kc);
  tft.print(k);
  tft.setTextColor(vc);
  tft.print(v);
}

static void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h, int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  tft.drawRect(x, y, w, h, ST77XX_WHITE);
  int fillw = (w-2) * percent / 100;
  tft.fillRect(x+1, y+1, fillw, h-2, ST77XX_GREEN);
}

// =================== Seitenwechsel ===================
static void goPage(Page p) {
  currentPage = p;
  switch (p) {
    case PAGE_HOME:     renderHome(true); break;
    case PAGE_INFO:     renderInfo(true); break;
    case PAGE_SETTINGS: renderSettings(true); break;
    case PAGE_CALIB:    renderCalib(true); break;
    default: break;
  }
}

// =================== OTA – Website ===================
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

static bool isAuthenticated() {
  if (!HTTP_USER || !*HTTP_USER) return true;
  if (server.authenticate(HTTP_USER, HTTP_PASS)) return true;
  server.requestAuthentication();
  return false;
}

static void handleRoot() {
  if (!isAuthenticated()) return;
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleNotFound() {
  if (!isAuthenticated()) return;
  server.send(404, "text/plain", "Not Found");
}

static void handleUpdateUpload() {
  if (!isAuthenticated()) return;
  HTTPUpload& upload = server.upload();
  static bool beginOk = false;

  if (upload.status == UPLOAD_FILE_START) {
    beginOk = Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (beginOk) Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (beginOk && Update.end(true)) {
      server.send(200, "text/plain", "OK");
      delay(200);
      ESP.restart();
    } else {
      server.send(500, "text/plain", "Update failed");
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    server.send(500, "text/plain", "Upload abgebrochen");
  }
}

// =================== Renderfunktionen ===================
static String apIP = "0.0.0.0";

// (1) Startscreen + Live Button-Status
static void renderHome(bool full) {
  if (full) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("HOME");

    tft.setTextSize(1);
    printKV(6, 30, "SSID: ", String(AP_SSID));
    printKV(6, 42, "IP  : ", apIP);
    printKV(6, 58, "BTN1: ", btn1.state ? "GEDRUECKT" : "LOS");
    printKV(6, 70, "BTN2: ", btn2.state ? "GEDRUECKT" : "LOS");

    drawHintBar("BTN1: weiter  BTN2: OK | BTN2 lang: HOME");
  } else {
    // nur die Button-Zeilen aktualisieren
    tft.fillRect(40, 58, TFT_W-46, 10, ST77XX_BLACK);
    tft.setCursor(40, 58); tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
    tft.print(btn1.state ? "GEDRUECKT" : "LOS");

    tft.fillRect(40, 70, TFT_W-46, 10, ST77XX_BLACK);
    tft.setCursor(40, 70);
    tft.print(btn2.state ? "GEDRUECKT" : "LOS");
  }
}

static void renderInfo(bool full) {
  (void)full;
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.setCursor(6, 6);
  tft.print("INFO");

  tft.setTextSize(1);
  printKV(6, 30, "SSID: ", String(AP_SSID));
  printKV(6, 42, "IP  : ", apIP);
  printKV(6, 54, "LED : ", digitalRead(LED_PIN) ? "AN" : "AUS");
  drawHintBar("BTN1: weiter  BTN2: LED toggeln  BTN2 lang: HOME");
}

static void renderSettings(bool full) {
  (void)full;
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(6, 6);
  tft.print("SETTINGS");

  tft.setTextSize(1);
  tft.setCursor(6, 32);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("OK: Kalibrierungsseite\nBTN1 lang: Progress-Demo");

  drawHintBar("BTN1: weiter  BTN2: OK  BTN2 lang: HOME");
}

static void renderCalib(bool full) {
  (void)full;
  tft.fillScreen(ST77XX_BLACK);

  // Rahmen und Markierungen
  tft.drawRect(0, 0, TFT_W, TFT_H, ST77XX_WHITE);
  tft.drawRect(1, 1, TFT_W-2, TFT_H-2, ST77XX_RED);
  tft.drawRect(2, 2, TFT_W-4, TFT_H-4, ST77XX_GREEN);

  // Eckmarken
  tft.fillRect(0,0,5,5,ST77XX_WHITE);
  tft.fillRect(TFT_W-5,0,5,5,ST77XX_WHITE);
  tft.fillRect(0,TFT_H-5,5,5,ST77XX_WHITE);
  tft.fillRect(TFT_W-5,TFT_H-5,5,5,ST77XX_WHITE);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(6, 10);
  tft.print("CALIB: Pruefe, ob alles sichtbar ist.");

  tft.setCursor(6, 24);
  tft.print("TAB: BLACKTAB");

  drawHintBar("BTN1: weiter  BTN2: OK  BTN2 lang: HOME");
}

// =================== Setup / Loop ===================
void setup() {
  Serial.begin(115200);
  delay(150);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  initButton(btn1);
  initButton(btn2);

  tft.initR(CALIB_TAB);
  tft.setRotation(1); // Querformat 160x128
  tft.fillScreen(ST77XX_BLACK);
  showMessage("Boot...", ST77XX_WHITE, ST77XX_BLACK);

  WiFi.mode(WIFI_MODE_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  apIP = ip.toString();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST,
    [](){ if(!Update.isFinished()) server.send(500,"text/plain","Update nicht abgeschlossen"); },
    handleUpdateUpload
  );
  server.onNotFound(handleNotFound);
  server.begin();

  goPage(PAGE_HOME);
}

void loop() {
  server.handleClient();

  BtnEvent e1 = pollBtnEvent(btn1);
  BtnEvent e2 = pollBtnEvent(btn2);

  if (currentPage == PAGE_HOME) {
    renderHome(false);
  }

  if (currentPage == PAGE_HOME) {
    if (e1 == EV_SHORT) { goPage(PAGE_INFO); }
    if (e2 == EV_SHORT) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); renderHome(true); }
    if (e1 == EV_LONG) {
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
      tft.setCursor(6, 6); tft.print("Progress-Demo");
      for (int p = 0; p <= 100; p+=5) { drawProgressBar(10, 30, TFT_W-20, 14, p); delay(50); }
      delay(300); renderHome(true);
    }
    if (e2 == EV_LONG) { goPage(PAGE_HOME); }
  }
  else if (currentPage == PAGE_INFO) {
    if (e1 == EV_SHORT) { goPage(PAGE_SETTINGS); }
    if (e2 == EV_SHORT) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); renderInfo(true); }
    if (e2 == EV_LONG)  { goPage(PAGE_HOME); }
  }
  else if (currentPage == PAGE_SETTINGS) {
    if (e1 == EV_SHORT) { goPage(PAGE_CALIB); }
    if (e1 == EV_LONG) {
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
      tft.setCursor(6, 6); tft.print("Progress-Demo");
      for (int p = 0; p <= 100; p+=5) { drawProgressBar(10, 30, TFT_W-20, 14, p); delay(50); }
      delay(300); renderSettings(true);
    }
    if (e2 == EV_SHORT) { goPage(PAGE_CALIB); }
    if (e2 == EV_LONG)  { goPage(PAGE_HOME); }
  }
  else if (currentPage == PAGE_CALIB) {
    if (e1 == EV_SHORT) { goPage(PAGE_HOME); }
    if (e2 == EV_SHORT) { goPage(PAGE_HOME); }
    if (e2 == EV_LONG)  { goPage(PAGE_HOME); }
  }
}
