#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_PWMServoDriver.h>

/* ========= CONFIG ========= */
#define SERVO_COUNT 6
#define SERVO_FREQ  50        // 50 Hz für analoge Servos
#define AP_SSID     "ESP32-RobotArm"
#define AP_PASS     "12345678"

// PCA9685 Adresse
Adafruit_PWMServoDriver pca(0x40);
WebServer server(80);
Preferences prefs;

// Servo-Kanäle (anpassen falls nötig)
uint8_t servoCh[SERVO_COUNT] = {0,1,2,3,4,5};

// Kalibrierwerte
uint16_t sMin[SERVO_COUNT];
uint16_t sMax[SERVO_COUNT];
uint16_t sHome[SERVO_COUNT];
uint16_t sCur[SERVO_COUNT];

uint8_t activeServo = 0;
uint16_t stepUS = 5;

/* ========= HELPER ========= */
uint16_t usToTicks(uint16_t us){
  return (uint32_t)us * SERVO_FREQ * 4096 / 1000000;
}

void moveServo(uint8_t i, uint16_t us){
  us = constrain(us, 300, 3000);
  sCur[i] = us;
  pca.setPWM(servoCh[i], 0, usToTicks(us));
}

/* ========= STORAGE ========= */
void loadCal(){
  prefs.begin("cal", true);
  for(int i=0;i<SERVO_COUNT;i++){
    sMin[i]  = prefs.getUShort(String("min")+i, 500);
    sMax[i]  = prefs.getUShort(String("max")+i, 2500);
    sHome[i] = prefs.getUShort(String("home")+i,1500);
    sCur[i]  = sHome[i];
  }
  prefs.end();
}

void saveCal(){
  prefs.begin("cal", false);
  for(int i=0;i<SERVO_COUNT;i++){
    prefs.putUShort(String("min")+i,  sMin[i]);
    prefs.putUShort(String("max")+i,  sMax[i]);
    prefs.putUShort(String("home")+i, sHome[i]);
  }
  prefs.end();
}

/* ========= WEB ========= */
String stateJson(){
  String j="{\"servo\":"+String(activeServo)+",\"step\":"+String(stepUS)+",\"data\":[";
  for(int i=0;i<SERVO_COUNT;i++){
    if(i) j+=",";
    j+="{\"min\":"+String(sMin[i])+",\"max\":"+String(sMax[i])+
       ",\"home\":"+String(sHome[i])+",\"cur\":"+String(sCur[i])+"}";
  }
  j+="]}";
  return j;
}

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html><body style="background:#111;color:#0af;font-family:sans-serif">
<h2>Servo Kalibrierung</h2>
<p>Servo: <select id=s onchange=sel()></select>
Step: <input id=st type=number value=5 onchange=step()></p>
<button onclick=mv(-1)>-</button>
<button onclick=mv(1)>+</button><br><br>
<button onclick=setm()>SET MIN</button>
<button onclick=setx()>SET MAX</button>
<button onclick=seth()>SET HOME</button>
<button onclick=save()>SAVE</button>
<p><a href="/ota">OTA Update</a></p>
<pre id=o></pre>
<script>
async function api(q){return fetch('/api?'+q).then(r=>r.json())}
async function upd(){
 let d=await api('state');
 o.textContent=JSON.stringify(d,null,2);
 s.innerHTML='';
 d.data.forEach((_,i)=>{let o=document.createElement('option');o.value=i;o.text='Servo '+(i+1);s.appendChild(o)})
 s.value=d.servo; st.value=d.step;
}
async function sel(){await api('sel='+s.value);upd()}
async function step(){await api('step='+st.value);upd()}
async function mv(d){await api('mv='+d);upd()}
async function setm(){await api('min');upd()}
async function setx(){await api('max');upd()}
async function seth(){await api('home');upd()}
async function save(){await api('save');alert('Gespeichert')}
upd();
</script></body></html>
)HTML";

/* ========= API ========= */
void handleApi(){
  if(server.hasArg("state")) { server.send(200,"application/json",stateJson()); return; }
  if(server.hasArg("sel"))   activeServo=server.arg("sel").toInt();
  if(server.hasArg("step"))  stepUS=server.arg("step").toInt();
  if(server.hasArg("mv"))    moveServo(activeServo, sCur[activeServo]+stepUS*server.arg("mv").toInt());
  if(server.hasArg("min"))   sMin[activeServo]=sCur[activeServo];
  if(server.hasArg("max"))   sMax[activeServo]=sCur[activeServo];
  if(server.hasArg("home"))  sHome[activeServo]=sCur[activeServo];
  if(server.hasArg("save"))  saveCal();
  server.send(200,"application/json",stateJson());
}

/* ========= OTA ========= */
void otaUpload(){
  HTTPUpload& u=server.upload();
  if(u.status==UPLOAD_FILE_START) Update.begin();
  else if(u.status==UPLOAD_FILE_WRITE) Update.write(u.buf,u.currentSize);
  else if(u.status==UPLOAD_FILE_END) Update.end(true);
}

void otaDone(){
  server.send(200,"text/plain","OK");
  delay(300); ESP.restart();
}

/* ========= SETUP ========= */
void setup(){
  Wire.begin();
  pca.begin();
  pca.setPWMFreq(SERVO_FREQ);

  loadCal();
  for(int i=0;i<SERVO_COUNT;i++) moveServo(i,sHome[i]);

  WiFi.softAP(AP_SSID,AP_PASS);

  server.on("/",[](){server.send_P(200,"text/html",PAGE);});
  server.on("/api",handleApi);
  server.on("/ota",[](){
    server.send(200,"text/html",
      "<form method=POST action=/update enctype=multipart/form-data>"
      "<input type=file name=update><input type=submit></form>");
  });
  server.on("/update",HTTP_POST,otaDone,otaUpload);
  server.begin();
}

void loop(){
  server.handleClient();
}
