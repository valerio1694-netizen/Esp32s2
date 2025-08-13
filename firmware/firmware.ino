/***** ESP32-S2 mini: Scope + Uhr + OTA (AsyncWebServer) *****/
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

/* ----------------------- Display-Pins ----------------------- */
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

/* ----------------------- ADC / Teiler ----------------------- */
const int   PIN_ADC     = 2;
const float ADC_REF     = 3.3f;
const float ADC_COUNTS  = 4095.0f;
const float DIV_GAIN    = (110000.0f + 10000.0f) / 10000.0f; // ≈ 12.0

/* ----------------------- WiFi / OTA ------------------------- */
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT";   // leer => AP-Fallback
AsyncWebServer server(80);

/* ----------------------- Scope-Puffer ----------------------- */
const int N = 128;
uint16_t buf[N], plotBuf[N];
uint16_t trigLevel = 2048;           // ~50 %
uint32_t lastDraw = 0;

/* ----------------------- Uhren-Zeiten ----------------------- */
uint32_t tStart = 0, tClock = 0;

/* ----------------------- Messwerte-Struct ------------------- */
/* Wichtig: steht VOR computeMetrics(), damit der Typ bekannt ist */
struct Metrics {
  float vpp;    // Peak-to-Peak-Spannung
  float vrms;   // RMS (AC)
  float freq;   // Frequenz in Hz (grob)
};

/* ----------------------- Vorab-Deklarationen ---------------- */
void drawHeader();
void drawClock();
void scopeLoop();
void drawSplash();               // 5 s Startbild
Metrics computeMetrics(const uint16_t* s, int len, float vref, float counts, float gain, float Ts_us);

/* =======================================================================
   Splash-Screen: vorbereitetes Sinusprofil (y-Koordinaten) zeichnen
   ======================================================================= */
void drawSplash() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(16, 8);
  tft.print("ESP32-S2 MINI  |  SCOPE");

  // „Logo“-Zeile
  tft.setCursor(22, 22);
  tft.setTextColor(ST77XX_CYAN);
  tft.print("Booting... OTA ready");

  // vorbereitete Sinus-Stuetzpunkte (0..159) im Bereich 32..95
  const int W = 160;
  static const uint8_t sinY[160] = {
    64,66,67,69,70,72,73,75,76,78,79,80,82,83,84,85,86,87,88,89,
    90,91,91,92,92,92,93,93,93,93,93,92,92,92,91,91,90,89,88,87,
    86,85,84,83,82,80,79,78,76,75,73,72,70,69,67,66,64,63,61,60,
    58,57,55,54,52,51,50,48,47,46,45,44,43,42,41,40,39,39,38,38,
    38,38,38,38,38,39,39,40,41,42,43,44,45,46,47,48,50,51,52,54,
    55,57,58,60,61,63,64,66,67,69,70,72,73,75,76,78,79,80,82,83,
    84,85,86,87,88,89,90,91,91,92,92,92,93,93,93,93,93,92,92,92
  };
  // Achsen
  tft.drawLine(0, 64, 159, 64, ST77XX_DARKGREY);
  tft.drawLine(0, 32, 0, 95, ST77XX_DARKGREY);

  // Sinus zeichnen
  for (int x = 0; x < W - 1; x++) {
    tft.drawLine(x, sinY[x], x + 1, sinY[x + 1], ST77XX_GREEN);
  }

  // kleine „Laufpunkt“-Animation für ~5 Sekunden
  uint32_t t0 = millis();
  int dotX = 0;
  while (millis() - t0 < 5000) {
    tft.fillCircle(dotX, sinY[dotX], 2, ST77XX_ORANGE);
    delay(20);
    tft.fillCircle(dotX, sinY[dotX], 2, ST77XX_BLACK);
    dotX = (dotX + 1) % W;
  }
}

/* =======================================================================
   Kopfzeile + Uhr
   ======================================================================= */
void drawHeader() {
  tft.fillRect(0, 0, 160, 16, ST77XX_BLACK);
  tft.setCursor(2, 2);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  tft.print("Mode: SCOPE");
}

void drawClock() {
  uint32_t sec = (millis() - tStart) / 1000;
  int h = (sec / 3600) % 24, m = (sec / 60) % 60, s = sec % 60;
  char txt[9]; snprintf(txt, sizeof(txt), "%02d:%02d:%02d", h, m, s);
  tft.fillRect(100, 0, 60, 16, ST77XX_BLACK);
  tft.setCursor(102, 2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK); tft.setTextSize(1);
  tft.print(txt);
}

/* =======================================================================
   einfache Metriken aus einem Buffer: Vpp, Vrms, Frequenz (grob)
   Ts_us: Abtastintervall in Mikrosekunden (nur fuer Frequenzschaetzung)
   ======================================================================= */
Metrics computeMetrics(const uint16_t* s, int len, float vref, float counts, float gain, float Ts_us) {
  Metrics m{0, 0, 0};

  if (len <= 2) return m;

  uint16_t mn = 0xFFFF, mx = 0;
  double acc2 = 0.0;

  for (int i = 0; i < len; i++) {
    uint16_t v = s[i];
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }

  // ADC -> Eingangsspannung
  auto adc2v = [&](uint16_t a) -> float {
    return (a * (vref / counts)) * gain;
  };

  float vmax = adc2v(mx);
  float vmin = adc2v(mn);
  m.vpp = vmax - vmin;

  // Vrms (AC-Anteil): Mittelwert abziehen
  double mean = 0.0;
  for (int i = 0; i < len; i++) mean += s[i];
  mean /= len;

  for (int i = 0; i < len; i++) {
    double va = (s[i] - mean) * (vref / counts) * gain;
    acc2 += va * va;
  }
  m.vrms = sqrt(acc2 / len);

  // Frequenz (grob) via Nulldurchgaenge um den Mittelwert
  int lastSign = (s[0] > mean) ? 1 : -1;
  int crossings = 0;
  for (int i = 1; i < len; i++) {
    int sign = (s[i] > mean) ? 1 : -1;
    if (sign != lastSign) { crossings++; lastSign = sign; }
  }
  // 2 Nulldurchgänge ≈ 1 Periode
  if (crossings >= 2) {
    float periods = crossings / 2.0f;
    float T = (len * Ts_us) / 1e6f;       // Messzeit in s
    m.freq = periods / T;                 // sehr grob, reicht als Anzeige
  }

  return m;
}

/* =======================================================================
   Scope-Schleife (zeichnen + einfache Kennzahlen)
   ======================================================================= */
void scopeLoop() {
  for (int i = 0; i < N; i++) buf[i] = analogRead(PIN_ADC);

  // Trigger auf steigende Flanke am trigLevel
  int idx = -1;
  for (int i = 1; i < N; i++) {
    if (buf[i - 1] < trigLevel && buf[i] >= trigLevel) { idx = i; break; }
  }
  if (idx > 0) {
    int k = 0;
    for (int i = idx; i < N && k < N; i++) plotBuf[k++] = buf[i];
    for (int i = 0;   i < idx && k < N; i++) plotBuf[k++] = buf[i];
  } else {
    memcpy(plotBuf, buf, sizeof(plotBuf));
  }

  if (millis() - lastDraw > 50) {
    lastDraw = millis();
    // Zeichenbereich (16..95)
    tft.fillRect(0, 16, 160, 80, ST77XX_BLACK);
    for (int x = 0; x < min(N - 1, 159); x++) {
      int y1 = map(plotBuf[x],   0, 4095, 95, 16);
      int y2 = map(plotBuf[x+1], 0, 4095, 95, 16);
      tft.drawLine(x, y1, x + 1, y2, ST77XX_GREEN);
    }

    // Kennzahlen
    // Sampling-Zeit (grob) – hier einfach 50 µs als Beispiel (20 kS/s)
    const float Ts_us = 50.0f;
    Metrics mm = computeMetrics(plotBuf, N, ADC_REF, ADC_COUNTS, DIV_GAIN, Ts_us);

    tft.fillRect(0, 96, 160, 32, ST77XX_BLACK);
    tft.setCursor(2, 100); tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
    tft.print("Trig "); tft.print((int)(trigLevel * 100 / 4095)); tft.print("%");

    tft.setCursor(72, 100); tft.setTextColor(ST77XX_CYAN);
    tft.print("Vpp "); tft.print(mm.vpp, 2); tft.print("V");

    tft.setCursor(2, 112);  tft.setTextColor(ST77XX_ORANGE);
    tft.print("Vrms "); tft.print(mm.vrms, 2); tft.print("V");

    tft.setCursor(90, 112); tft.setTextColor(ST77XX_MAGENTA);
    tft.print("f "); tft.print(mm.freq, 1); tft.print("Hz");
  }

  if (Serial.available() && Serial.read() == 't')
    trigLevel = (trigLevel + 256 > 4095) ? 1024 : trigLevel + 256;
}

/* =======================================================================
   Setup
   ======================================================================= */
void setup() {
  pinMode(TFT_LED, OUTPUT); digitalWrite(TFT_LED, HIGH);
  Serial.begin(115200);
  analogReadResolution(12);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);

  // Splash (5 s) mit vorbereiteter Sinuskurve
  drawSplash();

  // Normale Kopfzeile + Uhr
  tft.fillScreen(ST77XX_BLACK);
  drawHeader();
  tStart = millis();
  drawClock();

  /* ------------------ WiFi / OTA ------------------ */
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (strlen(WIFI_PASS) && WiFi.status() != WL_CONNECTED && millis() - t0 < 8000)
    delay(200);

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Scope_OTA", "12345678");
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
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

  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *r){
      bool ok = !Update.hasError();
      r->send(200, "text/plain", ok ? "Update OK, rebooting..." : "Update FAILED");
      if (ok) { delay(300); ESP.restart(); }
    },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
      if (!index) { Update.begin(UPDATE_SIZE_UNKNOWN); }
      if (Update.write(data, len) != len) { Update.printError(Serial); }
      if (final) { if (!Update.end(true)) Update.printError(Serial); }
    }
  );

  server.begin();
}

/* =======================================================================
   Loop
   ======================================================================= */
void loop() {
  if (millis() - tClock > 1000) { tClock = millis(); drawClock(); }
  scopeLoop();
}
