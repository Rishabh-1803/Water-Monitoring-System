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
- 5-minute max watering safety cutoff
- Manual override (Water ON / OFF / Auto Mode) via web UI
- WiFiManager captive portal (no hardcoded credentials)
- HTTPS for OpenWeatherMap API calls
- HTTP Basic Auth on all routes
- CSRF protection on all POST endpoints
- Automatic WiFi reconnect

### Advanced (P2)
- OTA firmware updates at `/update`
- NTP time sync (IST)
- Scheduled watering window (configurable start/end hour)
- Sensor fault detection (DHT11 + moisture) with UI badges
- Forecast-aware watering (skips if rain ≥ 1mm in next 3 hours)
- 24-hour trends chart (LittleFS + Chart.js)
- Moisture sensor calibration wizard

### Cloud & Integration (P3)
- **MQTT integration** — publish sensor data to Home Assistant / Node-RED / any MQTT broker; subscribe to remote commands
- **Telegram alerts** — push notifications for sensor faults, safety trips, watering events, WiFi/MQTT disconnects, and device boots
- **TLS certificate validation** — optionally validate OpenWeatherMap and Telegram certs using a user-provided root CA (replaces `setInsecure()`)
- **Diagnostics dashboard** — uptime, free heap, WiFi RSSI, LittleFS usage, MQTT state, pump stats, last boot time
- **Pump cycle counter** — tracks total pump activations and cumulative runtime, persisted to NVS
- **Backup/Restore configuration** — export all settings as JSON, import on another device
- **CSV data export** — download historical sensor log as CSV from the UI

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
| AsyncElegantOTA | Ayush Sharma | OTA firmware updates |
| **PubSubClient** | Nick O'Leary | **MQTT client** (new in P3) |

The following come bundled with the ESP32 board package (no install needed):
- `WiFi.h`, `WiFiClientSecure.h`, `HTTPClient.h`, `Preferences.h`, `ESPmDNS.h`, `LittleFS.h`, `time.h`

### Board Settings (Arduino IDE)

- **Board:** `ESP32S3 Dev Module`
- **Upload Speed:** 921600
- **USB CDC On Boot:** `Enabled`
- **USB Mode:** `USB-OTG (TinyUSB)`
- **Flash Size:** 4MB (or whatever your board has)
- **Partition Scheme:** `Default 4MB with ffat` (LittleFS works on either FAT or LittleFS partitions)

### First Boot Setup

1. Flash the firmware to the ESP32-S3 via USB
2. Connect to the `SmartAgri-Setup` WiFi network from your phone/laptop
3. A captive portal page should pop up automatically (if not, browse to `http://192.168.4.1`)
4. Enter your WiFi credentials and click Save
5. The device reboots and connects to your WiFi — check Serial Monitor for the IP
6. Open `http://project.local/` (or the IP) in a browser
7. Log in with default credentials: `admin` / `admin`
8. Go to **Configuration** → enter OpenWeatherMap API key, city, country
9. **Change the default password** (UI shows a warning until you do)
10. (Optional) Configure MQTT, Telegram, TLS validation as needed
11. (Optional) Calibrate the moisture sensor in the **Sensor Calibration** card

## MQTT Integration (P3-1)

### Configuration
In the web UI, go to the **MQTT Integration** card and set:
- Enable MQTT: `true`
- Broker Host: e.g. `broker.hivemq.com` (public test broker) or `192.168.1.50` (your Home Assistant IP)
- Broker Port: `1883` (or `8883` for TLS — note: TLS broker not yet supported, use 1883)
- Topic Prefix: `smartfarm` (default)
- Username / Password: optional, if your broker requires auth

### Published Topics
The device publishes to these topics every 30 seconds (retained = true):

| Topic | Payload | Example |
|-------|---------|---------|
| `<prefix>/sensor/temperature` | float | `25.3` |
| `<prefix>/sensor/humidity` | float | `60.5` |
| `<prefix>/sensor/moisture` | float | `45.2` |
| `<prefix>/sensor/relay` | string | `ON` or `OFF` |
| `<prefix>/status` | JSON | `{"temperature":25.3,"humidity":60.5,"moisture":45.2,"watering":false,"auto_mode":true,...}` |

### Subscribed Topics
The device listens on these topics for remote commands:

| Topic | Action |
|-------|--------|
| `<prefix>/cmd/water_on` | Turn pump ON (switches to manual mode) |
| `<prefix>/cmd/water_off` | Turn pump OFF (switches to manual mode) |
| `<prefix>/cmd/auto_mode` | Toggle auto mode |

### Home Assistant Example
Add this to your `configuration.yaml` to track moisture:

```yaml
mqtt:
  sensor:
    - name: "Soil Moisture"
      state_topic: "smartfarm/sensor/moisture"
      unit_of_measurement: "%"
      device_class: moisture
    - name: "Greenhouse Temperature"
      state_topic: "smartfarm/sensor/temperature"
      unit_of_measurement: "°C"
      device_class: temperature
```

## Telegram Alerts (P3-2)

### Setup
1. Open Telegram and message [@BotFather](https://t.me/BotFather)
2. Send `/newbot` and follow the prompts to create a bot
3. Copy the bot token (format: `123456789:ABCdefGHIjklMNOpqrSTUvwxYZ`)
4. Message [@userinfobot](https://t.me/userinfobot) to get your Chat ID (a number like `123456789`)
5. In the web UI, go to the **Telegram Alerts** card:
   - Enable Telegram: `true`
   - Chat ID: your numeric chat ID
   - Bot Token: the token from step 3
6. Click **Save Telegram Config**
7. Click **Send Test Alert** — you should receive a message in Telegram

### Alert Types
The device sends alerts for:
- 🚀 Device boot (with IP and timestamp)
- ⚠️ DHT11 sensor fault detected
- ✅ DHT11 sensor recovered
- ⚠️ Moisture sensor fault detected
- ✅ Moisture sensor recovered
- 🚨 Safety trip (pump ran > 5 minutes)
- 💧 Watering started (with sensor values)
- 🛑 Watering stopped (with duration)
- 📡 WiFi disconnected > 5 minutes
- 📡 WiFi reconnected (with new IP)
- 🔌 MQTT disconnected

Each alert type has a 5-minute cooldown to prevent spam.

## TLS Certificate Validation (P3-3)

By default, the device uses `setInsecure()` for HTTPS calls (encrypted but not authenticated — vulnerable to MITM). To enable proper cert validation:

1. Get the root CA for `api.openweathermap.org`:
   ```bash
   openssl s_client -showcerts -connect api.openweathermap.org:443 </dev/null 2>/dev/null \
     | openssl x509 -outform PEM
   ```
2. In the web UI **Configuration** card:
   - Set **Validate TLS Cert** to `Enabled`
   - Paste the PEM cert (including `-----BEGIN CERTIFICATE-----` and `-----END CERTIFICATE-----`) into the **Root CA Certificate** field
3. Click **Save Configuration**

The same cert is used for both OpenWeatherMap and Telegram API calls.

**Note:** If the cert expires or the provider changes their CA chain, HTTPS calls will fail silently. The device will continue operating with cached/stale weather data.

## Backup / Restore (P3-6)

### Backup
1. Click **Backup Config** in the Configuration card
2. A JSON file `smartfarm-config-backup.json` downloads automatically
3. This file contains all settings except WiFi credentials and the web UI password

### Restore
1. Click **Restore Config**
2. Select a previously-backed-up JSON file
3. All settings are imported and the device reboots

Use this to clone config across multiple devices, or to restore after a reflash.

## CSV Export (P3-7)

Click **Download CSV** in the Trends card to download the full sensor log as a CSV file. The file is named `smartfarm_log_YYYYMMDDTHHMMSS.csv` and contains columns: `timestamp,temperature,humidity,moisture,watering`.

## Updating Firmware via OTA

1. In Arduino IDE: **Sketch → Export Compiled Binary**
2. Open `http://project.local/update`
3. Upload the `.bin` file
4. The device reboots automatically with the new firmware

## Auto-Watering Decision Logic

The auto-mode loop checks **all** of these conditions before turning the pump ON:

1. ✅ Within the configured watering window
2. ✅ No rain ≥ 1 mm forecast in next 3 hours
3. ✅ Moisture sensor is healthy (not in fault state)
4. ✅ Soil moisture is below the threshold (with 5% hysteresis)
5. ✅ Max watering duration not exceeded

If any condition becomes false mid-watering, the pump turns OFF immediately and a Telegram alert is sent.

## Security Notes

- **No hardcoded secrets in source.** All credentials (WiFi, API key, MQTT, Telegram, web UI) are in NVS and configurable at runtime.
- **HTTPS** for OpenWeatherMap and Telegram API calls. Cert validation is optional (off by default; can be enabled with a user-provided root CA).
- **HTTP Basic Auth** protects all routes. For production, also enable HTTPS on the local web server.
- **CSRF protection** on all POST endpoints.
- **Default credentials are `admin`/`admin`** — the UI warns until you change them.
- **Backup files contain plaintext secrets** (API key, MQTT password, Telegram bot token). Store them securely.
