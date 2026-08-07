# Water-Monitoring-System

ESP32-S3 based IoT water monitoring system for monitoring or measuring water parameters in real time. The system integrates sensors, embedded programs, and wireless connectivity for collection, processing, and visualization of water data, with applications in smart agriculture, water management, and environmental monitoring.It has a future scope of developing into a smart agriculture IOT system with seamless realtime operations of the farm ,monitored and controlled from all over the world.

## Project Structure

```
Water-Monitoring-System/
├── docs/                 # Abstract, project report, system architecture, flowchart
├── hardware/             # Components list and pin connection documentation
├── media/                # Photos of the assembled system
├── software/
│   ├── dashboard/        # Standalone HTML preview of the web UI
│   └── water_monitoring/ # Arduino .ino firmware (main deliverable)
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

### Core (P0 + P1)
- Real-time soil moisture, temperature, and humidity monitoring
- Auto-irrigation with 5% hysteresis to prevent relay chatter
- **5-minute max watering safety cutoff** (prevents flooding if sensor fails)
- Manual override (Water ON / OFF / Auto Mode) via web UI
- Threshold + configuration persistence in NVS (survives reboot)
- **WiFiManager captive portal** for WiFi setup (no hardcoded credentials)
- **HTTPS** for OpenWeatherMap API calls
- **HTTP Basic Auth** on all routes (configurable username/password)
- **CSRF protection** on all POST endpoints
- **Automatic WiFi reconnect** if connection drops
- Live local weather from OpenWeatherMap (10-min refresh)
- mDNS hostname: `http://project.local/`

### Advanced (P2)
- **OTA firmware updates** — upload new `.bin` files at `/update` (no USB cable needed)
- **NTP time sync** — device clock stays accurate; powers scheduled watering
- **Scheduled watering window** — only water during configured hours (e.g., 6 AM to 8 AM)
- **Sensor fault detection** — DHT11 NaN streak and moisture sensor out-of-range detection, with red fault badges in the UI
- **Forecast-aware watering** — skips watering if rain ≥ 1 mm is forecast in the next 3 hours (saves water, prevents overwatering)
- **24-hour trends chart** — Chart.js visualization of temperature, humidity, and soil moisture history (logged every 60 seconds to LittleFS)
- **Moisture sensor calibration wizard** — set DRY/WET calibration points from the web UI (no reflash needed)

## Firmware

The main firmware is [`software/water_monitoring/water_monitoring.ino`](software/water_monitoring/water_monitoring.ino).

### Required Arduino Libraries

Install via Arduino IDE Library Manager (Sketch → Include Library → Manage Libraries):

| Library | Author | Purpose |
|---------|--------|---------|
| ArduinoJson v6 | Benoit Blanchon | JSON parsing/serialization |
| ESPAsyncWebServer | me-no-dev | Async HTTP server |
| AsyncTCP | me-no-dev | Async TCP for ESP32 |
| DHT sensor library | Adafruit | DHT11 driver |
| WiFiManager | tzapu | Captive portal for WiFi setup |
| **AsyncElegantOTA** | Ayush Sharma | **OTA firmware updates** at `/update` |

The following come bundled with the ESP32 board package (no install needed):
- `WiFi.h`, `WiFiClientSecure.h`, `HTTPClient.h`, `Preferences.h`, `ESPmDNS.h`, `LittleFS.h`, `time.h`

### Board Settings (Arduino IDE)

- **Board:** `ESP32S3 Dev Module` (or your specific S3 board variant)
- **Port:** the COM port that appears when you plug in the S3 via USB
- **Upload Speed:** 921600
- **USB CDC On Boot:** `Enabled` (so `Serial.print` shows up over USB)
- **USB Mode:** `USB-OTG (TinyUSB)`
- **Flash Size:** 4MB (or whatever your board has)
- **Partition Scheme:** `Default 4MB with ffat` (or `FAT 1MB app + 2MB FATFS` if available — LittleFS works on either)

### First Boot Setup

1. Flash the firmware to the ESP32-S3 via USB
2. On your phone or computer, look for a new WiFi network called **`SmartAgri-Setup`** and connect to it
3. A captive portal page should pop up automatically (if not, browse to `http://192.168.4.1`)
4. Enter your WiFi network name (SSID) and password, then click Save
5. The ESP32-S3 will reboot and connect to your WiFi
6. Open the Arduino IDE Serial Monitor at 115200 baud to see the assigned IP address (also visible as `http://project.local/` if your OS supports mDNS)
7. Open the URL in a browser. Log in with the **default credentials**: username `admin`, password `admin`
8. Go to the **Configuration** section and enter your OpenWeatherMap API key, city, and country code
9. **Change the default username/password** in the same section (the UI will warn you if defaults are still in use)
10. (Optional) Go to **Sensor Calibration**, place the moisture sensor in dry air, click **Set Dry**, then place in water and click **Set Wet**
11. (Optional) Adjust the **Watering Window** in Settings to limit auto-watering to specific hours

### Updating Firmware via OTA

Once the device is on your WiFi and the initial USB flash is done, future updates can be done over the air:

1. In Arduino IDE: **Sketch → Export Compiled Binary** (this produces a `.bin` file in the sketch folder)
2. Open `http://project.local/update` in a browser
3. Log in with your web UI credentials
4. Upload the `.bin` file
5. The device reboots automatically with the new firmware

### Changing WiFi Credentials Later

1. Open the web UI
2. Go to the Configuration section
3. Click **"Reset WiFi Configuration"**
4. The device will reboot into the captive portal — repeat the First Boot Setup steps above

## Web UI Overview

The dashboard includes these cards:

| Card | Purpose |
|------|---------|
| Header | Title + live device clock |
| Field Sensors | Temperature, humidity, soil moisture (with fault badges) + system/motor status + manual controls |
| Local Weather | Current conditions + rain forecast for next 3 hours |
| Trends | 24-hour line chart of temp/hum/moisture (Chart.js) |
| Settings | Moisture/humidity thresholds + watering window hours |
| Sensor Calibration | View current raw ADC + set dry/wet calibration points |
| Configuration | API key, city, country, auth credentials, OTA link, WiFi reset |

## Auto-Watering Decision Logic

The auto-mode loop checks **all** of these conditions before turning the pump ON:

1. ✅ Within the configured watering window (e.g., 6 AM – 8 AM)
2. ✅ No rain ≥ 1 mm forecast in the next 3 hours
3. ✅ Moisture sensor is healthy (not in fault state)
4. ✅ Soil moisture is below the threshold (with 5% hysteresis to prevent chatter)

If any condition becomes false while the pump is running, the pump is turned OFF immediately. Additionally, a hard 5-minute cutoff trips if the pump runs continuously (sensor failure protection).

## Security Notes

- **No hardcoded secrets in source.** WiFi credentials, API key, and web UI credentials are all stored in NVS and configured at runtime.
- **HTTPS** is used for OpenWeatherMap API calls. Certificate validation is currently skipped (`setInsecure()`) to keep the firmware simple. For a production deployment, bundle the OpenWeatherMap root CA certificate using `WiFiClientSecure::setCACert()`.
- **HTTP Basic Auth** protects all routes. Credentials travel in plaintext over HTTP — for a production deployment, also enable HTTPS on the local web server (the ESP32-S3 has the resources for TLS).
- **Default credentials are `admin`/`admin`**. The web UI displays a yellow warning banner until you change them.
- **CSRF protection** requires the `X-Requested-With: XMLHttpRequest` header on all POSTs, blocking cross-origin attacks from malicious websites.
