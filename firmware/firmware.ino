#include <WiFi.h>

const char* AP_SSID = "Auto_Wled";
const char* AP_PASS = "Keineahnung123";
const int   AP_CHANNEL  = 6;
const int   MAX_CLIENTS = 8;

void setup() {
  Serial.begin(115200);
  WiFi.setCountry("DE");
  WiFi.setSleep(false);
  IPAddress ip(192,168,4,1), gw(192,168,4,1), mask(255,255,255,0);
  WiFi.softAPConfig(ip, gw, mask);
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, false, MAX_CLIENTS);
}

void loop() {}
