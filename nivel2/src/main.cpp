/*
 * Servo Tester (Nivel 2) for ESP32-C3 Super Mini
 * -------------------------------------------------
 * EXTENDS Nivel 1: same architecture (MENU / MANUAL / SWEEP / CENTER / SETTINGS,
 * encoder interrupt, OLED, servo, Preferences) PLUS an ANALYZER:
 *   - Servo supply voltage via a 10k/10k resistor divider on ADC1 (GPIO3).
 *   - Bus current + power via an INA219 in series with the servo 5 V line (I2C).
 *   - Overcurrent / low-voltage alarm shown on the OLED and sounded on a buzzer.
 *
 * ENCODER CHOICE (unchanged from L1):
 *   CHANGE-interrupt on CLK, reads DT, timestamp debounce. Rock-solid on the C3,
 *   zero dependency risk. One detent == +/-1 degree (or +/-10 us / +/-50 mA).
 *
 * V-SENSE PIN CHOICE:
 *   We use GPIO3 (ADC1 ch3) as specified. The C3 Super Mini routes GPIO1/3 to the
 *   onboard CH340 USB-UART, which has a weak pull-up to 3.3 V. With a 10k/10k
 *   divider the Thevenin source resistance is 5k, so a tens-of-kOhm board pull-up
 *   only adds a small, fairly constant offset. That offset is removed by the
 *   one-point calibration constant `vCal` (see README section 7). If you need
 *   maximum accuracy, move the divider to GPIO1 (same situation) or cut the
 *   CH340 pull-up; functionally either is fine.
 *
 * DEBUG NOTE (unchanged from L1):
 *   Serial is UART0 (GPIO1/3); not exposed on USB. All feedback is on the OLED.
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <Adafruit_INA219.h>

// ----------------------------- Pinout (3.3 V logic) -------------------------
#define OLED_SDA   6
#define OLED_SCL   7
#define SERVO_PIN  10
#define ENC_CLK    4
#define ENC_DT     5
#define ENC_SW     20
#define VSENSE_PIN 3      // ADC1 ch3, 10k/10k divider from servo 5 V rail
#define BUZZER_PIN 21     // active-high piezo buzzer

// ----------------------------- OLED object ----------------------------------
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ----------------------------- Servo ----------------------------------------
Servo servo;

// ----------------------------- INA219 (analyzer) ----------------------------
Adafruit_INA219 ina219;           // default address 0x40

// ----------------------------- Settings (persisted) -------------------------
// Angle domain fixed at 0..180 deg. minUs/maxUs = pulse at 0/180 deg.
// centerAngle = angle held in CENTER. iThresh = overcurrent alarm (mA).
// vCal = calibration multiplier for the V divider.
Preferences prefs;
int   minUs       = 500;    // pulse width (us) at 0 deg
int   maxUs       = 2500;   // pulse width (us) at 180 deg
int   centerAngle = 90;     // angle held in CENTER mode
int   iThresh     = 800;    // overcurrent threshold (mA)
float vCal        = 1.0;    // divider calibration constant

const int minAng = 0;
const int maxAng = 180;

// ----------------------------- Encoder (interrupt based) --------------------
volatile int  encoderDelta = 0;
volatile unsigned long lastEncMs = 0;
volatile bool buttonFlag = false;
volatile unsigned long lastBtnMs = 0;

// ----------------------------- State machine --------------------------------
enum Mode { MENU, MANUAL, SWEEP, CENTER, SETTINGS };
Mode mode = MENU;
const char* modeNames[] = { "MENU", "MANUAL", "SWEEP", "CENTER", "SETTINGS" };

int menuIndex = 0;
const int MENU_COUNT = 4;

// SETTINGS sub-fields: 0=minUs, 1=maxUs, 2=center, 3=iThresh
int settingField = 0;
const int SETTINGS_FIELDS = 4;

int angle = 90;

// ----------------------------- Analyzer sample ------------------------------
float vServo = 0.0;   // supply voltage from divider (V)
float iServo = 0.0;   // current from INA219 (mA)
float pServo = 0.0;   // power = V * I (W)
bool  alarmOn  = false; // overcurrent or low voltage

// ----------------------------- ISRs -----------------------------------------
void IRAM_ATTR encoderISR() {
  unsigned long now = millis();
  if (now - lastEncMs < 5) return;     // 5 ms timestamp debounce
  lastEncMs = now;
  int clk = digitalRead(ENC_CLK);
  if (clk == HIGH) {                   // count on rising edge of CLK
    int dt = digitalRead(ENC_DT);
    encoderDelta += (dt == LOW) ? 1 : -1;   // CW vs CCW
  }
}

void IRAM_ATTR buttonISR() {
  unsigned long now = millis();
  if (now - lastBtnMs < 50) return;    // 50 ms debounce
  lastBtnMs = now;
  buttonFlag = true;
}

// ----------------------------- Helpers --------------------------------------
int angleToUs(int a) {
  a = constrain(a, minAng, maxAng);
  return (int)map((long)a, minAng, maxAng, minUs, maxUs);
}

int sweepAngle() {
  unsigned long half = 1500;
  unsigned long t = millis();
  unsigned long phase = t % (2 * half);
  int a;
  if (phase < half) a = (int)map((long)phase, 0, (long)half, 0, 180);
  else              a = (int)map((long)phase, (long)half, 2 * (long)half, 180, 0);
  return constrain(a, 0, 180);
}

void loadSettings() {
  prefs.begin("servotester", true);   // read-only
  minUs       = prefs.getUInt("minUs", 500);
  maxUs       = prefs.getUInt("maxUs", 2500);
  centerAngle = prefs.getUInt("center", 90);
  iThresh     = prefs.getUInt("iThresh", 800);
  vCal        = prefs.getFloat("vCal", 1.0);
  prefs.end();
  minUs       = constrain(minUs, 400, 1500);
  maxUs       = constrain(maxUs, 1500, 2600);
  centerAngle = constrain(centerAngle, 0, 180);
  iThresh     = constrain(iThresh, 100, 5000);
  if (vCal <= 0.0 || isnan(vCal)) vCal = 1.0;
}

void saveSettings() {
  prefs.begin("servotester", false);  // read-write
  prefs.putUInt("minUs",   (unsigned int)minUs);
  prefs.putUInt("maxUs",   (unsigned int)maxUs);
  prefs.putUInt("center",  (unsigned int)centerAngle);
  prefs.putUInt("iThresh", (unsigned int)iThresh);
  prefs.putFloat("vCal",   vCal);
  prefs.end();
}

// Read the servo supply voltage from the 10k/10k divider on GPIO3.
// Vservo = adc * (3.3/4095) * 2.0, corrected by the calibration constant.
float readVservo() {
  int raw = analogRead(VSENSE_PIN);
  float v = raw * (3.3f / 4095.0f) * 2.0f * vCal;
  return v;
}

// Read current (mA) and bus voltage (V) from the INA219; derive power (W).
void readINA219() {
  float busV = ina219.getBusVoltage_V();
  iServo = ina219.getCurrent_mA();
  pServo = busV * (iServo / 1000.0f);
}

// Buzzer driver: beep while alarm is active, silent otherwise.
void updateBuzzer() {
  static unsigned long lastBeep = 0;
  static bool beepState = false;
  if (alarmOn) {
    if (millis() - lastBeep > 250) {   // ~2 Hz beep
      lastBeep = millis();
      beepState = !beepState;
      digitalWrite(BUZZER_PIN, beepState);
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    beepState = false;
  }
}

// Evaluate the analyzer alarm conditions.
void updateAlarm() {
  alarmOn = (iServo > (float)iThresh) || (vServo < 4.5f);
}

// ----------------------------- Input handling -------------------------------
void handleInput() {
  noInterrupts();
  int d = encoderDelta;
  encoderDelta = 0;
  interrupts();

  if (d != 0) {
    switch (mode) {
      case MENU:
        menuIndex = constrain(menuIndex + d, 0, MENU_COUNT - 1);
        break;
      case MANUAL:
        angle = constrain(angle + d, minAng, maxAng);
        break;
      case SETTINGS:
        if (settingField == 0)      minUs       = constrain(minUs + d * 10, 400, 1500);
        else if (settingField == 1) maxUs       = constrain(maxUs + d * 10, 1500, 2600);
        else if (settingField == 2) centerAngle = constrain(centerAngle + d, 0, 180);
        else                        iThresh     = constrain(iThresh + d * 50, 100, 5000);
        break;
      default:
        break; // SWEEP / CENTER ignore the encoder
    }
  }

  noInterrupts();
  bool b = buttonFlag;
  buttonFlag = false;
  interrupts();

  if (b) {
    switch (mode) {
      case MENU:
        if      (menuIndex == 0) mode = MANUAL;
        else if (menuIndex == 1) mode = SWEEP;
        else if (menuIndex == 2) mode = CENTER;
        else if (menuIndex == 3) { mode = SETTINGS; settingField = 0; }
        break;
      case MANUAL:
      case SWEEP:
      case CENTER:
        mode = MENU;
        break;
      case SETTINGS:
        settingField++;
        if (settingField >= SETTINGS_FIELDS) {
          saveSettings();
          mode = MENU;
        }
        break;
    }
  }
}

// ----------------------------- Rendering -------------------------------------
void drawGauge(int ang) {
  int x = 4, y = 56, w = SCREEN_W - 8, h = 6;
  display.drawRect(x, y, w, h, WHITE);
  int fill = map(constrain(ang, 0, 180), 0, 180, 0, w);
  display.fillRect(x, y, fill, h, WHITE);
}

// Analyzer-aware servo screen: angle, pulse, gauge, V/I rows, alarm flag.
void drawServo(const char* label, int ang) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print(label);

  // Alarm indicator at top-right.
  if (alarmOn) {
    display.setCursor(80, 0);
    display.print("ALARMA");
  }

  display.setTextSize(3);
  display.setCursor(20, 14);
  display.print(ang);

  display.setTextSize(1);
  display.setCursor(0, 42);
  display.print(angleToUs(ang));
  display.print(" us");

  display.setCursor(0, 42 + 10);
  display.print("V:");
  display.print(vServo, 2);
  display.print("V I:");
  display.print((int)iServo);
  display.print("mA");

  drawGauge(ang);
  display.display();
}

void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("MENU");

  const char* items[] = { "MANUAL", "SWEEP", "CENTER", "SETTINGS" };
  for (int i = 0; i < MENU_COUNT; i++) {
    display.setCursor(0, 14 + i * 10);
    display.print(i == menuIndex ? "> " : "  ");
    display.println(items[i]);
  }
  // Analyzer line at the bottom of the menu.
  display.setCursor(0, 54);
  display.print("V:");
  display.print(vServo, 2);
  display.print(" I:");
  display.print((int)iServo);
  display.print("mA");
  display.display();
}

void drawSettings() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("SETTINGS");

  display.print(settingField == 0 ? " >" : "  ");
  display.print("minUs : "); display.println(minUs);
  display.print(settingField == 1 ? " >" : "  ");
  display.print("maxUs : "); display.println(maxUs);
  display.print(settingField == 2 ? " >" : "  ");
  display.print("center: "); display.println(centerAngle);
  display.print(settingField == 3 ? " >" : "  ");
  display.print("iThrsh: "); display.print(iThresh); display.println("mA");

  display.setCursor(0, 54);
  display.println("SW=next  turn=edit");
  display.display();
}

// ----------------------------- Setup -----------------------------------------
void setup() {
  Serial.begin(115200);                 // UART0 only; NOT visible over USB

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // ADC for the V divider: 12-bit, full 0..3.3 V range.
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(VSENSE_PIN, INPUT);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (true) { delay(1000); }      // No OLED: fault obvious on the bench.
  }
  display.clearDisplay();
  display.display();

  // INA219 on the same I2C bus (address 0x40). Halt if missing so the bench
  // fault is clear. 32 V / 2 A calibration covers standard hobby servos.
  if (!ina219.begin(&Wire)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("INA219 ??");
    display.display();
    while (true) { delay(1000); }
  }
  ina219.setCalibration_32V_2A();

  loadSettings();
  servo.attach(SERVO_PIN, minUs, maxUs);

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_SW),  buttonISR, FALLING);
}

// ----------------------------- Loop ------------------------------------------
void loop() {
  handleInput();

  // Sample the analyzer every frame.
  vServo = readVservo();
  readINA219();
  updateAlarm();
  updateBuzzer();

  int driveAngle = angle;
  switch (mode) {
    case MANUAL:   driveAngle = angle;       break;
    case CENTER:   driveAngle = centerAngle; angle = centerAngle; break;
    case SWEEP:    driveAngle = sweepAngle(); angle = driveAngle; break;
    case SETTINGS: driveAngle = angle;       break;
    case MENU:     driveAngle = -1;          break;
  }

  if (mode != MENU) {
    servo.writeMicroseconds(angleToUs(driveAngle));
  }

  switch (mode) {
    case MENU:    drawMenu();                        break;
    case MANUAL:  drawServo("MANUAL", angle);        break;
    case SWEEP:   drawServo("SWEEP", sweepAngle());   break;
    case CENTER:  drawServo("CENTER", centerAngle);   break;
    case SETTINGS: drawSettings();                   break;
  }

  delay(20); // ~50 Hz UI refresh, headroom for servo timing
}
