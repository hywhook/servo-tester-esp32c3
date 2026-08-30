# Servo Tester ESP32-C3 Super Mini

Probador de servos **profesional** para **ESP32-C3 Super Mini**, en 3 niveles
crecientes. Cada nivel es un proyecto **PlatformIO autónomo** (su propio
`platformio.ini` + `src/main.cpp` + manual).

> ¿Los querés en repos separados? Están en carpetas independientes
> (`nivel1/`, `nivel2/`, `nivel3/`); cada una compila y sube por su cuenta.
> Si preferís 3 repos distintos, lo divido sin problema.

## Niveles

| Carpeta   | Qué agrega                                                                 |
|-----------|----------------------------------------------------------------------------|
| `nivel1/` | Menú navegable + modos **Manual / Sweep / Center** + lectura en **µs** + límites persistidos (Preferences). Manual completo en `nivel1/README.md`. |
| `nivel2/` | Nivel 1 + **analizador**: tensión de alimentación (divisor) + corriente (**INA219** en serie) + alarma por sobrecorriente/bajo voltaje. Manual en `nivel2/README.md`. |
| `nivel3/` | Nivel 2 + **premium**: 4 canales, **BLE** (NimBLE,控制 desde el celu) y **gráfico de respuesta** en el OLED. Manual en `nivel3/README.md`. |

## Hardware común

- ESP32-C3 Super Mini (USB nativo `303a:1001`)
- OLED SSD1306 128×64 I2C
- Encoder rotativo HW-040 (como KY-040)
- Servo(s) + fuente de **5 V externa** (GND común)
- Nivel lógico **3.3 V**

El pinout exacto y el diagrama de conexiones de cada nivel están en su manual.

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
├── nivel1/  platformio.ini  src/main.cpp  README.md
├── nivel2/  platformio.ini  src/main.cpp  README.md
└── nivel3/  platformio.ini  src/main.cpp  README.md
```
