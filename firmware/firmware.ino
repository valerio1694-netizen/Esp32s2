/***** ESP32-S2 mini – Start-Splash + Menü + Scope + Uhr + Einstellungen *****
 * Display: ST7735 160x128 (landscape)
 * Navigation (Seriell): w=hoch, s=runter, e=OK, b=zurück
 *****************************************************************************/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>

// ---------- Pins (anpassen falls nötig) ----------
#define TFT_CS   5
#define TFT_RST  6
#define TFT_DC   7
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_LED  13     // TFT-Backlight (PWM)

#define PIN_ADC  2      // Analogeingang fürs Scope

// ---------- Display + Farben ----------
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// Zusätzliche Farbkonstanten (falls in Lib nicht vorhanden)
#ifndef ST77XX_DARKGREY
  #define ST77XX_DARKGREY 0x7BEF
#endif
#ifndef ST77XX_NAVY
  #define ST77XX_NAVY 0x000F
#endif
#ifndef ST77XX_ORANGE
  #define ST77XX_ORANGE 0xFD20
#endif
#ifndef ST77XX_OLIVE
  #define ST77XX_OLIVE 0x7BE0
#endif
#ifndef ST77XX_MAROON
  #define ST77XX_MAROON 0x7800
#endif

// ---------- Settings / Konstanten ----------
const float ADC_REF    = 3.3f;
const float ADC_COUNTS = 4095.0f;
const float DIV_GAIN   = 12.0f;      // Beispiel (Spannungsteiler 110k/10k)

Preferences prefs;

// ---------- Zustände ----------
enum State { SPLASH, MENU, MODE_SCOPE, MODE_CLOCK, MODE_SETTINGS, MODE_ABOUT };
State state = SPLASH;

// ---------- Menü ----------
const char* MENU_ITEMS[] = {
  "Oszilloskop",
  "Uhr",
  "Einstellungen",
  "Info"
};
const uint8_t MENU_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);
int8_t menuIndex = 0;

// ---------- Settings (persistiert) ----------
uint8_t  backlight = 200;  // 0..255
uint16_t trigLevel = 2048; // 0..4095

// ---------- Hilfsdaten ----------
uint32_t tStart = 0;
uint16_t scopeBuf[160];

// ---------- Vorbereitete Sinus-Punkte (y auf 128px Höhe) ----------
const uint8_t SIN_W = 160;
uint8_t sinY[SIN_W];

void buildSinusLUT() {
  // Sinus zwischen y=24..103 (sauber im sichtbaren Bereich)
  for (int x = 0; x < SIN_W; x++) {
    float a = (2.0f * PI) * (float)x / (float)SIN_W;
    float s = (sinf(a) * 0.45f + 0.5f);      // 0..1
    sinY[x] = (uint8_t) (24 + s * (103 - 24));
  }
}

// ---------- Backlight ----------
void setBacklight(uint8_t v) {
  // LEDC PWM: 5kHz, 8-bit
  static bool init = false;
  if (!init) {
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_LED, 0);
    init = true;
  }
  ledcWrite(0, v);
}

// ---------- UI Grundgerüst ----------
void header(const char* title) {
  tft.fillRect(0, 0, 160, 16, ST77XX_NAVY);
  tft.setCursor(4, 3);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(title);
}

void showMenu() {
  header("Menue");
  tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);
  for (uint8_t i = 0; i < MENU_COUNT; i++) {
    int y = 22 + i * 18;
    if (i == menuIndex) {
      tft.fillRect(4, y - 2, 152, 14, ST77XX_DARKGREY);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(8, y);
    tft.print(MENU_ITEMS[i]);
  }
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 112);
  tft.print("w/s=wahl, e=OK, b=zurueck");
}

// ---------- Splash (5 s) ----------
void showSplash() {
  tft.fillScreen(ST77XX_BLACK);

  // Titel
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(10, 6);
  tft.print("Robin's Oszilloskop");

  // Sinus
  for (int x = 0; x < SIN_W - 1; x++) {
    tft.drawLine(x, sinY[x], x + 1, sinY[x + 1], ST77XX_GREEN);
  }

  // Text
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 20);
  tft.print("wird geladen...");

  // Ladebalken
  int x0 = 10, y0 = 110, w = 140, h = 10;
  tft.drawRect(x0 - 1, y0 - 1, w + 2, h + 2, ST77XX_DARKGREY);
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    float p = (millis() - t0) / 5000.0f;  // 0..1
    int fillW = (int)(w * p);
    tft.fillRect(x0, y0, fillW, h, ST77XX_YELLOW);
    delay(20);
  }
}

// ---------- Uhr ----------
void drawClock() {
  header("Uhr");
  tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);
}

void runClock() {
  static uint32_t last = 0;
  if (millis() - last >= 200) {
    last = millis();
    uint32_t sec = (millis() - tStart) / 1000;
    int h = (sec / 3600) % 24;
    int m = (sec / 60) % 60;
    int s = sec % 60;
    char buf[16];
    sprintf(buf, "%02d:%02d:%02d", h, m, s);

    tft.fillRect(0, 40, 160, 40, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE);
    int16_t x = 20;
    int16_t y = 48;
    tft.setCursor(x, y);
    tft.print(buf);
    tft.setTextSize(1);
  }
}

// ---------- Oszilloskop ----------
void drawScopeFrame() {
  header("Oszilloskop");
  tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);

  // Achsen
  tft.drawFastHLine(0, 64, 160, ST77XX_DARKGREY);
  tft.drawFastVLine(0, 16, 112, ST77XX_DARKGREY);

  // Legende
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 18);
  tft.print("Trig:");
  tft.setCursor(34, 18);
  tft.print((int)(trigLevel * 100 / 4095));
  tft.print("%  (t=+  m=menu)");
}

void runScope() {
  // Samples holen (simple, ungetriggert; Trigger nur als Linie)
  for (int i = 0; i < 160; i++) {
    scopeBuf[i] = analogRead(PIN_ADC);
  }

  // Plot-Bereich löschen
  tft.fillRect(1, 17, 158, 110, ST77XX_BLACK);

  // Trigger-Linie
  int yTrig = map(trigLevel, 0, 4095, 127, 16);
  tft.drawFastHLine(1, yTrig, 158, ST77XX_RED);

  // Plot
  for (int x = 0; x < 159; x++) {
    int y1 = map(scopeBuf[x],     0, 4095, 127, 16);
    int y2 = map(scopeBuf[x + 1], 0, 4095, 127, 16);
    tft.drawLine(x + 1, y1, x + 2, y2, ST77XX_GREEN);
  }

  // Mittelwert / Vin
  uint32_t acc = 0;
  for (int i = 0; i < 160; i++) acc += scopeBuf[i];
  float vIn = (acc / 160.0f) * (ADC_REF / ADC_COUNTS) * DIV_GAIN;

  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(4, 110);
  tft.print("Vin: ");
  tft.print(vIn, 2);
  tft.print(" V");
}

// ---------- Einstellungen ----------
void drawSettings() {
  header("Einstellungen");
  tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);

  tft.setCursor(6, 28);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("Helligkeit: ");
  tft.print(backlight);

  tft.setCursor(6, 46);
  tft.print("Trigger:    ");
  tft.print((int)(trigLevel * 100 / 4095));
  tft.print("%");

  tft.setCursor(6, 86);
  tft.setTextColor(ST77XX_YELLOW);
  tft.print("a/d = Hell -, +");
  tft.setCursor(6, 100);
  tft.print("j/l = Trig -, +");
  tft.setCursor(6, 114);
  tft.print("b = zurueck (speichern)");
}

void applySettings() {
  setBacklight(backlight);
}

// ---------- Info ----------
void drawAbout() {
  header("Info");
  tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);
  tft.setCursor(6, 28);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("Robin's Oszilloskop");
  tft.setCursor(6, 44);
  tft.print("ESP32-S2 + ST7735");
  tft.setCursor(6, 60);
  tft.print("Build: GitHub Actions");
  tft.setCursor(6, 92);
  tft.setTextColor(ST77XX_YELLOW);
  tft.print("b = zurueck");
}

// ---------- Eingabe (Seriell) ----------
void pollInput() {
  if (!Serial.available()) return;
  int c = Serial.read();
  switch (state) {
    case MENU:
      if (c == 'w') { menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT; showMenu(); }
      if (c == 's') { menuIndex = (menuIndex + 1) % MENU_COUNT; showMenu(); }
      if (c == 'e') {
        if (menuIndex == 0) { state = MODE_SCOPE;     drawScopeFrame(); }
        if (menuIndex == 1) { state = MODE_CLOCK;     drawClock(); }
        if (menuIndex == 2) { state = MODE_SETTINGS;  drawSettings(); }
        if (menuIndex == 3) { state = MODE_ABOUT;     drawAbout(); }
      }
      break;

    case MODE_SCOPE:
      if (c == 'm' || c == 'b') { state = MENU; showMenu(); }
      if (c == 't') {
        if (trigLevel + 128 > 4095) trigLevel = 1024;
        else trigLevel += 128;
        drawScopeFrame();
      }
      break;

    case MODE_CLOCK:
      if (c == 'b' || c == 'm') { state = MENU; showMenu(); }
      break;

    case MODE_SETTINGS:
      if (c == 'a' && backlight > 0)   { backlight--; applySettings(); drawSettings(); }
      if (c == 'd' && backlight < 255) { backlight++; applySettings(); drawSettings(); }
      if (c == 'j' && trigLevel >= 16) { trigLevel -= 16; drawSettings(); }
      if (c == 'l' && trigLevel <= 4095 - 16) { trigLevel += 16; drawSettings(); }
      if (c == 'b') {
        // speichern
        prefs.begin("scope", false);
        prefs.putUChar("backlight", backlight);
        prefs.putUShort("trig", trigLevel);
        prefs.end();
        state = MENU;
        showMenu();
      }
      break;

    case MODE_ABOUT:
      if (c == 'b') { state = MENU; showMenu(); }
      break;

    default: break;
  }
}

// ---------- Setup / Loop ----------
void setup() {
  pinMode(TFT_LED, OUTPUT);
  Serial.begin(115200);
  delay(100);

  // Display
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3); // 160x128 landscape
  tft.fillScreen(ST77XX_BLACK);

  // Sinus vorbereiten
  buildSinusLUT();

  // Settings laden
  prefs.begin("scope", true);
  backlight = prefs.getUChar("backlight", backlight);
  trigLevel = prefs.getUShort("trig", trigLevel);
  prefs.end();
  setBacklight(backlight);

  // ADC
  analogReadResolution(12);

  // Splash
  showSplash();

  // Startzeit für Uhr
  tStart = millis();

  // Menü zeichnen
  state = MENU;
  showMenu();
}

void loop() {
  pollInput();

  switch (state) {
    case MODE_SCOPE: {
      static uint32_t last = 0;
      if (millis() - last > 40) { // ~25 FPS
        last = millis();
        runScope();
      }
      break;
    }
    case MODE_CLOCK:
      runClock();
      break;

    default:
      // MENU / SETTINGS / ABOUT werden ereignisgesteuert neu gezeichnet
      delay(10);
      break;
  }
}
