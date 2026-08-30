/*
 * Servo Tester (Nivel 3) for ESP32-C3 DevKitM-1
 * -------------------------------------------------
 * EXTENDE Nivel 2 (menu, manual, sweep, center, settings, analizador V/I,
 * alarma, Preferences) con PREMIUM:
 *   1. MULTICANAL: 4 servos (CH1..CH4). Un canal ACTIVO recibe los comandos
 *      MANUAL/SWEEP/CENTER; los otros mantienen su ultima posicion.
 *   2. BLE (NimBLE): servidor "ServoTester" con caracteristica de telemetria
 *      (read+notify) y caracteristica de escritura (canal/modo/angulo).
 *   3. GRAFICO de respuesta: buffer circular (~64 muestras) del angulo del
 *      canal activo, dibujado como grafica de linea en la OLED.
 *
 * PINOUT NUEVO (documentado en README):
 *   OLED SSD1306 I2C : SDA=GPIO6, SCL=GPIO7, addr 0x3C  (Adafruit SSD1306/GFX)
 *   INA219            : mismo bus I2C (0x40)
 *   Encoder HW-040    : CLK=GPIO4, DT=GPIO5, SW=GPIO20
 *   V-sense divider   : GPIO3 (10k/10k desde el 5 V del servo)
 *   Buzzer piezo      : GPIO8  (tambien es el LED onboard; usarlo como buzzer
 *                               es seguro)
 *   4 servos          : CH1=GPIO10, CH2=GPIO1, CH3=GPIO2, CH4=GPIO21
 *      GPIO1 es el TX de UART0 (no usado por USB); GPIO2 es strapping pero
 *      seguro como SALIDA de servo tras el arranque. Ver README.
 *
 * NOTA USB-CDC: no se usa ARDUINO_USB_CDC_ON_BOOT. Serial == UART0 (GPIO1/3),
 * no visible por USB. Toda la info va en la OLED.
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <Adafruit_INA219.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ----------------------------- Pinout (3.3 V logic) -------------------------
#define OLED_SDA    6
#define OLED_SCL    7
#define ENC_CLK     4
#define ENC_DT      5
#define ENC_SW      20
#define VSENSE_PIN  3      // ADC1 ch3, 10k/10k divider desde el 5 V del servo
#define BUZZER_PIN  8      // piezo buzzer (GPIO8 = LED onboard, seguro)

#define N_CH        4
const int SERVO_PINS[N_CH] = { 10, 1, 2, 21 };   // CH1..CH4

// ----------------------------- OLED object ----------------------------------
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ----------------------------- Servos (multicanal) --------------------------
Servo servos[N_CH];
int chanAngle[N_CH] = { 90, 90, 90, 90 };  // posicion retenida por canal
int activeChannel = 0;                      // 0..3 (CH1..CH4)

// ----------------------------- INA219 (analyzer) ----------------------------
Adafruit_INA219 ina219;           // direccion por defecto 0x40

// ----------------------------- Settings (persisted) -------------------------
Preferences prefs;
int   minUs       = 500;
int   maxUs       = 2500;
int   centerAngle    = 90;
int   sweepDurationSec = 3;   // sweep one-way duration in seconds (1-9)
int   iThresh     = 800;
float vCal        = 1.0;

const int minAng = 0;
const int maxAng = 180;

// ----------------------------- Encoder (interrupt based) --------------------
volatile int  encoderDelta = 0;
volatile unsigned long lastEncMs = 0;

// Boton: se sondea en loop() (flanco) para distinguir corto/largo.
// El ISR solo atiende el encoder (rotatorio), igual que N2.
unsigned long btnPressMs = 0;
bool btnPrev = HIGH;
bool btnLongFired = false;
bool buttonFlag = false;       // pulso corto (accion normal)
bool longPressFlag = false;    // pulso largo (cambio de canal)
const unsigned long LONG_PRESS_MS = 700;

// ----------------------------- State machine --------------------------------
enum Mode { MENU, MANUAL, SWEEP, CENTER, GRAFICO, SETTINGS };
Mode mode = MENU;
const char* modeNames[] = { "MENU", "MANUAL", "SWEEP", "CENTER", "GRAFICO", "SETTINGS" };

int menuIndex = 0;
const int MENU_COUNT = 6;
const char* menuItems[] = { "MANUAL", "SWEEP", "CENTER", "CANAL", "GRAFICO", "SETTINGS" };

int settingField = 0;
const int SETTINGS_FIELDS = 5;

// ----------------------------- Analyzer sample ------------------------------
float vServo = 0.0;
float iServo = 0.0;
float pServo = 0.0;
bool  alarmOn  = false;

// ----------------------------- Response graph (ring buffer) -----------------
const int GRAPH_N = 64;
int   graphBuf[GRAPH_N];
int   graphHead = 0;   // siguiente escritura
int   graphCount = 0;  // muestras validas (<= GRAPH_N)

void pushGraph(int v) {
  graphBuf[graphHead] = constrain(v, 0, 180);
  graphHead = (graphHead + 1) % GRAPH_N;
  if (graphCount < GRAPH_N) graphCount++;
}

// ----------------------------- BLE ------------------------------------------
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define TELEMETRY_UUID      "beb5483e-36e1-4688-b7f0-0000000000a1"
#define WRITE_UUID          "beb5483e-36e1-4688-b7f0-0000000000a2"

BLEServer*        pServer = nullptr;
BLECharacteristic* pTelemetry = nullptr;
BLECharacteristic* pWrite = nullptr;
bool                 bleConnected = false;
unsigned long        lastNotifyMs = 0;
const unsigned long  NOTIFY_MS = 250;
String               lastTelemetry = "";

// ----------------------------- ISR (encoder) --------------------------------
void IRAM_ATTR encoderISR() {
  unsigned long now = millis();
  if (now - lastEncMs < 5) return;     // 5 ms timestamp debounce
  lastEncMs = now;
  int clk = digitalRead(ENC_CLK);
  if (clk == HIGH) {
    int dt = digitalRead(ENC_DT);
    encoderDelta += (dt == LOW) ? 1 : -1;   // CW vs CCW
  }
}

// ----------------------------- Helpers --------------------------------------
int angleToUs(int a) {
  a = constrain(a, minAng, maxAng);
  return (int)map((long)a, minAng, maxAng, minUs, maxUs);
}

int sweepAngle() {
  unsigned long period = (unsigned long)sweepDurationSec * 1000;
  unsigned long t = millis();
  unsigned long phase = t % (2 * period);
  int a;
  if (phase < period) a = (int)map((long)phase, 0, (long)period, 0, 180);
  else               a = (int)map((long)phase, (long)period, 2 * (long)period, 180, 0);
  return constrain(a, 0, 180);
}

void cycleChannel() {
  activeChannel = (activeChannel + 1) % N_CH;
}

void loadSettings() {
  prefs.begin("servotester", true);
  minUs       = prefs.getUInt("minUs", 500);
  maxUs       = prefs.getUInt("maxUs", 2500);
  centerAngle = prefs.getUInt("center", 90);
  sweepDurationSec = prefs.getUInt("sweepDur", 3);
  iThresh     = prefs.getUInt("iThresh", 800);
  vCal        = prefs.getFloat("vCal", 1.0);
  prefs.end();
  minUs       = constrain(minUs, 400, 1500);
  maxUs       = constrain(maxUs, 1500, 2600);
  centerAngle = constrain(centerAngle, 0, 180);
  sweepDurationSec = constrain(sweepDurationSec, 1, 9);
  iThresh     = constrain(iThresh, 100, 5000);
  if (vCal <= 0.0 || isnan(vCal)) vCal = 1.0;
}

void saveSettings() {
  prefs.begin("servotester", false);
  prefs.putUInt("minUs",   (unsigned int)minUs);
  prefs.putUInt("maxUs",   (unsigned int)maxUs);
  prefs.putUInt("center",  (unsigned int)centerAngle);
  prefs.putUInt("sweepDur",(unsigned int)sweepDurationSec);
  prefs.putUInt("iThresh", (unsigned int)iThresh);
  prefs.putFloat("vCal",   vCal);
  prefs.end();
}

float readVservo() {
  int raw = analogRead(VSENSE_PIN);
  float v = raw * (3.3f / 4095.0f) * 2.0f * vCal;
  return v;
}

void readINA219() {
  float busV = ina219.getBusVoltage_V();
  iServo = ina219.getCurrent_mA();
  pServo = busV * (iServo / 1000.0f);
}

void updateBuzzer() {
  static unsigned long lastBeep = 0;
  static bool beepState = false;
  if (alarmOn) {
    if (millis() - lastBeep > 250) {
      lastBeep = millis();
      beepState = !beepState;
      digitalWrite(BUZZER_PIN, beepState);
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    beepState = false;
  }
}

void updateAlarm() {
  alarmOn = (iServo > (float)iThresh) || (vServo < 4.5f);
}

// ----------------------------- BLE: parse write -----------------------------
// Acepta tokens "clave:valor" o "clave=valor" separados por ',', ';' o espacio.
//   ch:2          -> canal activo 1..4
//   mode:manual   -> manual | sweep | center  (tambien M/S/C)
//   angle:120     -> angulo objetivo 0..180 (en manual)
void applyBleCommand(const String& s) {
  int start = 0;
  while (start < s.length()) {
    int sep = s.indexOf(',', start);
    if (sep < 0) sep = s.length();
    String tok = s.substring(start, sep);
    tok.trim();
    if (tok.length()) {
      int c = tok.indexOf(':');
      if (c < 0) c = tok.indexOf('=');
      if (c > 0) {
        String key = tok.substring(0, c);
        String val = tok.substring(c + 1);
        key.toLowerCase();
        val.toLowerCase();
        key.trim(); val.trim();
        if (key == "ch" || key == "channel") {
          int ch = val.toInt();
          if (ch >= 1 && ch <= N_CH) activeChannel = ch - 1;
        } else if (key == "mode") {
          if (val.startsWith("m")) mode = MANUAL;
          else if (val.startsWith("s")) mode = SWEEP;
          else if (val.startsWith("c")) mode = CENTER;
        } else if (key == "angle" || key == "a") {
          int ang = val.toInt();
          ang = constrain(ang, minAng, maxAng);
          if (mode == MANUAL) chanAngle[activeChannel] = ang;
        }
      }
    }
    start = sep + 1;
  }
}

// ----------------------------- BLE callbacks --------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* p) override { bleConnected = true; }
  void onDisconnect(BLEServer* p) override {
    bleConnected = false;
    BLEDevice::startAdvertising();   // reanudar advertising
  }
};

class WriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    std::string v = pCharacteristic->getValue();
    if (v.length()) applyBleCommand(String(v.c_str()));
  }
};

void initBLE() {
  BLEDevice::init("ServoTester");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // Telemetria: read + notify. El valor se actualiza en loop().
  pTelemetry = pService->createCharacteristic(
      TELEMETRY_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pTelemetry->setValue("boot");

  // Escritura: comandos desde el telefono.
  pWrite = pService->createCharacteristic(
      WRITE_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pWrite->setCallbacks(new WriteCallbacks());

  pService->start();
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->start();
}

// ----------------------------- Input handling -------------------------------
void sampleButton() {
  bool btnNow = digitalRead(ENC_SW);
  unsigned long now = millis();
  if (btnPrev == HIGH && btnNow == LOW) {
    btnPressMs = now;
    btnLongFired = false;
  }
  if (btnPrev == LOW && btnNow == HIGH) {
    // liberacion: corto solo si no fue largo
    if (!btnLongFired && (now - btnPressMs) < LONG_PRESS_MS) {
      buttonFlag = true;
    }
    btnLongFired = false;
  }
  if (btnNow == LOW && !btnLongFired && (now - btnPressMs) > LONG_PRESS_MS) {
    btnLongFired = true;
    longPressFlag = true;   // pulsacion larga: cambio de canal
  }
  btnPrev = btnNow;
}

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
        chanAngle[activeChannel] = constrain(chanAngle[activeChannel] + d, minAng, maxAng);
        break;
      case SETTINGS:
        if (settingField == 0)      minUs       = constrain(minUs + d * 10, 400, 1500);
        else if (settingField == 1) maxUs       = constrain(maxUs + d * 10, 1500, 2600);
        else if (settingField == 2) centerAngle = constrain(centerAngle + d, 0, 180);
        else if (settingField == 3) sweepDurationSec = constrain(sweepDurationSec + d, 1, 9);
        else                        iThresh     = constrain(iThresh + d * 50, 100, 5000);
        if (minUs >= maxUs) minUs = maxUs - 1;
        break;
      case SWEEP:
        sweepDurationSec = constrain(sweepDurationSec + d, 1, 9);
        saveSettings();
        break;
      default:
        break; // CENTER / GRAFICO ignoran el encoder
    }
  }

  // Pulsacion larga: cambiar de canal (en cualquier modo, salvo SETTINGS).
  if (longPressFlag) {
    longPressFlag = false;
    if (mode != SETTINGS) cycleChannel();
  }

  if (buttonFlag) {
    buttonFlag = false;
    switch (mode) {
      case MENU:
        if      (menuIndex == 0) mode = MANUAL;
        else if (menuIndex == 1) mode = SWEEP;
        else if (menuIndex == 2) mode = CENTER;
        else if (menuIndex == 3) { cycleChannel(); /* CANAL: vuelve al MENU */ }
        else if (menuIndex == 4) mode = GRAFICO;
        else if (menuIndex == 5) { mode = SETTINGS; settingField = 0; }
        break;
      case MANUAL:
      case SWEEP:
      case CENTER:
      case GRAFICO:
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
void drawGauge(int x0, int y0, int w, int h, int ang) {
  display.drawRect(x0, y0, w, h, WHITE);
  int fill = map(constrain(ang, 0, 180), 0, 180, 0, w);
  display.fillRect(x0, y0, fill, h, WHITE);
}

// Grafica de linea del buffer circular. Mapea angulo 0..180 al area.
void drawGraph(int x0, int y0, int w, int h) {
  // marco
  display.drawRect(x0, y0, w, h, WHITE);
  if (graphCount < 2) return;
  int step = (w - 2) / (GRAPH_N - 1);
  int prevX = x0 + 1, prevY = 0;
  for (int i = 0; i < GRAPH_N; i++) {
    int idx = (graphHead + i) % GRAPH_N;       // mas antiguo a la izquierda
    int val = graphBuf[idx];
    int x = x0 + 1 + i * step;
    int y = y0 + h - 1 - map(constrain(val, 0, 180), 0, 180, 0, h - 2);
    if (i == 0) { prevX = x; prevY = y; }
    else display.drawLine(prevX, prevY, x, y, WHITE);
    prevX = x; prevY = y;
  }
}

void drawServo(const char* label, int ang, bool withGraph) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print("CH"); display.print(activeChannel + 1);
  display.print(" "); display.print(label);

  if (alarmOn) {
    display.setCursor(82, 0);
    display.print("ALARMA");
  }

  if (withGraph) {
    display.setCursor(0, 10);
    display.print("V:"); display.print(vServo, 2);
    display.print(" I:"); display.print((int)iServo); display.println("mA");

    display.setTextSize(3);
    display.setCursor(60, 20);
    display.print(ang);
    display.setTextSize(1);
    display.setCursor(0, 44);
    display.print(angleToUs(ang)); display.print(" us");
    drawGraph(0, 44, SCREEN_W, 20);
  } else {
    display.setTextSize(3);
    display.setCursor(20, 14);
    display.print(ang);

    display.setTextSize(1);
    display.setCursor(0, 42);
    display.print(angleToUs(ang));
    display.print(" us");

    display.setCursor(0, 42 + 10);
    display.print("V:"); display.print(vServo, 2);
    display.print("V I:"); display.print((int)iServo); display.print("mA");

    drawGauge(4, 56, SCREEN_W - 8, 6, ang);
  }
  display.display();
}

void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("MENU  CH" + String(activeChannel + 1));

  for (int i = 0; i < MENU_COUNT; i++) {
    display.setCursor(0, 12 + i * 9);
    display.print(i == menuIndex ? "> " : "  ");
    display.println(menuItems[i]);
  }
  display.setCursor(0, 56);
  display.print("V:"); display.print(vServo, 2);
  display.print(" I:"); display.print((int)iServo); display.print("mA");
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
  display.print("sweep : "); display.print(sweepDurationSec); display.println("s");
  display.print(settingField == 4 ? " >" : "  ");
  display.print("iThrsh: "); display.print(iThresh); display.println("mA");

  display.setCursor(0, 56);
  display.println("SW=next  turn=edit");
  display.display();
}

void drawGraphScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print("RESPUESTA CH"); display.print(activeChannel + 1);
  if (alarmOn) { display.setCursor(82, 0); display.print("ALARMA"); }
  drawGraph(0, 10, SCREEN_W, SCREEN_H - 10);
  display.display();
}

// ----------------------------- Telemetry string -----------------------------
void buildTelemetry() {
  int a = chanAngle[activeChannel];
  lastTelemetry = "CH:" + String(activeChannel + 1) +
                  ",MODE:" + String(modeNames[mode]) +
                  ",ANG:" + String(a) +
                  ",PULSE:" + String(angleToUs(a)) +
                  ",V:" + String(vServo, 2) +
                  ",I:" + String((int)iServo);
}

// ----------------------------- Setup -----------------------------------------
void setup() {
  Serial.begin(115200);                 // UART0 solo; NO visible por USB

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(VSENSE_PIN, INPUT);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (true) { delay(1000); }
  }
  display.clearDisplay();
  display.display();

  if (!ina219.begin(&Wire)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("INA219 ??");
    display.display();
    while (true) { delay(1000); }
  }
  ina219.setCalibration_32V_2A();

  loadSettings();

  // Multicanal: conectar los 4 servos con el mismo rango de pulso.
  for (int i = 0; i < N_CH; i++) {
    servos[i].attach(SERVO_PINS[i], minUs, maxUs);
    servos[i].writeMicroseconds(angleToUs(chanAngle[i]));
  }

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);

  initBLE();   // servidor BLE "ServoTester"
}

// ----------------------------- Loop ------------------------------------------
void loop() {
  sampleButton();
  handleInput();

  vServo = readVservo();
  readINA219();
  updateAlarm();
  updateBuzzer();

  int driveAngle = chanAngle[activeChannel];
  switch (mode) {
    case MANUAL:   driveAngle = chanAngle[activeChannel];                 break;
    case CENTER:   chanAngle[activeChannel] = centerAngle;
                  driveAngle = centerAngle;                              break;
    case SWEEP:    driveAngle = sweepAngle();
                  chanAngle[activeChannel] = driveAngle;                 break;
    case GRAFICO:  driveAngle = chanAngle[activeChannel];                 break;
    case SETTINGS: driveAngle = chanAngle[activeChannel];                 break;
    case MENU:     driveAngle = -1;                                       break;
  }

  // Solo el canal ACTIVO recibe el comando; los otros mantienen su posicion.
  if (mode != MENU) {
    servos[activeChannel].writeMicroseconds(angleToUs(driveAngle));
  }

  // Muestra del grafico de respuesta (angulo del canal activo).
  pushGraph(chanAngle[activeChannel]);

  switch (mode) {
    case MENU:    drawMenu();                              break;
    case MANUAL:  drawServo("MANUAL", chanAngle[activeChannel], true);  break;
    case SWEEP:   drawServo("SWEEP",  driveAngle, false);  break;
    case CENTER:  drawServo("CENTER", centerAngle, false); break;
    case GRAFICO: drawGraphScreen();                       break;
    case SETTINGS: drawSettings();                         break;
  }

  // BLE: actualizar y notificar telemetria periodicamente.
  buildTelemetry();
  pTelemetry->setValue(lastTelemetry.c_str());
  if (bleConnected && (millis() - lastNotifyMs > NOTIFY_MS)) {
    lastNotifyMs = millis();
    pTelemetry->notify();
  }

  delay(20);   // ~50 Hz
}
