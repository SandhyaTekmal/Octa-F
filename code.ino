/* EcoTag - OLED animated UI + BME680 + UV + Buzzer + Blynk
   Features:
   - Welcome animation + welcome tone
   - 5s calibration for gas baseline
   - Per-parameter screens (2s each), final combined screen (8s)
   - Air-quality geiger-like beeps: >600 (1 beep/sec), >800 (2/sec), >1000 (3/sec)
   - Blinking alert screens when parameter exceeds threshold
   - Priority: Air > Temp > UV > Humidity > Pressure
   - Blynk V0..V4 updates
   - Temperature offset -6°C applied
*/

#define BLYNK_TEMPLATE_ID "TMPsfugdsifhLQqs-Kp"
#define BLYNK_TEMPLATE_NAME "Project Eco-Tag"
#define BLYNK_AUTH_TOKEN "S9tzrrv1LpGqxuhfuhrhCIStQzVDHj"

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_BME680.h"
#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>

///// HARDWARE PINS /////
#define OLED_ADDR 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

Adafruit_BME680 bme; // I2C

#define UV_PIN A0
#define BUZZER_PIN D5

///// BLYNK & WIFI /////

char ssid[] = "N.15";
char pass[] = "12345678";

BlynkTimer timer;
bool wifiConnected = false;


// WiFi / Blynk interval settings
const unsigned long WIFI_INTERVAL_SEC = 10;  // change as needed
unsigned long lastWiFiAction = 0;

///// USER SETTINGS /////
// Temperature offset requested: subtract 6°C from measured
const float TEMP_OFFSET = -3.0;

// Calibration duration (ms)
const unsigned long CALIB_MS = 1000UL; // 5 seconds (as requested)

// Blynk update interval (ms)
const unsigned long BLYNK_UPDATE_MS = 2000UL;

///// THRESHOLDS (standard, you approved) /////
const float TEMP_COLD = 15.0;
const float TEMP_HOT  = 37.0;

const float HUM_LOW = 30.0;
const float HUM_HIGH = 99.0;

const float PRES_LOW = 709.0;
const float PRES_HIGH = 1200.0;

// UV index thresholds (official)
const float UV_SAFE_MAX = 2.0;
const float UV_MODERATE_MAX = 5.0;
const float UV_HIGH_MAX = 7.0;
const float UV_VERYHIGH_MAX = 10.0;
// UV extreme > 11+

// Air quality mapping: we use pseudo-ppm from gas resistance baseline.
// Alarm thresholds (ppm)
const float AQ_ALERT_1 = 350.0;  // 1 beep/sec
const float AQ_ALERT_2 = 600.0;  // 2 beeps/sec
const float AQ_ALERT_3 = 1000.0; // 3 beeps/sec

///// STATE VARIABLES /////
unsigned long startupMillis = 0;
unsigned long calibrationStart = 0;
bool calibrated = false;
float gasBaselineSum = 0.0;
unsigned long gasBaselineCount = 0;
float gasBaseline = 100000.0; // default fallback (ohms)

unsigned long lastBlynkSend = 0;

// Screen rotation
// 0: temp+hum (2s)
// 1: pressure (2s)
// 2: UV (2s)
// 3: Air quality (2s)
// 4: All params (8s)
int screenIndex = -1;
unsigned long screenStart = 0;

// For blinking alert
bool blinkState = false;
unsigned long lastBlinkToggle = 0;
const unsigned long BLINK_MS = 500UL; // blink every 500 ms

// For geiger style beeps scheduling
unsigned long lastBeepCycleStart = 0;
unsigned long beepCycleMs = 1000UL; // 1 second cycle
int beepsThisCycle = 0;             // 0..3
int beepsEmittedInCycle = 0;
unsigned long nextBeepTime = 0;
const int BEEP_FREQ = 3000; // Hz
const int BEEP_DUR = 100;    // ms (50ms beep length)



// Buzzer state variables
unsigned long beepTimer = 0;
bool buzzerState = false;
int currentBeep = 0;
int beepCount = 0;
int beepInterval = 1000;  // 1 second cycle by default


// Blynk virtual pins mapping
// V0 Temp, V1 Humidity, V2 Pressure, V3 UV, V4 AQ(ppm)
#define VP_TEMP V0
#define VP_HUM V1
#define VP_PRES V2
#define VP_UV V3
#define VP_AQ V4
#define VP_ALT V5

///// HELPERS /////
String tempStatus(float t) {
  if (t < TEMP_COLD) return "Cold";
  if (t > TEMP_HOT) return "Hot";
  return "Good";
}

String humStatus(float h) {
  if (h < HUM_LOW) return "Dry";
  if (h > HUM_HIGH) return "Humid";
  return "Good";
}

String presStatus(float p) {
  if (p < PRES_LOW) return "Low";
  if (p > PRES_HIGH) return "High";
  return "Normal";
}

String uvStatus(float uv) {
  if (uv <= UV_SAFE_MAX) return "Safe";
  if (uv <= UV_MODERATE_MAX) return "Moderate";
  if (uv <= UV_HIGH_MAX) return "High";
  if (uv <= UV_VERYHIGH_MAX) return "VeryHigh";
  return "Extreme";
}

String aqStatus(float ppm) {
  if (ppm < 170) return "Excellent";
  if (ppm < 300) return "Good";
  if (ppm < 500) return "Moderate";
  if (ppm < 1000) return "Unhealthy";
  return "Danger";
}

// compute pseudo-ppm from gas resistance
float gasToPpm(float gas_res) {
  // gasBaseline should be average baseline in ohms
  if (gas_res <= 0.0) return 0.0;
  float ratio = gasBaseline / gas_res;
  float ppm = ratio * 450.0; // scale factor (approx)
  if (ppm < 0.0) ppm = 0.0;

  return ppm;
}

// set screen index and reset timers
void setScreen(int idx) {
  screenIndex = idx;
  screenStart = millis();
  blinkState = false;
  lastBlinkToggle = millis();
}

// --- ANIMATION HELPERS ---

void drawSlidingText(String text, int y, int size=2, int delayMs=5) {
  display.clearDisplay();
  display.setTextSize(size);
  for (int i = -SCREEN_WIDTH; i < 5; i+=5) {
    display.clearDisplay();
    display.setCursor(i, y);
    display.print(text);
    display.display();
    delay(delayMs);
  }
}

void drawFadeInText(String text, int y, int size=2, int steps=5, int delayMs=50) {
  for (int i = 0; i <= steps; i++) {
    display.clearDisplay();
    display.setTextSize(size);
    display.setCursor((SCREEN_WIDTH - (text.length()*size*6))/2, y);
    for (int j=0; j<text.length(); j++) {
      if (random(steps) <= i) display.print(text[j]); 
      else display.print(" ");
    }
    display.display();
    delay(delayMs);
  }
}

void drawProgressBar(int progress, int total) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(20, 20);
  display.print("Calibrating...");
  display.drawRect(10, 40, 108, 10, SSD1306_WHITE);
  display.fillRect(10, 40, (progress*108)/total, 10, SSD1306_WHITE);
  display.display();
}

// --- SOUND HELPERS ---
#define BUZZER_PIN D5

void playStartupTune() {
  int melody[] = { 784, 880, 988, 1175 }; // G5, A5, B5, D6
  int noteDurations[] = { 200, 200, 200, 400 };
  for (int i = 0; i < 4; i++) {
    tone(BUZZER_PIN, melody[i], noteDurations[i]);
    delay(noteDurations[i] + 30);
  }
  noTone(BUZZER_PIN);
}

void playWarningSiren() {
  for (int i = 0; i < 2; i++) {
    for (int f = 1000; f < 2000; f += 50) {
      tone(BUZZER_PIN, f, 20);
      delay(20);
    }
    for (int f = 2000; f > 1000; f -= 50) {
      tone(BUZZER_PIN, f, 20);
      delay(20);
    }
  }
  noTone(BUZZER_PIN);
}


// draw big centered text helper
void drawCenteredBig(const char* txt, int y, int size = 3) {
  display.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(txt, 0, y, &x1, &y1, &w, &h);
  int x = (SCREEN_WIDTH - w) / 2;
  display.setCursor(x, y);
  display.print(txt);
}

// welcome melody (rising triad)
void playWelcomeTone() {
  tone(BUZZER_PIN, 1500, 150);
  delay(180);
  tone(BUZZER_PIN, 1700, 150);
  delay(180);
  tone(BUZZER_PIN, 3000, 200);
  delay(220);
  noTone(BUZZER_PIN);
}

// emit a single short beep (50ms) at BEEP_FREQ
void emitShortBeep() {
  tone(BUZZER_PIN, BEEP_FREQ, BEEP_DUR);
  // let non-blocking scheduling decide; just ensure noTone after BEEP_DUR
  // using delay here is okay because BEEP_DUR is short; but we schedule to avoid blocking longer periods
}

// non-blocking geiger-style beeping controller 
void geigerBeepController(float ppm) {
  // determine beeps per 1s cycle based on ppm
  int targetBeeps = 0;
 // Decide beep pattern based on ppm
if (ppm > 1000) { beepCount = 3; beepInterval = 1000; }
else if (ppm > 800) { beepCount = 2; beepInterval = 1000; }
else if (ppm > 600) { beepCount = 1; beepInterval = 1000; }
else { beepCount = 0; }


  unsigned long now = millis();

  // If target changed, restart cycle
  if (targetBeeps != beepsThisCycle) {
    beepsThisCycle = targetBeeps;
    lastBeepCycleStart = now;
    beepsEmittedInCycle = 0;
    nextBeepTime = now; // immediate start in this cycle
  }

  if (beepsThisCycle == 0) {
    // nothing to do: ensure buzzer off
    noTone(BUZZER_PIN);
    return;
  }

  // duration of a cycle (1 second)
  beepCycleMs = 1000UL;

  // spacing: we emit beeps evenly within the 1s cycle
  unsigned long cycleElapsed = (now - lastBeepCycleStart) % beepCycleMs;
  int beepsPlanned = beepsThisCycle;
  // compute when each beep should happen: positions (0..beepsPlanned-1)
  for (int i = 0; i < beepsPlanned; ++i) {
    unsigned long idealStart = (unsigned long)(((unsigned long)i * (beepCycleMs)) / beepsPlanned);
    // if we are within a small window and haven't emitted this beep in current cycle, emit
    if (cycleElapsed >= idealStart && cycleElapsed < idealStart + BEEP_DUR) {
      // ensure we emit only when within window and not re-emit repeatedly
      // Use beepsEmittedInCycle to limit, but a simpler method: call tone with short duration each loop if cycleElapsed within small window
      tone(BUZZER_PIN, BEEP_FREQ, BEEP_DUR);
    }
  }
  // noTone handled automatically after duration; ensure at extremes we call noTone occasionally
  // If cycle rolled over, reset emission count
  if (now - lastBeepCycleStart >= beepCycleMs) {
    lastBeepCycleStart = now - ((now - lastBeepCycleStart) % beepCycleMs);
  }
}

// draw parameter screen with large value and large status
void drawParamScreen(const char* label, String bigVal, String status, bool alert) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(label);
  // big value
  display.setTextSize(3);
  int16_t x1, y1; uint16_t w,h;
  display.getTextBounds(bigVal, 0, 20, &x1, &y1, &w, &h);
  int x = (SCREEN_WIDTH - w) / 2;
  display.setCursor(x, 18);
  display.print(bigVal);
  // status in large font
  display.setTextSize(2);
  String st = status;
  display.getTextBounds(st, 0, 48, &x1, &y1, &w, &h);
  x = (SCREEN_WIDTH - w) / 2;
  display.setCursor(x, 48);
  if (alert) {
    // blinking
    unsigned long now = millis();
    if (now - lastBlinkToggle >= BLINK_MS) { blinkState = !blinkState; lastBlinkToggle = now; }
    if (blinkState) display.print(st);
  } else {
    display.print(st);
  }
  display.display();
}

// draw final all-in-one page
void drawAllParams(float t, float h, float p, float uv, float ppm, float alt) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("EcoTag by 08.15");

  display.setCursor(0,14); display.print("T:"); display.print(t,1); display.print("C ");
  display.print("H:"); display.print(h,1); display.println("%");
  
  display.setCursor(0,28); display.print("P:"); display.print(p,1); display.print("hPa ");
  display.print("UV:"); display.print(uv,1);
  
  display.setCursor(0,42); display.print("Air:"); display.print(ppm,0); display.println(" ppm");

  display.setCursor(0,56); display.print("Alt:"); display.print(alt,1); display.print(" m");

  display.display();
}




///// SETUP /////
void setup() {
  Serial.begin(115200);
  delay(50);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
    while (1);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

    display.setTextSize(3);
  display.setCursor(10,18);
  display.print("EcoTag");
  display.setTextSize(2);
  display.setCursor(5, 44);
  display.print("");
  display.display();
  playWelcomeTone();
  delay(3000);

      display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10,18);
  display.print("HYP");
  display.setTextSize(2);
  display.setCursor(26, 44);
  display.print("7.0");
  display.display();
  //playWelcomeTone();
  delay(2000);

  // Show welcome big + tone
    display.clearDisplay();
 // --- Welcome Animation ---
drawFadeInText("Octa-F", 18, 3);
drawFadeInText("", 44, 2);
//playStartupTune();
delay(1000);

// --- Calibration with progress bar (20s) ---
unsigned long calibrationStart = millis();
while (millis() - calibrationStart < 10000) {
  int progress = map(millis() - calibrationStart, 0, 10000, 0, 100);
  drawProgressBar(progress, 100);
  delay(200);
  
}
 playStartupTune();

  // initialize Blynk & WiFi (non-blocking Blynk.begin will connect)
  WiFi.begin(ssid, pass);
  Blynk.config(BLYNK_AUTH_TOKEN);

  // BME680 init (auto-detect 0x77 or 0x76)
  if (!bme.begin(0x77)) {
    if (!bme.begin(0x76)) {
      Serial.println("BME680 not found!");
      while (1);
    }
  }

  // configure BME sensor
  bme.setTemperatureOversampling(BME680_OS_16X);
  bme.setHumidityOversampling(BME680_OS_16X);
  bme.setPressureOversampling(BME680_OS_16X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320,150);

  // calibration start
  calibrationStart = millis();
  calibrated = false;
  gasBaselineSum = 0.0;
  gasBaselineCount = 0;
  screenIndex = -1;
  screenStart = millis();
  lastBlynkSend = 0;
  lastBeepCycleStart = millis();
}

///// MAIN LOOP /////
void loop() {
  Blynk.run(); // keep cloud alive
  if(WiFi.status() == WL_CONNECTED){
    wifiConnected = true;
    Blynk.run();
  }
  else{
    wifiConnected = false;
  }
  // Buzzer handler
if (beepCount > 0) {
  if (millis() - beepTimer >= (buzzerState ? 50 : (beepInterval / (beepCount + 1)))) {
    beepTimer = millis();
    buzzerState = !buzzerState;

    if (buzzerState && currentBeep < beepCount) {
      tone(BUZZER_PIN, 3000, 50); // short beep
      currentBeep++;
    } else if (!buzzerState && currentBeep >= beepCount) {
      currentBeep = 0; // reset for next cycle
    }
  }
} else {
  noTone(BUZZER_PIN); // stop buzzer if safe
}


  // read BME680 (use performReading pattern to be safe)
  if (!bme.performReading()) {
    // try again next loop
    delay(20);
    return;
  }

  // raw values
  float rawTemp = bme.temperature; // degC
  float temp = rawTemp + TEMP_OFFSET; // apply -6°C offset per request
  float humi = bme.humidity;
  float hum = humi+16;
  float pres = bme.pressure / 100.0; // hPa
  float gas_res = bme.gas_resistance; // ohms
  // Altitude calculation (assuming sea level pressure 1013.25 hPa)
  float altitude = bme.readAltitude(1013.25);  

  // UV analog read → convert to UV index approx
  int uvRaw = analogRead(UV_PIN);
  float uvVoltage = uvRaw * (3.3 / 1023.0);
  float uvIndex = uvVoltage * 1.0; // approx scaling (you can calibrate later)

  // During calibration period (5s), average gas_res
  if (!calibrated) {
    unsigned long now = millis();
    gasBaselineSum += gas_res;
    gasBaselineCount++;
    if (now - calibrationStart >= CALIB_MS) {
      // avoid division by zero
      if (gasBaselineCount > 0) {
        gasBaseline = gasBaselineSum / (float)gasBaselineCount;
      } else {
        gasBaseline = 100000.0;
      }
      calibrated = true;
      Serial.print("Calibration complete; gasBaseline = ");
      Serial.println(gasBaseline);
      // start screen rotation after calibration
      setScreen(0);
      screenStart = millis();
    } else {
      // still calibrating - keep showing calibrating screen
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(10,25); display.println("Please wait,");
      display.setCursor(10,40); display.println("Calibrating sensors...");
      display.display();
      delay(50);
      return;
    }
  }

  // compute pseudo-ppm
  float ppm = gasToPpm(gas_res);

  // determine alerts for each parameter
// --- Air Quality Alerts ---
if (ppm > 1000) {
  playWarningSiren();   // Continuous siren-like
} else if (ppm > 800) {
  tone(BUZZER_PIN, 3000, 50); delay(300);
  tone(BUZZER_PIN, 3000, 50); delay(300);
  noTone(BUZZER_PIN);
} else if (ppm > 600) {
  tone(BUZZER_PIN, 3000, 50);
  delay(1000);
  noTone(BUZZER_PIN);
}

  bool alertTemp = (temp < TEMP_COLD || temp > TEMP_HOT);
  bool alertUV = (uvIndex > UV_MODERATE_MAX); // moderate+ considered alert
  bool alertHum = (hum < HUM_LOW || hum > HUM_HIGH);
  bool alertPres = (pres < PRES_LOW || pres > PRES_HIGH);

  // priority selection: Air > Temp > UV > Hum > Pres
  int alertPriority = -1; // -1 none, 0 air, 1 temp, 2 uv, 3 hum, 4 pres
  //if (alertAir) alertPriority = 0;
   if (alertTemp) alertPriority = 1;
  else if (alertUV) alertPriority = 2;
  else if (alertHum) alertPriority = 3;
  else if (alertPres) alertPriority = 4;

  // If any alert active, override screen to show the top priority parameter (blinking) until safe
  if (alertPriority != -1) {
    // override screen display for alert priority
    switch(alertPriority) {
      case 0: { // Air
        // big ppm and big status
        String bigVal = String((int)ppm);
        String status = aqStatus(ppm);
        drawParamScreen("AIR QUALITY", bigVal + " ppm", status, true);
        break;
      }
      case 1: { // Temp
        String bigVal = String(temp,1) + "C";
        String status = tempStatus(temp);
        drawParamScreen("TEMPERATURE", bigVal, status, true);
        break;
      }
      case 2: { // UV
        String bigVal = String(uvIndex,1);
        String status = uvStatus(uvIndex);
        drawParamScreen("UV INDEX", bigVal, status, true);
        break;
      }
      case 3: { // Humidity
        String bigVal = String(hum,1) + "%";
        String status = humStatus(hum);
        drawParamScreen("HUMIDITY", bigVal, status, true);
        break;
      }
      case 4: { // Pressure
        String bigVal = String(pres,1) + "hPa";
        String status = presStatus(pres);
        drawParamScreen("PRESSURE", bigVal, status, true);
        break;
      }
    }
    // buzzer alert logic: air uses geiger style; other params a single short beep per second
    if (alertPriority == 0) {
      geigerBeepController(ppm);
    } else {
      // simple short beep once per 1 second when in alert
      unsigned long now = millis();
      static unsigned long lastAlertBeep = 0;
      if (now - lastAlertBeep >= 1000UL) {
        lastAlertBeep = now;
        tone(BUZZER_PIN, 3000, 80); // 3kHz 80ms
      }
    }

    // send Blynk values too (still)
   // --- WiFi duty-cycled connection ---
unsigned long now = millis();

// Check if it's time to connect & send data
if (now - lastWiFiAction >= WIFI_INTERVAL_SEC * 1000UL) {
  lastWiFiAction = now;

  // Connect WiFi
  WiFi.begin(ssid, pass);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 5000) {
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Blynk.connect(2000); // try 2s
    if (Blynk.connected()) {
      // Send latest sensor data
      Blynk.virtualWrite(VP_TEMP, temp);
      Blynk.virtualWrite(VP_HUM, hum);
      Blynk.virtualWrite(VP_PRES, pres);
      Blynk.virtualWrite(VP_UV, uvIndex);
      Blynk.virtualWrite(VP_AQ, ppm);
      Blynk.virtualWrite(V6, altitude); // <- altitude added
    }
    Blynk.disconnect(); // disconnect from Blynk
  } else {
    wifiConnected = false;
  }

  WiFi.disconnect(true); // fully shut down WiFi radio
}


    delay(20); // small delay to let tone play short
    return; // skip normal rotation while alert persists (as requested)
  }

  // No alerts active - proceed with normal screen rotation
  unsigned long now = millis();
  unsigned long elapsed = now - screenStart;
  // screen timings defined: 2s each for first 4 screens, final combined 8s
  // we have 5 parameter screens + combined. We'll step them sequentially.
  // if screenIndex is -1 (first time after calibration), set to 0
  if (screenIndex == -1) setScreen(0);

  // compute which screen to show based on elapsed since screenStart
  // We'll keep current screenIndex until its duration expires and then increment
  unsigned long durationForCurrent = 4000UL; // default 2s
  if (screenIndex == 4) durationForCurrent = 8000UL; // combined
  if (elapsed >= durationForCurrent) {
    // move to next screen
    screenIndex++;
    if (screenIndex > 4) screenIndex = 0;
    screenStart = now;
  }

  // show selected screen (no blinking) but show big values + status text
  switch(screenIndex) {
    case 0: { // Temp + Humidity combined on one screen (as you asked earlier)
      // But you wanted separate screens for each parameter earlier; user asked multiple times.
      // Here implement Temp+Humidity on screen 0.
      // Big temp and below humidity smaller.
      display.clearDisplay();
      display.setTextSize(2);
      display.setCursor(0, 0);
      display.print("Temp:");
      display.setTextSize(3);
      display.setCursor(0, 18);
      display.print(temp,1);
      display.print("C");
      display.setTextSize(2);
      display.setCursor(0, 46);
      display.print("H:"); display.print(hum,0); display.print("% ");
      display.setCursor(70,46);
      display.print(tempStatus(temp)); // small status
      display.display();
      break;
    }
    case 1: { // Pressure and UV (as you first requested earlier)
      display.clearDisplay();
      display.setTextSize(2);
      display.setCursor(0, 0);
      display.print("Pressure:");
      display.setTextSize(2);
      display.setCursor(0, 18);
      display.print(pres,1); display.print(" hPa");
      display.setTextSize(2);
      display.setCursor(0, 36);
      display.print("UV:"); display.print(uvIndex,1);
      display.setCursor(70,36);
      display.print(uvStatus(uvIndex));
      display.display();
      break;
    }
    case 2: { // Air quality large
      String bigVal = String((int)ppm);
      String status = aqStatus(ppm);
      drawParamScreen("AIR QUALITY", bigVal + " ppm", status, false);
      break;
    }
    case 3: { // Individual UV screen (we already had UV shown but keep extra screen)
      String bigVal = String(uvIndex,1);
      String status = uvStatus(uvIndex);
      drawParamScreen("UV INDEX", bigVal, status, false);
      break;
    }
    case 4: { // combined all params
     drawAllParams(temp, hum, pres, uvIndex, ppm, altitude);

      break;
    }
  }

  // Blynk push periodic
  
 if (wifiConnected && millis() - lastBlynkSend >= BLYNK_UPDATE_MS) {
    Blynk.virtualWrite(VP_TEMP, temp);
    Blynk.virtualWrite(VP_HUM, hum);
    Blynk.virtualWrite(VP_PRES, pres);
    Blynk.virtualWrite(VP_UV, uvIndex);
    Blynk.virtualWrite(VP_AQ, ppm);
    Blynk.virtualWrite(VP_ALT, altitude);

    lastBlynkSend = millis();
  }

  // In non-alert mode, still run geiger beeps if air moderate (>=600 etc.)

  geigerBeepController(ppm);

  // tiny delay for loop stability

  delay(20);
}
