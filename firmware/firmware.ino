/***** ESP32-S2 mini: Scope + Splash (5s) + Uhr + OTA (AsyncWebServer) *****/

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <string.h>   // memcpy (Sicherheits-Hedder)

// ---------- TFT-Pins (deine Verdrahtung)
#define TFT_CS    5
#define TFT_RST   6
#define TFT_DC    7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// ---------- ADC-Eingang
const int PIN_ADC = 2;      // dein Messpin

// ---------- ADC/Teiler-Parameter
const float ADC_REF    = 3.3f;     // Referenz
const float ADC_COUNTS = 4095.0f;  // 12 Bit
const float DIV_GAIN   = (110000.0f + 10000.0f) / 10000.0f; // 12.0 bei 110k/10k

// ---------- WLAN + OTA (Async)
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "";               // leer => AP-Fallback
AsyncWebServer server(80);

// ---------- Scope-Puffer/Status
const int N = 128;
uint16_t buf[N], plotBuf[N];
uint16_t trigLevel = 2048;   // ~50%
uint32_t lastDraw = 0;
uint32_t tStart = 0, tClock = 0;

// --------- kleine Hilfsfunktion (ersetzt map(), vermeidet Konflikte)
static inline int imap(int x, int in_min, int in_max, int out_min, int out_max) {
  return (int)((long)(x - in_min) * (out_max - out_min) / (long)(in_max - in_min) + out_min);
}

/* ===================== Splash-Screen ===================== */
void showSplash(uint32_t duration_ms = 5000) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(36, 22);
  tft.print("SCOPE");

  tft.setTextSize(1);
  tft.setCursor(16, 44);
  tft.setTextColor(ST77XX_CYAN);
  tft.print("Startet... bitte warten");

  // Ladebalken-Geometrie
  const int w = 120, h = 10;
  const int x = (160 - w) / 2;
  const int y = 70;

  tft.drawRect(x - 1, y - 1, w + 2, h + 2, ST77XX_WHITE);  // Rahmen

  uint32_t t0 = millis();
  uint32_t lastPct = 200; // unrealistisch groß, damit erste Anzeige sicher triggert
  while (millis() - t0 < duration_ms) {
    float p = (millis() - t0) / (float)duration_ms;  // 0..1
    if (p > 1.0f) p = 1.0f;
    int pw = (int)(w * p);

    // Fortschritt füllen (nur neu zeichnen, um Flackern zu reduzieren)
    tft.fillRect(x, y, pw, h, ST77XX_GREEN);

    // Prozentzahl nur aktualisieren, wenn sich was ändert
    uint32_t pct = (uint32_t)(p * 100.0f + 0.5f);
    if (pct != lastPct) {
      lastPct = pct;
      tft.fillRect(0, 88, 160, 12, ST77XX_BLACK);
      tft.setCursor(70, 88);
      tft.setTextColor(ST77XX_WHITE);
      tft.print((int)pct);
      tft.print("%");
    }
    delay(20);
  }

  delay(120);
  tft.fillScreen(ST77XX_BLACK);
}

/* ===================== UI ===================== */
void drawHeader() {
  tft.fillRect(0, 0, 160, 16, ST77XX_BLACK);
  tft.setCursor(2, 2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.print("Mode: SCOPE");
}

void drawClock() {
  uint32_t sec = (millis() - tStart) / 1000;
  int h = (sec / 3600) % 24, m = (sec / 60) % 60, s = sec % 60;
  char txt[9];
  snprintf(txt, sizeof(txt), "%02d:%02d:%02d", h, m, s);
  tft.fillRect(100, 0, 60, 16, ST77XX_BLACK);
  tft.setCursor(102, 2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.print(txt);
}

/* ===================== Scope ===================== */
void scopeLoop() {
  // 1) Samplen
  for (int i = 0; i < N; i++) buf[i] = analogRead(PIN_ADC);

  // 2) Trigger (steigende Flanke)
  int idx = -1;
  for (int i = 1; i < N; i++) {
    if (buf[i - 1] < trigLevel && buf[i] >= trigLevel) { idx = i; break; }
  }
  if (idx > 0) {
    int k = 0;
    for (int i = idx; i < N && k < N; i++) plotBuf[k++] = buf[i];
    for (int i = 0; i < idx && k < N; i++) plotBuf[k++] = buf[i];
  } else {
    memcpy(plotBuf, buf, sizeof(plotBuf));
  }

  // 3) Zeichnen (max. ~20 FPS)
  if (millis() - lastDraw > 50) {
    lastDraw = millis();

    // Plotbereich (Y: 16..95)
    tft.fillRect(0, 16, 160, 80, ST77XX_BLACK);
    for (int x = 0; x < (N - 1) && x < 159; x++) {
      int y1 = imap(plotBuf[x],     0, 4095, 95, 16);
      int y2 = imap(plotBuf[x + 1], 0, 4095, 95, 16);
      tft.drawLine(x, y1, x + 1, y2, ST77XX_GREEN);
    }

    // Vin grob (Mittelwert)
    uint32_t acc = 0; for (int i = 0; i < N; i++) acc += plotBuf[i];
    float vIn = (acc / (float)N) * (ADC_REF / ADC_COUNTS) * DIV_GAIN;

    // Statuszeile
    tft.fillRect(0, 96, 160, 32, ST77XX_BLACK);
    tft.setCursor(2, 100); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
    tft.print("Trig "); tft.print((int)(trigLevel * 100 / 4095)); tft.print("%");
    tft.setCursor(90, 100); tft.setTextColor(ST77XX_CYAN);
    tft.print("Vin "); tft.print(vIn, 2); tft.print("V");
  }

  // Trigger per Seriell anheben (Test)
  if (Serial.available() && Serial.read() == 't') {
    trigLevel = (trigLevel + 256 > 4095) ? 1024 : trigLevel + 256;
  }
}

/* ===================== Setup ===================== */
void setup() {
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);         // Backlight an

  Serial.begin(115200);
  analogReadResolution(12);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);                   // 180° zu vorher – Uhr oben rechts
  showSplash(5000);                     // <<< NEU: 5s Splash

  tft.fillScreen(ST77XX_BLACK);
  drawHeader();
  tStart = millis();
  drawClock();

  // WLAN verbinden, AP-Fallback
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (strlen(WIFI_PASS) && WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Scope_OTA", "12345678");
  }

  // Rootseite: einfaches OTA-Formular
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    String ip = (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    r->send(200, "text/html",
            "<h3>ESP32-S2 Scope</h3>"
            "<p>IP: " + ip + "</p>"
            "<form method='POST' action='/update' enctype='multipart/form-data'>"
            "<input type='file' name='firmware'>"
            "<input type='submit' value='Flash'>"
            "</form>"
            "<p>Seriell 't' = Triggerlevel +</p>");
  });

  // OTA Upload-Handler
  server.on(
    "/update", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      bool ok = !Update.hasError();
      r->send(200, "text/plain", ok ? "Update OK, rebooting..." : "Update FAILED");
      if (ok) { delay(300); ESP.restart(); }
    },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) {
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      }
      if (Update.write(data, len) != (int)len) {
        Update.printError(Serial);
      }
      if (final) {
        if (!Update.end(true)) Update.printError(Serial);
      }
    });

  server.begin();
}

/* ===================== Loop ===================== */
void loop() {
  if (millis() - tClock > 1000) { tClock = millis(); drawClock(); }
  scopeLoop();
}
