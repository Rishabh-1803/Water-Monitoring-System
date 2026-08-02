# Water-Monitoring-System

ESP32-C3 based IoT water monitoring system for monitoring or measuring water parameters in real time. The system integrates sensors, embedded programs, and wireless connectivity for collection, processing, and visualization of water data, with applications in smart agriculture, water management, and environmental monitoring.

## Project Structure

```
Water-Monitoring-System/
├── docs/                 # Abstract, project report, system architecture, flowchart
├── hardware/             # Components list and pin connection documentation
├── media/                # Photos of the assembled system
├── software/
│   ├── dashboard/        # Standalone HTML preview of the web UI
│   └── water_monitoring/ # Arduino .ino firmware (main deliverable)
├── CHANGELOG.md          # Version history of P0/P1/P2 fixes
├── LICENSE
└── README.md
```

## Hardware

| Component                | Specification                          |
|--------------------------|----------------------------------------|
| Microcontroller          | ESP32-C3 Super Mini                    |
| Soil Moisture Sensor     | Analog capacitive/resistive module     |
| Temperature & Humidity   | DHT11                                  |
| Relay Module             | 5V single-channel (active-LOW)         |
| Solenoid Valve           | 12V DC                                 |
| Power                    | 12V adapter + DC-DC buck to 5V         |

See [`hardware/pin-connections.md`](hardware/pin-connections.md) for full wiring details.

## Features

- Real-time soil moisture, temperature, and humidity monitoring
- Auto-irrigation with 5% hysteresis to prevent relay chatter
- Manual override (Water ON / OFF / Auto Mode) via web UI
- Threshold persistence in NVS (survives reboot)
- Live local weather from OpenWeatherMap (10-min refresh)
- mDNS hostname: `http://project.local/`
- Responsive single-page web dashboard

## Firmware

The main firmware is [`software/water_monitoring/water_monitoring.ino`](software/water_monitoring/water_monitoring.ino).

### Required Arduino Libraries
- `WiFi.h`, `HTTPClient.h`, `Preferences.h`, `ESPmDNS.h` (bundled with ESP32 board package)
- [ArduinoJson](https://arduinojson.org/) v6
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)
- [AsyncTCP](https://github.com/me-no-dev/AsyncTCP)
- [DHT sensor library](https://github.com/adafruit/DHT-sensor-library)

### Board Settings (Arduino IDE)
- Board: **ESP32C3 Dev Module** (or "ESP32-C3 Super Mini" if your package provides it)
- Flash Size: 4MB
- Upload Speed: 921600

## Status

This branch contains the **P0 critical fixes** applied. See [`CHANGELOG.md`](CHANGELOG.md) for what was changed and why.

Outstanding P1 / P2 / feature work (WiFiManager, HTTPS, auth, OTA, MQTT, sensor fault detection, etc.) is tracked separately.

## Security Reminder

⚠️ The OpenWeatherMap API key and WiFi credentials are currently hardcoded in `water_monitoring.ino`. If this repository has ever been pushed to GitHub (even private) or shared externally, **rotate the API key immediately** at https://home.openweathermap.org/api_keys and update your WiFi password. The long-term fix (WiFiManager + runtime key entry) is tracked as P1.
