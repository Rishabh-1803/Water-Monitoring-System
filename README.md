# Water-Monitoring-System

ESP32-S3 based IoT water monitoring system for monitoring or measuring water parameters in real time. The system integrates sensors, embedded programs, and wireless connectivity for collection, processing, and visualization of water data, with applications in smart agriculture, water management, and environmental monitoring.

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
| Microcontroller          | ESP32-S3 Development Board             |
| Soil Moisture Sensor     | Analog capacitive/resistive module     |
| Temperature & Humidity   | DHT11                                  |
| Relay Module             | 5V single-channel (active-LOW)         |
| Solenoid Valve           | 12V DC                                 |
| Power                    | 12V adapter + DC-DC buck to 5V         |

See [`hardware/pin-connections.md`](hardware/pin-connections.md) for full wiring details.

## Features

- Real-time soil moisture, temperature, and humidity monitoring
- Auto-irrigation with 5% hysteresis to prevent relay chatter
- **5-minute max watering safety cutoff** (prevents flooding if sensor fails)
- Manual override (Water ON / OFF / Auto Mode) via web UI
- Threshold + configuration persistence in NVS (survives reboot)
- **WiFiManager captive portal** for WiFi setup (no hardcoded credentials)
- **HTTPS** for OpenWeatherMap API calls (API key encrypted in transit)
- **HTTP Basic Auth** on all routes (configurable username/password)
- **CSRF protection** on all POST endpoints
- **Automatic WiFi reconnect** if connection drops
- Live local weather from OpenWeatherMap (10-min refresh)
- mDNS hostname: `http://project.local/`
- Responsive single-page web dashboard with in-browser configuration

## Firmware

The main firmware is [`software/water_monitoring/water_monitoring.ino`](software/water_monitoring/water_monitoring.ino).

### Required Arduino Libraries

Install via Arduino IDE Library Manager (Sketch → Include Library → Manage Libraries):

| Library | Version | Author | Purpose |
|---------|---------|--------|---------|
| ArduinoJson | v6.x | Benoit Blanchon | JSON parsing/serialization |
| ESPAsyncWebServer | latest | me-no-dev | Async HTTP server |
| AsyncTCP | latest | me-no-dev | Async TCP for ESP32 |
| DHT sensor library | latest | Adafruit | DHT11 driver |
| **WiFiManager** | latest | tzapu | **Captive portal for WiFi setup** (new in P1) |

The following come bundled with the ESP32 board package (no install needed):
- `WiFi.h`, `WiFiClientSecure.h`, `HTTPClient.h`, `Preferences.h`, `ESPmDNS.h`

### Board Settings (Arduino IDE)

- **Board:** `ESP32S3 Dev Module` (or your specific S3 board variant)
- **Port:** the COM port that appears when you plug in the S3 via USB
- **Upload Speed:** 921600
- **USB CDC On Boot:** `Enabled` (so `Serial.print` shows up over USB)
- **USB Mode:** `USB-OTG (TinyUSB)`
- **Flash Size:** 4MB (or whatever your board has)
- **Partition Scheme:** `Default 4MB with ffat` (or `Minimal SPIFFS` if you need more app space)

### First Boot Setup

1. Flash the firmware to the ESP32-S3
2. On your phone or computer, look for a new WiFi network called **`SmartAgri-Setup`** and connect to it
3. A captive portal page should pop up automatically (if not, browse to `http://192.168.4.1`)
4. Enter your WiFi network name (SSID) and password, then click Save
5. The ESP32-S3 will reboot and connect to your WiFi
6. Open the Arduino IDE Serial Monitor at 115200 baud to see the assigned IP address (also visible as `http://project.local/` if your OS supports mDNS)
7. Open the URL in a browser. Log in with the **default credentials**: username `admin`, password `admin`
8. Go to the **Configuration** section and enter your OpenWeatherMap API key, city, and country code
9. **Change the default username/password** in the same section (the UI will warn you if defaults are still in use)

### Changing WiFi Credentials Later

If you move the device to a new network or your WiFi password changes:
1. Open the web UI
2. Go to the Configuration section
3. Click **"Reset WiFi Configuration"**
4. The device will reboot into the captive portal — repeat the First Boot Setup steps above

## Status

This branch contains the **P0 critical fixes** AND **P1 security/reliability fixes** applied.
See [`CHANGELOG.md`](CHANGELOG.md) for the full list of changes.

Outstanding P2 / feature work (OTA updates, MQTT, NTP scheduling, forecast-aware watering,
sensor fault detection, history charts, flow sensor, tank level sensor, Telegram alerts,
etc.) is tracked separately.

## Security Notes

- **No hardcoded secrets in source.** WiFi credentials, API key, and web UI credentials are all stored in NVS (non-volatile storage) and configured at runtime.
- **HTTPS** is used for OpenWeatherMap API calls. Note: certificate validation is currently skipped (`setInsecure()`) to keep the firmware simple. For a production deployment, bundle the OpenWeatherMap root CA certificate using `WiFiClientSecure::setCACert()`.
- **HTTP Basic Auth** protects all routes. Credentials travel in plaintext over HTTP — for a production deployment, also enable HTTPS on the local web server (the ESP32-S3 has the resources for TLS).
- **Default credentials are `admin`/`admin`**. The web UI displays a yellow warning banner until you change them.
