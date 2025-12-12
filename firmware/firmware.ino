#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

const char* ssid = "ESP32-RobotArm";
const char* password = "12345678";

WebServer server(80);
Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

// Servo Pulsbreiten (safe start)
#define SERVO_MIN 110   // ca. 0°
#define SERVO_MAX 500   // ca. 180°

uint16_t servoPos[6] = {300, 300, 300, 300, 300, 300};

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{background:#111;color:#0af;font-family:sans-serif;text-align:center}
.slider{width:90%}
.box{margin:20px;padding:10px;background:#1e1e1e;border-radius:10px}
</style>
</head>
<body>
<h2>ESP32 Robot Arm</h2>
<p><a href="/ota">OTA Update</a></p>

<script>
function setServo(ch,val){
 fetch("/set?ch="+ch+"&val="+val);
}
</script>

<div class="box">Base<br>
<input class="slider" type="range" min="110" max="500" value="300" oninput="setServo(0,this.value)">
</div>

<div class="box">Schulter<br>
<input class="slider" type="range" min="110" max="500" value="300" oninput="setServo(1,this.value)">
</div>

<div class="box">Ellenbogen<br>
<input class="slider" type="range" min="110" max="500" value="300" oninput="setServo(2,this.value)">
</div>

<div class="box">Drehen<br>
<input class="slider" type="range" min="110" max="500" value="300" oninput="setServo(3,this.value)">
</div>

<div class="box">Kippen<br>
<input class="slider" type="range" min="110" max="500" value="300" oninput="setServo(4,this.value)">
</div>

<div class="box">Greifer<br>
<input class="slider" type="range" min="110" max="500" value="300" oninput="setServo(5,this.value)">
</div>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleSet() {
  int ch = server.arg("ch").toInt();
  int val = server.arg("val").toInt();
  if (ch >= 0 && ch < 6) {
    val = constrain(val, SERVO_MIN, SERVO_MAX);
    servoPos[ch] = val;
    pwm.setPWM(ch, 0, val);
  }
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);

  WiFi.softAP(ssid, password);
  delay(200);

  Wire.begin(21, 22);
  pwm.begin();
  pwm.setPWMFreq(50);

  for (int i = 0; i < 6; i++) {
    pwm.setPWM(i, 0, servoPos[i]);
  }

  ArduinoOTA.setHostname("ESP32-RobotArm");
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/ota", [](){
    server.send(200, "text/plain", "OTA aktiv. Nutze Arduino IDE oder WebOTA.");
  });

  server.begin();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
}


