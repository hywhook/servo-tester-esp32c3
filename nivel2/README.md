# Servo Tester Nivel 2 — ESP32-C3 Super Mini (con analizador)

Manual completo del probador de servos para la placa **ESP32-C3 Super Mini**,
versión **Nivel 2**. Esta versión extiende el Nivel 1 (menú, manual, barrido,
centro y ajustes con encoder y OLED) añadiendo un **analizador eléctrico**:
medición de la tensión de alimentación del servo (divisor resistivo) y de la
corriente y potencia consumidas (INA219 en serie), con **alarma** de
sobrecorriente y bajo voltaje indicada en pantalla y con un zumbador.

Todo el control se hace con un encoder rotatorio con pulsador (HW-040 / KY-040).
El servo se alimenta **siempre** desde una fuente externa de 5 V con **masa
(GND) común** con la placa. La lógica de la placa y del encoder es de 3.3 V.

> ⚠️ **Nota importante sobre USB-CDC:** este firmware **no** usa la salida por
> USB del C3 como puerto serie. El `Serial` cae a UART0 (GPIO1/3), que la
> Super Mini no conecta al conector USB. Por eso **toda la información se
> muestra en la pantalla OLED**, no en el monitor serie.

---

## 1. Introducción

El Nivel 2 es un **probador y analizador de servos de hobby**. Además de mover
el servo (como el Nivel 1), ahora puedes **ver lo que le pasa eléctamente**:

- **Tensión de alimentación del servo** (`V`), medida con un divisor 10k/10k
  sobre el rail de 5 V y leída por el ADC del C3 (GPIO3).
- **Corriente** (`I`, en mA) y **potencia** (`W`) del servo, medidas con un
  INA219 conectado **en serie** con la línea de 5 V del servo.
- **Alarma**: si la corriente supera un umbral configurable (800 mA por defecto)
  o la tensión baja de 4.5 V, la OLED muestra **"ALARMA"** y suena el zumbador.

Esto permite **detectar un servo atascado o muerto** (ver sección 4.3).

---

## 2. Lista de materiales (BOM)

| Cantidad | Componente | Notas |
|----------|------------|-------|
| 1 | ESP32-C3 Super Mini | Placa de desarrollo |
| 1 | Pantalla OLED SSD1306 128×64 I²C | Dirección 0x3C |
| 1 | Servo de hobby (estándar, 50 Hz) | p. ej. SG90, MG996R |
| 1 | Encoder rotatorio HW-040 (KY-040) | CLK/DT/SW |
| 1 | Sensor de corriente INA219 | I²C, dirección 0x40, en serie con el 5 V |
| 2 | Resistencia 10 kΩ | Para el divisor de tensión (10k/10k) |
| 1 | Zumbador piezoeléctrico (activo, activo a nivel alto) | GPIO21 |
| 1 | Fuente de 5 V externa para el servo | ≥1 A recomendado (el INA219 la mide) |
| – | Cables Dupont, protoboard | |
| – | Cable USB-C (datos) | Para programar la placa |

---

## 3. Diagrama de conexión

### 3.1 Tabla de pines

| Señal | ESP32-C3 GPIO | Destino |
|-------|---------------|---------|
| OLED SDA | GPIO6 | SDA de la OLED (y del INA219) |
| OLED SCL | GPIO7 | SCL de la OLED (y del INA219) |
| Servo señal | GPIO10 | Cable de señal del servo (naranja/amarillo) |
| Encoder CLK | GPIO4 | CLK del HW-040 |
| Encoder DT | GPIO5 | DT del HW-040 |
| Encoder SW | GPIO20 | Pulsador (SW) del HW-040 |
| V-sense | GPIO3 | Punto medio del divisor 10k/10k (desde el 5 V del servo) |
| Buzzer | GPIO21 | Zumbador piezo (activo a nivel alto) |
| GND | GND | Masa común (OLED, encoder, INA219, servo, fuente 5 V) |
| 3V3 | 3V3 | VCC de la OLED y del encoder |
| 5V ext. | – | Entra al **VIN+** del INA219 (ver 3.2) |

> **Sobre GPIO3 (V-sense):** el C3 Super Mini lleva el CH340 de USB-UART con
> pull-up débil a 3.3 V en GPIO1/3. Con un divisor 10k/10k la resistencia
> Thévenin es 5 kΩ, así que un pull-up de decenas de kΩ solo añade un pequeño
> offset casi constante, que se elimina con la constante de calibración `vCal`
> (sección 7). Si quieres máxima precisión, puedes mover el divisor a GPIO1
> (misma situación) o cortar el pull-up del CH340; funcionalmente cualquiera
> sirve.

### 3.2 Diagrama ASCII

```
                 ESP32-C3 Super Mini
             +--------------------------+
    USB-C -->|                          |
             |  3V3 ---+--------+       |
             |         |        |       |
             |  GPIO6 -+-- SDA  |       |
             |  GPIO7 -+-- SCL  |  OLED SSD1306 (I2C, 0x3C)
             |  GND  --+-- GND  |       |
             |         |        |       |
             |  GPIO4 -+-- CLK  |       |
             |  GPIO5 -+-- DT   |  HW-040 encoder
             |  GPIO20-+-- SW   |       |
             |  GND  --+-- GND  |       |
             |         |        |       |
             |  GPIO10-+-- SIGNAL SERVO |  (naranja/amarillo)
             |  GPIO21-+-- BUZZER        |  (piezo, activo a nivel alto)
             |  GND  --+-- GND  |       |
             |         |        |       |
             |  GPIO3 -+--+     |  <-- punto medio del divisor (V-sense)
             |         |  |     |       |
             +---------|--|-----+       |
                       |  |             |     Divisor 10k / 10k
                  (10k)|  |(10k)         |
                       |  |             |
                       |  +-------------+---- al rail de 5 V (después del INA219)
                       |                |
                       +----------------+---- a GND (masa común)

   INA219 EN SERIE con el 5 V del servo (mide V e I):
        +-------------------+          +-------------------+
        |  Fuente 5V ext.   |          |  Servo            |
        |   +5V --> VIN+    |          |   V+  <-- VIN-    |
        |          INA219   |          |   GND <-- GND     |
        |   GND  --> GND    |----------|                   |
        |   SDA --> GPIO6   |          +-------------------+
        |   SCL --> GPIO7   |
        +-------------------+
   Nota: VIN+ se alimenta de la fuente; VIN- alimenta al servo. La masa
   (GND) de la fuente, del INA219 y de la placa deben unirse.
```

Resumen de masa común: el GND de la placa, OLED, encoder, INA219, servo y
fuente de 5 V **deben estar unidos**. El INA219 va **en serie** con el positivo
del servo, no en paralelo.

---

## 4. Explicación de los modos

Al arrancar se muestra el **MENU**. Girar el encoder mueve el cursor; pulsar el
botón (SW) entra en la opción seleccionada. En cualquier submodo, **pulsar SW
vuelve al MENU** (salvo en SETTINGS).

| Modo | Qué hace |
|------|----------|
| **MANUAL** | Cada detent ajusta el ángulo ±1° dentro de [0,180], moviendo el servo en vivo. |
| **SWEEP** | Oscila de 0° a 180° y vuelta, ~1.5 s por dirección. Muestra el ángulo en vivo. |
| **CENTER** | Mantiene el servo en el ángulo central (por defecto 90° = 1500 µs). |
| **SETTINGS** | Configura `minUs`, `maxUs`, `center` y `iThresh`; se guarda con Preferences. |

En **todos** los modos la pantalla muestra además la **tensión de alimentación**
(del divisor) y la **corriente en mA** (del INA219), y marca **ALARMA** si
procede.

### 4.1 SETTINGS (detalle)
- Girar el encoder **edita el campo seleccionado**:
  - `minUs` (400–1500 µs) en pasos de 10.
  - `maxUs` (1500–2600 µs) en pasos de 10.
  - `center` (0–180°) en pasos de 1.
  - `iThrsh` (100–5000 mA) en pasos de 50: umbral de sobrecorriente de la alarma.
- Pulsar **SW avanza al siguiente campo**. Tras el último, otra pulsación
  **guarda** y vuelve al MENU.

### 4.2 La pantalla (modos MANUAL/SWEEP/CENTER)
Línea 1: modo (y "ALARMA" arriba a la derecha si hay alarma). Número grande:
ángulo. Debajo: ancho de pulso en µs; luego `V: x.xxV I: nnnmA`; y una barra
gauge del ángulo. En el MENU también aparece la línea `V:/I:` abajo.

### 4.3 El analizador: qué te dicen V e I y cómo detectar un servo atascado/muerto
- **En reposo (sin mover):** un servo sano consume poco (decenas de mA) y la
  tensión del rail se mantiene cerca de 5 V.
- **Al mover (MANUAL/SWEEP):** la corriente sube (p. ej. 100–400 mA según par).
  Si la corriente se dispara hasta el umbral (≥800 mA) y se queda alta aunque el
  servo no debería esforzarse, el servo está **atascado** (tope mecánico forzado,
  engranaje roto o carga excesiva). La alarma suena.
- **Tensión baja (<4.5 V):** la fuente externa no da suficiente corriente o hay
  caída en el cable/INA219. Suggestivo de fuente débil o cortocircuito parcial.
- **Servo muerto:** corriente ~0 mA y no se mueve aunque mandas pulsos, o bien
  corriente muy alta y tensión colapsada (corto interno). El analizador lo
  evidencia al instante antes de quemar la placa o la fuente.
- **Potencia (W):** se deriva como `Vbus × I`. Útil para estimar el consumo
  pico y elegir la fuente.

---

## 5. Recorrido del código por secciones (`src/main.cpp`)

1. **Includes y pines** — Wire, Adafruit SSD1306/GFX, ESP32Servo, Preferences y
   `Adafruit_INA219`. Se añaden `VSENSE_PIN` (GPIO3) y `BUZZER_PIN` (GPIO21).
2. **Objetos globales** — `display` (OLED), `servo` y `ina219` (I²C 0x40).
3. **Ajustes persistidos** — `minUs`, `maxUs`, `centerAngle`, `iThresh` y `vCal`
   se cargan/guardan con Preferences (namespace `servotester`). Dominio fijo 0..180°.
4. **Encoder por interrupciones** — Igual que L1: CHANGE en CLK, debounce 5 ms,
   un detent = ±1 (o ±10 µs / ±50 mA en SETTINGS).
5. **ISRs** — `encoderISR()` acumula `encoderDelta`; `buttonISR()` marca
   `buttonFlag`. Ambas `IRAM_ATTR` y variables `volatile`.
6. **Helpers del analizador** — `readVservo()` lee el ADC (12 bits, attenuación
   11 dB) y aplica `vCal`; `readINA219()` lee bus voltage y corriente del INA219
   y calcula la potencia; `updateAlarm()` y `updateBuzzer()` gestionan la alarma.
7. **Máquina de estados** — `handleInput()` consume encoder/botón; `drawMenu`,
   `drawServo` (ahora con fila V/I y marca ALARMA), `drawSettings`, `drawGauge`.
8. **`setup()`** — Arranca I²C, OLED, configura el ADC, inicializa el INA219
   (`setCalibration_32V_2A()`) y se detiene si falta; conecta servo, buzzer y
   encoder; carga ajustes.
9. **`loop()`** — Cada frame muestrea V/I, evalúa la alarma, suena el zumbador,
   aplica el ángulo según el modo y dibuja.

---

## 6. Cómo calibrar los límites (igual que Nivel 1)

1. Entra en **SETTINGS**.
2. Ajusta `minUs` al tope donde el servo **empieza** a moverse (típico 500 µs).
3. Ajusta `maxUs` al otro tope (típico 2500 µs).
4. Fija `center` (normalmente 90°) y pulsa SW para **guardar**.

Estos límites definen el pulso entregado en 0° y 180°, adaptándose a servos no
estándar. El nuevo campo `iThrsh` fija el umbral de sobrecorriente de la alarma.

---

## 7. Cómo calibrar el divisor de tensión (`vCal`)

El divisor 10k/10k entrega la mitad de la tensión del servo al ADC. El código
calcula `Vservo = adc × (3.3/4095) × 2.0 × vCal`, con `vCal = 1.0` por defecto.
Para calibrar de un punto (recomendado, corrige el pull-up de la placa y las
tolerancias de las resistencias):

1. Con el servo alimentado y el firmware leyendo, mide con un polímetro la
   **tensión real** del rail de 5 V del servo (en el punto donde va el divisor).
2. Lee en la OLED la `V:` que muestra el firmware (sin calibrar, `vCal=1`).
3. Calcula `vCal = V_real_medida_polimetro / V_mostrada_OLED`.
4. Para fijar `vCal` de forma permanente, edítalo en `loadSettings()`/guárdalo
   vía Preferences (clave `vCal`) o recompila con el valor. Con un punto de
   calibración a ~5 V el error en todo el rango es despreciable.

(Opcional) Si el pull-up del CH340 molesta, mueve el divisor a GPIO1 o corta el
pull-up; el procedimiento de calibración es idéntico.

---

## 8. Solución de problemas

| Síntoma | Causa probable | Solución |
|---------|----------------|----------|
| Pantalla en blanco | OLED mal cableada o dir. distinta | Revisa SDA/SCL/GND; confirma 0x3C. |
| "INA219 ??" al arrancar | INA219 ausente / mal cableado | Revisa I²C (GPIO6/7) y que VIN± tengan 5 V/GND. |
| El servo no se mueve | Sin masa común o fuente 5 V ausente | Une GND de todo; alimenta el servo por separado. |
| OLED congelada en "INA219 ??" | INA219 no responde en 0x40 | Verifica dirección (0x40) y pull-ups de SDA/SCL. |
| V siempre alta aunque servo apagado | Pull-up del CH340 en GPIO3 | Calibra con `vCal` (§7) o usa GPIO1. |
| Servo tiembla/garra | Fuente 5 V insuficiente | Usa fuente con más corriente (≥1 A). |
| Alarma suena sin motivo | Umbral `iThrsh` muy bajo o ruido | Sube `iThrsh` en SETTINGS. |
| Encoder salta pasos | Rebote mecánico | Ya hay debounce de 5 ms; reduce velocidad de giro. |
| `Serial` no muestra nada | Es UART0, no USB | Esperado; mira la OLED (ver nota USB-CDC). |
| Error de compilación `Serial` | Flag USB-CDC mal puesto | No se usa `ARDUINO_USB_CDC_ON_BOOT` a propósito. |

---

## 9. La trampa de USB-CDC (importante)

El core de Arduino para ESP32 usado aquí (framework-arduinoespressif32 7.0.1)
**no incluye `usb_serial.h` para el C3**. Si se añade
`build_flags = -D ARDUINO_USB_CDC_ON_BOOT=1`, la compilación falla con
`"'Serial' was not declared"`. Por eso el `platformio.ini` **no** pone ese flag.
Sin él, `Serial` es UART0 (GPIO1/3), que la Super Mini no saca por USB. La
carga del firmware se hace igualmente por USB nativo con `esptool`
(`/dev/ttyACM0`), sin necesidad de pulsar BOOT.

---

## 10. Compilar y cargar

Desde el directorio del proyecto:

```bash
# Compilar (pio descarga las librerías automáticamente)
pio run

# Cargar (esptool por USB nativo, /dev/ttyACM0; sin botón BOOT)
pio run -t upload

# Monitor serie (solo UART0, no visible por USB)
pio device monitor
```

`pio run` descarga las librerías (Adafruit SSD1306, Adafruit GFX, ESP32Servo,
Adafruit INA219) automáticamente si hay red. No es necesario subir para compilar.
