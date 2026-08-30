# Servo Tester ESP32-C3 Super Mini

Probador de servos **profesional** para **ESP32-C3 Super Mini**, en 3 niveles
crecientes. Cada nivel es un proyecto **PlatformIO autónomo** (su propio
`platformio.ini` + `src/main.cpp` + manual).

> ¿Los querés en repos separados? Están en carpetas independientes
> (`nivel1/`, `nivel2/`, `nivel3/`); cada una compila y sube por su cuenta.
> Si preferís 3 repos distintos, lo divido sin problema.

## 📘 Manuales en HTML (versión definitiva)

Cada nivel tiene un manual HTML autocontenido, con colores, diagramas de
conexión, teoría, código fuente completo y troubleshooting. Esta es la
versión recomendada:

- **[Index / Landing](index.html)** — página de entrada con los 3 niveles.
- **[Manual Nivel 1](nivel1/manual.html)** — Menú + µs.
- **[Manual Nivel 2](nivel2/manual.html)** — Analizador V/I.
- **[Manual Nivel 3](nivel3/manual.html)** — Premium (BLE + multicanal).

(Los `README.md` de cada carpeta quedan como fuente en Markdown.)

## Niveles

| Carpeta   | Qué agrega                                                                 |
|-----------|----------------------------------------------------------------------------|
| `nivel1/` | Menú navegable + modos **Manual / Sweep / Center** + lectura en **µs** + límites persistidos (Preferences). Manual HTML en `nivel1/manual.html`. |
| `nivel2/` | Nivel 1 + **analizador**: tensión de alimentación (divisor) + corriente (**INA219** en serie) + alarma por sobrecorriente/bajo voltaje. Manual HTML en `nivel2/manual.html`. |
| `nivel3/` | Nivel 2 + **premium**: 4 canales, **BLE** (NimBLE, control desde el celu) y **gráfico de respuesta** en el OLED. Manual HTML en `nivel3/manual.html`. |

## Hardware común

- ESP32-C3 Super Mini (USB nativo `303a:1001`)
- OLED SSD1306 128×64 I2C
- Encoder rotativo HW-040 (como KY-040)
- Servo(s) + fuente de **5 V externa** (GND común)
- Nivel lógico **3.3 V**

El pinout exacto y el diagrama de conexiones de cada nivel están en su manual HTML.

## ⚠️ Gotcha importante (USB-CDC)

El core de Arduino instalado (`framework-arduinoespressif32` 7.0.1) **no incluye
`usb_serial.h` para C3**. Por eso **ningún nivel** usa
`ARDUINO_USB_CDC_ON_BOOT=1`: al ponerlo, `Serial` queda sin definir y no compila.
`Serial` cae a UART0 (GPIO1/3), que el Super Mini **no cablea al USB**, así que
el debug se hace por el **OLED**, no por el monitor serie USB.

## Compilar y subir

En cada carpeta:

```bash
pio run            # compilar
pio run -t upload  # subir por el USB nativo (esptool, reset por RTS)
```

## Estructura

```
servo-tester-esp32c3/
├── README.md          (este archivo)
├── index.html         (landing de manuales)
├── nivel1/  platformio.ini  src/main.cpp  README.md  manual.html
├── nivel2/  platformio.ini  src/main.cpp  README.md  manual.html
└── nivel3/  platformio.ini  src/main.cpp  README.md  manual.html
```
