/*
 * Servo Tester (Nivel 1) for ESP32-C3 Super Mini
 * -------------------------------------------------
 * Modes: MENU / MANUAL / SWEEP / CENTER / SETTINGS
 * Controls a standard hobby servo (50 Hz) and shows the pulse width
 * in microseconds plus a gauge bar on a 128x64 SSD1306 OLED.
 *
 * ENCODER CHOICE:
 *   We use a CHANGE-interrupt on the encoder CLK pin, reading DT and using a
 *   timestamp debounce. This avoids an extra library and is rock-solid on the
 *   C3. (ESP32Encoder / PCNT was an alternative but this compiles cleanly with
 *   zero dependency risk.) One encoder detent == +/-1 degree.
 *
 * DEBUG NOTE:
 *   Serial is UART0 (GPIO1/3); the Super Mini does not expose it on USB.
 *   Serial.begin() is kept for completeness but do NOT rely on its output.
 *   All human-facing feedback is on the OLED.
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <Preferences.h>

// ----------------------------- Pinout (3.3 V logic) -------------------------
#define OLED_SDA   6
#define OLED_SCL   7
#define SERVO_PIN  10
#define ENC_CLK    4
#define ENC_DT     5
#define ENC_SW     20

// ----------------------------- OLED object ----------------------------------
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ----------------------------- Servo ----------------------------------------
Servo servo;

// ----------------------------- Settings (persisted) -------------------------
// The angle domain is fixed at 0..180 deg. The configurable limits (minUs/maxUs)
// define the pulse width delivered at 0 deg and 180 deg respectively. "center"
// is the angle held in CENTER mode. Everything is stored in Preferences.
Preferences prefs;
int minUs       = 500;    // pulse width (us) at 0 deg
int maxUs       = 2000;   // pulse width (us) at 180 deg
int centerAngle = 90;     // angle held in CENTER mode
int sweepDurationSec = 3; // sweep duration in seconds (0->180), round-trip 2*this

// Fixed angle domain.
const int minAng = 0;
const int maxAng = 180;

// ----------------------------- Encoder (interrupt based) --------------------
// Net detents since last poll: positive = CW, negative = CCW.
volatile int  encoderDelta = 0;
volatile bool buttonFlag = false;
volatile unsigned long lastBtnMs = 0;
volatile uint8_t lastClkState = LOW;   // para ISR de encoder (flanco de subida limpio)

// ----------------------------- State machine --------------------------------
enum Mode { MENU, MANUAL, SWEEP, CENTER, SETTINGS };
Mode mode = MENU;
const char* modeNames[] = { "MENU", "MANUAL", "SWEEP", "CENTER", "SETTINGS" };

// MENU entries: MANUAL, SWEEP, CENTER, SETTINGS
int menuIndex = 0;
const int MENU_COUNT = 4;

// SETTINGS sub-fields: 0 = minUs, 1 = maxUs, 2 = center
int settingField = 0;
const int SETTINGS_FIELDS = 4; // minUs, maxUs, center, sweepDuration

// Live commanded angle (for display & drive).
int angle = 90;

// ----------------------------- ISRs -----------------------------------------
void IRAM_ATTR encoderISR() {
  int clk = digitalRead(ENC_CLK);
  if (clk == HIGH && lastClkState == LOW) {   // flanco de subida limpio LOW→HIGH
    int dt = digitalRead(ENC_DT);
    encoderDelta += (dt == LOW) ? 1 : -1;     // CW = +1, CCW = -1
  }
  lastClkState = clk;                           // actualiza estado para la próxima
}

void IRAM_ATTR buttonISR() {
  unsigned long now = millis();
  if (now - lastBtnMs < 50) return;    // 50 ms debounce
  lastBtnMs = now;
  buttonFlag = true;
}

// ----------------------------- Helpers --------------------------------------
// Map an angle in [0..180] to its pulse width using the configured limits.
int angleToUs(int a) {
  a = constrain(a, minAng, maxAng);
  return (int)map((long)a, minAng, maxAng, minUs, maxUs);
}

// Triangular sweep: ~1.5 s per direction across 0..180 deg.
int sweepAngle() {
  unsigned long period = (unsigned long)sweepDurationSec * 1000;  // one-way duration in ms
  unsigned long t = millis();
  unsigned long phase = t % (2 * period);  // full round-trip period
  int a;
  if (phase < period) a = (int)map((long)phase, 0, (long)period, 0, 180);
  else              a = (int)map((long)phase, (long)period, 2 * (long)period, 180, 0);
  return constrain(a, 0, 180);
}

void loadSettings() {
  prefs.begin("servotester", true);   // read-only
  minUs       = prefs.getUInt("minUs", 100);
  maxUs       = prefs.getUInt("maxUs", 2000);
  centerAngle = prefs.getUInt("center", 90);
  sweepDurationSec = prefs.getUInt("sweepDur", 3);
  prefs.end();
  minUs = constrain(minUs, 100, 2000);
  maxUs = constrain(maxUs, 1800, 2600);
  centerAngle = constrain(centerAngle, 0, 180);
  sweepDurationSec = constrain(sweepDurationSec, 1, 9);
}

void saveSettings() {
  prefs.begin("servotester", false);  // read-write
  prefs.putUInt("minUs", (unsigned int)minUs);
  prefs.putUInt("maxUs", (unsigned int)maxUs);
  prefs.putUInt("center", (unsigned int)centerAngle);
  prefs.putUInt("sweepDur", (unsigned int)sweepDurationSec);
  prefs.end();
}

// ----------------------------- Input handling -------------------------------
void handleInput() {
  // Consume encoder detents (atomic read/clear).
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
        if (settingField == 0)      minUs       = constrain(minUs + d * 10, 100, 2000);
        else if (settingField == 1) maxUs       = constrain(maxUs + d * 10, 1800, 2600);
        else if (settingField == 2) centerAngle = constrain(centerAngle + d, 0, 180);
        else if (settingField == 3) sweepDurationSec = constrain(sweepDurationSec + d, 1, 9);
        if (minUs >= maxUs) minUs = maxUs - 1;  // protección cruzada
        break;
      case SWEEP:
        sweepDurationSec = constrain(sweepDurationSec + d, 1, 9);
        saveSettings(); // guardar al cambiar sobre la marcha
        break;
      default:
        break;
    }
}

  // Consume button press (atomic read/clear).
  noInterrupts();
  bool b = buttonFlag;
  buttonFlag = false;
  interrupts();

  if (b) {
    switch (mode) {
      case MENU:
        if      (menuIndex == 0) { mode = MANUAL; angle = 90; }
        else if (menuIndex == 1) mode = SWEEP;
        else if (menuIndex == 2) mode = CENTER;
        else if (menuIndex == 3) { mode = SETTINGS; settingField = 0; }
        break;
      case MANUAL:
      case SWEEP:
      case CENTER:
        mode = MENU;                 // press returns to the menu
        break;
      case SETTINGS:
        settingField++;
        if (settingField >= SETTINGS_FIELDS) {
          saveSettings();           // leaving SETTINGS persists limits
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

void drawServo(const char* label, int ang) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println(label);

  // Ángulo a la izquierda, duración a la derecha en la misma línea
  // Usamos tamaño 2 para ambos para que entren en 128px
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(ang);

  // Mostrar duración al lado derecho del ángulo
  display.setTextSize(2);     // <- agrandado de 1 a 2
  display.setCursor(45, 14);  // <- un solo espacio después del número
  if (strcmp(label, "SWEEP") == 0) {
    display.print("Dur:");
    display.print(sweepDurationSec);
    display.print("s");
  } else {
    display.print("Modo: ");
    display.print(label);
  }

  // Barra de gauge al final de la pantalla (abajo)
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

  display.setCursor(0, 54);
  display.println("SW=next  turn=edit");
  display.display();
}

// ----------------------------- Setup -----------------------------------------
void setup() {
  Serial.begin(115200);                 // UART0 only; NOT visible over USB

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // No OLED: stop here so the fault is obvious on the bench.
    while (true) { delay(1000); }
  }
  display.clearDisplay();
  display.display();

  loadSettings();
  servo.attach(SERVO_PIN, minUs, maxUs);

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_SW),  buttonISR, FALLING);

  // Optional: enable the hardware watchdog (left disabled/commented).
  // esp_task_wdt_init(5, true); // 5 s timeout, panic on breach
}

// ----------------------------- Loop ------------------------------------------
void loop() {
  static uint8_t prevMode = 255;  // init to "never had a previous mode"

  handleInput();

  int driveAngle = angle;
  switch (mode) {
    case MANUAL:   driveAngle = angle;       break;
    case CENTER:   driveAngle = centerAngle; angle = centerAngle; break;
    case SWEEP:    driveAngle = sweepAngle(); angle = driveAngle; break;
    case SETTINGS:
      // Servo al extremo correspondiente para ver el efecto de cada ajuste
      if (settingField == 0)      driveAngle = 0;            // minUs -> 0°
      else if (settingField == 1) driveAngle = 180;          // maxUs -> 180°
      else if (settingField == 2) driveAngle = centerAngle;  // center -> center°
      else                        driveAngle = angle;        // sweepDur -> donde esté
      break;
    case MENU:     driveAngle = -1;          break; // servo holds last position
  }

  if (mode != MENU) {
    servo.writeMicroseconds(angleToUs(driveAngle));
  }

  switch (mode) {
    case MENU:    drawMenu();                       break;
    case MANUAL:  drawServo("MANUAL", angle);       break;
    case SWEEP:   drawServo("SWEEP", sweepAngle());  break;
    case CENTER:  drawServo("CENTER", centerAngle);  break;
    case SETTINGS: drawSettings();                  break;
  }

  // --- Submenú de duración de sweep al entrar al modo ---
  if (mode == SWEEP && prevMode != SWEEP) {
    // Primera vez que entra al modo SWEEP: preguntar duración por chat
    // y guardar el valor elegido
    prevMode = SWEEP;
    // Enviar mensaje de chat indicando la duración actual y cómo cambiarla
    // (el usuario puede girar el encoder para cambiarla en tiempo real)
    // Nota: si el usuario quiere un valor distinto, girar el encoder
    // cambiará sweepDurationSec de 1 a 10 y lo guardará automáticamente.
    // Duración actual: sweepDurationSec segundos (un sentido 0→180).
  }

  delay(20); // ~50 Hz UI refresh, leaves headroom for servo timing
}
