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
| Status LED (P4)          | Built-in LED on GPIO 2, or external LED + 220Ω resistor |

See [`hardware/pin-connections.md`](hardware/pin-connections.md) for full wiring details.

## Features

### Core (P0 + P1)
- Real-time soil moisture, temperature, and humidity monitoring
- Auto-irrigation with 5% hysteresis
- 5-minute max watering safety cutoff
- WiFiManager captive portal for WiFi provisioning (no credentials in source)
- HTTP Basic Auth, CSRF protection and per-IP rate limiting on the local API
- HTTPS for all *outbound* calls (weather, Telegram, InfluxDB)
- Automatic WiFi reconnect

### Advanced (P2)
- OTA firmware updates at `/update`
- NTP time sync (IST)
- Scheduled watering window
- Sensor fault detection
- Forecast-aware watering (rain ≥ 1mm)
- 24-hour trends chart
- Moisture sensor calibration wizard

### Cloud & Integration (P3)
- MQTT integration (PubSubClient)
- Telegram alerts
- Optional TLS certificate validation
- Diagnostics dashboard
- Pump cycle counter
- Backup/Restore configuration
- CSV data export

### Hardening (P4)
- **InfluxDB v2 cloud integration** — push sensor data to InfluxDB Cloud using line protocol
- **Access log** — all admin actions logged to LittleFS, viewable in UI
- **Scheduled weekly reboot** — configurable day-of-week + hour (defaults to Monday 3 AM)
- **Status LED** — GPIO 2 blinks at different rates to indicate system state
- **Factory reset** — wipes NVS + LittleFS + WiFi config, returns to captive portal
- **Rate limiting** — 5 failed auth attempts from same IP triggers 60s block

## Firmware

The main firmware is [`software/water_monitoring/water_monitoring.ino`](software/water_monitoring/water_monitoring.ino).

### Required Arduino Libraries

Install via Arduino IDE Library Manager:

| Library | Author | Purpose |
|---------|--------|---------|
| ArduinoJson v6 | Benoit Blanchon | JSON parsing/serialization |
| DHT sensor library | Adafruit | DHT11 driver |
| WiFiManager | tzapu | WiFi provisioning captive portal |
| PubSubClient | Nick O'Leary | MQTT client |

Only these four are needed. The firmware uses the **synchronous** `WebServer.h`
and the core `Update.h`, both bundled with the ESP32 board package, so no async
web server or async OTA library is required — that choice keeps RAM free for the
TLS stack and the embedded dashboard.

The following come bundled with the ESP32 board package and need no installation:
- `WiFi.h`, `WiFiClientSecure.h`, `HTTPClient.h`, `Preferences.h`, `time.h`
- `WebServer.h` (local HTTP server), `Update.h` (OTA), `ESPmDNS.h`, `LittleFS.h`

### Board Settings (Arduino IDE)

- **Board:** `ESP32S3 Dev Module`
- **Upload Speed:** 921600
- **USB CDC On Boot:** `Enabled`
- **USB Mode:** `USB-OTG (TinyUSB)`
- **Flash Size:** 4MB (or whatever your board has)
- **Partition Scheme:** `Default 4MB with ffat`

## Status LED (P4-5)

The status LED on GPIO 2 blinks at different rates to indicate system state:

| State | Blink Interval | Meaning |
|-------|---------------|---------|
| Very slow (3s) | 3000 ms | System OK, idle |
| Slow (1s) | 1000 ms | Watering in progress |
| Medium (0.5s) | 500 ms | MQTT disconnected (when enabled) |
| Fast (0.2s) | 200 ms | WiFi disconnected |
| Medium-fast (0.25s) | 250 ms | Safety trip triggered |
| Very fast (0.1s) | 100 ms | Sensor fault (DHT11 or moisture) |

If your ESP32-S3 board has a built-in LED on GPIO 2 (most do), no extra wiring needed. Otherwise connect an external LED + 220Ω resistor between GPIO 2 and GND.

## Transport Security

Worth being precise about, because the two directions differ.

**Outbound (device to internet): HTTPS.** Weather, forecast, Telegram and InfluxDB
calls all go through `WiFiClientSecure`. Certificate validation is available but
**off by default** — without a configured root CA the client falls back to
`setInsecure()`, which encrypts the connection but does not verify the server. To
turn validation on, paste the service's root CA into the Configuration card and set
**Validate TLS Certificate** to `true`. Be aware that a pinned CA which later
rotates will break telemetry until the device is reconfigured.

**Inbound (browser to device): plain HTTP on port 80.** There is no HTTPS listener
and no port 443. Basic Auth credentials therefore cross the LAN base64-encoded, not
encrypted. This is a deliberate trade-off — terminating TLS handshakes on the same
core that controls a solenoid valve costs RAM and adds latency to the control loop --
but it means the dashboard should only be exposed on a network you trust, and the
device should never be port-forwarded to the internet.

The local API is instead defended in depth:

| Control | Behaviour |
|---|---|
| HTTP Basic Auth | Required on every endpoint |
| Rate limiting | 5 failed attempts per IP per minute, then a 60 s block (HTTP 429) |
| CSRF | `X-Requested-With` header required on every state-changing POST |
| Access log | Authenticated actions and rate-limit events appended to LittleFS |
| Credential warning | Dashboard flags the `admin`/`admin` default in red |

The CSRF control works because a browser will not attach a custom header to a
cross-origin form post, and adding one to a `fetch()` forces a CORS preflight the
device never approves. It depends on the header being registered with
`server.collectHeaders()` during setup — `WebServer` silently discards any header
not registered there.

## InfluxDB Cloud Setup (P4-2)

1. Create a free account at [cloud2.influxdata.com](https://cloud2.influxdata.com)
2. Create a bucket named `smartfarm`
3. Note your organization ID (visible in the URL when you log in)
4. Generate an API token: **Data → API Tokens → Generate API Token → All Access**
5. In the web UI → **InfluxDB Cloud (v2)** card:
   - Enable InfluxDB: `true`
   - InfluxDB URL: e.g. `https://eu-central-1-1.aws.cloud2.influxdata.com`
   - Organization: your org ID
   - Bucket: `smartfarm`
   - API Token: paste your token
6. Click **Save InfluxDB Config**
7. Click **Send Test Point** to verify

The device pushes data every 60 seconds using line protocol:
```
sensor,device=smartfarm temperature=25.30,humidity=60.50,moisture=45.20,watering=0i,auto_mode=1i
```

## Scheduled Reboot (P4-4)

To prevent memory leaks or stuck states over long uptimes, you can configure a weekly automatic reboot:

1. Web UI → Settings card
2. Set **Scheduled Reboot** to `Enabled`
3. Set **Reboot Day** (0=Sunday, 1=Monday, ..., 6=Saturday)
4. Set **Reboot Hour** (0-23)
5. Save

Default: Monday at 3 AM. A Telegram alert is sent before the reboot.

## Factory Reset (P4-6)

⚠️ **This wipes everything.** Use only as last resort.

1. Web UI → Configuration card
2. Click **Factory Reset**
3. Confirm twice
4. Device wipes:
   - All NVS settings (API key, MQTT, Telegram, thresholds, etc.)
   - All LittleFS data (sensor logs, access logs)
   - WiFi credentials
5. Device reboots into captive portal (`SmartAgri-Setup` WiFi)

## Access Log (P4-3)

All admin actions (control commands, config changes, reboots, rate-limit triggers) are logged to LittleFS. View the last 50 entries in the **Access Log** card in the UI. Use **Clear Logs** to wipe the log file.

## Rate Limiting (P4-7)

To prevent brute-force attacks on the HTTP Basic Auth:

- After 5 failed auth attempts from the same IP within 60 seconds, that IP is blocked for 60 seconds
- Blocked IPs receive HTTP 429 (Too Many Requests)
- The rate-limit event is logged to the access log
- Up to 8 IPs are tracked simultaneously

## Auto-Watering Decision Logic

The auto-mode loop checks all of these conditions before turning the pump ON:

1. Within the configured watering window
2. No rain ≥ 1 mm forecast in the next 3 hours
3. Moisture sensor is healthy (not in a fault state)
4. Soil moisture is below the configured threshold

If conditions 1-3 stop holding mid-watering, the valve closes immediately and a
Telegram alert is sent. Watering also stops once moisture reaches
`threshold + 5%` — the hysteresis band that stops the relay chattering around the
setpoint.

The 5-minute maximum duration is **not** one of the four preconditions. It is a
separate latching check that runs outside auto-mode, so it also covers a valve
opened by hand from the dashboard. When it trips it closes the valve, disables auto
mode, persists that state and alerts — it does not resume on its own.

## Security Notes

- **No hardcoded secrets in source.** All credentials live in NVS and are set at runtime.
- **HTTPS on all outbound calls** (weather, Telegram, InfluxDB), with optional root-CA
  validation that is off by default.
- **The local web server is HTTP only.** Basic Auth credentials are base64-encoded, not
  encrypted, on the LAN. Do not expose this device to the internet.
- **HTTP Basic Auth** on every route.
- **CSRF protection** on every state-changing POST, via a required custom header.
- **Rate limiting** blocks an IP for 60 s after 5 failed auth attempts in a minute.
- **Access log** records authenticated actions and security events for audit.
- **Default credentials are `admin`/`admin`** — change them immediately. The
  Diagnostics card shows in red while the defaults are still in place.
- **A blank username or password disables auth entirely.** The dashboard reports this
  as `AUTH DISABLED`.
- **Backup files contain plaintext secrets** — store them securely.

A frank list of the design's limitations is kept in
[`docs/system-architecture.md`](docs/system-architecture.md#10-known-limitations).
