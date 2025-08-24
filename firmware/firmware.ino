/*
  ESP32-S2 MASTER — MQTT + OTA + 1 Button (FW v1.1.4)
  - SoftAP (OTA):      SSID ESP2_MASTER / PW flashme123
  - STA (WLAN):        SSID "Peng" / PW "Keineahnung123"
  - MQTT:              192.168.178.65:1883, User "firstclass55555", PW "Zehn+551996"
  - Publish:           esp2panel/online (LWT), esp2panel/test ("hallo" beim Boot),
                       esp2panel/event → {"src":"A","btn":"L","type":"short|double|long"}
  - TFT ST7735 1.8":   CS=5, DC=7, RST=6, SCK=12, MOSI=11; Backlight PWM: GPIO13
  - Button:            GPIO8 gegen GND, Pullup, kurz/doppelt/lang, short erst nach Double-Timeout
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <PubSubClient.h>

// ---------- WLAN ----------
static const char* WIFI_SSID   = "Peng";
static const char* WIFI_PASS   = "Keineahnung123";

// ---------- MQTT ----------
static const char* MQTT_HOST   = "192.168.178.65";
static const uint16_t MQTT_PORT= 1883;
static const char* MQTT_USER   = "firstclass55555";
static const char* MQTT_PASSW  = "Zehn+551996";

// ---------- OTA (AP) ----------
static const char* AP_SSID = "ESP2_MASTER";
static const char* AP_PASS = "flashme123";

// ---------- Version ----------
static const char* FW_VERSION = "1.1.4";

// ---------- TFT / Pins ----------
static const int TFT_CS=5, TFT_DC=7, TFT_RST=6, TFT_SCK=12, TFT_MOSI=11;
static const int TFT_BL_PIN=13;
#define CALIB_TAB INITR_BLACKTAB
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
#ifndef ST77XX_DARKGREY
  #define ST77XX_DARKGREY 0x7BEF
#endif
static const int TFT_W=160, TFT_H=128;

// ---------- Backlight ----------
static const int BL_CH=0, BL_FREQ=5000, BL_RES=8;
static uint8_t bl_level=200;
static inline void setBL(uint8_t v){ bl_level=v; ledcWrite(BL_CH,v); }

// ---------- Button (GPIO8) ----------
static const int BTN1_PIN = 8; // gegen GND, interner Pullup

// Zeiten (ms)
static const uint32_t BTN_DEBOUNCE_MS   = 30;
static const uint32_t BTN_SHORT_MAX_MS  = 300;
static const uint32_t BTN_LONG_MIN_MS   = 700;
static const uint32_t BTN_DBL_WIN_MS    = 250;

// ---- VORWÄRTSDEKLARATIONEN (fix gegen Autoprototype)
struct Btn;
enum BtnEvent : uint8_t;                // nur Vorwärtsdeklaration
static void     initBtn(Btn& b);
static BtnEvent pollBtn(Btn& b);
static BtnEvent checkPendingShort(Btn& b);

// ---- JETZT die vollständigen Typen
enum BtnEvent : uint8_t { EV_NONE, EV_SHORT, EV_DOUBLE, EV_LONG };

struct Btn {
  int pin; bool pullup;

  // Entprellen
  bool state; bool last; uint32_t tchg;

  // Press/Release
  bool pressed; uint32_t tpress;

  // Short-Decision
  bool pendingShort; uint32_t pendingDeadline;
};

static Btn btn1{BTN1_PIN
