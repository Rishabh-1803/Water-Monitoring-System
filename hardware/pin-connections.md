# Pin Connections

This document describes the pin connections between the ESP32 C3 Super Mini Microcontroller and all the external hardware components used in the project.

## Pin Mapping Table

| Component              | ESP32 Pin | Type        | Description                                  |
|------------------------|----------:|-------------|----------------------------------------------|
| Soil Moisture Sensor   | GPIO 1   | Analog Input| Reads soil moisture level                    |
| DHT Sensor (Data)      | GPIO 4    | Digital I/O | Reads temperature and humidity data          |
| Relay Module (IN)      | GPIO 8   | Digital Output | Controls solenoid valve ON/OFF           |
| Relay Module (VCC)     | 5V        | Power       | Power supply from DC-DC converter            |
| Relay Module (GND)     | GND       | Ground      | Common ground                                |
| Soil Sensor (VCC)      | 3.3V      | Power       | Power supply from ESP32                      |
| Soil Sensor (GND)      | GND       | Ground      | Common ground                                |
| DHT Sensor (VCC)       | 3.3V      | Power       | Power supply from ESP32                      |
| DHT Sensor (GND)       | GND       | Ground      | Common ground                                |

## Power Connections
- 12V Adapter --> Solenoid Valve (via Relay)
- 12V Adapter --> DC-DC Converter
- DC-DC Converter (5V) --> Relay Module
- ESP32 powered via Regulated 5V

> Note: All grounds (GND) must be connected together to ensure proper operation.
