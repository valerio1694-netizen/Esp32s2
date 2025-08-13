/***** ESP32-S2 mini: Scope + Splash + OTA (AsyncWebServer) *****/
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

/* ======================= Konfiguration ======================= */
// TFT-Pins (Software-SPI-Variante von Adafruit_ST7735)
#define TFT_CS    5
#define TFT_RST   6
#define TFT_DC    7
#define TFT_MOSI  11
#define TFT_SCLK  12
#define TFT_LED   13

// ADC
const int PIN_ADC = 2;            // Analogeingang
const float ADC_REF    = 3.3f;    // Referenz 3.3V
const float ADC_COUNTS = 4095.0f; // 12 Bit
const float DIV_GAIN   = (110000.0f + 10000.0f) / 10000.0f; // Teiler 110k/10k = 12.0

// WLAN
const char* WIFI_SSID = "Peng";
const char* WIFI_PASS = "DEIN_PASSWORT";  // leer lassen => AP-Fallback "Scope_OTA/12345678"

/* ======================= Objekte ======================= */
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
AsyncWebServer server(80);

/* ======================= Splash: SCOPE + Sinus LUT ======================= */
/* Eine feste, „vorbereitete“ Sinus-Kurve: Y-Wert je X-Pixel (Breite 160).
   Bereich 16..95 (passt in unseren Plot-Bereich). */
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
/* Hinweis: Das ist eine „hübsche“ Kurve, keine exakte Sinus-Mathe – für
   Splash-Optik völlig ausreichend. */

/* ======================= Scope-Daten ======================= */
const int N = 128;
uint16_t buf[N], plotBuf[N];
uint16_t trigLevel = 2048;

uint32_t lastDraw = 0;
uint32_t tStart = 0, tClock = 0;

/* ======================= Hilfen ======================= */
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
  char txt[9]; sprintf(txt, "%02d:%02d:%02d", h, m, s);
  tft.fillRect(100, 0, 60, 16, ST77XX_BLACK);
  tft.setCursor(102, 2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.print(txt);
}

void drawSplash() {
  tft.fillScreen(ST77XX_BLACK);

  // großes „SCOPE“
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(12, 18);
  tft.print("S"); tft.setCursor(36,18);
  tft.print("C"); tft.setCursor(60,18);
  tft.print("O"); tft.setCursor(84,18);
  tft.print("P"); tft.setCursor(108,18);
  tft.print("E");

  // horizontale Nulllinie
  uint16_t grey = tft.color565(64,64,64);
  tft.drawLine(0, 64, 159, 64, grey);

  // vorbereitete Sinuskurve aus LUT
  for (int x = 0; x < 159; x++) {
    uint8_t y1 = pgm_read_byte(&SINE_Y[x]);
    uint8_t y2 = pgm_read_byte(&SINE_Y[x+1]);
    tft.drawLine(x, y1, x+1, y2, ST77XX_CYAN);
  }
}

void scopeLoop() {
  // Probe
  for (int i = 0; i < N; i++) buf[i] = analogRead(PIN_ADC);

  // Trigger (steigende Flanke um trigLevel)
  int idx = -1;
  for (int i = 1; i < N; i++) {
    if (buf[i-1] < trigLevel && buf[i] >= trigLevel) { idx = i; break; }
  }
  if (idx > 0) {
    int k = 0;
    for (int i = idx; i < N && k < N; i++) plotBuf[k++] = buf[i];
    for (int i = 0;  i < idx && k < N; i++) plotBuf[k++] = buf[i];
  } else {
    memcpy(plotBuf, buf, sizeof(plotBuf));
  }

  // Zeichnen ~20 Hz
  if (millis() - lastDraw > 50) {
    lastDraw = millis();
    // Plotfenster
    tft.fillRect(0, 16, 160, 80, ST77XX_BLACK);
    for (int x = 0; x < min(N-1, 159); x++) {
      int y1 = map(plotBuf[x],   0, 4095, 95, 16);
      int y2 = map(plotBuf[x+1], 0, 4095, 95, 16);
      tft.drawLine(x, y1, x+1, y2, ST77XX_GREEN);
    }

    // Mittelwert -> Vin
    uint32_t acc = 0; for (int i = 0; i < N; i++) acc += plotBuf[i];
    float vIn = (acc / (float)N) * (ADC_REF / ADC_COUNTS) * DIV_GAIN;

    // Statuszeile
    tft.fillRect(0, 96, 160, 32, ST77XX_BLACK);
    tft.setCursor(2, 100);  tft.setTextColor(ST77XX_YELLOW); tft.setTextSize(1);
    tft.print("Trig "); tft.print((int)(trigLevel * 100 / 4095)); tft.print("%");
    tft.setCursor(90, 100); tft.setTextColor(ST77XX_CYAN);
    tft.print("Vin "); tft.print(vIn, 2); tft.print("V");
  }

  // Trigger via seriell erhöhen
  if (Serial.available() && Serial.read() == 't') {
    trigLevel = (trigLevel + 256 > 4095) ? 1024 : trigLevel + 256;
  }
}

/* ======================= OTA-Routes ======================= */
void setupOTA() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    String ip = (WiFi.getMode()==WIFI_AP) ? WiFi.softAPIP().toString()
                                          : WiFi.localIP().toString();
    String html;
    html += "<h3>ESP32-S2 Scope</h3>";
    html += "<p>IP: " + ip + "</p>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='firmware'> ";
    html += "<input type='submit' value='Flash'>";
    html += "</form>";
    html += "<p>Seriell 't' = Triggerlevel +</p>";
    r->send(200, "text/html", html);
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

/* ======================= Arduino-Hooks ======================= */
void setup() {
  pinMode(TFT_LED, OUTPUT); digitalWrite(TFT_LED, HIGH);
  Serial.begin(115200);
  analogReadResolution(12);

  // Display
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  // Splash 5 s
  drawSplash();
  delay(5000);

  // Kopf + Uhr
  drawHeader();
  tStart = millis();
  drawClock();

  // WLAN
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_PASS)) WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (strlen(WIFI_PASS) && WiFi.status()!=WL_CONNECTED && millis()-t0 < 8000) {
    delay(200);
  }
  if (WiFi.status()!=WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Scope_OTA", "12345678");
  }

  setupOTA();
}

void loop() {
  if (millis() - tClock > 1000) { tClock = millis(); drawClock(); }
  scopeLoop();
}
