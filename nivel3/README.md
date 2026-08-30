# Servo Tester Nivel 3 — ESP32-C3 DevKitM-1 (multicanal + BLE + gráfico)

Manual completo del probador de servos para la placa **ESP32-C3 DevKitM-1**,
versión **Nivel 3**. Esta versión extiende el Nivel 2 (menú, manual, barrido,
centro, ajustes, analizador V/I con INA219, alarma y Preferences) con tres
funciones **premium**:

1. **Multicanal** — 4 servos (CH1…CH4). Un canal **activo** recibe los comandos
   MANUAL/SWEEP/CENTER; los otros 3 **mantienen su última posición**.
2. **BLE** — servidor Bluetooth Low Energy llamado **"ServoTester"** (NimBLE,
   integrado en el core de Arduino-ESP32). Expone telemetría (read+notify) y una
   característica de escritura para controlar el equipo desde el móvil.
3. **Gráfico de respuesta** — buffer circular de las últimas ~64 muestras del
   ángulo del canal activo, dibujado como gráfica de línea en la OLED para
   visualizar overshoot/rebote/ jitter.

Todo el control local se hace con el encoder rotatorio (HW-040). Los servos se
alimentan **siempre** desde una fuente externa de 5 V con **masa (GND) común**
con la placa. La lógica de la placa y del encoder es de 3.3 V.

> ⚠️ **Nota importante sobre USB-CDC:** este firmware **no** usa la salida por
> USB del C3 como puerto serie. El `Serial` es UART0 (GPIO1/3); el DevKitM-1 no
> saca UART0 por el conector USB-C (ese puerto USB es solo para esptool). Por eso
> **toda la información se muestra en la pantalla OLED**, no en el monitor serie.
> La carga del firmware se hace por USB nativo con esptool, sin botón BOOT.

---

## 1. Introducción

El Nivel 3 es un **probador, analizador y controlador multicanales de servos de
hobby**. Además de todo lo del Nivel 2 (mover el servo, ver tensión/corriente,
alarma), ahora puedes:

- Manejar **hasta 4 servos** distintos y elegir cuál está activo (menú o
  pulsación larga del encoder).
- **Ver y comandar el equipo por BLE** desde un móvil (nRF Connect, tu propia
  app, etc.): leer telemetría en vivo (canal, modo, ángulo, pulso µs, V, I) y
  escribir el canal activo, el modo y el ángulo objetivo.
- **Visualizar la respuesta mecánica** del servo con una gráfica de línea de las
  últimas 64 muestras del ángulo del canal activo (muy útil para ver rebote,
  overshoot o jitter al cambiar de posición).

---

## 2. Lista de materiales (BOM)

| Cantidad | Componente | Notas |
|----------|------------|-------|
| 1 | ESP32-C3 DevKitM-1 | Placa de desarrollo (C3) |
| 1 | Pantalla OLED SSD1306 128×64 I²C | Dirección 0x3C |
| 4 | Servos de hobby (estándar, 50 Hz) | p. ej. SG90, MG996R (CH1…CH4) |
| 1 | Encoder rotatorio HW-040 (KY-040) | CLK/DT/SW |
| 1 | Sensor de corriente INA219 | I²C, dirección 0x40, en serie con el 5 V |
| 2 | Resistencia 10 kΩ | Para el divisor de tensión (10k/10k) |
| 1 | Zumbador piezoeléctrico (activo a nivel alto) | GPIO8 (también el LED onboard) |
| 1 | Fuente de 5 V externa para los servos | ≥2 A recomendado (el INA219 la mide) |
| – | Cables Dupont, protoboard | |
| – | Cable USB-C (datos) | Para programar la placa |

---

## 3. Diagrama de conexión

### 3.1 Tabla de pines (NUEVO en Nivel 3)

| Señal | ESP32-C3 GPIO | Destino |
|-------|---------------|---------|
| OLED SDA | GPIO6 | SDA de la OLED (y del INA219) |
| OLED SCL | GPIO7 | SCL de la OLED (y del INA219) |
| Encoder CLK | GPIO4 | CLK del HW-040 |
| Encoder DT | GPIO5 | DT del HW-040 |
| Encoder SW | GPIO20 | Pulsador (SW) del HW-040 |
| V-sense | GPIO3 | Punto medio del divisor 10k/10k (desde el 5 V) |
| Buzzer | GPIO8 | Zumbador piezo (activo a nivel alto) — también LED onboard |
| Servo CH1 señal | GPIO10 | Señal servo 1 (naranja/amarillo) |
| Servo CH2 señal | GPIO1 | Señal servo 2 — es el TX de UART0 (no usado por USB) |
| Servo CH3 señal | GPIO2 | Señal servo 3 — pin strapping (ver nota) |
| Servo CH4 señal | GPIO21 | Señal servo 4 |
| GND | GND | Masa común (OLED, encoder, INA219, servos, fuente 5 V) |
| 3V3 | 3V3 | VCC de la OLED y del encoder |
| 5V ext. | – | Entra al **VIN+** del INA219 (ver 3.2) |

> **Cambios respecto al Nivel 2 (documentados):**
> - El **zumbador pasa de GPIO21 a GPIO8**. GPIO8 es el LED onboard del
>   DevKitM-1; usarlo como salida de buzzer es totalmente seguro (solo pierdes
>   ese LED).
> - Aparecen **3 canales de servo nuevos**: CH2=GPIO1, CH3=GPIO2, CH4=GPIO21.
>   El antiguo servo único de L2 (GPIO10) sigue siendo **CH1**.
> - **GPIO1** es el pin TX de UART0. Como no usamos USB-CDC, UART0 no va por USB,
>   así que GPIO1 queda libre como **salida de servo** sin conflicto.
> - **GPIO2** es un *strapping pin* (configura el voltaje de flash en el arranque).
>   Es seguro usarlo como **salida de servo tras el boot**; no lo fuerces a un
>   nivel concreto antes de arrancar (el firmware no lo toca hasta `setup()`).
> - **GPIO21** (CH4) deja de ser buzzer y pasa a servo.

> **Sobre GPIO3 (V-sense):** el DevKitM-1 lleva el chip USB-UART con pull-up
> débil a 3.3 V en GPIO1/3. Con un divisor 10k/10k la resistencia Thévenin es
> 5 kΩ, así que un pull-up de decenas de kΩ solo añade un pequeño offset casi
> constante, que se elimina con la constante de calibración `vCal` (sección 8).

### 3.2 Diagrama ASCII

```
                  ESP32-C3 DevKitM-1
              +--------------------------+
     USB-C -->|  (solo esptool: carga)  |
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
              |  GPIO10-+-- S1   |  Servo CH1 (naranja/amarillo)
              |  GPIO1 -+-- S2   |  Servo CH2  (antes TX UART0)
              |  GPIO2 -+-- S3   |  Servo CH3  (strapping, ok como salida)
              |  GPIO21-+-- S4   |  Servo CH4
              |  GPIO8 -+-- BUZZER|  (piezo, activo a nivel alto)
              |  GPIO3 -+--+      |  <-- punto medio del divisor (V-sense)
              |         |  |      |       |
              +---------|--|------+       |
                        |  |             |     Divisor 10k / 10k
                   (10k)|  |(10k)         |
                        |  |             |
                        |  +-------------+---- al rail de 5 V (después del INA219)
                        |                |
                        +----------------+---- a GND (masa común)

    INA219 EN SERIE con el 5 V de los servos (mide V e I):
         +-------------------+          +-------------------+
         |  Fuente 5V ext.   |          |  Servos (CH1..CH4)|
         |   +5V --> VIN+    |          |   V+  <-- VIN-    |
         |          INA219   |          |   GND <-- GND     |
         |   GND  --> GND    |----------|                   |
         |   SDA --> GPIO6   |          +-------------------+
         |   SCL --> GPIO7   |
         +-------------------+
    Nota: VIN+ se alimenta de la fuente; VIN- alimenta a los servos. La masa
    (GND) de la fuente, del INA219 y de la placa deben unirse.
```

Resumen de masa común: el GND de la placa, OLED, encoder, INA219, los 4 servos
y la fuente de 5 V **deben estar unidos**. El INA219 va **en serie** con el
positivo de los servos, no en paralelo.

---

## 4. Explicación de los modos y del multicanal

Al arrancar se muestra el **MENU**. Girar el encoder mueve el cursor; pulsar el
botón (SW) entra en la opción seleccionada. En cualquier submodo (salvo
SETTINGS), **pulsar SW corto vuelve al MENU**.

| Modo (menú) | Qué hace |
|-------------|----------|
| **MANUAL** | El canal activo ajusta su ángulo ±1°/detent dentro de [0,180]; se mueve en vivo. Muestra el **gráfico de respuesta** abajo. |
| **SWEEP** | El canal activo oscila 0°↔180°, ~1.5 s por dirección. |
| **CENTER** | El canal activo se mantiene en el ángulo central (por defecto 90° = 1500 µs). |
| **CANAL** | Cambia el **canal activo** (CH1→CH2→CH3→CH4→CH1) y vuelve al MENU. |
| **GRAFICO** | Pantalla completa del gráfico de respuesta del canal activo. |
| **SETTINGS** | Configura `minUs`, `maxUs`, `center` y `iThresh`; se guarda con Preferences. |

### 4.1 Multicanal
- Hay **4 servos** conectados y **uno activo** a la vez (`CH` mostrado en la OLED
  y en la telemetría BLE).
- Los comandos MANUAL/SWEEP/CENTER solo afectan al **canal activo**. Los otros
  3 **mantienen su última posición** (el driver ESP32Servo sigue generando el
  pulso que se les envió; no se reescriben).
- Cambias de canal activo con la opción **CANAL** del menú, o con una
  **pulsación LARGA** (≈0.7 s) del encoder en cualquier modo (excepto SETTINGS).
- Cada canal recuerda su ángulo de forma independiente; al volver a un canal,
  retoma su última posición.

### 4.2 La pantalla
- **MENU:** lista de opciones y, arriba a la derecha, el **canal activo**
  (`CH1`…`CH4`); abajo la línea `V:/I:`.
- **MANUAL:** cabecera `CHx MANUAL`, línea `V: x.xx I: nnnmA`, ángulo grande,
  pulso µs y **gráfico de respuesta** (tira inferior).
- **SWEEP / CENTER:** cabecera `CHx …`, ángulo grande, pulso µs, `V:/I:` y gauge.
- **GRAFICO:** cabecera `RESPUESTA CHx` y gráfica a pantalla completa.
- **SETTINGS:** campos editables.
- Si hay alarma, la OLED marca **"ALARMA"** arriba a la derecha.

---

## 5. La sección BLE (NimBLE integrado)

El core de Arduino-ESP32 **incluye BLE (NimBLE) de fábrica**; no se añade
ninguna librería. El dispositivo se anuncia como **"ServoTester"**.

### 5.1 UUIDs del servicio y las características
| Elemento | UUID |
|----------|------|
| Servicio | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Telemetría (read + notify) | `beb5483e-36e1-4688-b7f0-0000000000a1` |
| Escritura (comandos) | `beb5483e-36e1-4688-b7f0-0000000000a2` |

### 5.2 Característica de telemetría (READ + NOTIFY)
Cada ~250 ms (y al conectar) se envía una cadena CSV, p. ej.:
```
CH:1,MODE:MANUAL,ANG:90,PULSE:1500,V:5.01,I:120
```
| Campo | Significado |
|-------|-------------|
| `CH` | Canal activo (1…4) |
| `MODE` | MENU / MANUAL / SWEEP / CENTER / GRAFICO / SETTINGS |
| `ANG` | Ángulo del canal activo (0…180) |
| `PULSE` | Ancho de pulso en µs (`angleToUs(ANG)`) |
| `V` | Tensión de alimentación del servo (divisor), en V |
| `I` | Corriente del bus (INA219), en mA |

Suscríbete (notify) para recibirla en vivo; también puedes leerla bajo demanda.

### 5.3 Característica de escritura (comandos desde el móvil)
Acepta **una o varias** claves separadas por `,`, `;` o espacio, en formato
`clave:valor` o `clave=valor` (mayús/minús indistinto):

| Clave | Valores | Efecto |
|-------|---------|--------|
| `ch` / `channel` | 1…4 | Canal activo |
| `mode` | `manual` / `sweep` / `center` (o `m`/`s`/`c`) | Modo |
| `angle` / `a` | 0…180 | Ángulo objetivo (en modo MANUAL) |

Ejemplos (pega uno en el campo de escritura de nRF Connect):
```
ch:2
mode:sweep
angle:120
ch:3,mode:center
ch:1,mode:manual,angle:45
```
Si escribes `mode:manual` y luego `angle:120`, el canal activo se moverá al
ángulo indicado. Si cambias de canal, los demás mantienen su posición.

### 5.4 Cómo conectar con nRF Connect
1. Instala **nRF Connect for Mobile** (Android/iOS).
2. Escanea y toca **"ServoTester"** → **Connect**.
3. Despliega el servicio `4fafc201-…914b`.
4. Para **leer/recibir telemetría**: abre la característica
   `beb5483e-…00a1`, pulsa el icono de **download** (READ) y luego el de
   **subscribe** (bell/notify). Verás líneas como `CH:1,MODE:MANUAL,…` cada ~250 ms.
5. Para **mandar comandos**: abre la característica `beb5483e-…00a2`, pulsa
   **Write**, pega p. ej. `ch:2,mode:sweep` y envía (UTF-8/sin campo de longitud).
6. Al desconectar, el dispositivo reanuda el advertising automáticamente.

---

## 6. El gráfico de respuesta

Un **buffer circular de 64 muestras** guarda el ángulo del **canal activo** cada
frame (~50 Hz). Se dibuja como gráfica de línea:

- En **MANUAL**, como tira inferior de la pantalla (junto al ángulo y la tele).
- En **GRAFICO**, a pantalla completa.
- El eje X es el tiempo (más antiguo a la izquierda); el eje Y va de 0° (abajo)
  a 180° (arriba).

Úsalo para **ver el overshoot / rebote / jitter** de un servo: al soltar o cambiar
el ángulo, el trazo mostrará si el servo se pasa de la consigna y vuelve (el
clásico "rebote" de los SG90), o si tiembla (jitter) por alimentación débil.

---

## 7. El analizador (igual que Nivel 2)

- **Tensión de alimentación del servo** (`V`), medida con el divisor 10k/10k en
  GPIO3 (ADC) y corregida por `vCal`.
- **Corriente** (`I`, mA) y **potencia** (`W`) del servo, medidas con el INA219
  en serie con el 5 V.
- **Alarma**: si `I > iThresh` (800 mA por defecto) o `V < 4.5 V`, la OLED marca
  **ALARMA** y suena el zumbador.

Qué te dicen V e I (y cómo detectar un servo atascado/muerto):
- **En reposo:** consumo bajo (decenas de mA), rail cerca de 5 V.
- **Al mover:** la corriente sube (100–400 mA según par). Si se dispara al umbral
  y se queda alta sin esfuerzo, el servo está **atascado** (tope forzado, engranaje
  roto, carga excesiva) → suena la alarma.
- **Tensión baja (<4.5 V):** fuente externa débil o caída en cable/INA219.
- **Servo muerto:** ~0 mA y no se mueve, o corriente muy alta con tensión
  colapsada (corto interno).

---

## 8. Recorrido del código por secciones (`src/main.cpp`)

1. **Includes y pines** — Wire, Adafruit SSD1306/GFX, ESP32Servo, Preferences,
   Adafruit_INA219 y **BLEDevice/BLEServer/BLEUtils/BLE2902**. Se definen el
   nuevo pinout: `SERVO_PINS[4] = {10,1,2,21}`, `BUZZER_PIN=8`, etc.
2. **Objetos globales** — `display`, `servos[4]` (array de `Servo`), `ina219`.
3. **Multicanal** — `chanAngle[4]` (posición retenida por canal) y
   `activeChannel`.
4. **Ajustes persistidos** — `minUs`, `maxUs`, `centerAngle`, `iThresh`, `vCal`
   con Preferences (namespace `servotester`).
5. **Encoder por interrupciones** — igual que L2: CHANGE en CLK, debounce 5 ms,
   un detent = ±1. El **botón se sondea en `loop()`** para distinguir pulsación
   **corta** (acción) de **larga** (cambio de canal, 700 ms).
6. **ISRs** — `encoderISR()` acumula `encoderDelta` (`IRAM_ATTR`, `volatile`).
7. **Helpers** — `angleToUs()`, `sweepAngle()`, `cycleChannel()`,
   `loadSettings()`/`saveSettings()`, `readVservo()`, `readINA219()`,
   `updateBuzzer()`, `updateAlarm()`.
8. **Gráfico** — `graphBuf[64]` circular y `pushGraph()`; `drawGraph()`.
9. **BLE** — `applyBleCommand()` parsea los comandos de escritura;
   `ServerCallbacks`/`WriteCallbacks`; `initBLE()` crea servicio,
   características (telemetría read+notify, escritura) y arranca advertising.
10. **Máquina de estados** — `sampleButton()`, `handleInput()` (menu, manual,
    settings, CANAL, gráfico, long-press→canal), `drawMenu/drawServo/
    drawGraphScreen/drawSettings/drawGauge`.
11. **`setup()`** — I²C, OLED, INA219 (`setCalibration_32V_2A()`), carga ajustes,
    **adjunta los 4 servos** (`attach(pin,500,2500)`) y `initBLE()`.
12. **`loop()`** — muestrea botón/encoder, V/I, alarma, buzzer; calcula el ángulo
    del **canal activo** y escribe **solo ese canal**; empuja al gráfico;
    dibuja; **construye y notifica la telemetría BLE** cada 250 ms.

---

## 9. Cómo calibrar los límites (igual que N1/N2)

1. Entra en **SETTINGS**.
2. Ajusta `minUs` al tope donde el servo **empieza** a moverse (típico 500 µs).
3. Ajusta `maxUs` al otro tope (típico 2500 µs).
4. Fija `center` (normalmente 90°) y pulsa SW para **guardar**.
Estos límites definen el pulso en 0° y 180° de **todos** los canales.

---

## 10. Cómo calibrar el divisor de tensión (`vCal`)

El divisor 10k/10k entrega la mitad de la tensión del servo al ADC:
`Vservo = adc × (3.3/4095) × 2.0 × vCal`, con `vCal = 1.0` por defecto.

1. Con el servo alimentado, mide con polímetro la **tensión real** del rail de 5 V.
2. Lee en la OLED la `V:` que muestra el firmware (sin calibrar, `vCal=1`).
3. Calcula `vCal = V_real_medida_polimetro / V_mostrada_OLED`.
4. Fíjalo de forma permanente en `loadSettings()`/Preferences (clave `vCal`) o
   recompila con el valor.

---

## 11. Solución de problemas

| Síntoma | Causa probable | Solución |
|---------|----------------|----------|
| Pantalla en blanco | OLED mal cableada o dir. distinta | Revisa SDA/SCL/GND; confirma 0x3C. |
| "INA219 ??" al arrancar | INA219 ausente / mal cableado | Revisa I²C (GPIO6/7) y que VIN± tengan 5 V/GND. |
| Un servo no se mueve | Sin masa común o fuente 5 V ausente | Une GND de todo; alimenta los servos por separado. |
| OLED congelada en "INA219 ??" | INA219 no responde en 0x40 | Verifica dirección y pull-ups de SDA/SCL. |
| V siempre alta | Pull-up del USB-UART en GPIO3 | Calibra con `vCal` (§10). |
| Servo tiembla/garra | Fuente 5 V insuficiente | Usa fuente con más corriente (≥2 A para 4 servos). |
| Alarma suena sin motivo | Umbral `iThrsh` muy bajo o ruido | Sube `iThrsh` en SETTINGS. |
| No aparece "ServoTester" en BLE | Fuera de rango / permisos | Acerca el móvil; en iOS dale permiso de Bluetooth; reescanea. |
| nRF Connect no escribe | Formato incorrecto | Usa `clave:valor` (p. ej. `ch:2,mode:sweep`), sin bytes extra. |
| `Serial` no muestra nada | Es UART0, no USB | Esperado; mira la OLED (ver nota USB-CDC). |
| Error de compilación `Serial` | Flag USB-CDC mal puesto | No se usa `ARDUINO_USB_CDC_ON_BOOT` a propósito. |
| GPIO2 no arranca / servo raro | Pin strapping mal manejado | No fuerces GPIO2 antes del boot; el firmware lo usa solo como salida. |

---

## 12. La trampa de USB-CDC (importante)

El core de Arduino-ESP32 usado aquí **no necesita** (y no se usa)
`ARDUINO_USB_CDC_ON_BOOT`. Sin ese flag, `Serial` es UART0 (GPIO1/3); el
DevKitM-1 no saca UART0 por el USB-C (ese puerto es solo para esptool). La carga
del firmware se hace por USB nativo con `esptool` (`/dev/ttyACM0` en Linux),
**sin necesidad de pulsar BOOT**. Por eso toda la información va en la OLED.

---

## 13. Compilar y cargar

Desde el directorio del proyecto:

```bash
# Compilar (pio descarga las librerías automáticamente; BLE viene en el core)
pio run

# Cargar (esptool por USB nativo, /dev/ttyACM0; sin botón BOOT)
pio run -t upload

# Monitor serie (solo UART0, no visible por USB)
pio device monitor
```

`pio run` descarga las librerías (Adafruit SSD1306, Adafruit GFX, ESP32Servo,
Adafruit INA219) automáticamente si hay red. El BLE (NimBLE) ya viene incluido
en el framework, así que **no se añade ninguna librería BLE** en `lib_deps`.
No es necesario subir para compilar.
