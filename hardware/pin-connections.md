# Pin Connections

This document describes the pin connections between the **ESP32-S3** development board and all the external hardware components used in the project.

> ## Board Change Notice
> The project was migrated from the ESP32-C3 Super Mini to the ESP32-S3.
> All three pins used by this project (GPIO 1, 4, 8) are **safe on the ESP32-S3**:
>
> | GPIO | ESP32-C3                  | ESP32-S3                          |
> |-----:|---------------------------|-----------------------------------|
> | 1    | ADC1_CH1 ✅               | ADC1_CH0 ✅                       |
> | 4    | Safe ✅                   | ADC1_CH3 ✅                       |
> | 8    | ⚠️ Strapping pin          | Safe ✅ (NOT a strapping pin)    |
>
> ESP32-S3 strapping pins are: **GPIO 0, GPIO 3, GPIO 45, GPIO 46**. None are used by this project.

## Pin Mapping Table

| Component              | ESP32 Pin | Type            | Description                                  | Notes                                                  |
|------------------------|----------:|-----------------|----------------------------------------------|--------------------------------------------------------|
| Soil Moisture Sensor   | GPIO 1    | Analog Input    | Reads soil moisture level                    | ADC1_CH0 on ESP32-S3 — valid analog pin               |
| DHT Sensor (Data)      | GPIO 4    | Digital I/O     | Reads temperature and humidity data          | ADC1_CH3 on ESP32-S3 (also usable as digital)         |
| Relay Module (IN)      | GPIO 8    | Digital Output  | Controls solenoid valve ON/OFF               | Safe general-purpose output on ESP32-S3               |
| Relay Module (VCC)     | 5V        | Power           | Power supply from DC-DC converter            |                                                        |
| Relay Module (GND)     | GND       | Ground          | Common ground                                |                                                        |
| Soil Sensor (VCC)      | 3.3V      | Power           | Power supply from ESP32                      |                                                        |
| Soil Sensor (GND)      | GND       | Ground          | Common ground                                |                                                        |
| DHT Sensor (VCC)       | 3.3V      | Power           | Power supply from ESP32                      |                                                        |
| DHT Sensor (GND)       | GND       | Ground          | Common ground                                |                                                        |

## Power Connections
- 12V Adapter --> Solenoid Valve (via Relay)
- 12V Adapter --> DC-DC Converter
- DC-DC Converter (5V) --> Relay Module
- ESP32-S3 powered via Regulated 5V (or USB-C, depending on board)

> Note: All grounds (GND) must be connected together to ensure proper operation.

## ESP32-S3 ADC Reference

The ESP32-S3 has two ADC blocks:

| ADC Block | GPIO Range       | Channels     | WiFi Conflict? |
|-----------|------------------|--------------|----------------|
| ADC1      | GPIO 1 - GPIO 10 | ADC1_CH0-9   | No ✅          |
| ADC2      | GPIO 11 - GPIO 20| ADC2_CH0-9   | Yes ⚠️         |

Our moisture sensor uses **GPIO 1 = ADC1_CH0** — no WiFi conflict.

## ESP32-S3 Strapping Pins (Avoid for Outputs)

| GPIO | Boot Function                                | Safe to Use?          |
|-----:|----------------------------------------------|-----------------------|
| 0    | Boot mode selection (must be HIGH at boot)   | Avoid for outputs     |
| 3    | JTAG signal source selection                 | Avoid for outputs     |
| 45   | VDD_SPI / LDO voltage selection              | Avoid for outputs     |
| 46   | Boot mode selection                          | Avoid for outputs     |

All other GPIO pins are safe for general-purpose I/O.

## ESP32-S3 Internal SPI Flash / PSRAM Pins

On ESP32-S3-WROOM modules with octal SPI flash or PSRAM, the following GPIOs are
used **internally** by the module and are NOT exposed on the dev board pins:

| GPIO | Used For                          |
|-----:|-----------------------------------|
| 26-32 | Octal SPI flash / PSRAM (internal) |

These do not affect our pin assignments (GPIO 1, 4, 8 are all externally available).
