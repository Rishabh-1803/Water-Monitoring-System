# Changelog

All notable changes to this project are documented in this file.
Format loosely based on [Keep a Changelog](https://keepachangelog.com/).

---

## [P1-Fixed] — 2026-08-02

This release applies the **P1 security and reliability fixes** on top of the
P0-critical fixes from the previous release. The board has also been migrated
from ESP32-C3 Super Mini to ESP32-S3.

### Board Migration: ESP32-C3 Super Mini → ESP32-S3

**Files affected:**
- `software/water_monitoring/water_monitoring.ino` (header comment + board-specific notes)
- `hardware/pin-connections.md` (rewritten for S3)
- `hardware/components-list.md` (updated component + rationale)
- `README.md` (updated board settings, library list)

**Why the change:**
1. GPIO 8 (relay) is a **strapping pin** on the ESP32-C3 — risky for outputs. On the S3 it is a normal GPIO.
2. The S3 has more GPIOs (45 vs 22) — headroom for future expansion (multi-zone valves, flow sensors, status LEDs, etc.).
3. Native USB-OTG support on S3 for USB CDC serial without occupying UART0.
4. More RAM/flash — needed for the TLS stack used by HTTPS weather fetches.

**Pin assignments unchanged** (GPIO 1, 4, 8 are all valid on the S3):
- `MOISTURE_PIN = 1` → ADC1_CH0 on S3
- `DHTPIN = 4` → ADC1_CH3 (used as digital)
- `RELAY_PIN = 8` → general-purpose output (no strapping pin issue on S3)

The previous C3-only strapping-pin warning has been removed; replaced with an
S3 strapping-pin reference table (GPIO 0, 3, 45, 46) in `pin-connections.md`.

---

### Added (P1)

#### P1-1: WiFiManager captive portal (no hardcoded WiFi credentials)
**File:** `software/water_monitoring/water_monitoring.ino`

**Problem:** WiFi SSID and password were hardcoded in source. Even with the
strings cleared (empty `""`), the architecture itself was insecure — anyone
reading the source could see the credential format and where to plug in their
own.

**Fix:** Replaced the manual `WiFi.begin()` with the [tzapu/WiFiManager](https://github.com/tzapu/WiFiManager) library. On first boot (or after a WiFi reset):

1. The ESP32-S3 boots into Access Point mode, broadcasting `SmartAgri-Setup`
2. The user connects to this AP from their phone/laptop
3. A captive portal page automatically opens (or browse to `http://192.168.4.1`)
4. The user enters their WiFi SSID + password
5. The device reboots and connects to the user's WiFi

WiFi credentials are stored by WiFiManager in NVS — they survive reboots. They
are never in the source code.

A new **"Reset WiFi Configuration"** button in the web UI calls `wm.resetSettings()`
and reboots into the captive portal, so the user can reconfigure WiFi without
reflashing.

The captive portal has a 3-minute timeout — if no one configures it, the device
reboots and tries again.

#### P1-2: HTTPS for OpenWeatherMap API
**File:** `software/water_monitoring/water_monitoring.ino`

**Problem:** Weather API calls used `http://`, sending the API key in plaintext
over the internet. Anyone on the path (ISP, coffee-shop WiFi, etc.) could
intercept the key.

**Fix:** Switched to `https://` using `WiFiClientSecure`:

```cpp
WiFiClientSecure client;
client.setInsecure();  // skip cert validation (acceptable tradeoff for ESP32)
HTTPClient http;
http.begin(client, weatherURL);  // weatherURL is now https://...
```

The weather URL is built dynamically in `buildWeatherURL()` after the API key
and city are loaded from NVS. If the user changes the API key or city via the
web UI, the URL is rebuilt and the weather is immediately re-fetched.

**Tradeoff documented:** `setInsecure()` skips TLS certificate validation,
which means the connection is encrypted but not authenticated — vulnerable to
man-in-the-middle attacks. For a hobby/educational project this is acceptable.
For production, bundle the OpenWeatherMap root CA certificate and use
`client.setCACert(root_ca_pem)` instead. This is noted in the code comments
and README.

#### P1-3: HTTP Basic Auth on all routes
**File:** `software/water_monitoring/water_monitoring.ino`

**Problem:** Anyone on the local network could open `http://project.local/`
and toggle the water pump. No authentication of any kind.

**Fix:** Added a `requireAuth()` helper that wraps every route handler:

```cpp
bool requireAuth(AsyncWebServerRequest *request) {
  if (authUser.length() == 0 || authPass.length() == 0) return true;  // not configured
  if (!request->authenticate(authUser.c_str(), authPass.c_str())) {
    request->requestAuthentication();
    return false;
  }
  return true;
}
```

Every `server.on(...)` handler now starts with `if (!requireAuth(request)) return;`.

Credentials are stored in NVS and configurable via the web UI's new
**Configuration** section. Default credentials are `admin`/`admin` — the UI
displays a yellow warning banner until the user changes them.

The `/config` GET endpoint never echoes the password back to the browser. It
only returns `using_default_pass: true/false` so the UI can show the warning.

#### P1-4: WiFi auto-reconnect logic
**File:** `software/water_monitoring/water_monitoring.ino`

**Problem:** If WiFi dropped (router reboot, signal loss, etc.), the device
never tried to reconnect. It would silently lose connectivity forever.

**Fix:** Added a `checkWifiReconnect()` function called from `loop()` every
10 seconds:

```cpp
void checkWifiReconnect() {
  if (millis() - lastWifiCheck < WIFI_CHECK_INTERVAL_MS) return;
  lastWifiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    // wait up to 5s for reconnect
  }
}
```

This catches transient drops without blocking the main loop.

#### P1-5: Max watering duration safety cutoff
**File:** `software/water_monitoring/water_monitoring.ino`

**Problem:** If the moisture sensor fails (e.g., disconnects and reads 4095
= "fully dry"), the auto-watering logic would keep the pump ON forever,
flooding the field. The 5% hysteresis only helped if the sensor was working.

**Fix:** Added a 5-minute hard cutoff. When the pump turns ON, a
`wateringStartTime` timestamp is recorded. In `loop()`, if the pump has been
running for longer than `MAX_WATER_TIME_MS` (300,000 ms = 5 min):

1. The pump is forced OFF (`digitalWrite(RELAY_PIN, HIGH)`)
2. `autoMode` is set to `false` (so it doesn't immediately turn back on)
3. A `safetyTrip` flag is set to `true`
4. The state is persisted to NVS
5. The web UI displays a red warning banner: *"Pump was force-stopped after
   running longer than 5 minutes. Auto-mode has been disabled. Check your
   moisture sensor and re-enable auto-mode manually."*

The user must manually clear the warning by toggling Auto Mode or clicking
Water ON/OFF.

#### P1-6: CSRF protection on POST endpoints
**File:** `software/water_monitoring/water_monitoring.ino`, `software/dashboard/index.html`

**Problem:** The `/control`, `/settings`, and (new) `/config` POST endpoints
accepted any request, including cross-origin requests from other websites.
A malicious website the user visits could trigger `fetch('/control', {method:
'POST', ...})` and toggle their pump.

**Fix:** Added a `requireCsrf()` helper that rejects any POST without an
`X-Requested-With: XMLHttpRequest` header:

```cpp
bool requireCsrf(AsyncWebServerRequest *request) {
  if (!request->hasHeader("X-Requested-With")) {
    request->send(403, "application/json", "{\"success\":false,\"error\":\"missing CSRF header\"}");
    return false;
  }
  return true;
}
```

Browsers will not send custom headers like `X-Requested-With` cross-origin
without a CORS preflight, which the ESP32 server does not grant. So a
malicious third-party page cannot craft a valid request.

The frontend JS now sends the header on every POST:
```javascript
const CSRF_HEADER = {'Content-Type':'application/json','X-Requested-With':'XMLHttpRequest'};
fetch('/control', {method:'POST', headers:CSRF_HEADER, body:...});
```

#### P1-7: All secrets moved to NVS, configurable via web UI
**File:** `software/water_monitoring/water_monitoring.ino`

**Problem:** API key, city, country code were hardcoded in source. Even with
the strings cleared (empty `""`), the architecture forced the user to edit
source code to change them.

**Fix:** All configuration now lives in NVS via the `Preferences` library:

| Setting            | NVS Key     | Default       |
|--------------------|-------------|---------------|
| OpenWeatherMap key | `apiKey`    | `""` (empty)  |
| City               | `city`      | `Ahmedabad`   |
| Country code       | `country`   | `IN`          |
| Web UI username    | `authUser`  | `admin`       |
| Web UI password    | `authPass`  | `admin`       |
| Moisture threshold | `moisture`  | `30.0`        |
| Humidity threshold | `humidity`  | `50.0`        |
| Auto mode enabled  | `automode`  | `true`        |

A new **`/config`** GET/POST endpoint exposes the non-sensitive settings
(API key is shown in plaintext since the user needs to see what's currently
set; password is never echoed back).

The web UI's new **Configuration** card lets the user change all of these
without reflashing. If the API key or city changes, the weather URL is
rebuilt and weather is immediately re-fetched.

---

### Changed

- `setup()` WiFi connection flow rewritten to use `wm.autoConnect()` instead of `WiFi.begin()`.
- Weather URL is now built by `buildWeatherURL()` (called after config load and after config changes).
- All route handlers now start with `if (!requireAuth(request)) return;`.
- All POST route handlers now also call `if (!requireCsrf(request)) return;`.
- Auto-watering logic now records `wateringStartTime` and checks against `MAX_WATER_TIME_MS`.
- `loop()` now calls `checkWifiReconnect()` and the max-watering safety check.
- HTML embedded in `.ino` and the standalone `dashboard/index.html` are kept byte-identical (both updated with the Configuration card, CSRF header, safety-trip banner, and default-creds warning).
- README rewritten with new library list (adds WiFiManager), S3 board settings, and first-boot setup instructions.

### Files Changed

| File | Change |
|------|--------|
| `software/water_monitoring/water_monitoring.ino` | Major rewrite (P1-1 through P1-7) |
| `software/dashboard/index.html` | Major rewrite (matches HTML in .ino) |
| `hardware/pin-connections.md` | Rewritten for ESP32-S3 |
| `hardware/components-list.md` | Updated to S3, added migration rationale |
| `README.md` | Rewritten (S3 settings, new library, first-boot flow, security notes) |
| `CHANGELOG.md` | This file (new P1 section) |

### Files Unchanged

- `docs/abstract.txt`
- `docs/flowchart.png`
- `docs/project-report.docx`
- `docs/system-architecture.jpg`
- `docs/system-architecture.md`
- `media/Front View.jpeg`
- `media/Top View.jpeg`
- `LICENSE`

---

## [P0-Fixed] — 2026-08-02

This release applied only the P0 (critical) fixes identified during code review,
on the ESP32-C3 Super Mini board. See the section below for details.

### Fixed

#### P0-1: Race condition in `/control` POST body handler
The `onBody` callback used a `static String body` shared across all concurrent
requests. Two simultaneous POSTs would corrupt each other's buffer. Replaced
with `request->arg("plain")` (per-request, race-free).

#### P0-2: DHT11 read inside HTTP handler caused stale/NaN data
DHT11 has a minimum 2-second sampling rate. The frontend polled every 5s, but
multiple tabs or clients could cause NaN readings. Added a DHT cache sampled
in `loop()` on a 2.5s cadence; HTTP handler serves cached values.

#### P0-3: Removed meaningless `#define BOARD_HAS_PSRAM 0`
`BOARD_HAS_PSRAM` is a board-package build flag, not a runtime user flag.
Defining it in user code has no effect. Removed.

#### P0-4: Fixed misleading `MOISTURE_PIN` comment
The comment said "changed from 1" but the value was still 1. GPIO 1 is actually
a valid ADC1 channel on ESP32-C3 (ADC1_CH1). Comment corrected.

### Documented (No Code Change)

#### P0-5: Hardcoded OpenWeatherMap API key — rotate immediately
Warning added at the declaration site. Long-term fix (move to Preferences)
implemented in P1-7.

#### P0-6: GPIO 8 is a strapping pin on ESP32-C3
Documented in `pin-connections.md` and in inline code comments. The user
needed to rewire the relay to a non-strapping pin. **Resolved by migrating
to ESP32-S3 in the P1 release** — GPIO 8 is safe on S3.

---

## Next Steps (Not in This Release)

The following P2 items are queued for the next release:

1. **OTA firmware updates** via `AsyncElegantOTA`
2. **MQTT** integration (publish sensor data to Home Assistant / Node-RED)
3. **NTP time sync** + scheduled watering (e.g., "only water between 6 AM and 8 AM")
4. **Forecast-aware watering** (skip watering if rain forecast within 3 hours)
5. **Sensor fault detection** (consecutive NaN counter, sensor-fault status in UI)
6. **HTTPS on local web server** (currently Basic Auth over plaintext HTTP)
7. **Bundle root CA cert** for OpenWeatherMap (replace `setInsecure()`)
8. **History charts** in the UI (Chart.js + SPIFFS data logging)
9. **Flow sensor** for water usage measurement and leak detection
10. **Tank level sensor** to prevent pump dry-run
11. **Telegram / email alerts** for faults and threshold breaches
12. **Multi-zone support** (multiple moisture sensors + valves)
13. **Calibration wizard** in the UI (set MOISTURE_DRY/WET interactively)
