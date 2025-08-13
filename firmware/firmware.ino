/***** ESP32-S2 mini: Scope + Splash (5s, Bitmap-Sinus) + Uhr + Messwerte + Zeitbasis + OTA *****/

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <string.h>   // memcpy

// ---------- TFT-Pins (deine Verdrahtung)
#define TFT_CS    5
#define TFT_RST   6
#define TFT_DC    7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// ---------- ADC-Eingang
const int PIN_ADC = 2;      // dein Messpin (Frontend/Teiler hierhin)

// ---------- ADC/Teiler-Parameter
const float ADC_REF    = 3.3f;     // Referenzspannung
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
const int N = 128;              // Samples pro Frame
uint16_t buf[N], plotBuf[N];
uint16_t trigLevel = 2048;      // ~50% für steigende Flanke
uint32_t lastDraw = 0;
uint32_t tStart = 0, tClock = 0;

// ---------- Zeitbasis (us pro Sample) & Bedienung über Seriell
const uint32_t timebaseUsList[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
int timebaseIdx = 4; // start mit 1000 us/Sample (= ~1 ms/Sample)
inline uint32_t samplePeriodUs() { return timebaseUsList[timebaseIdx]; }

// ---------- kleine Hilfsfunktion (ersetzt map)
static inline int imap(int x, int in_min, int in_max, int out_min, int out_max) {
  return (int)((long)(x - in_min) * (out_max - out_min) / (long)(in_max - in_min) + out_min);
}

/* ===================== Bitmap-Logo (Sinus) =====================
   1-Bit-Monochrom, 64x32 Pixel. Einfaches, sauberes Sinus-Logo im Oszi-Rahmen.
   (Nur ein Beispiel – du kannst später jede Bitmap ersetzen.)
*/
const uint8_t PROGMEM sinusLogo64x32[] = {
  // Ein kleines 64x32-Bitmap (1 Bit/Pixel). Für Kürze hier generisches Muster:
  // Rahmen + Achsen + Sinuskurve – nicht perfekt fotorealistisch, aber glatt.
  // 64 * 32 / 8 = 256 Bytes
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x7F,0xFF,0xFF,0xE0,0x40,0x00,0x00,0x20,0x40,0x00,0x00,0x20,0x40,0x00,0x00,0x20,
  0x40,0x00,0x00,0x20,0x40,0x00,0x00,0x20,0x40,0x00,0x00,0x20,0x40,0x00,0x00,0x20,
  0x40,0x00,0x00,0x20,0x43,0x00,0x00,0x20,0x46,0x00,0x00,0x20,0x48,0x00,0x00,0x20,
  0x50,0x00,0x00,0x20,0x60,0x00,0x00,0x20,0x70,0x00,0x00,0x20,0x58,0x00,0x00,0x20,
  0x4C,0x00,0x00,0x20,0x46,0x00,0x00,0x20,0x43,0x00,0x00,0x20,0x41,0x80,0x00,0x20,
  0x40,0xC0,0x00,0x20,0x40,0x60,0x00,0x20,0x40,0x30,0x00,0x20,0x40,0x18,0x00,0x20,
  0x40,0x0C,0x00,0x20,0x40,0x06,0x00,0x20,0x40,0x03,0x00,0x20,0x40,0x01,0x80,0x20,
  0x40,0x00,0xC0,0x20,0x40,0x00,0x60,0x20,0x40,0x00,0x30,0x20,0x40,0x00,0x18,0x20,
  0x40,0x00,0x0C,0x20,0x40,0x00,0x06,0x20,0x40,0x00,0x03,0x20,0x40,0x00,0x01,0xA0,
  0x40,0x00,0x00,0xE0,0x40,0x00,0x00,0x60,0x40,0x00,0x00,0x20,0x40,0x00,0x00,0x20,
  0x40,0x00,0x00,0x20,0x40,0x00,0x00,0x20,0x40,0x00,0x00,0x20,0x7F,0xFF,0xFF,0xE0,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
// (Hinweis: Das ist ein schlichtes, aber glattes 1‑Bit‑Logo. Du kannst später ein eigenes 64x32‑Bitmap einsetzen.)

/* ===================== Splash-Screen (5s, Bitmap + Ladebalken) ===================== */
void showSplash(uint32_t duration_ms = 5000) {
  tft.fillScreen(ST77XX_BLACK);

  // Bitmap mittig zeichnen (64x32)
  int bx = (160 - 64) / 2;
  int by = 18;
  tft.drawBitmap(bx, by, sinusLogo64x32, 64, 32, ST77XX_GREEN, ST77XX_BLACK);

  // Untertitel
  tft.setTextSize(1);
  tft.setCursor(18, 56);
  tft.setTextColor(ST77XX_CYAN);
  tft.print("Startet... bitte warten");

  // Ladebalken
  const int w = 120, h = 10;
  const int x = (160 - w) / 2;
  const int y = 80;

  tft.drawRect(x - 1, y - 1, w + 2, h + 2, ST77XX_WHITE);  // Rahmen

  uint32_t t0 = millis();
  uint32_t lastPct = 200;
  while (millis() - t0 < duration_ms) {
    float p = (millis() - t0) / (float)duration_ms;  // 0..1
    if (p > 1.0f) p = 1.0f;
    int pw = (int)(w * p);

    // Fortschritt füllen
    tft.fillRect(x, y, pw, h, ST77XX_GREEN);

    // Prozentzahl sparsam aktualisieren
    uint32_t pct = (uint32_t)(p * 100.0f + 0.5f);
    static uint32_t shown = 999;
    if (pct != shown) {
      shown = pct;
      tft.fillRect(0, 98, 160, 12, ST77XX_BLACK);
      tft.setCursor(70, 98);
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

/* ===================== Messwerte berechnen ===================== */
struct Metrics {
  float vpp;     // Peak-to-Peak
  float vrms;    // RMS (AC; um Mittelwert bereinigt)
  float freq;    // Hz (geschätzt)
};

Metrics computeMetrics(const uint16_t* s, int len, float vref, float counts, float gain, float Ts_us) {
  Metrics m{0,0,0};

  // Min/Max & Mittelwert (ADC → Volt)
  uint16_t minAdc = 0xFFFF, maxAdc = 0;
  double sum = 0.0;
  for (int i = 0; i < len; ++i) {
    uint16_t v = s[i];
    if (v < minAdc) minAdc = v;
    if (v > maxAdc) maxAdc = v;
    sum += v;
  }
  double meanAdc = sum / len;
  float vMin = (minAdc / counts) * vref * gain;
  float vMax = (maxAdc / counts) * vref * gain;
  float vMean = (meanAdc / counts) * vref * gain;

  m.vpp = vMax - vMin;

  // RMS (AC): sqrt( Mittelwert( (v - vMean)^2 ) )
  double acc2 = 0.0;
  for (int i = 0; i < len; ++i) {
    float v = (s[i] / counts) * vref * gain;
    float d = v - vMean;
    acc2 += (double)d * d;
  }
  m.vrms = sqrt(acc2 / len);

  // Frequenzschätzung über Zero-Crossing (steigende Flanke um den Mittelwert)
  // Finde zwei aufeinanderfolgende Crossings
  int first = -1, second = -1;
  uint16_t meanAdcU = (uint16_t)(meanAdc + 0.5);
  for (int i = 1; i < len; ++i) {
    if (s[i - 1] < meanAdcU && s[i] >= meanAdcU) {
      if (first < 0) first = i;
      else { second = i; break; }
    }
  }
  if (first >= 0 && second > first) {
    float period_s = (second - first) * (Ts_us / 1e6f);
    if (period_s > 0.0f) m.freq = 1.0f / period_s;
  } else {
    m.freq = 0.0f;
  }

  return m;
}

/* ===================== Scope ===================== */
void scopeLoop() {
  // 1) Samplen mit Zeitbasis
  uint32_t Ts = samplePeriodUs();
  for (int i = 0; i < N; i++) {
    buf[i] = analogRead(PIN_ADC);
    if (Ts >= 5) delayMicroseconds(Ts);
  }

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

  // 3) Messwerte berechnen
  Metrics M = computeMetrics(plotBuf, N, ADC_REF, ADC_COUNTS, DIV_GAIN, (float)Ts);

  // 4) Zeichnen (max. ~20 FPS)
  if (millis() - lastDraw > 50) {
    lastDraw = millis();

    // Plotbereich (Y: 16..95)
    tft.fillRect(0, 16, 160, 80, ST77XX_BLACK);
    for (int x = 0; x < (N - 1) && x < 159; x++) {
      int y1 = imap(plotBuf[x],     0, 4095, 95, 16);
      int y2 = imap(plotBuf[x + 1], 0, 4095, 95, 16);
      tft.drawLine(x, y1, x + 1, y2, ST77XX_GREEN);
    }

    // Status/Messwerte (unten)
    tft.fillRect(0, 96, 160, 32, ST77XX_BLACK);
    tft.setTextSize(1);

    // Zeile 1: Trigger + Zeitbasis
    tft.setCursor(2, 98);
    tft.setTextColor(ST77XX_YELLOW);
    tft.print("Trig ");
    tft.print((int)(trigLevel * 100 / 4095));
    tft.print("%  ");
    tft.setTextColor(ST77XX_WHITE);
    tft.print("T/div ");
    // 10 Divisions: N Samples pro Frame
    float timePerDiv_ms = (N * Ts) / 1000.0f / 10.0f;
    if (timePerDiv_ms >= 1.0f) { tft.print(timePerDiv_ms, 1); tft.print("ms"); }
    else { tft.print(timePerDiv_ms * 1000.0f, 0); tft.print("us"); }

    // Zeile 2: Vpp, Vrms, Freq
    tft.setCursor(2, 110);
    tft.setTextColor(ST77XX_CYAN);
    tft.print("Vpp ");
    tft.print(M.vpp, 2);
    tft.print("  Vrms ");
    tft.print(M.vrms, 2);
    tft.print("  f ");
    if (M.freq > 0.1f) {
      if (M.freq >= 1000.0f) { tft.print(M.freq / 1000.0f, 2); tft.print("kHz"); }
      else { tft.print(M.freq, 1); tft.print("Hz"); }
    } else {
      tft.print("--");
    }
  }

  // Zeitbasis-Steuerung per Seriell
  if (Serial.available()) {
    int c = Serial.read();
    if (c == '+') { if (timebaseIdx > 0) timebaseIdx--; }         // schneller (kleineres Ts)
    if (c == '-') { if (timebaseIdx < (int)(sizeof(timebaseUsList)/sizeof(timebaseUsList[0])) - 1) timebaseIdx++; } // langsamer
    if (c == 't') { trigLevel = (trigLevel + 256 > 4095) ? 1024 : trigLevel + 256; }
  }
}

/* ===================== Setup ===================== */
void setup() {
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);         // Backlight an

  Serial.begin(115200);
  analogReadResolution(12);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);                   // Landscape, Uhr oben rechts
  showSplash(5000);                     // 5s Splash mit Bitmap + Balken

  tft.fillScreen(ST77XX_BLACK);
  // Header + Uhr
  tft.setTextWrap(false);
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

  // Rootseite: simples OTA-Formular
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    String ip = (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    r->send(200, "text/html",
            "<h3>ESP32-S2 Scope</h3>"
            "<p>IP: " + ip + "</p>"
            "<form method='POST' action='/update' enctype='multipart/form-data'>"
            "<input type='file' name='firmware'>"
            "<input type='submit' value='Flash'>"
            "</form>"
            "<p>Seriell: '+' schneller, '-' langsamer, 't' Trigger hoch</p>");
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
