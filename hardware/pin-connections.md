# Pin Connections

This document describes the pin connections between the ESP32 C3 Super Mini Microcontroller and all the external hardware components used in the project.

> ## ⚠️ Important Hardware Note (P0 Fix)
> **GPIO 8 is a strapping pin on the ESP32-C3.** It controls VDD_SPI voltage selection at boot.
> If your relay module pulls this pin LOW at power-on, the chip may enter an abnormal boot state.
>
> **Recommended fix (requires rewiring):** Move the relay to **GPIO 5, 6, 7, or 10** (non-strapping free pins), then update `#define RELAY_PIN` in `water_monitoring.ino` to match.
>
> If rewiring is not possible right now, verify your relay module has a pull-up on its input (most do) and confirm the board boots reliably before deploying.

## Pin Mapping Table

| Component              | ESP32 Pin | Type            | Description                                  | Notes                                                  |
|------------------------|----------:|-----------------|----------------------------------------------|--------------------------------------------------------|
| Soil Moisture Sensor   | GPIO 1    | Analog Input    | Reads soil moisture level                    | ADC1_CH1 on ESP32-C3 — valid analog pin               |
| DHT Sensor (Data)      | GPIO 4    | Digital I/O     | Reads temperature and humidity data          | ADC1_CH4 on ESP32-C3 (also usable as digital)         |
| Relay Module (IN)      | GPIO 8    | Digital Output  | Controls solenoid valve ON/OFF               | ⚠️ Strapping pin — see warning above                  |
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
- ESP32 powered via Regulated 5V

> Note: All grounds (GND) must be connected together to ensure proper operation.

## ESP32-C3 ADC Reference

The ESP32-C3 has 5 ADC1 channels (no ADC2 conflict with WiFi, unlike classic ESP32):

| GPIO | ADC Channel |
|-----:|:------------|
| 0    | ADC1_CH0    |
| 1    | ADC1_CH1    | ← used by soil moisture sensor
| 2    | ADC1_CH2    |
| 3    | ADC1_CH3    |
| 4    | ADC1_CH4    | ← used by DHT11 (as digital)

This is why `MOISTURE_PIN = 1` is **valid** on this board — it is ADC1_CH1.
(On a classic ESP32, GPIO 1 would be UART0 TX and unsuitable, but that does not apply to ESP32-C3.)

## ESP32-C3 Strapping Pins (Avoid for Outputs)

| GPIO | Boot Function                | Safe to Use?                  |
|-----:|------------------------------|-------------------------------|
| 2    | Boot mode selection          | Avoid for outputs             |
| 8    | VDD_SPI voltage select       | ⚠️ Avoid for outputs (current relay pin) |
| 9    | Boot mode (download/normal)  | Avoid for outputs             |

All other GPIO pins are safe for general-purpose I/O.
