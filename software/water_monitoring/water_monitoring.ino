// =============================================================================
// Smart Agricultural System - ESP32-S3
// Water Monitoring + Auto Irrigation + Weather + Trends + Cloud + Hardening
// -----------------------------------------------------------------------------
// This build serves the local web UI over plain HTTP only (port 80).
// Outbound calls to cloud services (OpenWeatherMap, Telegram, InfluxDB) still
// use HTTPS via WiFiClientSecure.
// OTA Updates are handled via the core ESP32 Update.h library to save RAM.
// =============================================================================

// ====== Includes ======
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DHT.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <time.h>
#include <PubSubClient.h>
#include <Update.h>

// ====== Hardware Pins (ESP32-S3) ======
#define DHTPIN 4
#define DHTTYPE DHT11
#define MOISTURE_PIN 1
#define RELAY_PIN 8
#define STATUS_LED_PIN 2

// ====== Objects ======
DHT dht(DHTPIN, DHTTYPE);
Preferences preferences;
WebServer server(80);
WiFiClient mqttWifiClient;
PubSubClient mqtt(mqttWifiClient);

// ====== Device Configuration =====================
String apiKey       = "";
String city         = "Ahmedabad";
String countryCode  = "IN";
String authUser     = "admin";
String authPass     = "admin";
String weatherURL   = "";
String forecastURL  = "";

String weatherDescription = "";
float  weatherTemp = NAN;
float  weatherHum  = NAN;
String locationName = "—";

float  rainNext3h = 0.0f;
bool   rainExpected = false;
const float RAIN_THRESHOLD_MM = 1.0f;

unsigned long lastWeatherUpdate = 0;
unsigned long lastForecastUpdate = 0;
const unsigned long WEATHER_INTERVAL_MS  = 600000UL;
const unsigned long FORECAST_INTERVAL_MS = 1800000UL;

// ====== Control & Thresholds ======
bool watering = false;
bool autoMode = true;
float moistureThreshold = 30.0f;
float humidityThreshold = 50.0f;

int MOISTURE_DRY = 4095;
int MOISTURE_WET = 1500;

// ====== DHT11 Reading Cache ======
float  cachedTemp = NAN;
float  cachedHum  = NAN;
unsigned long lastDhtRead = 0;
const unsigned long DHT_INTERVAL_MS = 2500UL;

// ====== Sensor Fault Detection ======
int  dhtFailCount = 0;
bool dhtFault = false;
int  moistureFailCount = 0;
bool moistureFault = false;
const int DHT_FAULT_THRESHOLD = 4;
const int MOISTURE_FAULT_THRESHOLD = 10;

// ====== Max Watering Safety ======
unsigned long wateringStartTime = 0;
const unsigned long MAX_WATER_TIME_MS = 300000UL;
bool safetyTrip = false;

// ====== Scheduled Watering Window ======
int wateringStartHour = 6;
int wateringEndHour   = 8;

// ====== WiFi Reconnect ======
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000UL;
unsigned long lastWifiDisconnect = 0;
bool wifiWasDisconnected = false;

// ====== MQTT Config ======
String mqttBroker = "";
int    mqttPort = 1883;
String mqttUser = "";
String mqttPass = "";
String mqttTopicPrefix = "smartfarm";
bool   mqttEnabled = false;
bool   mqttConnectedNow = false;
unsigned long lastMqttReconnect = 0;
unsigned long lastMqttPublish = 0;
const unsigned long MQTT_PUBLISH_INTERVAL_MS = 30000UL;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000UL;
const size_t       MQTT_MAX_PACKET = 1024;

// ====== Telegram Config ======
String telegramBotToken = "";
String telegramChatId = "";
bool   telegramEnabled = false;
unsigned long lastTelegramAlert[12] = {0};
const unsigned long TELEGRAM_COOLDOWN_MS = 300000UL;

enum AlertType {
  ALERT_DHT_FAULT = 0, ALERT_DHT_RECOVER, ALERT_MOISTURE_FAULT,
  ALERT_MOISTURE_RECOVER, ALERT_SAFETY_TRIP, ALERT_WATERING_START,
  ALERT_WATERING_STOP, ALERT_WIFI_DISCONNECT, ALERT_WIFI_RECONNECT,
  ALERT_MQTT_DISCONNECT, ALERT_BOOT, ALERT_COUNT
};

// ====== TLS Cert Validation ======
bool   validateTlsCert = false;
String rootCaPem = "";

// ====== Pump Cycle Counter ======
unsigned int pumpCycles = 0;
unsigned long pumpTotalRuntimeSec = 0;
unsigned long pumpStartTime = 0;

// ====== Diagnostics ======
unsigned long bootTime = 0;
String lastBootTimeStr = "—";

// ====== Credential Hygiene ======
// Recomputed at boot and whenever credentials change. Exposed through
// /diagnostics so a device still running on factory credentials shows up in
// the dashboard instead of silently staying open to anyone on the LAN.
bool usingDefaultCredentials = false;
bool httpAuthDisabled = false;

// ====== History Logging ======
const char* LOG_FILE = "/log.csv";
const char* ACCESS_LOG_FILE = "/access.log";
const unsigned long LOG_INTERVAL_MS = 60000UL;
unsigned long lastLogTime = 0;
const size_t  MAX_LOG_SIZE = 200000;
const size_t  MAX_ACCESS_LOG_SIZE = 50000;

// ====== NTP / Time ======
const long  GMT_OFFSET_SEC = 19800;
const int   DST_OFFSET_SEC = 0;
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

String getLocalTimeStr() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) return "—";
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

int getLocalHour() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) return -1;
  return timeinfo.tm_hour;
}

int getLocalWeekday() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) return -1;
  return timeinfo.tm_wday;
}

String getIsoTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 2000)) return String(millis());
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buf);
}

// ====== InfluxDB Config ======
bool   influxEnabled = false;
String influxUrl = "";
String influxOrg = "";
String influxBucket = "smartfarm";
String influxToken = "";
unsigned long lastInfluxWrite = 0;
const unsigned long INFLUX_INTERVAL_MS = 60000UL;

// ====== Scheduled Reboot ======
bool   scheduledRebootEnabled = false;
int    scheduledRebootWeekday = 1;
int    scheduledRebootHour = 3;
bool   lastScheduledRebootCheck = false;
unsigned long lastRebootCheckMinute = 0;

// ====== Status LED ======
unsigned long lastLedUpdate = 0;
int ledState = LOW;
unsigned long ledBlinkInterval = 1000;

// ====== Rate Limiting ======
struct RateLimitEntry {
  IPAddress ip;
  int failCount;
  unsigned long firstFailTime;
  unsigned long blockedUntil;
};
const int MAX_RATE_LIMIT_ENTRIES = 8;
RateLimitEntry rateLimits[MAX_RATE_LIMIT_ENTRIES];

// ====== HTML (single page app) ======
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0"/>
<title>Smart Agricultural System</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
:root{
  --primary:#2c3e50;--secondary:#3498db;--accent:#e74c3c;--light:#ecf0f1;--dark:#2c3e50;--success:#27ae60;
  --warn:#e67e22;
}
*{box-sizing:border-box}
body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;margin:0;background:#f5f5f5;color:var(--dark)}
.container{max-width:980px;margin:0 auto;padding:16px}
header{background:linear-gradient(120deg,#89f7fe,#66a6ff);color:#073b4c;padding:22px;border-radius:14px;margin:10px 0;text-align:center}
.grid{display:grid;gap:16px}
@media(min-width:860px){.grid{grid-template-columns:2fr 1fr}}
.card{background:#fff;border-radius:12px;box-shadow:0 6px 18px rgba(0,0,0,.08);padding:16px}
.card h2{margin:0 0 10px 0}
.sensor-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
.sensor-box{background:var(--light);border-radius:10px;padding:12px;text-align:center;position:relative}
.sensor-name{font-weight:600}
.sensor-value{font-size:26px;font-weight:800;margin:8px 0}
.fault-badge{position:absolute;top:6px;right:6px;background:#c0392b;color:#fff;font-size:10px;padding:2px 6px;border-radius:999px;display:none}
.fault-badge.show{display:inline-block}
.status{padding:10px;border-radius:8px;margin:8px 0;font-weight:700;text-align:center}
.status-auto{background:#d5f5e3;color:var(--success)}
.status-manual{background:#fdebd0;color:var(--warn)}
.status-on{background:#d5f5e3;color:#27ae60}
.status-off{background:#fadbd8;color:#c0392b}
.controls{display:flex;gap:10px;flex-wrap:wrap}
.btn{padding:10px 16px;border:none;border-radius:8px;cursor:pointer;font-weight:700;transition:transform .1s ease,opacity .1s ease;text-decoration:none;display:inline-block}
.btn:active{transform:scale(.98)}
.btn-primary{background:var(--secondary);color:#fff}
.btn-danger{background:var(--accent);color:#fff}
.btn-success{background:var(--success);color:#fff}
.btn-warn{background:var(--warn);color:#fff}
.form-row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.input{width:100%;padding:10px;border:1px solid #ddd;border-radius:8px}
textarea.input{font-family:monospace;font-size:11px}
.notice{font-size:12px;opacity:.7}
.kv{display:flex;justify-content:space-between;margin:6px 0}
.badge{display:inline-block;background:#eef;border-radius:999px;padding:6px 10px;font-weight:700}
.badge-rain{background:#aed6f1;color:#1b4f72}
.badge-ok{background:#d5f5e3;color:#1e8449}
.badge-bad{background:#fadbd8;color:#c0392b}
.small{font-size:12px}
.warn-box{background:#fff3cd;border:1px solid #ffe69c;color:#664d03;padding:10px;border-radius:8px;margin:8px 0;font-size:13px}
.danger-box{background:#f8d7da;border:1px solid #f1aeb5;color:#58151c;padding:10px;border-radius:8px;margin:8px 0;font-size:13px}
.info-box{background:#d1ecf1;border:1px solid #bee5eb;color:#0c5460;padding:10px;border-radius:8px;margin:8px 0;font-size:13px}
.chart-container{position:relative;height:300px}
.log-viewer{background:#1e1e1e;color:#d4d4d4;padding:12px;border-radius:8px;font-family:monospace;font-size:11px;max-height:300px;overflow-y:auto;white-space:pre-wrap}
</style>
</head>
<body>
<div class="container">

  <header>
    <h1>Smart Agricultural System</h1>
    <div class="small">ESP32-S3 • Cloud + Hardening • Weather + Trends + MQTT + Alerts + InfluxDB + OTA</div>
    <div class="small" id="clock" style="margin-top:4px">—</div>
  </header>

  <div id="safety-warn" class="danger-box" style="display:none">
    <strong>Safety Trip:</strong> Pump was force-stopped after running longer than 5 minutes.
    Auto-mode has been disabled. Check your moisture sensor and re-enable auto-mode manually.
  </div>

  <div id="default-creds-warn" class="warn-box" style="display:none">
    <strong>Security Warning:</strong> Web UI is using default credentials (admin/admin).
    Change them in the Configuration section below.
  </div>

  <div id="rain-warn" class="info-box" style="display:none">
    <strong>Rain forecast:</strong> <span id="rain-detail">—</span> mm expected in the next 3 hours.
    Auto-watering is paused to avoid overwatering.
  </div>

  <div id="fault-warn" class="danger-box" style="display:none">
    <strong>Sensor Fault:</strong> <span id="fault-detail">—</span>
    Please check sensor wiring and replace if necessary.
  </div>

  <div class="grid">
    <div class="card">
      <h2>Field Sensors</h2>
      <div class="sensor-grid">
        <div class="sensor-box">
          <span class="fault-badge" id="temp-fault">FAULT</span>
          <div class="sensor-name">Temperature</div>
          <div class="sensor-value" id="temperature">--</div>
          <div class="sensor-unit">°C</div>
        </div>
        <div class="sensor-box">
          <span class="fault-badge" id="hum-fault">FAULT</span>
          <div class="sensor-name">Humidity</div>
          <div class="sensor-value" id="humidity">--</div>
          <div class="sensor-unit">%</div>
        </div>
        <div class="sensor-box">
          <span class="fault-badge" id="moist-fault">FAULT</span>
          <div class="sensor-name">Soil Moisture</div>
          <div class="sensor-value" id="moisture">--</div>
          <div class="sensor-unit">%</div>
        </div>
      </div>

      <div class="status" id="system-status">System Status: Loading...</div>
      <div class="status" id="relay-status">Motor Status: Loading...</div>

      <div class="controls">
        <button class="btn btn-primary" id="water-on">Turn Water ON</button>
        <button class="btn btn-danger"  id="water-off">Turn Water OFF</button>
        <button class="btn btn-success" id="auto-mode">Auto Mode: ON</button>
      </div>
    </div>

    <div class="card">
      <h2>Local Weather</h2>
      <div class="kv"><span>Location</span><span class="badge" id="w-name">—</span></div>
      <div class="kv"><span>Conditions</span><span id="w-desc">—</span></div>
      <div class="kv"><span>Temperature</span><span id="w-temp">—</span></div>
      <div class="kv"><span>Humidity</span><span id="w-hum">—</span></div>
      <div class="kv"><span>Rain (next 3h)</span><span class="badge badge-rain" id="w-rain">—</span></div>
      <div class="controls" style="margin-top:10px">
        <button class="btn btn-primary" id="w-refresh">Refresh Weather</button>
      </div>
      <div class="notice">Current conditions every 10 min. Forecast every 30 min.</div>
    </div>
  </div>

  <div class="card">
    <h2>Trends (last 24 hours)</h2>
    <div class="chart-container">
      <canvas id="trends-chart"></canvas>
    </div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-primary" id="refresh-chart">Refresh Chart</button>
      <a class="btn btn-success" href="/export.csv" target="_blank">Download CSV</a>
    </div>
    <div class="notice">Data is logged every 60 seconds on the device.</div>
  </div>

  <div class="card">
    <h2>Diagnostics</h2>
    <div class="kv"><span>Uptime</span><span id="diag-uptime">—</span></div>
    <div class="kv"><span>Free Heap</span><span id="diag-heap">—</span></div>
    <div class="kv"><span>WiFi RSSI</span><span id="diag-rssi">—</span></div>
    <div class="kv"><span>WiFi SSID</span><span id="diag-ssid">—</span></div>
    <div class="kv"><span>IP Address</span><span id="diag-ip">—</span></div>
    <div class="kv"><span>LittleFS Used</span><span id="diag-fs">—</span></div>
    <div class="kv"><span>MQTT Status</span><span id="diag-mqtt">—</span></div>
    <div class="kv"><span>InfluxDB Status</span><span id="diag-influx">—</span></div>
    <div class="kv"><span>Pump Cycles</span><span id="diag-cycles">—</span></div>
    <div class="kv"><span>Pump Total Runtime</span><span id="diag-runtime">—</span></div>
    <div class="kv"><span>LED State</span><span id="diag-led">—</span></div>
    <div class="kv"><span>Last Boot</span><span id="diag-boot">—</span></div>
    <div class="kv"><span>Scheduled Reboot</span><span id="diag-sched">—</span></div>
    <div class="kv"><span>Credentials</span><span id="diag-creds">—</span></div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-primary" id="diag-refresh">Refresh</button>
      <button class="btn btn-warn" id="diag-reboot">Reboot Device</button>
    </div>
  </div>

  <div class="card">
    <h2>Access Log (last 50 events)</h2>
    <div class="log-viewer" id="access-log">Loading...</div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-primary" id="refresh-logs">Refresh Logs</button>
      <button class="btn btn-warn" id="clear-logs">Clear Logs</button>
    </div>
  </div>

  <div class="card">
    <h2>Settings</h2>
    <div class="form-row">
      <div>
        <label>Moisture Threshold (%)</label>
        <input id="moisture-threshold" class="input" type="number" step="1" min="0" max="100" />
      </div>
      <div>
        <label>Humidity Alert Threshold (%)</label>
        <input id="humidity-threshold" class="input" type="number" step="1" min="0" max="100" />
      </div>
    </div>
    <div class="form-row" style="margin-top:10px">
      <div>
        <label>Watering Window Start Hour (0-23)</label>
        <input id="water-start-hour" class="input" type="number" step="1" min="0" max="23" />
      </div>
      <div>
        <label>Watering Window End Hour (0-23)</label>
        <input id="water-end-hour" class="input" type="number" step="1" min="0" max="23" />
      </div>
    </div>
    <div class="form-row" style="margin-top:10px">
      <div>
        <label>Scheduled Reboot</label>
        <select id="sched-reboot-enabled" class="input">
          <option value="false">Disabled</option>
          <option value="true">Enabled</option>
        </select>
      </div>
      <div>
        <label>Reboot Day (0=Sun, 6=Sat)</label>
        <input id="sched-reboot-day" class="input" type="number" step="1" min="0" max="6" />
      </div>
    </div>
    <div class="form-row" style="margin-top:10px">
      <div>
        <label>Reboot Hour (0-23)</label>
        <input id="sched-reboot-hour" class="input" type="number" step="1" min="0" max="23" />
      </div>
      <div></div>
    </div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-success" id="save-settings">Save</button>
    </div>
  </div>

  <div class="card">
    <h2>Sensor Calibration</h2>
    <div class="kv"><span>Current Raw ADC</span><span class="badge" id="calib-raw">—</span></div>
    <div class="kv"><span>Dry Calibration (raw)</span><span class="badge" id="calib-dry">—</span></div>
    <div class="kv"><span>Wet Calibration (raw)</span><span class="badge" id="calib-wet">—</span></div>
    <div class="notice">Place sensor in dry air, click "Set Dry". Place in water, click "Set Wet".</div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-warn" id="calib-dry-btn">Set Dry</button>
      <button class="btn btn-primary" id="calib-wet-btn">Set Wet</button>
      <button class="btn btn-success" id="calib-refresh">Refresh</button>
    </div>
  </div>

  <div class="card">
    <h2>Configuration</h2>
    <div class="form-row">
      <div>
        <label>OpenWeatherMap API Key</label>
        <input id="cfg-api-key" class="input" type="text" placeholder="Enter API key" />
      </div>
      <div>
        <label>City</label>
        <input id="cfg-city" class="input" type="text" placeholder="Ahmedabad" />
      </div>
    </div>
    <div class="form-row" style="margin-top:10px">
      <div>
        <label>Country Code (2 letters)</label>
        <input id="cfg-country" class="input" type="text" maxlength="2" placeholder="IN" />
      </div>
      <div>
        <label>Web UI Username</label>
        <input id="cfg-auth-user" class="input" type="text" placeholder="admin" />
      </div>
    </div>
    <div class="form-row" style="margin-top:10px">
      <div>
        <label>New Web UI Password (leave blank to keep current)</label>
        <input id="cfg-auth-pass" class="input" type="password" placeholder="••••••" />
      </div>
      <div>
        <label>Validate TLS Cert for outbound calls (advanced)</label>
        <select id="cfg-tls-validate" class="input">
          <option value="false">Disabled (insecure, default)</option>
          <option value="true">Enabled (paste root CA below)</option>
        </select>
      </div>
    </div>
    <div style="margin-top:10px">
      <label>Root CA Certificate (PEM format, optional)</label>
      <textarea id="cfg-root-ca" class="input" rows="3" placeholder="-----BEGIN CERTIFICATE-----&#10;...&#10;-----END CERTIFICATE-----"></textarea>
    </div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-success" id="save-config">Save Configuration</button>
      <button class="btn btn-warn" id="reset-wifi">Reset WiFi</button>
      <a class="btn btn-primary" href="/update" target="_blank">OTA Update</a>
      <button class="btn btn-primary" id="backup-config">Backup Config</button>
      <button class="btn btn-warn" id="restore-config">Restore Config</button>
      <button class="btn btn-danger" id="factory-reset">Factory Reset</button>
    </div>
    <div class="warn-box" style="margin-top:10px">
      <strong>Note:</strong> The web UI is served over plain HTTP on your local network.
      Credentials are sent with HTTP Basic Auth, which is not encrypted on the wire —
      only use this on a trusted local network.
    </div>
    <div class="danger-box" style="margin-top:8px">
      <strong>Factory Reset:</strong> Wipes ALL data (NVS, LittleFS, WiFi config, sensor logs).
      Device reboots into captive portal. Use only as last resort.
    </div>
  </div>

  <div class="card">
    <h2>MQTT Integration</h2>
    <div class="form-row">
      <div>
        <label>Enable MQTT</label>
        <select id="cfg-mqtt-enabled" class="input">
          <option value="false">Disabled</option>
          <option value="true">Enabled</option>
        </select>
      </div>
      <div>
        <label>Broker Host</label>
        <input id="cfg-mqtt-broker" class="input" type="text" placeholder="broker.hivemq.com" />
      </div>
    </div>
    <div class="form-row" style="margin-top:10px">
      <div>
        <label>Broker Port</label>
        <input id="cfg-mqtt-port" class="input" type="number" placeholder="1883" />
      </div>
      <div>
        <label>Topic Prefix</label>
        <input id="cfg-mqtt-prefix" class="input" type="text" placeholder="smartfarm" />
      </div>
    </div>
    <div class="form-row" style="margin-top:10px">
      <div>
        <label>Username (optional)</label>
        <input id="cfg-mqtt-user" class="input" type="text" />
      </div>
      <div>
        <label>Password (optional)</label>
        <input id="cfg-mqtt-pass" class="input" type="password" />
      </div>
    </div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-success" id="save-mqtt">Save MQTT Config</button>
    </div>
    <div class="notice">
      Publishes: <code>&lt;prefix&gt;/sensor/{temperature,humidity,moisture,relay}</code> every 30s, <code>&lt;prefix&gt;/status</code> on change.<br>
      Subscribes: <code>&lt;prefix&gt;/cmd/{water_on,water_off,auto_mode}</code>
    </div>
  </div>

  <div class="card">
    <h2>Telegram Alerts</h2>
    <div class="form-row">
      <div>
        <label>Enable Telegram</label>
        <select id="cfg-tg-enabled" class="input">
          <option value="false">Disabled</option>
          <option value="true">Enabled</option>
        </select>
      </div>
      <div>
        <label>Chat ID</label>
        <input id="cfg-tg-chat" class="input" type="text" placeholder="123456789" />
      </div>
    </div>
    <div style="margin-top:10px">
      <label>Bot Token</label>
      <input id="cfg-tg-token" class="input" type="text" placeholder="123456789:ABCdef..." />
    </div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-success" id="save-telegram">Save Telegram Config</button>
      <button class="btn btn-primary" id="test-telegram">Send Test Alert</button>
    </div>
  </div>

  <div class="card">
    <h2>InfluxDB Cloud (v2)</h2>
    <div class="form-row">
      <div>
        <label>Enable InfluxDB</label>
        <select id="cfg-influx-enabled" class="input">
          <option value="false">Disabled</option>
          <option value="true">Enabled</option>
        </select>
      </div>
      <div>
        <label>Bucket</label>
        <input id="cfg-influx-bucket" class="input" type="text" placeholder="smartfarm" />
      </div>
    </div>
    <div class="form-row" style="margin-top:10px">
      <div>
        <label>Organization</label>
        <input id="cfg-influx-org" class="input" type="text" placeholder="your-email@example.com" />
      </div>
      <div></div>
    </div>
    <div style="margin-top:10px">
      <label>InfluxDB URL</label>
      <input id="cfg-influx-url" class="input" type="text" placeholder="https://eu-central-1-1.aws.cloud2.influxdata.com" />
    </div>
    <div style="margin-top:10px">
      <label>API Token</label>
      <input id="cfg-influx-token" class="input" type="password" placeholder="your-influx-token-here" />
    </div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-success" id="save-influx">Save InfluxDB Config</button>
      <button class="btn btn-primary" id="test-influx">Send Test Point</button>
    </div>
    <div class="notice">
      Pushes sensor data every 60s using <a href="https://docs.influxdata.com/influxdb/v2/reference/syntax/line-protocol/" target="_blank">line protocol</a>.
      Get a free account at <a href="https://cloud2.influxdata.com" target="_blank">cloud2.influxdata.com</a>.
    </div>
  </div>

</div>

<script>
const CSRF_HEADER = {'Content-Type':'application/json','X-Requested-With':'XMLHttpRequest'};
const $ = id => document.getElementById(id);

const temperatureEl = $('temperature'), humidityEl = $('humidity'), moistureEl = $('moisture');
const systemStatusEl = $('system-status'), relayStatusEl = $('relay-status'), clockEl = $('clock');
const waterOnBtn = $('water-on'), waterOffBtn = $('water-off'), autoModeBtn = $('auto-mode');
const wName = $('w-name'), wDesc = $('w-desc'), wTemp = $('w-temp'), wHum = $('w-hum'), wRain = $('w-rain'), wRefresh = $('w-refresh');
const rainWarn = $('rain-warn'), rainDetail = $('rain-detail');
const moistInp = $('moisture-threshold'), humInp = $('humidity-threshold');
const waterStartInp = $('water-start-hour'), waterEndInp = $('water-end-hour');
const saveBtn = $('save-settings');
const cfgApiKey = $('cfg-api-key'), cfgCity = $('cfg-city'), cfgCountry = $('cfg-country');
const cfgAuthUser = $('cfg-auth-user'), cfgAuthPass = $('cfg-auth-pass');
const cfgTlsValidate = $('cfg-tls-validate'), cfgRootCa = $('cfg-root-ca');
const saveCfgBtn = $('save-config'), resetWifiBtn = $('reset-wifi');
const backupBtn = $('backup-config'), restoreBtn = $('restore-config'), factoryResetBtn = $('factory-reset');
const safetyWarn = $('safety-warn'), defaultCredsWarn = $('default-creds-warn');
const faultWarn = $('fault-warn'), faultDetail = $('fault-detail');
const tempFaultBadge = $('temp-fault'), humFaultBadge = $('hum-fault'), moistFaultBadge = $('moist-fault');
const calibRaw = $('calib-raw'), calibDry = $('calib-dry'), calibWet = $('calib-wet');
const calibDryBtn = $('calib-dry-btn'), calibWetBtn = $('calib-wet-btn'), calibRefresh = $('calib-refresh');
const refreshChartBtn = $('refresh-chart');
const diagRefresh = $('diag-refresh'), diagReboot = $('diag-reboot');
const saveMqttBtn = $('save-mqtt'), saveTelegramBtn = $('save-telegram'), testTelegramBtn = $('test-telegram');
const saveInfluxBtn = $('save-influx'), testInfluxBtn = $('test-influx');
const refreshLogsBtn = $('refresh-logs'), clearLogsBtn = $('clear-logs');
let trendsChart = null;
let moistureThreshold = 30, humidityThreshold = 50;

function updateSensorData(){
  fetch('/sensor-data').then(r=>r.json()).then(d=>{
    temperatureEl.textContent = (typeof d.temperature==='number') ? d.temperature.toFixed(1) : '--';
    humidityEl.textContent = (typeof d.humidity==='number') ? d.humidity.toFixed(1) : '--';
    moistureEl.textContent = (typeof d.moisture==='number') ? d.moisture.toFixed(1) : '--';
    calibRaw.textContent = (typeof d.raw_moisture==='number') ? d.raw_moisture : '—';
  }).catch(()=>{});
}

function updateSystemStatus(){
  fetch('/system-status').then(r=>r.json()).then(d=>{
    systemStatusEl.textContent = `System Status: ${d.status}`;
    systemStatusEl.className = `status ${d.status==='Auto'?'status-auto':'status-manual'}`;
    relayStatusEl.textContent = `Motor Status: ${d.relay?'ON':'OFF'}`;
    relayStatusEl.className = `status ${d.relay?'status-on':'status-off'}`;
    updateAutoModeButton(d.status==='Auto');
    safetyWarn.style.display = d.safety_trip ? 'block' : 'none';
    let faults = [];
    if (d.dht_fault) { tempFaultBadge.classList.add('show'); humFaultBadge.classList.add('show'); faults.push('DHT11'); }
    else { tempFaultBadge.classList.remove('show'); humFaultBadge.classList.remove('show'); }
    if (d.moisture_fault) { moistFaultBadge.classList.add('show'); faults.push('Soil Moisture'); }
    else { moistFaultBadge.classList.remove('show'); }
    if (faults.length) { faultDetail.textContent = faults.join(', ') + ' sensor(s) not responding.'; faultWarn.style.display = 'block'; }
    else { faultWarn.style.display = 'none'; }
  }).catch(()=>{});
}

function fetchWeather(){
  fetch('/weather').then(r=>r.json()).then(w=>{
    wName.textContent = w.name || '—';
    wDesc.textContent = w.desc || '—';
    wTemp.textContent = (typeof w.temp==='number') ? `${w.temp.toFixed(1)} °C` : '—';
    wHum.textContent = (typeof w.hum==='number') ? `${w.hum.toFixed(0)} %` : '—';
    if (typeof w.rain_3h==='number') {
      wRain.textContent = `${w.rain_3h.toFixed(1)} mm`;
      if (w.rain_expected) { rainDetail.textContent = w.rain_3h.toFixed(1); rainWarn.style.display = 'block'; }
      else rainWarn.style.display = 'none';
    } else { wRain.textContent = '—'; rainWarn.style.display = 'none'; }
  }).catch(()=>{});
}

function loadSettings(){
  fetch('/settings').then(r=>r.json()).then(s=>{
    moistureThreshold = s.moisture_threshold ?? 30;
    humidityThreshold = s.humidity_threshold ?? 50;
    moistInp.value = moistureThreshold;
    humInp.value = humidityThreshold;
    waterStartInp.value = s.watering_start_hour ?? 6;
    waterEndInp.value = s.watering_end_hour ?? 8;
    $('sched-reboot-enabled').value = s.scheduled_reboot_enabled ? 'true' : 'false';
    $('sched-reboot-day').value = s.scheduled_reboot_weekday ?? 1;
    $('sched-reboot-hour').value = s.scheduled_reboot_hour ?? 3;
  }).catch(()=>{});
}

function saveSettings(){
  const body = {
    moisture_threshold: parseFloat(moistInp.value||30),
    humidity_threshold: parseFloat(humInp.value||50),
    watering_start_hour: parseInt(waterStartInp.value||0),
    watering_end_hour: parseInt(waterEndInp.value||0),
    scheduled_reboot_enabled: $('sched-reboot-enabled').value === 'true',
    scheduled_reboot_weekday: parseInt($('sched-reboot-day').value||1),
    scheduled_reboot_hour: parseInt($('sched-reboot-hour').value||3)
  };
  fetch('/settings',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify(body)})
    .then(r=>r.json()).then(_=>loadSettings()).catch(()=>{});
}

function sendCommand(command){
  fetch('/control',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify({command})})
    .then(r=>r.json()).then(_=>updateSystemStatus()).catch(()=>{});
}

function updateAutoModeButton(autoOn){ autoModeBtn.textContent = autoOn ? 'Auto Mode: ON' : 'Auto Mode: OFF'; }

waterOnBtn.onclick = ()=>sendCommand('water_on');
waterOffBtn.onclick= ()=>sendCommand('water_off');
autoModeBtn.onclick= ()=>sendCommand('auto_mode');
wRefresh.onclick   = ()=>fetchWeather();
saveBtn.onclick    = ()=>saveSettings();

function loadConfig(){
  fetch('/config').then(r=>r.json()).then(c=>{
    cfgApiKey.value = c.api_key || '';
    cfgCity.value = c.city || '';
    cfgCountry.value = c.country || '';
    cfgAuthUser.value = c.auth_user || 'admin';
    cfgAuthPass.value = '';
    cfgTlsValidate.value = c.validate_tls_cert ? 'true' : 'false';
    cfgRootCa.value = c.root_ca || '';
    if (c.auth_user === 'admin' && c.using_default_pass) defaultCredsWarn.style.display = 'block';
    else defaultCredsWarn.style.display = 'none';
    $('cfg-mqtt-enabled').value = c.mqtt_enabled ? 'true' : 'false';
    $('cfg-mqtt-broker').value = c.mqtt_broker || '';
    $('cfg-mqtt-port').value = c.mqtt_port || 1883;
    $('cfg-mqtt-prefix').value = c.mqtt_prefix || 'smartfarm';
    $('cfg-mqtt-user').value = c.mqtt_user || '';
    $('cfg-mqtt-pass').value = '';
    $('cfg-tg-enabled').value = c.telegram_enabled ? 'true' : 'false';
    $('cfg-tg-chat').value = c.telegram_chat_id || '';
    $('cfg-tg-token').value = c.telegram_bot_token || '';
    $('cfg-influx-enabled').value = c.influx_enabled ? 'true' : 'false';
    $('cfg-influx-url').value = c.influx_url || '';
    $('cfg-influx-org').value = c.influx_org || '';
    $('cfg-influx-bucket').value = c.influx_bucket || 'smartfarm';
    $('cfg-influx-token').value = c.influx_token || '';
  }).catch(()=>{});
}

function saveConfig(){
  const body = {
    api_key: cfgApiKey.value,
    city: cfgCity.value,
    country: cfgCountry.value.toUpperCase(),
    auth_user: cfgAuthUser.value,
    validate_tls_cert: cfgTlsValidate.value === 'true',
    root_ca: cfgRootCa.value,
    mqtt_enabled: $('cfg-mqtt-enabled').value === 'true',
    mqtt_broker: $('cfg-mqtt-broker').value,
    mqtt_port: parseInt($('cfg-mqtt-port').value || 1883),
    mqtt_prefix: $('cfg-mqtt-prefix').value || 'smartfarm',
    mqtt_user: $('cfg-mqtt-user').value,
    telegram_enabled: $('cfg-tg-enabled').value === 'true',
    telegram_chat_id: $('cfg-tg-chat').value,
    telegram_bot_token: $('cfg-tg-token').value,
    influx_enabled: $('cfg-influx-enabled').value === 'true',
    influx_url: $('cfg-influx-url').value,
    influx_org: $('cfg-influx-org').value,
    influx_bucket: $('cfg-influx-bucket').value || 'smartfarm',
    influx_token: $('cfg-influx-token').value
  };
  if (cfgAuthPass.value) body.auth_pass = cfgAuthPass.value;
  if ($('cfg-mqtt-pass').value) body.mqtt_pass = $('cfg-mqtt-pass').value;
  fetch('/config',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{
      if (d.success) {
        alert('Configuration saved.');
        if (d.rebuild_url) setTimeout(fetchWeather, 1500);
        loadConfig();
      } else alert('Failed: ' + (d.error || 'unknown'));
    }).catch(()=>{ alert('Network error'); });
}

function resetWifi(){
  if (!confirm('Erase WiFi credentials and reboot into captive portal?')) return;
  fetch('/reset-wifi',{method:'POST',headers:CSRF_HEADER})
    .then(r=>r.json()).then(d=>{ if (d.success) alert('Rebooting. Connect to "SmartAgri-Setup" WiFi.'); })
    .catch(()=>{});
}

saveCfgBtn.onclick = saveConfig;
resetWifiBtn.onclick = resetWifi;

backupBtn.onclick = ()=>{
  fetch('/backup').then(r=>r.json()).then(data=>{
    const blob = new Blob([JSON.stringify(data, null, 2)], {type:'application/json'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = 'smartfarm-config-backup.json';
    a.click(); URL.revokeObjectURL(url);
  }).catch(()=>{ alert('Backup failed'); });
};

restoreBtn.onclick = ()=>{
  const input = document.createElement('input');
  input.type = 'file'; input.accept = '.json,application/json';
  input.onchange = e => {
    const file = e.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = ev => {
      try {
        const data = JSON.parse(ev.target.result);
        fetch('/restore',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify(data)})
          .then(r=>r.json()).then(d=>{
            if (d.success) alert('Config restored. Device will reboot.');
            else alert('Restore failed: ' + (d.error || 'unknown'));
          }).catch(()=>{ alert('Network error'); });
      } catch(err) { alert('Invalid JSON file: ' + err.message); }
    };
    reader.readAsText(file);
  };
  input.click();
};

factoryResetBtn.onclick = ()=>{
  if (!confirm('FACTORY RESET will erase ALL data (NVS, LittleFS, logs, WiFi). Device will reboot into captive portal. Are you sure?')) return;
  if (!confirm('This is your final warning. Continue with factory reset?')) return;
  fetch('/factory-reset',{method:'POST',headers:CSRF_HEADER})
    .then(r=>r.json()).then(d=>{ if (d.success) alert('Factory reset complete. Rebooting...'); })
    .catch(()=>{});
};

saveMqttBtn.onclick = ()=>{
  const body = {
    mqtt_enabled: $('cfg-mqtt-enabled').value === 'true',
    mqtt_broker: $('cfg-mqtt-broker').value,
    mqtt_port: parseInt($('cfg-mqtt-port').value || 1883),
    mqtt_prefix: $('cfg-mqtt-prefix').value || 'smartfarm',
    mqtt_user: $('cfg-mqtt-user').value
  };
  if ($('cfg-mqtt-pass').value) body.mqtt_pass = $('cfg-mqtt-pass').value;
  fetch('/config',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{ if (d.success) alert('MQTT config saved.'); else alert('Failed: ' + (d.error || 'unknown')); })
    .catch(()=>{ alert('Network error'); });
};

saveTelegramBtn.onclick = ()=>{
  const body = {
    telegram_enabled: $('cfg-tg-enabled').value === 'true',
    telegram_chat_id: $('cfg-tg-chat').value,
    telegram_bot_token: $('cfg-tg-token').value
  };
  fetch('/config',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{ if (d.success) alert('Telegram config saved.'); else alert('Failed: ' + (d.error || 'unknown')); })
    .catch(()=>{ alert('Network error'); });
};

testTelegramBtn.onclick = ()=>{
  fetch('/telegram/test',{method:'POST',headers:CSRF_HEADER})
    .then(r=>r.json()).then(d=>{
      if (d.success) alert('Test alert sent.');
      else alert('Failed: ' + (d.error || 'unknown'));
    }).catch(()=>{ alert('Network error'); });
};

saveInfluxBtn.onclick = ()=>{
  const body = {
    influx_enabled: $('cfg-influx-enabled').value === 'true',
    influx_url: $('cfg-influx-url').value,
    influx_org: $('cfg-influx-org').value,
    influx_bucket: $('cfg-influx-bucket').value || 'smartfarm',
    influx_token: $('cfg-influx-token').value
  };
  fetch('/config',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{ if (d.success) alert('InfluxDB config saved.'); else alert('Failed: ' + (d.error || 'unknown')); })
    .catch(()=>{ alert('Network error'); });
};

testInfluxBtn.onclick = ()=>{
  fetch('/influx/test',{method:'POST',headers:CSRF_HEADER})
    .then(r=>r.json()).then(d=>{
      if (d.success) alert('Test point sent. Check your InfluxDB bucket.');
      else alert('Failed: ' + (d.error || 'unknown'));
    }).catch(()=>{ alert('Network error'); });
};

function loadCalibration(){
  fetch('/calibrate').then(r=>r.json()).then(c=>{
    calibDry.textContent = c.dry ?? '—';
    calibWet.textContent = c.wet ?? '—';
  }).catch(()=>{});
}
function setCalibration(action){
  fetch('/calibrate',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify({action})})
    .then(r=>r.json()).then(d=>{
      if (d.success) { loadCalibration(); alert(`${action === 'set_dry' ? 'Dry' : 'Wet'} set: ${d.value}`); }
      else alert('Failed: ' + (d.error || 'unknown'));
    }).catch(()=>{ alert('Network error'); });
}
calibDryBtn.onclick = ()=>setCalibration('set_dry');
calibWetBtn.onclick = ()=>setCalibration('set_wet');
calibRefresh.onclick = ()=>{ updateSensorData(); loadCalibration(); };

function fetchAndRenderChart(){
  fetch('/history?hours=24').then(r=>r.json()).then(data=>{
    const labels = data.map(p => p.t);
    const ctx = $('trends-chart').getContext('2d');
    if (trendsChart) trendsChart.destroy();
    trendsChart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: labels,
        datasets: [
          { label: 'Temperature (°C)', data: data.map(p=>p.temp),  borderColor: '#e74c3c', backgroundColor: 'rgba(231,76,60,0.1)', tension: 0.3 },
          { label: 'Humidity (%)',     data: data.map(p=>p.hum),   borderColor: '#3498db', backgroundColor: 'rgba(52,152,219,0.1)', tension: 0.3 },
          { label: 'Soil Moisture (%)',data: data.map(p=>p.moist), borderColor: '#27ae60', backgroundColor: 'rgba(39,174,96,0.1)', tension: 0.3 }
        ]
      },
      options: { responsive: true, maintainAspectRatio: false, scales: { y: { min: 0, max: 100 } }, plugins: { legend: { position: 'top' } } }
    });
  }).catch(()=>{});
}
refreshChartBtn.onclick = fetchAndRenderChart;

function updateDiagnostics(){
  fetch('/diagnostics').then(r=>r.json()).then(d=>{
    $('diag-uptime').textContent = d.uptime ? `${Math.floor(d.uptime/3600)}h ${Math.floor((d.uptime%3600)/60)}m ${d.uptime%60}s` : '—';
    $('diag-heap').textContent = d.free_heap ? `${(d.free_heap/1024).toFixed(1)} KB` : '—';
    $('diag-rssi').textContent = (typeof d.rssi === 'number') ? `${d.rssi} dBm` : '—';
    $('diag-ssid').textContent = d.ssid || '—';
    $('diag-ip').textContent = d.ip || '—';
    $('diag-fs').textContent = d.fs_used ? `${(d.fs_used/1024).toFixed(0)} / ${(d.fs_total/1024).toFixed(0)} KB` : '—';
    $('diag-mqtt').textContent = d.mqtt_enabled ? (d.mqtt_connected ? 'Connected' : 'Disconnected') : 'Disabled';
    $('diag-influx').textContent = d.influx_enabled ? 'Enabled' : 'Disabled';
    $('diag-cycles').textContent = d.pump_cycles ?? '—';
    $('diag-runtime').textContent = d.pump_runtime_sec ? `${Math.floor(d.pump_runtime_sec/60)} min ${d.pump_runtime_sec%60} s` : '—';
    $('diag-led').textContent = d.led_state || '—';
    $('diag-boot').textContent = d.last_boot || '—';
    $('diag-sched').textContent = d.scheduled_reboot_enabled ? `Every ${['Sun','Mon','Tue','Wed','Thu','Fri','Sat'][d.scheduled_reboot_weekday]} at ${String(d.scheduled_reboot_hour).padStart(2,'0')}:00` : 'Disabled';
    const credsEl = $('diag-creds');
    if (d.auth_disabled) { credsEl.textContent = 'AUTH DISABLED'; credsEl.style.color = '#dc2626'; }
    else if (d.default_credentials) { credsEl.textContent = 'Default admin/admin — change these'; credsEl.style.color = '#dc2626'; }
    else { credsEl.textContent = 'Custom'; credsEl.style.color = ''; }
  }).catch(()=>{});
}
diagRefresh.onclick = updateDiagnostics;
diagReboot.onclick = ()=>{
  if (!confirm('Reboot the device now?')) return;
  fetch('/reboot',{method:'POST',headers:CSRF_HEADER}).then(r=>r.json()).then(d=>{
    if (d.success) { alert('Rebooting...'); setTimeout(updateDiagnostics, 10000); }
  }).catch(()=>{});
};

function refreshLogs(){
  fetch('/logs').then(r=>r.text()).then(t=>{
    $('access-log').textContent = t || '(no log entries)';
  }).catch(()=>{ $('access-log').textContent = 'Failed to load logs'; });
}
refreshLogsBtn.onclick = refreshLogs;
clearLogsBtn.onclick = ()=>{
  if (!confirm('Clear all access log entries?')) return;
  fetch('/logs/clear',{method:'POST',headers:CSRF_HEADER}).then(r=>r.json()).then(d=>{
    if (d.success) refreshLogs();
  }).catch(()=>{});
};

function updateClock(){ fetch('/time').then(r=>r.json()).then(d=>{ clockEl.textContent = d.time || '—'; }).catch(()=>{}); }

function init(){
  loadSettings(); loadConfig(); loadCalibration();
  updateSensorData(); updateSystemStatus(); fetchWeather();
  fetchAndRenderChart(); updateClock(); updateDiagnostics(); refreshLogs();
  setInterval(()=>{ updateSensorData(); updateSystemStatus(); }, 5000);
  setInterval(()=>{ fetchWeather(); }, 600000);
  setInterval(()=>{ updateClock(); }, 30000);
  setInterval(()=>{ fetchAndRenderChart(); updateDiagnostics(); }, 300000);
}
init();
</script>
</body>
</html>
)rawliteral";

// Forward declaration
bool sendTelegramMessage(const String& text);

void sendAlert(AlertType type) {
  if (!telegramEnabled) return;
  if (millis() - lastTelegramAlert[type] < TELEGRAM_COOLDOWN_MS) return;
  lastTelegramAlert[type] = millis();

  String msg = "🌱 <b>Smart Agricultural System</b>\n";
  switch (type) {
    case ALERT_DHT_FAULT:
      msg += "⚠️ <b>Sensor Fault:</b> DHT11 not responding.";
      break;
    case ALERT_DHT_RECOVER:
      msg += "✅ <b>Sensor Recovered:</b> DHT11 OK.";
      break;
    case ALERT_MOISTURE_FAULT:
      msg += "⚠️ <b>Sensor Fault:</b> Soil moisture sensor out-of-range.";
      break;
    case ALERT_MOISTURE_RECOVER:
      msg += "✅ <b>Sensor Recovered:</b> Soil moisture sensor OK.";
      break;
    case ALERT_SAFETY_TRIP:
      msg += "🚨 <b>Safety Trip:</b> Pump ran >5 min, force-stopped.";
      break;
    case ALERT_WATERING_START:
      msg += "💧 <b>Watering Started</b> (" + String(autoMode ? "Auto" : "Manual") + ")";
      break;
    case ALERT_WATERING_STOP:
      msg += "🛑 <b>Watering Stopped</b>";
      break;
    case ALERT_WIFI_DISCONNECT:
      msg += "📡 <b>WiFi Disconnected</b> >5 min.";
      break;
    case ALERT_WIFI_RECONNECT:
      msg += "📡 <b>WiFi Reconnected</b>\nIP: " + WiFi.localIP().toString();
      break;
    case ALERT_MQTT_DISCONNECT:
      msg += "🔌 <b>MQTT Disconnected</b>.";
      break;
    case ALERT_BOOT:
      msg += "🚀 <b>Device Booted</b>\nTime: " + getLocalTimeStr() + "\nIP: " + WiFi.localIP().toString();
      break;
    default: return;
  }
  sendTelegramMessage(msg);
}

// ====== Helpers ======
float readMoisturePercent() {
  const int SAMPLES = 8;
  long sum = 0;
  int minv = 4095, maxv = 0;
  for (int i = 0; i < SAMPLES; ++i) {
    int v = analogRead(MOISTURE_PIN);
    sum += v;
    if (v < minv) minv = v;
    if (v > maxv) maxv = v;
    delayMicroseconds(200);
  }
  long adjusted = sum;
  if (SAMPLES > 2) adjusted -= (minv + maxv);
  int denom = (SAMPLES > 2) ? (SAMPLES - 2) : SAMPLES;
  int raw = (int)(adjusted / denom);
  if (raw < 0) raw = 0;
  if (raw > 4095) raw = 4095;

  if (raw == 0 || raw == 4095) {
    moistureFailCount++;
    if (moistureFailCount >= MOISTURE_FAULT_THRESHOLD) {
      if (!moistureFault) { moistureFault = true; sendAlert(ALERT_MOISTURE_FAULT); }
    }
  } else {
    if (moistureFault) { moistureFault = false; sendAlert(ALERT_MOISTURE_RECOVER); }
    moistureFailCount = 0;
  }

  float denom_map = (float)(MOISTURE_DRY - MOISTURE_WET);
  float percent_unflipped = 0.0f;
  if (denom_map == 0.0f) percent_unflipped = 0.0f;
  else percent_unflipped = (float)(raw - MOISTURE_WET) * 100.0f / denom_map;
  percent_unflipped = constrain(percent_unflipped, 0.0f, 100.0f);
  float mapped = 100.0f - percent_unflipped;
  return mapped;
}

int readMoistureRaw() {
  const int SAMPLES = 8;
  long sum = 0;
  int minv = 4095, maxv = 0;
  for (int i = 0; i < SAMPLES; ++i) {
    int v = analogRead(MOISTURE_PIN);
    sum += v;
    if (v < minv) minv = v;
    if (v > maxv) maxv = v;
    delayMicroseconds(200);
  }
  long adjusted = sum;
  if (SAMPLES > 2) adjusted -= (minv + maxv);
  int denom = (SAMPLES > 2) ? (SAMPLES - 2) : SAMPLES;
  int raw = (int)(adjusted / denom);
  if (raw < 0) raw = 0;
  if (raw > 4095) raw = 4095;
  return raw;
}

void updateDhtCache() {
  if (millis() - lastDhtRead < DHT_INTERVAL_MS) return;
  lastDhtRead = millis();
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    dhtFailCount++;
    if (dhtFailCount >= DHT_FAULT_THRESHOLD) {
      if (!dhtFault) { dhtFault = true; sendAlert(ALERT_DHT_FAULT); }
    }
  } else {
    cachedTemp = t;
    cachedHum  = h;
    if (dhtFault) { dhtFault = false; sendAlert(ALERT_DHT_RECOVER); }
    dhtFailCount = 0;
  }
}

// ====== Pump tracking ======
void onPumpTurnedOn() {
  wateringStartTime = millis();
  pumpCycles++;
  pumpStartTime = millis();
  preferences.begin("watering", false);
  preferences.putUInt("pumpCycles", pumpCycles);
  preferences.end();
}

void onPumpTurnedOff() {
  if (pumpStartTime > 0) {
    unsigned long runtimeSec = (millis() - pumpStartTime) / 1000;
    pumpTotalRuntimeSec += runtimeSec;
    pumpStartTime = 0;
    preferences.begin("watering", false);
    preferences.putULong("pumpRuntime", pumpTotalRuntimeSec);
    preferences.end();
  }
}

// ====== Access Log ======
void logAccess(const String& entry) {
  if (LittleFS.exists(ACCESS_LOG_FILE)) {
    File f = LittleFS.open(ACCESS_LOG_FILE, "r");
    if (f && f.size() > MAX_ACCESS_LOG_SIZE) {
      f.close();
      LittleFS.remove(ACCESS_LOG_FILE);
    } else if (f) {
      f.close();
    }
  }
  File logFile = LittleFS.open(ACCESS_LOG_FILE, "a");
  if (!logFile) return;
  String ts = getIsoTimestamp();
  logFile.printf("[%s] %s\n", ts.c_str(), entry.c_str());
  logFile.close();
}

String getAccessLogTail(int lines) {
  if (!LittleFS.exists(ACCESS_LOG_FILE)) return "(no log entries)";
  File f = LittleFS.open(ACCESS_LOG_FILE, "r");
  if (!f) return "(failed to open log)";

  String *buffer = new String[lines];
  int idx = 0, count = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    buffer[idx] = line;
    idx = (idx + 1) % lines;
    if (count < lines) count++;
  }
  f.close();

  String out;
  int start = (count < lines) ? 0 : idx;
  for (int i = 0; i < count; i++) {
    int j = (start + i) % lines;
    out += buffer[j] + "\n";
  }
  delete[] buffer;
  return out;
}

// ====== Rate Limiting ======
RateLimitEntry* findRateLimit(IPAddress ip) {
  for (int i = 0; i < MAX_RATE_LIMIT_ENTRIES; i++) {
    if (rateLimits[i].ip == ip) return &rateLimits[i];
  }
  return nullptr;
}

RateLimitEntry* findFreeRateLimitSlot() {
  for (int i = 0; i < MAX_RATE_LIMIT_ENTRIES; i++) {
    if (rateLimits[i].ip == IPAddress(0, 0, 0, 0)) return &rateLimits[i];
  }
  for (int i = 0; i < MAX_RATE_LIMIT_ENTRIES; i++) {
    if (millis() > rateLimits[i].blockedUntil && rateLimits[i].failCount == 0) {
      rateLimits[i].ip = IPAddress(0, 0, 0, 0);
      return &rateLimits[i];
    }
  }
  return nullptr;
}

bool isRateLimited(IPAddress ip) {
  RateLimitEntry* entry = findRateLimit(ip);
  if (!entry) return false;
  if (millis() < entry->blockedUntil) return true;
  if (millis() - entry->firstFailTime > 60000UL) {
    entry->failCount = 0;
    entry->firstFailTime = 0;
  }
  return false;
}

void recordAuthFailure(IPAddress ip) {
  RateLimitEntry* entry = findRateLimit(ip);
  if (!entry) {
    entry = findFreeRateLimitSlot();
    if (!entry) return;
    entry->ip = ip;
    entry->failCount = 0;
    entry->firstFailTime = millis();
    entry->blockedUntil = 0;
  }
  if (entry->firstFailTime == 0 || millis() - entry->firstFailTime > 60000UL) {
    entry->firstFailTime = millis();
    entry->failCount = 0;
  }
  entry->failCount++;
  if (entry->failCount >= 5) {
    entry->blockedUntil = millis() + 60000UL;
    logAccess("Rate limit triggered for " + ip.toString() + " (5 failed auth attempts)");
  }
}

// ====== Config load/save ======
void loadConfig() {
  preferences.begin("watering", true);
  apiKey       = preferences.getString("apiKey", "");
  city         = preferences.getString("city", "Ahmedabad");
  countryCode  = preferences.getString("country", "IN");
  authUser     = preferences.getString("authUser", "admin");
  authPass     = preferences.getString("authPass", "admin");
  moistureThreshold = preferences.getFloat("moisture", 30.0f);
  humidityThreshold = preferences.getFloat("humidity", 50.0f);
  autoMode          = preferences.getBool("automode", true);
  MOISTURE_DRY      = preferences.getInt("moistDry", 4095);
  MOISTURE_WET      = preferences.getInt("moistWet", 1500);
  wateringStartHour = preferences.getInt("waterStart", 6);
  wateringEndHour   = preferences.getInt("waterEnd", 8);
  validateTlsCert = preferences.getBool("tlsCert", false);
  rootCaPem       = preferences.getString("rootCa", "");
  mqttEnabled     = preferences.getBool("mqttEn", false);
  mqttBroker      = preferences.getString("mqttBroker", "");
  mqttPort        = preferences.getInt("mqttPort", 1883);
  mqttUser        = preferences.getString("mqttUser", "");
  mqttPass        = preferences.getString("mqttPass", "");
  mqttTopicPrefix = preferences.getString("mqttPrefix", "smartfarm");
  telegramEnabled  = preferences.getBool("tgEn", false);
  telegramBotToken = preferences.getString("tgToken", "");
  telegramChatId   = preferences.getString("tgChat", "");
  pumpCycles         = preferences.getUInt("pumpCycles", 0);
  pumpTotalRuntimeSec= preferences.getULong("pumpRuntime", 0);
  influxEnabled = preferences.getBool("influxEn", false);
  influxUrl     = preferences.getString("influxUrl", "");
  influxOrg     = preferences.getString("influxOrg", "");
  influxBucket  = preferences.getString("influxBucket", "smartfarm");
  influxToken   = preferences.getString("influxToken", "");
  scheduledRebootEnabled = preferences.getBool("schedRebootEn", false);
  scheduledRebootWeekday = preferences.getInt("schedRebootDay", 1);
  scheduledRebootHour    = preferences.getInt("schedRebootHour", 3);
  preferences.end();
}

void saveConfigToNvs() {
  preferences.begin("watering", false);
  preferences.putString("apiKey", apiKey);
  preferences.putString("city", city);
  preferences.putString("country", countryCode);
  preferences.putString("authUser", authUser);
  preferences.putString("authPass", authPass);
  preferences.putFloat("moisture", moistureThreshold);
  preferences.putFloat("humidity", humidityThreshold);
  preferences.putBool("automode", autoMode);
  preferences.putInt("moistDry", MOISTURE_DRY);
  preferences.putInt("moistWet", MOISTURE_WET);
  preferences.putInt("waterStart", wateringStartHour);
  preferences.putInt("waterEnd", wateringEndHour);
  preferences.putBool("tlsCert", validateTlsCert);
  preferences.putString("rootCa", rootCaPem);
  preferences.putBool("mqttEn", mqttEnabled);
  preferences.putString("mqttBroker", mqttBroker);
  preferences.putInt("mqttPort", mqttPort);
  preferences.putString("mqttUser", mqttUser);
  preferences.putString("mqttPass", mqttPass);
  preferences.putString("mqttPrefix", mqttTopicPrefix);
  preferences.putBool("tgEn", telegramEnabled);
  preferences.putString("tgToken", telegramBotToken);
  preferences.putString("tgChat", telegramChatId);
  preferences.putUInt("pumpCycles", pumpCycles);
  preferences.putULong("pumpRuntime", pumpTotalRuntimeSec);
  preferences.putBool("influxEn", influxEnabled);
  preferences.putString("influxUrl", influxUrl);
  preferences.putString("influxOrg", influxOrg);
  preferences.putString("influxBucket", influxBucket);
  preferences.putString("influxToken", influxToken);
  preferences.putBool("schedRebootEn", scheduledRebootEnabled);
  preferences.putInt("schedRebootDay", scheduledRebootWeekday);
  preferences.putInt("schedRebootHour", scheduledRebootHour);
  preferences.end();
}

void buildWeatherURL() {
  weatherURL  = "https://api.openweathermap.org/data/2.5/weather?q="  + city + "," + countryCode + "&appid=" + apiKey + "&units=metric";
  forecastURL = "https://api.openweathermap.org/data/2.5/forecast?q=" + city + "," + countryCode + "&appid=" + apiKey + "&units=metric";
}

// ====== Auth + CSRF ======
bool requireAuth() {
  IPAddress ip = server.client().remoteIP();
  if (isRateLimited(ip)) {
    server.send(429, "application/json", "{\"success\":false,\"error\":\"rate limited\"}");
    return false;
  }
  if (authUser.length() == 0 || authPass.length() == 0) return true;
  if (!server.authenticate(authUser.c_str(), authPass.c_str())) {
    recordAuthFailure(ip);
    server.requestAuthentication();
    return false;
  }
  return true;
}

bool requireCsrf() {
  if (!server.hasHeader("X-Requested-With")) {
    server.send(403, "application/json", "{\"success\":false,\"error\":\"missing CSRF header\"}");
    return false;
  }
  return true;
}

bool requireAuthLogged(const String& action) {
  bool ok = requireAuth();
  if (ok) {
    String method = (server.method() == HTTP_GET) ? "GET" : "POST";
    String entry = method + " " + server.uri() + " from " + server.client().remoteIP().toString() + " - " + action;
    logAccess(entry);
  }
  return ok;
}

// ====== Credential hygiene ======
// requireAuth() treats a blank username or password as 'no auth required',
// so both that state and the untouched admin/admin default are worth
// reporting rather than leaving silent.
void checkCredentialHygiene() {
  httpAuthDisabled = (authUser.length() == 0 || authPass.length() == 0);
  usingDefaultCredentials = (authUser == "admin" && authPass == "admin");
  if (httpAuthDisabled) {
    Serial.println(F("[SECURITY] HTTP auth is DISABLED - every endpoint is open on the LAN."));
    logAccess("WARNING: HTTP auth disabled (blank username or password)");
  } else if (usingDefaultCredentials) {
    Serial.println(F("[SECURITY] Default credentials admin/admin still in use - change them in Config."));
    logAccess("WARNING: default credentials admin/admin still in use");
  }
}

// ====== TLS-aware HTTPS client factory ======
WiFiClientSecure createSecureClient() {
  WiFiClientSecure client;
  if (validateTlsCert && rootCaPem.length() > 0) {
    client.setCACert(rootCaPem.c_str());
  } else {
    client.setInsecure();
  }
  return client;
}

// ====== Weather + Forecast ======
void getWeatherData() {
  if (WiFi.status() != WL_CONNECTED || apiKey.length() < 10) return;
  WiFiClientSecure client = createSecureClient();
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, weatherURL);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      weatherTemp = doc["main"]["temp"] | NAN;
      weatherHum  = doc["main"]["humidity"] | NAN;
      weatherDescription = doc["weather"][0]["description"] | String("");
      locationName = doc["name"] | String("");
    }
  } else Serial.printf("[Weather] HTTPS err: %d\n", code);
  http.end();
  lastWeatherUpdate = millis();
}

void getForecastData() {
  if (WiFi.status() != WL_CONNECTED || apiKey.length() < 10) return;
  WiFiClientSecure client = createSecureClient();
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, forecastURL);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<8192> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      float totalRain = 0.0f;
      JsonArray list = doc["list"].as<JsonArray>();
      if (list.size() > 0 && list[0].containsKey("rain")) {
        totalRain = list[0]["rain"]["3h"] | 0.0f;
      }
      rainNext3h = totalRain;
      rainExpected = (totalRain >= RAIN_THRESHOLD_MM);
    }
  } else Serial.printf("[Forecast] HTTPS err: %d\n", code);
  http.end();
  lastForecastUpdate = millis();
}

// ====== WiFi reconnect ======
void checkWifiReconnect() {
  if (millis() - lastWifiCheck < WIFI_CHECK_INTERVAL_MS) return;
  lastWifiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiWasDisconnected) {
      wifiWasDisconnected = true;
      lastWifiDisconnect = millis();
    }
    WiFi.reconnect();
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      if (wifiWasDisconnected) {
        wifiWasDisconnected = false;
        sendAlert(ALERT_WIFI_RECONNECT);
      }
    }
  } else {
    if (wifiWasDisconnected && (millis() - lastWifiDisconnect > 300000)) {
      sendAlert(ALERT_WIFI_DISCONNECT);
    }
  }
}

// ====== Scheduled watering window ======
bool isWithinWateringWindow() {
  if (wateringStartHour == wateringEndHour) return true;
  int h = getLocalHour();
  if (h < 0) return true;
  if (wateringStartHour < wateringEndHour) {
    return (h >= wateringStartHour && h < wateringEndHour);
  } else {
    return (h >= wateringStartHour || h < wateringEndHour);
  }
}

// ====== Scheduled Reboot Check ======
void checkScheduledReboot() {
  if (!scheduledRebootEnabled) return;
  unsigned long nowMin = millis() / 60000UL;
  if (nowMin == lastRebootCheckMinute) return;
  lastRebootCheckMinute = nowMin;

  int wd = getLocalWeekday();
  int hr = getLocalHour();
  if (wd < 0 || hr < 0) return;

  if (wd == scheduledRebootWeekday && hr == scheduledRebootHour) {
    if (!lastScheduledRebootCheck) {
      lastScheduledRebootCheck = true;
      logAccess("Scheduled reboot triggered");
      sendAlert(ALERT_BOOT);
      delay(500);
      ESP.restart();
    }
  } else {
    lastScheduledRebootCheck = false;
  }
}

// ====== Status LED ======
void updateStatusLed() {
  if (millis() - lastLedUpdate < ledBlinkInterval) return;
  lastLedUpdate = millis();
  ledState = !ledState;
  digitalWrite(STATUS_LED_PIN, ledState);

  if (dhtFault || moistureFault) {
    ledBlinkInterval = 100;
  } else if (safetyTrip) {
    ledBlinkInterval = 250;
  } else if (WiFi.status() != WL_CONNECTED) {
    ledBlinkInterval = 200;
  } else if (mqttEnabled && !mqtt.connected()) {
    ledBlinkInterval = 500;
  } else if (watering) {
    ledBlinkInterval = 1000;
  } else {
    ledBlinkInterval = 3000;
  }
}

String getLedStateStr() {
  if (dhtFault || moistureFault) return "Fault (fast blink)";
  if (safetyTrip) return "Safety trip";
  if (WiFi.status() != WL_CONNECTED) return "WiFi issue";
  if (mqttEnabled && !mqtt.connected()) return "MQTT issue";
  if (watering) return "Watering";
  return "OK (slow blink)";
}

// ====== LittleFS history logging ======
void setupLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println(F("[LittleFS] Mount failed"));
    return;
  }
  Serial.printf("[LittleFS] Used: %u bytes\n", (unsigned)LittleFS.usedBytes());
}

void logSensorData() {
  if (millis() - lastLogTime < LOG_INTERVAL_MS) return;
  lastLogTime = millis();

  if (LittleFS.exists(LOG_FILE)) {
    File f = LittleFS.open(LOG_FILE, "r");
    if (f && f.size() > MAX_LOG_SIZE) {
      f.close();
      LittleFS.remove(LOG_FILE);
    } else if (f) {
      f.close();
    }
  }

  File logFile = LittleFS.open(LOG_FILE, "a");
  if (!logFile) return;

  float t = isnan(cachedTemp) ? -1.0f : cachedTemp;
  float h = isnan(cachedHum)  ? -1.0f : cachedHum;
  float m = moistureFault ? -1.0f : readMoisturePercent();
  String ts = getIsoTimestamp();
  logFile.printf("%s,%.1f,%.1f,%.1f,%d\n", ts.c_str(), t, h, m, watering?1:0);
  logFile.close();
}

String getHistoryJson(int hours) {
  if (!LittleFS.exists(LOG_FILE)) return "[]";
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return "[]";

  time_t now;
  time(&now);
  time_t cutoff = now - (hours * 3600);

  String out = "[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int c1 = line.indexOf(',');
    if (c1 < 0) continue;
    String ts = line.substring(0, c1);
    String rest = line.substring(c1 + 1);

    int c2 = rest.indexOf(',');
    if (c2 < 0) continue;
    String tempStr = rest.substring(0, c2);
    rest = rest.substring(c2 + 1);
    int c3 = rest.indexOf(',');
    if (c3 < 0) continue;
    String humStr = rest.substring(0, c3);
    rest = rest.substring(c3 + 1);
    int c4 = rest.indexOf(',');
    String moistStr, waterStr;
    if (c4 < 0) { moistStr = rest; waterStr = "0"; }
    else { moistStr = rest.substring(0, c4); waterStr = rest.substring(c4 + 1); }

    if (ts.length() > 0 && isdigit(ts.charAt(0)) && ts.length() > 10) {
      time_t t = ts.toInt();
      if (t < cutoff) continue;
    }

    if (!first) out += ",";
    first = false;
    out += "{\"t\":\"" + ts + "\",";
    float temp = tempStr.toFloat();
    float hum  = humStr.toFloat();
    float moist= moistStr.toFloat();
    if (temp < 0)   out += "\"temp\":null,";
    else            out += "\"temp\":" + String(temp, 1) + ",";
    if (hum < 0)    out += "\"hum\":null,";
    else            out += "\"hum\":" + String(hum, 1) + ",";
    if (moist < 0)  out += "\"moist\":null,";
    else            out += "\"moist\":" + String(moist, 1) + ",";
    out += "\"water\":";
    out += (waterStr.toInt() ? "true" : "false");
    out += "}";
  }
  out += "]";
  f.close();
  return out;
}

String getHistoryCsv() {
  if (!LittleFS.exists(LOG_FILE)) return "timestamp,temperature,humidity,moisture,watering\n";
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return "timestamp,temperature,humidity,moisture,watering\n";
  String out = "timestamp,temperature,humidity,moisture,watering\n";
  while (f.available()) {
    out += f.readStringUntil('\n') + "\n";
  }
  f.close();
  return out;
}

// ====== MQTT ======
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr(topic);
  String payloadStr;
  for (unsigned int i = 0; i < length; i++) payloadStr += (char)payload[i];
  payloadStr.trim();
  logAccess("MQTT cmd: " + topicStr + " = " + payloadStr);

  String cmdTopic = mqttTopicPrefix + "/cmd/";
  if (topicStr == cmdTopic + "water_on") {
    watering = true; autoMode = false; safetyTrip = false;
    onPumpTurnedOn();
    digitalWrite(RELAY_PIN, LOW);
    saveConfigToNvs();
    sendAlert(ALERT_WATERING_START);
  } else if (topicStr == cmdTopic + "water_off") {
    if (watering) { onPumpTurnedOff(); sendAlert(ALERT_WATERING_STOP); }
    watering = false; autoMode = false;
    digitalWrite(RELAY_PIN, HIGH);
    saveConfigToNvs();
  } else if (topicStr == cmdTopic + "auto_mode") {
    autoMode = !autoMode;
    safetyTrip = false;
    if (!autoMode && watering) { onPumpTurnedOff(); digitalWrite(RELAY_PIN, HIGH); }
    saveConfigToNvs();
  }
}

void setupMqtt() {
  if (!mqttEnabled || mqttBroker.length() == 0) {
    mqttConnectedNow = false;
    return;
  }
  mqtt.setServer(mqttBroker.c_str(), mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(MQTT_MAX_PACKET);
}

void reconnectMqtt() {
  if (!mqttEnabled || mqttBroker.length() == 0) return;
  if (millis() - lastMqttReconnect < MQTT_RECONNECT_INTERVAL_MS) return;
  lastMqttReconnect = millis();
  if (mqtt.connected()) return;

  String clientId = "SmartAgri-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool connected;
  if (mqttUser.length() > 0) {
    connected = mqtt.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str());
  } else {
    connected = mqtt.connect(clientId.c_str());
  }

  if (connected) {
    mqttConnectedNow = true;
    String sub = mqttTopicPrefix + "/cmd/#";
    mqtt.subscribe(sub.c_str());
    String topic = mqttTopicPrefix + "/status";
    mqtt.publish(topic.c_str(), "online", true);
  } else {
    mqttConnectedNow = false;
  }
}

void publishMqttSensorData() {
  if (!mqttEnabled || !mqtt.connected()) return;
  if (millis() - lastMqttPublish < MQTT_PUBLISH_INTERVAL_MS) return;
  lastMqttPublish = millis();

  String base = mqttTopicPrefix + "/sensor/";
  if (!isnan(cachedTemp)) mqtt.publish((base + "temperature").c_str(), String(cachedTemp, 1).c_str(), true);
  if (!isnan(cachedHum))  mqtt.publish((base + "humidity").c_str(),    String(cachedHum, 1).c_str(), true);
  float m = moistureFault ? -1.0f : readMoisturePercent();
  if (m >= 0) mqtt.publish((base + "moisture").c_str(), String(m, 1).c_str(), true);
  mqtt.publish((base + "relay").c_str(), watering ? "ON" : "OFF", true);

  DynamicJsonDocument doc(512);
  if (isnan(cachedTemp)) doc["temperature"] = 0;
  else doc["temperature"] = cachedTemp;

  if (isnan(cachedHum)) doc["humidity"] = 0;
  else doc["humidity"] = cachedHum;
  
  doc["moisture"]    = m;
  doc["watering"]    = watering;
  doc["auto_mode"]   = autoMode;
  doc["dht_fault"]   = dhtFault;
  doc["moisture_fault"] = moistureFault;
  doc["rain_3h"]     = rainNext3h;
  doc["uptime"]      = (long)((millis() - bootTime) / 1000);
  String statusJson;
  serializeJson(doc, statusJson);
  mqtt.publish((mqttTopicPrefix + "/status").c_str(), statusJson.c_str(), true);
}

// ====== Telegram alerts ======
bool sendTelegramMessage(const String& text) {
  if (!telegramEnabled || telegramBotToken.length() < 10 || telegramChatId.length() == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client = createSecureClient();
  HTTPClient http;
  http.setTimeout(3000);
  String url = "https://api.telegram.org/bot" + telegramBotToken + "/sendMessage";
  http.begin(client, url);

  DynamicJsonDocument doc(512);
  doc["chat_id"] = telegramChatId;
  doc["text"] = text;
  doc["parse_mode"] = "HTML";
  String body;
  serializeJson(doc, body);

  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return code == 200;
}

// ====== InfluxDB push ======
bool pushToInflux() {
  if (!influxEnabled || influxUrl.length() == 0 || influxToken.length() == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (millis() - lastInfluxWrite < INFLUX_INTERVAL_MS) return false;
  lastInfluxWrite = millis();

  String payload;
  payload += "sensor,device=smartfarm ";
  if (!isnan(cachedTemp)) payload += "temperature=" + String(cachedTemp, 2) + ",";
  if (!isnan(cachedHum))  payload += "humidity=" + String(cachedHum, 2) + ",";
  float m = moistureFault ? -1.0f : readMoisturePercent();
  if (m >= 0) payload += "moisture=" + String(m, 2) + ",";
  payload += "watering=" + String(watering ? "1i" : "0i") + ",";
  payload += "auto_mode=" + String(autoMode ? "1i" : "0i");

  String url = influxUrl;
  if (!url.endsWith("/")) url += "/";
  url += "api/v2/write?org=" + influxOrg + "&bucket=" + influxBucket + "&precision=s";

  WiFiClientSecure client = createSecureClient();
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, url);
  http.addHeader("Authorization", "Token " + influxToken);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");

  int code = http.POST(payload);
  http.end();

  if (code == 204) {
    Serial.println(F("[InfluxDB] Pushed OK"));
    return true;
  } else {
    Serial.printf("[InfluxDB] Push failed: %d\n", code);
    return false;
  }
}

bool pushTestToInflux() {
  if (!influxEnabled || influxUrl.length() == 0 || influxToken.length() == 0) return false;

  String payload = "test,device=smartfarm value=1i";
  String url = influxUrl;
  if (!url.endsWith("/")) url += "/";
  url += "api/v2/write?org=" + influxOrg + "&bucket=" + influxBucket + "&precision=s";

  WiFiClientSecure client = createSecureClient();
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(client, url);
  http.addHeader("Authorization", "Token " + influxToken);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");

  int code = http.POST(payload);
  http.end();
  return code == 204;
}

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(100);
  bootTime = millis();

  dht.begin();
  pinMode(MOISTURE_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(STATUS_LED_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(MOISTURE_PIN, ADC_11db);

  loadConfig();
  setupLittleFS();
  checkCredentialHygiene();

  for (int i = 0; i < MAX_RATE_LIMIT_ENTRIES; i++) {
    rateLimits[i].ip = IPAddress(0, 0, 0, 0);
    rateLimits[i].failCount = 0;
    rateLimits[i].firstFailTime = 0;
    rateLimits[i].blockedUntil = 0;
  }

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  Serial.println(F("[WiFi] Starting WiFiManager..."));
  bool res = wm.autoConnect("SmartAgri-Setup");
  if (!res) {
    Serial.println(F("[WiFi] Portal timed out. Rebooting..."));
    delay(1000);
    ESP.restart();
  }
  Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  WiFi.setHostname("smartagri");
  if (MDNS.begin("project")) Serial.println(F("[mDNS] http://project.local/"));
  else Serial.println(F("[mDNS] failed."));

  Serial.println(F("[NTP] Configuring time..."));
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    lastBootTimeStr = getLocalTimeStr();
    Serial.printf("[NTP] Synced: %s\n", lastBootTimeStr.c_str());
  } else {
    lastBootTimeStr = "NTP not synced";
  }

  buildWeatherURL();
  getWeatherData();
  getForecastData();
  updateDhtCache();

  mqtt.setKeepAlive(60);
  setupMqtt();

  logAccess("Device booted");
  Serial.println(F("[HTTP] Web UI will be served on port 80 only (no local HTTPS)"));

  // ====== Collected request headers ======
  // WebServer only retains headers registered here; everything else is
  // discarded during parsing. Without this call hasHeader("X-Requested-With")
  // is always false, so requireCsrf() rejects every POST with 403 and the
  // whole control surface (watering, settings, calibration, reboot) stops
  // responding. This one line is what makes the CSRF check function.
  static const char* COLLECTED_HEADERS[] = { "X-Requested-With" };
  server.collectHeaders(COLLECTED_HEADERS, 1);

  // ====== Routes ======
  server.on("/", HTTP_GET, [](){
    if (!requireAuth()) return;
    server.send_P(200, "text/html", index_html);
  });

  server.on("/sensor-data", HTTP_GET, [](){
    if (!requireAuth()) return;
    int raw = readMoistureRaw();
    float m = readMoisturePercent();
    DynamicJsonDocument doc(256);
    if (!isnan(cachedTemp)) doc["temperature"] = cachedTemp; else doc["temperature"] = nullptr;
    if (!isnan(cachedHum))  doc["humidity"]    = cachedHum;  else doc["humidity"]    = nullptr;
    doc["moisture"] = m;
    doc["raw_moisture"] = raw;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/system-status", HTTP_GET, [](){
    if (!requireAuth()) return;
    DynamicJsonDocument doc(256);
    doc["status"] = (autoMode ? "Auto" : "Manual");
    doc["relay"]  = watering;
    doc["safety_trip"] = safetyTrip;
    doc["dht_fault"] = dhtFault;
    doc["moisture_fault"] = moistureFault;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/control", HTTP_POST, [](){
    if (!requireAuthLogged("control")) return;
    if (!requireCsrf()) return;
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    String command = doc["command"] | "";
    if (command == "water_on") {
      watering = true; autoMode = false; safetyTrip = false;
      onPumpTurnedOn();
      digitalWrite(RELAY_PIN, LOW);
      saveConfigToNvs();
      sendAlert(ALERT_WATERING_START);
      server.send(200, "application/json", "{\"success\":true}");
    } else if (command == "water_off") {
      if (watering) { onPumpTurnedOff(); sendAlert(ALERT_WATERING_STOP); }
      watering = false; autoMode = false; safetyTrip = false;
      digitalWrite(RELAY_PIN, HIGH);
      saveConfigToNvs();
      server.send(200, "application/json", "{\"success\":true}");
    } else if (command == "auto_mode") {
      autoMode = !autoMode;
      safetyTrip = false;
      if (!autoMode && watering) { onPumpTurnedOff(); digitalWrite(RELAY_PIN, HIGH); }
      saveConfigToNvs();
      DynamicJsonDocument res(128);
      res["success"] = true; res["auto_mode"] = autoMode;
      String out; serializeJson(res, out);
      server.send(200, "application/json", out);
    } else {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"unknown command\"}");
    }
  });

  server.on("/settings", HTTP_GET, [](){
    if (!requireAuth()) return;
    DynamicJsonDocument doc(256);
    doc["moisture_threshold"] = moistureThreshold;
    doc["humidity_threshold"] = humidityThreshold;
    doc["watering_start_hour"] = wateringStartHour;
    doc["watering_end_hour"] = wateringEndHour;
    doc["scheduled_reboot_enabled"] = scheduledRebootEnabled;
    doc["scheduled_reboot_weekday"] = scheduledRebootWeekday;
    doc["scheduled_reboot_hour"] = scheduledRebootHour;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/settings", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      server.send(400, "application/json", "{\"success\":false}");
      return;
    }
    float m = doc["moisture_threshold"].as<float>();
    float h = doc["humidity_threshold"].as<float>();
    moistureThreshold = constrain(m, 0.0f, 100.0f);
    humidityThreshold = constrain(h, 0.0f, 100.0f);
    if (doc.containsKey("watering_start_hour")) {
      wateringStartHour = constrain(doc["watering_start_hour"].as<int>(), 0, 23);
      wateringEndHour   = constrain(doc["watering_end_hour"].as<int>(), 0, 23);
    }
    if (doc.containsKey("scheduled_reboot_enabled")) {
      scheduledRebootEnabled = doc["scheduled_reboot_enabled"].as<bool>();
      scheduledRebootWeekday = constrain(doc["scheduled_reboot_weekday"].as<int>(), 0, 6);
      scheduledRebootHour    = constrain(doc["scheduled_reboot_hour"].as<int>(), 0, 23);
    }
    saveConfigToNvs();
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/config", HTTP_GET, [](){
    if (!requireAuth()) return;
    DynamicJsonDocument doc(2048);
    doc["api_key"]    = apiKey;
    doc["city"]       = city;
    doc["country"]    = countryCode;
    doc["auth_user"]  = authUser;
    doc["using_default_pass"] = (authPass == "admin");
    doc["validate_tls_cert"] = validateTlsCert;
    doc["root_ca"]    = rootCaPem;
    doc["mqtt_enabled"]    = mqttEnabled;
    doc["mqtt_broker"]     = mqttBroker;
    doc["mqtt_port"]       = mqttPort;
    doc["mqtt_prefix"]     = mqttTopicPrefix;
    doc["mqtt_user"]       = mqttUser;
    doc["telegram_enabled"]    = telegramEnabled;
    doc["telegram_chat_id"]    = telegramChatId;
    doc["telegram_bot_token"]  = telegramBotToken;
    doc["influx_enabled"]  = influxEnabled;
    doc["influx_url"]      = influxUrl;
    doc["influx_org"]      = influxOrg;
    doc["influx_bucket"]   = influxBucket;
    doc["influx_token"]    = influxToken;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/config", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    String body = server.arg("plain");
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, body)) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    bool urlNeedsRebuild = false;
    bool mqttChanged = false;
    bool influxChanged = false;

    if (doc.containsKey("api_key")) { String v = doc["api_key"].as<String>(); if (v != apiKey) { apiKey = v; urlNeedsRebuild = true; } }
    if (doc.containsKey("city")) { String v = doc["city"].as<String>(); if (v != city) { city = v; urlNeedsRebuild = true; } }
    if (doc.containsKey("country")) { String v = doc["country"].as<String>(); v.toUpperCase(); if (v != countryCode) { countryCode = v; urlNeedsRebuild = true; } }
    if (doc.containsKey("auth_user")) { String v = doc["auth_user"].as<String>(); if (v.length() > 0 && v != authUser) authUser = v; }
    if (doc.containsKey("auth_pass")) { String v = doc["auth_pass"].as<String>(); if (v.length() > 0) authPass = v; }
    if (doc.containsKey("validate_tls_cert")) { validateTlsCert = doc["validate_tls_cert"].as<bool>(); }
    if (doc.containsKey("root_ca")) { rootCaPem = doc["root_ca"].as<String>(); }
    if (doc.containsKey("mqtt_enabled"))    { mqttEnabled    = doc["mqtt_enabled"].as<bool>();    mqttChanged = true; }
    if (doc.containsKey("mqtt_broker"))     { mqttBroker     = doc["mqtt_broker"].as<String>();  mqttChanged = true; }
    if (doc.containsKey("mqtt_port"))       { mqttPort       = doc["mqtt_port"].as<int>();        mqttChanged = true; }
    if (doc.containsKey("mqtt_prefix"))     { mqttTopicPrefix= doc["mqtt_prefix"].as<String>();   mqttChanged = true; }
    if (doc.containsKey("mqtt_user"))       { mqttUser       = doc["mqtt_user"].as<String>();     mqttChanged = true; }
    if (doc.containsKey("mqtt_pass")) { String v = doc["mqtt_pass"].as<String>(); if (v.length() > 0) { mqttPass = v; mqttChanged = true; } }
    if (doc.containsKey("telegram_enabled"))   { telegramEnabled   = doc["telegram_enabled"].as<bool>(); }
    if (doc.containsKey("telegram_chat_id"))   { telegramChatId    = doc["telegram_chat_id"].as<String>(); }
    if (doc.containsKey("telegram_bot_token")) { telegramBotToken  = doc["telegram_bot_token"].as<String>(); }
    if (doc.containsKey("influx_enabled"))  { influxEnabled  = doc["influx_enabled"].as<bool>();   influxChanged = true; }
    if (doc.containsKey("influx_url"))      { influxUrl      = doc["influx_url"].as<String>();     influxChanged = true; }
    if (doc.containsKey("influx_org"))      { influxOrg      = doc["influx_org"].as<String>();     influxChanged = true; }
    if (doc.containsKey("influx_bucket"))   { influxBucket   = doc["influx_bucket"].as<String>(); influxChanged = true; }
    if (doc.containsKey("influx_token"))    { influxToken    = doc["influx_token"].as<String>();  influxChanged = true; }

    if (urlNeedsRebuild) { buildWeatherURL(); lastWeatherUpdate = 0; lastForecastUpdate = 0; }
    saveConfigToNvs();
    checkCredentialHygiene();
    if (mqttChanged) setupMqtt();

    DynamicJsonDocument res(128);
    res["success"] = true;
    res["rebuild_url"] = urlNeedsRebuild;
    if (influxChanged) res["influx_changed"] = true;
    String out; serializeJson(res, out);
    server.send(200, "application/json", out);
  });

  server.on("/calibrate", HTTP_GET, [](){
    if (!requireAuth()) return;
    DynamicJsonDocument doc(128);
    doc["dry"] = MOISTURE_DRY;
    doc["wet"] = MOISTURE_WET;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/calibrate", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    String action = doc["action"] | "";
    int raw = readMoistureRaw();
    DynamicJsonDocument res(128);
    if (action == "set_dry") { MOISTURE_DRY = raw; saveConfigToNvs(); res["success"] = true; res["value"] = raw; }
    else if (action == "set_wet") { MOISTURE_WET = raw; saveConfigToNvs(); res["success"] = true; res["value"] = raw; }
    else { server.send(400, "application/json", "{\"success\":false,\"error\":\"unknown action\"}"); return; }
    String out; serializeJson(res, out);
    server.send(200, "application/json", out);
  });

  server.on("/history", HTTP_GET, [](){
    if (!requireAuth()) return;
    int hours = 24;
    if (server.hasArg("hours")) {
      hours = server.arg("hours").toInt();
      if (hours < 1) hours = 24;
      if (hours > 168) hours = 168;
    }
    String json = getHistoryJson(hours);
    server.send(200, "application/json", json);
  });

  server.on("/export.csv", HTTP_GET, [](){
    if (!requireAuth()) return;
    String csv = getHistoryCsv();
    String filename = "smartfarm_log_" + getIsoTimestamp() + ".csv";
    filename.replace(":", "");
    filename.replace("-", "");
    server.sendHeader("Content-Disposition", "attachment; filename=" + filename);
    server.send(200, "text/csv", csv);
  });

  server.on("/time", HTTP_GET, [](){
    if (!requireAuth()) return;
    DynamicJsonDocument doc(128);
    doc["time"] = getLocalTimeStr();
    doc["hour"] = getLocalHour();
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/diagnostics", HTTP_GET, [](){
    if (!requireAuth()) return;
    DynamicJsonDocument doc(768);
    doc["uptime"] = (long)((millis() - bootTime) / 1000);
    doc["free_heap"] = (long)ESP.getFreeHeap();
    doc["rssi"] = WiFi.RSSI();
    doc["ssid"] = WiFi.SSID();
    doc["ip"]   = WiFi.localIP().toString();
    doc["fs_used"]  = (long)LittleFS.usedBytes();
    doc["fs_total"] = (long)LittleFS.totalBytes();
    doc["mqtt_enabled"]   = mqttEnabled;
    doc["mqtt_connected"] = mqtt.connected();
    doc["influx_enabled"] = influxEnabled;
    doc["pump_cycles"]    = pumpCycles;
    doc["pump_runtime_sec"] = (long)pumpTotalRuntimeSec;
    doc["led_state"]      = getLedStateStr();
    doc["last_boot"]      = lastBootTimeStr;
    doc["scheduled_reboot_enabled"] = scheduledRebootEnabled;
    doc["scheduled_reboot_weekday"] = scheduledRebootWeekday;
    doc["scheduled_reboot_hour"]    = scheduledRebootHour;
    doc["default_credentials"]      = usingDefaultCredentials;
    doc["auth_disabled"]            = httpAuthDisabled;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/telegram/test", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    DynamicJsonDocument res(128);
    if (!telegramEnabled) {
      res["success"] = false; res["error"] = "Telegram not enabled";
    } else {
      bool ok = sendTelegramMessage("🧪 <b>Test Alert</b>\nIf you see this, Telegram alerts are working.");
      res["success"] = ok;
      if (!ok) res["error"] = "Send failed - check bot token and chat ID";
    }
    String out; serializeJson(res, out);
    server.send(200, "application/json", out);
  });

  server.on("/influx/test", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    DynamicJsonDocument res(128);
    if (!influxEnabled) {
      res["success"] = false; res["error"] = "InfluxDB not enabled";
    } else {
      bool ok = pushTestToInflux();
      res["success"] = ok;
      if (!ok) res["error"] = "Push failed - check URL, org, bucket, and token";
    }
    String out; serializeJson(res, out);
    server.send(200, "application/json", out);
  });

  server.on("/backup", HTTP_GET, [](){
    if (!requireAuth()) return;
    DynamicJsonDocument doc(2048);
    doc["api_key"]    = apiKey;
    doc["city"]       = city;
    doc["country"]    = countryCode;
    doc["auth_user"]  = authUser;
    doc["moisture_threshold"] = moistureThreshold;
    doc["humidity_threshold"] = humidityThreshold;
    doc["moisture_dry"] = MOISTURE_DRY;
    doc["moisture_wet"] = MOISTURE_WET;
    doc["watering_start_hour"] = wateringStartHour;
    doc["watering_end_hour"]   = wateringEndHour;
    doc["auto_mode"]  = autoMode;
    doc["mqtt_enabled"]    = mqttEnabled;
    doc["mqtt_broker"]     = mqttBroker;
    doc["mqtt_port"]       = mqttPort;
    doc["mqtt_prefix"]     = mqttTopicPrefix;
    doc["mqtt_user"]       = mqttUser;
    doc["telegram_enabled"]    = telegramEnabled;
    doc["telegram_chat_id"]    = telegramChatId;
    doc["telegram_bot_token"]  = telegramBotToken;
    doc["influx_enabled"]  = influxEnabled;
    doc["influx_url"]      = influxUrl;
    doc["influx_org"]      = influxOrg;
    doc["influx_bucket"]   = influxBucket;
    doc["influx_token"]    = influxToken;
    doc["validate_tls_cert"]   = validateTlsCert;
    doc["scheduled_reboot_enabled"] = scheduledRebootEnabled;
    doc["scheduled_reboot_weekday"] = scheduledRebootWeekday;
    doc["scheduled_reboot_hour"]    = scheduledRebootHour;
    doc["backup_time"] = getIsoTimestamp();
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/restore", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    String body = server.arg("plain");
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, body)) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    if (doc.containsKey("api_key"))    apiKey = doc["api_key"].as<String>();
    if (doc.containsKey("city"))       city = doc["city"].as<String>();
    if (doc.containsKey("country"))    countryCode = doc["country"].as<String>();
    if (doc.containsKey("auth_user"))  authUser = doc["auth_user"].as<String>();
    if (doc.containsKey("moisture_threshold")) moistureThreshold = doc["moisture_threshold"].as<float>();
    if (doc.containsKey("humidity_threshold")) humidityThreshold = doc["humidity_threshold"].as<float>();
    if (doc.containsKey("moisture_dry")) MOISTURE_DRY = doc["moisture_dry"].as<int>();
    if (doc.containsKey("moisture_wet")) MOISTURE_WET = doc["moisture_wet"].as<int>();
    if (doc.containsKey("watering_start_hour")) wateringStartHour = doc["watering_start_hour"].as<int>();
    if (doc.containsKey("watering_end_hour"))   wateringEndHour   = doc["watering_end_hour"].as<int>();
    if (doc.containsKey("auto_mode"))  autoMode = doc["auto_mode"].as<bool>();
    if (doc.containsKey("mqtt_enabled"))    mqttEnabled    = doc["mqtt_enabled"].as<bool>();
    if (doc.containsKey("mqtt_broker"))     mqttBroker     = doc["mqtt_broker"].as<String>();
    if (doc.containsKey("mqtt_port"))       mqttPort       = doc["mqtt_port"].as<int>();
    if (doc.containsKey("mqtt_prefix"))     mqttTopicPrefix= doc["mqtt_prefix"].as<String>();
    if (doc.containsKey("mqtt_user"))       mqttUser       = doc["mqtt_user"].as<String>();
    if (doc.containsKey("telegram_enabled"))    telegramEnabled   = doc["telegram_enabled"].as<bool>();
    if (doc.containsKey("telegram_chat_id"))    telegramChatId    = doc["telegram_chat_id"].as<String>();
    if (doc.containsKey("telegram_bot_token"))  telegramBotToken  = doc["telegram_bot_token"].as<String>();
    if (doc.containsKey("influx_enabled"))  influxEnabled  = doc["influx_enabled"].as<bool>();
    if (doc.containsKey("influx_url"))      influxUrl      = doc["influx_url"].as<String>();
    if (doc.containsKey("influx_org"))      influxOrg      = doc["influx_org"].as<String>();
    if (doc.containsKey("influx_bucket"))   influxBucket   = doc["influx_bucket"].as<String>();
    if (doc.containsKey("influx_token"))    influxToken    = doc["influx_token"].as<String>();
    if (doc.containsKey("validate_tls_cert"))   validateTlsCert   = doc["validate_tls_cert"].as<bool>();
    if (doc.containsKey("scheduled_reboot_enabled")) scheduledRebootEnabled = doc["scheduled_reboot_enabled"].as<bool>();
    if (doc.containsKey("scheduled_reboot_weekday")) scheduledRebootWeekday = doc["scheduled_reboot_weekday"].as<int>();
    if (doc.containsKey("scheduled_reboot_hour"))    scheduledRebootHour    = doc["scheduled_reboot_hour"].as<int>();
    saveConfigToNvs();
    buildWeatherURL();
    setupMqtt();
    server.send(200, "application/json", "{\"success\":true}");
    delay(500);
    ESP.restart();
  });

  server.on("/logs", HTTP_GET, [](){
    if (!requireAuth()) return;
    String log = getAccessLogTail(50);
    server.send(200, "text/plain", log);
  });

  server.on("/logs/clear", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    LittleFS.remove(ACCESS_LOG_FILE);
    logAccess("Access log cleared");
    server.send(200, "application/json", "{\"success\":true}");
  });

  server.on("/factory-reset", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    logAccess("FACTORY RESET triggered by " + server.client().remoteIP().toString());
    preferences.begin("watering", false);
    preferences.clear();
    preferences.end();
    LittleFS.format();
    WiFiManager wm;
    wm.resetSettings();
    server.send(200, "application/json", "{\"success\":true}");
    delay(1000);
    ESP.restart();
  });

  server.on("/reboot", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    logAccess("Reboot triggered");
    server.send(200, "application/json", "{\"success\":true}");
    delay(500);
    ESP.restart();
  });

  server.on("/reset-wifi", HTTP_POST, [](){
    if (!requireAuth()) return;
    if (!requireCsrf()) return;
    WiFiManager wm;
    wm.resetSettings();
    DynamicJsonDocument res(64);
    res["success"] = true;
    String out; serializeJson(res, out);
    server.send(200, "application/json", out);
    delay(500);
    ESP.restart();
  });

  server.on("/weather", HTTP_GET, [](){
    if (!requireAuth()) return;
    if (millis() - lastWeatherUpdate > WEATHER_INTERVAL_MS) getWeatherData();
    if (millis() - lastForecastUpdate > FORECAST_INTERVAL_MS) getForecastData();
    DynamicJsonDocument doc(256);
    doc["name"] = locationName;
    if (!isnan(weatherTemp)) doc["temp"] = weatherTemp; else doc["temp"] = nullptr;
    if (!isnan(weatherHum))  doc["hum"]  = weatherHum;  else doc["hum"]  = nullptr;
    doc["desc"] = weatherDescription;
    doc["rain_3h"] = rainNext3h;
    doc["rain_expected"] = rainExpected;
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // ====== Lightweight Built-in OTA ======
  server.on("/update", HTTP_GET, []() {
    if (!requireAuth()) return;
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial,sans-serif;background:#f5f5f5;padding:20px;text-align:center;}";
    html += ".btn{padding:10px 20px;background:#3498db;color:#fff;border:none;border-radius:8px;cursor:pointer;font-size:16px;}";
    html += "</style></head><body>";
    html += "<h2>ESP32-S3 Firmware Update</h2>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='update' accept='.bin' style='margin:10px;'>";
    html += "<br><input type='submit' value='Upload & Flash' class='btn'></form>";
    html += "<p>Only upload .bin files compiled for ESP32-S3.</p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/update", HTTP_POST, []() {
    if (!requireAuth()) return;
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { Update.printError(Serial); }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize); } 
      else { Update.printError(Serial); }
    }
  });

  server.begin();
  Serial.println(F("[HTTP] server started on port 80"));

  sendAlert(ALERT_BOOT);
}

// ====== Loop ======
void loop() {
  server.handleClient();
  
  updateDhtCache();
  checkWifiReconnect();
  updateStatusLed();
  checkScheduledReboot();

  if (mqttEnabled) {
    if (!mqtt.connected()) {
      if (mqttConnectedNow) {
        mqttConnectedNow = false;
        sendAlert(ALERT_MQTT_DISCONNECT);
      }
      reconnectMqtt();
    } else {
      mqtt.loop();
      publishMqttSensorData();
    }
  }

  if (influxEnabled) {
    pushToInflux();
  }

  if (autoMode) {
    bool inWindow = isWithinWateringWindow();
    bool canWater = inWindow && !rainExpected && !moistureFault;

    float moisture = readMoisturePercent();
    if (canWater && moisture < moistureThreshold && !watering) {
      watering = true; safetyTrip = false;
      onPumpTurnedOn();
      digitalWrite(RELAY_PIN, LOW);
      sendAlert(ALERT_WATERING_START);
    } else if (!canWater && watering) {
      onPumpTurnedOff();
      watering = false;
      digitalWrite(RELAY_PIN, HIGH);
      sendAlert(ALERT_WATERING_STOP);
    } else if (moisture >= (moistureThreshold + 5.0f) && watering) {
      onPumpTurnedOff();
      watering = false;
      digitalWrite(RELAY_PIN, HIGH);
      sendAlert(ALERT_WATERING_STOP);
    }
  }

  if (watering && (millis() - wateringStartTime > MAX_WATER_TIME_MS)) {
    onPumpTurnedOff();
    watering = false;
    autoMode = false;
    safetyTrip = true;
    digitalWrite(RELAY_PIN, HIGH);
    sendAlert(ALERT_SAFETY_TRIP);
    saveConfigToNvs();
  }

  logSensorData();

  if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastWeatherUpdate > WEATHER_INTERVAL_MS) getWeatherData();
    if (millis() - lastForecastUpdate > FORECAST_INTERVAL_MS) getForecastData();
  }

  delay(100);
}
