# System Architecture

IoT Water Monitoring and Automated Irrigation System, built on an **ESP32-S3**.

This document describes the system as implemented in
[`software/water_monitoring/water_monitoring.ino`](../software/water_monitoring/water_monitoring.ino).
It supersedes an earlier revision that described the original ESP32-C3 Super Mini prototype and
covered only the sensing, control and actuation path. The firmware has since grown a local web
interface, an authenticated HTTP API, persistent logging, cloud telemetry and over-the-air
updates, and this document now reflects that.

---

## 1. Overview

The device is a self-contained irrigation controller. It samples soil moisture and air
temperature/humidity, decides whether the crop needs water, and drives a solenoid valve through a
relay. Every decision is gated by a set of conditions — time of day, rain forecast, sensor health
and a hard runtime limit — so the valve cannot be left open by a single bad reading or a lost
network connection.

Alongside the control loop, the ESP32-S3 hosts a web dashboard for live values, configuration and
history, and pushes telemetry to MQTT, InfluxDB and Telegram when those are enabled. All
configuration is stored in flash, so the device recovers its full state after a power cut without
needing the network.

The design is organised into six layers. The first three are the classic sensing/processing/
actuation chain; the remaining three were added as the project grew.

| Layer | Responsibility |
|---|---|
| Sensing | Soil moisture (analog) and DHT11 temperature/humidity, with filtering and fault detection |
| Processing and control | Irrigation decision logic, hysteresis, scheduling, safety cut-off |
| Actuation | Relay driving a 12 V solenoid valve, plus a status LED |
| Local interface | HTTP server, embedded dashboard, JSON API, OTA update endpoint |
| Persistence | NVS for configuration, LittleFS for sensor history and the access log |
| Connectivity and telemetry | Wi-Fi provisioning, NTP, weather forecast, MQTT, InfluxDB, Telegram |

---

## 2. Sensing layer

| Sensor | Pin | Interface | Notes |
|---|---|---|---|
| Soil moisture module | GPIO 1 | Analog, ADC1_CH0 | 12-bit resolution, 11 dB attenuation |
| DHT11 | GPIO 4 | Single-wire digital | Temperature and relative humidity |

**Moisture is not read once.** A raw sample from a resistive or capacitive soil probe is noisy, and
because the ADC shares its reference with a switching relay load, single samples are unreliable.
Each measurement therefore takes eight samples 200 µs apart, discards the highest and lowest, and
averages the remaining six. This trimmed mean rejects a single spike without the lag of a long
moving average.

The averaged raw count is then mapped to a percentage using two calibration points held in flash,
`MOISTURE_DRY` (default 4095) and `MOISTURE_WET` (default 1500). The mapping is inverted, because a
resistive probe reads a *higher* ADC count when the soil is drier. Calibration is performed from the
dashboard: the probe is held in air and in water, and the device captures each endpoint.

**Fault detection.** Both sensors are treated as untrustworthy rather than assumed good:

- The moisture channel is considered faulty if the filtered reading is pinned at exactly 0 or 4095
  for ten consecutive reads, which is what an open circuit or a shorted probe looks like.
- The DHT11 is considered faulty after four consecutive failed reads. Until a good read arrives,
  the last known-good temperature and humidity are served from a cache.

A moisture fault blocks automatic watering entirely. Both faults raise a Telegram alert on entry
and a second one on recovery.

The DHT11 cannot be polled faster than about once every two seconds, so it is read on a 2.5 s
cadence and every consumer reads the cache instead of the sensor.

---

## 3. Processing and control layer

Control runs as a single cooperative superloop with a 100 ms delay at the end, so the nominal
cycle time is a little over 100 ms. There is no RTOS scheduler and no preemption: every subsystem
is polled, and each long-running action (an HTTPS request, a flash write) briefly stretches the
loop. Latency figures for this system should be quoted against that ~10 Hz cadence.

### Irrigation decision

In automatic mode the valve is opened only when **all** of the following hold:

1. The current time is inside the configured watering window (default 06:00–08:00).
2. No rain is expected — the three-hour forecast predicts less than 1.0 mm.
3. The moisture sensor is not in a fault state.
4. Measured moisture is below the configured threshold (default 30%).

If any of the first three conditions stops holding while the valve is open, watering stops
immediately. This matters: it means a rain forecast arriving mid-cycle, or the probe failing
mid-cycle, closes the valve rather than waiting for a moisture reading that may never come.

### Hysteresis

Watering stops when moisture rises to `threshold + 5%`, not at the threshold itself. Without that
gap the valve would chatter on and off around the setpoint as wet soil settles, cycling the relay
and the pump unnecessarily.

### Safety cut-off

An independent timer, checked outside the automatic-mode branch, closes the valve if it has been
open for more than five minutes. When it trips the firmware also disables automatic mode, sets a
`safetyTrip` flag, writes the new state to flash and raises an alert. It is deliberately
latching — a stuck valve, a dry well or a miscalibrated probe should require a human to look at the
device, not silently resume on the next loop.

This check sits outside the `autoMode` branch on purpose, so it also covers a valve opened manually
from the dashboard.

### Manual override

A manual command from the dashboard turns automatic mode off. The device does not silently fight
the operator, and the mode is persisted, so a reboot does not resurrect automatic watering that was
deliberately turned off.

---

## 4. Actuation layer

| Output | Pin | Behaviour |
|---|---|---|
| Relay module (IN) | GPIO 8 | **Active low** — `LOW` energises the relay and opens the valve |
| Status LED | GPIO 2 | Blink rate encodes Wi-Fi, MQTT and fault state |

The relay is driven active-low, and GPIO 8 is initialised `HIGH` before anything else in `setup()`
so the valve is closed during boot rather than pulsing open while the firmware starts. The relay
switches the 12 V solenoid; the ESP32-S3 never carries valve current.

The status LED gives the device a diagnosis path that does not require a browser — useful when the
problem is that the dashboard is unreachable.

---

## 5. Local interface layer

An HTTP server on port 80 serves a single-page dashboard stored in flash as a PROGMEM string, so
the UI needs no filesystem and no internet connection. The page polls a JSON API.

The local interface is **plain HTTP**. TLS on the device would mean holding a certificate and a
private key on a hobby-grade board, and terminating handshakes in the same loop that controls a
valve; the memory and latency cost was not judged worthwhile for a LAN-only interface. Outbound
calls to cloud services do use HTTPS.

Endpoints, grouped by purpose:

| Group | Endpoints |
|---|---|
| Live data | `GET /sensor-data`, `GET /system-status`, `GET /weather`, `GET /time` |
| Control | `POST /control` (water on/off, auto-mode toggle) |
| Thresholds and schedule | `GET`/`POST /settings` |
| Device configuration | `GET`/`POST /config` (credentials, API keys, MQTT, Influx, Telegram, TLS) |
| Calibration | `GET`/`POST /calibrate` |
| History | `GET /history` (24 h trend JSON), `GET /export.csv` |
| Diagnostics | `GET /diagnostics`, `GET /logs`, `POST /logs/clear` |
| Connectivity tests | `POST /telegram/test`, `POST /influx/test` |
| Maintenance | `GET /backup`, `POST /restore`, `POST /factory-reset`, `POST /reboot`, `POST /reset-wifi` |
| Firmware | `GET`/`POST /update` (OTA) |

### Security controls

**HTTP Basic Authentication** guards every endpoint. Note that a blank username or password
disables authentication entirely; the dashboard reports this state, and it should never be used on
a shared network.

**Per-IP rate limiting** allows five failed authentication attempts per minute per address, then
blocks that address for sixty seconds. The table is a fixed-size array of slots, reclaimed
oldest-inactive-first, because a dynamic structure keyed on client IP is exactly the thing an
attacker would use to exhaust heap on a device with a few hundred kilobytes of it.

**CSRF protection** requires the header `X-Requested-With` on every state-changing POST. A browser
will not attach a custom header to a cross-origin form submission, and adding one to a `fetch()`
forces a CORS preflight that the device never approves — so a malicious page cannot forge a request
that opens the valve, even while the operator's browser holds valid Basic Auth credentials. This
relies on the header being registered with `server.collectHeaders()` during setup; `WebServer`
discards any header not registered there, so without that registration the check cannot see the
header and rejects every POST.

**An append-only access log** in LittleFS records authenticated requests, rate-limit events and
credential warnings, capped at 50 KB and readable from the dashboard.

**Credential hygiene** is reported rather than assumed. The device ships with `admin`/`admin`, and
the dashboard shows in red whether those defaults are still in place — a default credential that
nobody is reminded about is the most likely way this device gets compromised.

---

## 6. Persistence layer

| Store | Contents | Limits |
|---|---|---|
| NVS (`Preferences`, namespace `watering`) | Credentials, thresholds, calibration, watering window, MQTT/Influx/Telegram/TLS config, pump statistics, mode flags | Wear-levelled; written only on change |
| LittleFS `/log.csv` | Timestamped sensor history, written every 60 s | Rotated at 200 KB |
| LittleFS `/access.log` | Security and access events | Rotated at 50 KB |

Both log files are size-capped and rotated, because an embedded filesystem that fills up fails in
confusing ways — writes start failing silently while the device otherwise appears healthy.

Pump cycle count and cumulative runtime are persisted too, which turns the device into a crude
maintenance record: a valve or pump that has run far longer than expected is visible in
diagnostics.

Configuration can be exported through `GET /backup` and restored through `POST /restore`, so a
device can be reprovisioned without retyping every API key.

---

## 7. Connectivity and telemetry layer

**Wi-Fi provisioning** uses WiFiManager. On first boot, or when no known network is available, the
device raises an access point named `SmartAgri-Setup` and serves a captive portal for 180 seconds,
then reboots. No credentials are compiled into the firmware. The device is reachable at
`http://project.local/` via mDNS as well as by IP.

**Reconnection** is checked every 10 seconds; disconnect and reconnect both raise alerts, so a
device that quietly dropped off the network is visible.

**Time** comes from NTP (`pool.ntp.org`, `time.nist.gov`) at UTC+05:30. Time matters more than it
looks: the watering window, the history timestamps and the scheduled reboot all depend on it.

**Weather and forecast** come from OpenWeatherMap — current conditions every 10 minutes, the
three-hour forecast every 30 minutes. The forecast is what feeds the rain-suppression condition in
the control logic.

**MQTT** publishes sensor and status data every 30 seconds under a configurable topic prefix
(default `smartfarm`), reconnecting every 5 seconds when the broker is unreachable, and subscribes
for remote commands.

**InfluxDB v2** receives a line-protocol point every 60 seconds over HTTPS, which is what makes
long-term trend analysis possible beyond the 200 KB kept on-device.

**Telegram** delivers alerts for eleven distinct events — sensor faults and recoveries, watering
start and stop, safety trips, Wi-Fi and MQTT state changes, and boot. Each alert type has its own
five-minute cooldown, so a flapping sensor cannot turn the notification channel into noise the
operator learns to ignore.

**TLS** for all outbound HTTPS uses a configured root CA when one is supplied, and falls back to
`setInsecure()` otherwise. The fallback is a deliberate, documented trade-off for a teaching
platform, not an oversight: certificate validation is available and off by default because pinning
a CA that later rotates bricks the telemetry path with no easy recovery on an unattended device.

**OTA updates** are accepted at `/update` using the core `Update` library rather than a full OTA
web framework, to keep RAM available for the TLS stack and the web server.

**Scheduled reboot** is optional, configurable by weekday and hour, and off by default — a
pragmatic mitigation for slow resource leaks in a long-running device.

---

## 8. Data and control flow

```
                    ┌──────────────────────────────────────────┐
  Soil moisture ───►│  8-sample trimmed mean → calibration map │
  (GPIO 1, ADC)     │  → fault check                          │
                    └──────────────────┬───────────────────────┘
                                       │  moisture %
  DHT11 ───────────► 2.5 s cache ──────┤  temperature, humidity
  (GPIO 4)           + fault check     │
                                       ▼
  OpenWeatherMap ──► 3 h forecast ──►┌─────────────────────────┐
  NTP ─────────────► local time ────►│  Irrigation decision    │
  Dashboard ───────► thresholds ────►│  window ∧ ¬rain ∧ ¬fault│
                                     │  ∧ moisture < threshold │
                                     └────────────┬────────────┘
                                                  │
                     ┌────────────────────────────┼───────────────────────┐
                     ▼                            ▼                       ▼
            Relay (GPIO 8, active low)   5-minute safety timer    Status LED (GPIO 2)
                     │                            │
                     ▼                     latches valve closed,
            12 V solenoid valve            disables auto mode
                     │
                     ▼
              Water to crop

  Every cycle also: serve HTTP clients, publish MQTT (30 s), push InfluxDB (60 s),
  append /log.csv (60 s), evaluate alert cooldowns, check Wi-Fi, check scheduled reboot.
```

---

## 9. Design characteristics

**Layered and replaceable.** Sensor acquisition, decision logic and actuation are separate
functions with narrow interfaces. Swapping the DHT11 for an I²C sensor, or the relay for a MOSFET
driver, touches one layer.

**Fails closed.** Every failure path — sensor fault, safety timeout, boot, lost network — leaves
the valve shut. Wasting water is the expensive failure; missing one watering window is not.

**Survives power loss.** Configuration, calibration, mode and pump statistics all live in NVS.

**Observable.** Live values, 24-hour trends, CSV export, an access log, pump statistics and a
status LED mean the device can be diagnosed both with and without a browser.

**Defence in depth on the local API.** Authentication, rate limiting, CSRF protection and access
logging are independent controls rather than one mechanism.

---

## 10. Known limitations

Stated plainly, because they are the right starting points for the next revision:

- The local web interface is HTTP, so Basic Auth credentials cross the LAN base64-encoded rather
  than encrypted. Acceptable on a trusted network; not acceptable on an open one.
- Outbound certificate validation is off by default.
- Default credentials are `admin`/`admin`. The dashboard flags this, but the firmware does not
  force a change on first login.
- Control is a cooperative superloop, so a slow HTTPS request or flash write stretches the control
  cycle. A production build would move irrigation control to its own task.
- Firmware is a single ~2400-line `.ino` translation unit. Splitting it into modules would make it
  testable.
- The DHT11 is a low-accuracy part (±2 °C, ±5% RH) and is the limiting factor on data quality.
- There is no flow sensor, so the firmware cannot tell that the valve opened and water actually
  moved. The five-minute timer is a proxy for that missing feedback.

---

## 11. Hardware reference

Full component list: [`../hardware/components-list.md`](../hardware/components-list.md)
Pin-by-pin wiring, ADC notes and ESP32-S3 strapping pins:
[`../hardware/pin-connections.md`](../hardware/pin-connections.md)

The project was migrated from the ESP32-C3 Super Mini to the ESP32-S3 for GPIO headroom, to avoid a
strapping-pin conflict on GPIO 8, and for the additional RAM and flash needed once WiFiManager, the
TLS stack, the web server and OTA were all resident. The rationale is documented in the hardware
files.
