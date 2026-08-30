# Servo Tester Nivel 1 — ESP32-C3 Super Mini

Manual completo del probador de servos para la placa **ESP32-C3 Super Mini**.
Este documento explica el montaje, el funcionamiento de cada modo, la
calibración, la resolución de problemas y cómo compilar/cargar el firmware.

---

## 1. Introducción

Este proyecto convierte un ESP32-C3 Super Mini en un **probador de servos de
hobby** con pantalla OLED. Permite mover un servo manualmente, barrerlo de
extremo a extremo, centrarlo y configurar los límites de pulsos (µs) que se
guardan en memoria no volátil (Preferencias). Todo el control se hace con un
encoder rotatorio con pulsador (HW-040 / KY-040).

El servo se alimenta **siempre** desde una fuente externa de 5 V con **masa
(GND) común** con la placa. La lógica de la placa y del encoder es de 3.3 V.

> ⚠️ **Nota importante sobre USB-CDC:** este firmware **no** usa la salida por
> USB del C3 como puerto serie. El `Serial` cae a UART0 (GPIO1/3), que la
> Super Mini no conecta al conector USB. Por eso **toda la información se
> muestra en la pantalla OLED**, no en el monitor serie.

---

## 2. Lista de materiales (BOM)

| Cantidad | Componente | Notas |
|----------|------------|-------|
| 1 | ESP32-C3 Super Mini | Placa de desarrollo |
| 1 | Pantalla OLED SSD1306 128×64 I²C | Dirección 0x3C |
| 1 | Servo de hobby (estándar, 50 Hz) | p. ej. SG90, MG996R |
| 1 | Encoder rotatorio HW-040 (KY-040) | CLK/DT/SW |
| 1 | Fuente de 5 V externa para el servo | ≥1 A recomendado |
| – | Cables Dupont, protoboard | |
| – | Cable USB-C (datos) | Para programar la placa |

---

## 3. Diagrama de conexión

### 3.1 Tabla de pines

| Señal | ESP32-C3 GPIO | Destino |
|-------|---------------|---------|
| OLED SDA | GPIO6 | SDA de la OLED |
| OLED SCL | GPIO7 | SCL de la OLED |
| Servo señal | GPIO10 | Cable de señal del servo (naranja/amarillo) |
| Encoder CLK | GPIO4 | CLK del HW-040 |
| Encoder DT | GPIO5 | DT del HW-040 |
| Encoder SW | GPIO20 | Pulsador (SW) del HW-040 |
| GND | GND | Masa común (OLED, encoder, servo, fuente 5 V) |
| 3V3 | 3V3 | VCC de la OLED y del encoder |
| 5V ext. | – | VCC (rojo) del servo **desde fuente externa** |

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
            |  GPIO10-+-- SIGNALSERVO   |  (naranja/amarillo)
            |  GND  --+-- GND  |       |
            +--------------------------+      +-------------------+
                                            | Fuente 5V externa |
                                            |  +5V --> VCC servo |
                                            |  GND --> GND servo |
                                            +-------------------+
```

Resumen de masa común: el GND de la placa, el de la OLED, el del encoder, el
del servo y el de la fuente de 5 V **deben estar unidos**.

---

## 4. Explicación de los modos

Al arrancar se muestra el **MENU**. Girar el encoder mueve el cursor; pulsar el
botón (SW) entra en la opción seleccionada. En cualquier submodo, **pulsar SW
vuelve al MENU** (salvo en SETTINGS, ver abajo).

| Modo | Qué hace |
|------|----------|
| **MANUAL** | Cada detent del encoder ajusta el ángulo ±1° dentro de [0,180], moviendo el servo en vivo. |
| **SWEEP** | Oscila automáticamente de 0° a 180° y vuelta, ~1.5 s por dirección. Muestra el ángulo en vivo. |
| **CENTER** | Mantiene el servo en el ángulo central (por defecto 90° = 1500 µs). |
| **SETTINGS** | Configura `minUs`, `maxUs` y `center`; se guarda con Preferences (namespace `servotester`). |

### SETTINGS (detalle)
- Girar el encoder **edita el campo seleccionado**:
  - `minUs` (400–1500 µs) en pasos de 10.
  - `maxUs` (1500–2600 µs) en pasos de 10.
  - `center` (0–180°) en pasos de 1.
- Pulsar **SW avanza al siguiente campo**. Tras el último campo, otra pulsación
  **guarda** los valores y vuelve al MENU.

La pantalla OLED muestra siempre: línea 1 = modo, número grande del ángulo
(textSize 3), debajo el ancho de pulso en microsegundos y una barra gauge.

---

## 5. Recorrido del código por secciones (`src/main.cpp`)

1. **Includes y pines** — Se incluyen Wire, Adafruit SSD1306/GFX, ESP32Servo y
   Preferences. Se definen los pines según el apartado 3.1.
2. **Objetos globales** — `display` (OLED) y `servo`.
3. **Ajustes persistidos** — `minUs`, `maxUs`, `centerAngle` se cargan desde
   Preferences al arrancar (`loadSettings()`) y se guardan en SETTINGS
   (`saveSettings()`). El dominio de ángulo queda fijo en 0..180°.
4. **Encoder por interrupciones** — Se eligió una **interrupción CHANGE en CLK**
   que lee DT y usa un *debounce* por marca de tiempo (5 ms). Un detent = ±1°.
   Se descarta ESP32Encoder/PCNT para evitar dependencias y compilar limpio.
5. **ISRs** — `encoderISR()` acumula `encoderDelta`; `buttonISR()` marca
   `buttonFlag`. Ambas son `IRAM_ATTR` y usan variables `volatile`.
6. **Helpers** — `angleToUs()` mapea ángulo→µs con los límites configurados;
   `sweepAngle()` genera la rampa triangular de SWEEP.
7. **Máquina de estados** — `handleInput()` consume encoder y botón y cambia de
   modo; `drawMenu/drawServo/drawSettings/drawGauge` renderizan la OLED.
8. **`setup()`** — Arranca I²C, OLED, carga ajustes, conecta el servo y las
   interrupciones. (Hay un watchdog opcional comentado.)
9. **`loop()`** — Lee entradas, calcula el ángulo a aplicar según el modo,
   escribe el ancho de pulso con `servo.writeMicroseconds()` y dibuja.

---

## 6. Cómo calibrar los límites

1. Entra en **SETTINGS** desde el MENU.
2. Selecciona `minUs` y gira hasta el valor donde el servo **empieza** a moverse
   desde su tope mecánico (típico 500 µs). No fuerces más allá del tope.
3. Pulsa SW, selecciona `maxUs` y ajusta al otro tope (típico 2500 µs).
4. Pulsa SW, fija `center` (normalmente 90°) y pulsa SW de nuevo para **guardar**.
5. Los valores persisten tras reiniciar (Preferences, claves `minUs`/`maxUs`/`center`).

Estos límites definen el ancho de pulso entregado en 0° y 180°, respectivamente,
por lo que el mapeo de ángulo→µs se adapta a servos no estándar.

---

## 7. Solución de problemas

| Síntoma | Causa probable | Solución |
|---------|----------------|----------|
| Pantalla en blanco | OLED mal cableada o dirección distinta | Revisa SDA/SCL y GND; confirma 0x3C. |
| El servo no se mueve | Sin masa común o fuente 5 V ausente | Une GND de todo; alimenta el servo por separado. |
| Servo tiembla/garra | Fuente 5 V insuficiente | Usa fuente con más corriente (≥1 A). |
| Encoder salta pasos | Rebote mecánico | Ya hay debounce de 5 ms; reduce velocidad de giro. |
| `Serial` no muestra nada | Es UART0, no USB | Esperado; mira la OLED (ver nota USB-CDC). |
| Error de compilación `Serial` | Flag `ARDUINO_USB_CDC_ON_BOOT` | No se usa en este proyecto a propósito. |

---

## 8. La trampa de USB-CDC (importante)

El core de Arduino para ESP32 usado aquí (framework-arduinoespressif32 7.0.1)
**no incluye `usb_serial.h` para el C3**. Si se añade
`build_flags = -D ARDUINO_USB_CDC_ON_BOOT=1`, la compilación falla con
`"'Serial' was not declared"`. Por eso el `platformio.ini` **no** pone ese flag.
Sin él, `Serial` es UART0 (GPIO1/3), que la Super Mini no saca por USB. La
carga del firmware se hace igualmente por USB nativo con `esptool`
(`/dev/ttyACM0`), sin necesidad de pulsar BOOT.

---

## 9. Compilar y cargar

Desde el directorio del proyecto:

```bash
# Compilar
pio run

# Cargar (esptool por USB nativo, /dev/ttyACM0; sin botón BOOT)
pio run -t upload

# Además, abrir el monitor serie (solo UART0, no visible por USB)
pio device monitor
```

`pio run` descarga las librerías automáticamente (red disponible). No se
requiere subir el firmware para compilar; este manual asume solo build+upload
por parte del operador.
