// =============================================================================
// Smart Agricultural System - ESP32-S3
// Water Monitoring + Auto Irrigation + Live Weather + OTA + Trends
// -----------------------------------------------------------------------------
// P2 FEATURES APPLIED (on top of P0 + P1 fixes):
//
//   P2-1: OTA firmware updates via AsyncElegantOTA (web UI at /update)
//   P2-2: NTP time sync (pool.ntp.org, IST = UTC+5:30)
//   P2-3: Scheduled watering window (configurable start/end hour)
//   P2-4: Sensor fault detection (DHT NaN streak + moisture out-of-range)
//   P2-5: Forecast-aware watering (skip if rain expected in next 3 hours)
//   P2-6: LittleFS history logging + /history endpoint + Chart.js trends in UI
//   P2-7: Moisture sensor calibration wizard (Set Dry / Set Wet via web UI)
// =============================================================================

// ====== Includes ======
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>     // ArduinoJson v6
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DHT.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>     // tzapu/WiFiManager
#include <AsyncElegantOTA.h> // P2-1: OTA updates
#include <LittleFS.h>        // P2-6: history log storage
#include <time.h>            // P2-2: NTP time

// ====== Hardware Pins (ESP32-S3) ======
#define DHTPIN 4
#define DHTTYPE DHT11
#define MOISTURE_PIN 1
#define RELAY_PIN 8

// ====== Objects ======
DHT dht(DHTPIN, DHTTYPE);
Preferences preferences;
AsyncWebServer server(80);

// ====== Device Configuration (loaded from NVS at boot) =====================
String apiKey       = "";
String city         = "Ahmedabad";
String countryCode  = "IN";
String authUser     = "admin";
String authPass     = "admin";
String weatherURL   = "";
String forecastURL  = "";   // P2-5: forecast endpoint

String weatherDescription = "";
float  weatherTemp = NAN;
float  weatherHum  = NAN;
String locationName = "—";

// P2-5: Rain forecast state
float  rainNext3h = 0.0f;            // mm of rain expected in next 3 hours
bool   rainExpected = false;         // true if rainNext3h > RAIN_THRESHOLD_MM
const float RAIN_THRESHOLD_MM = 1.0f; // skip watering if >= 1mm rain forecast

unsigned long lastWeatherUpdate = 0;
unsigned long lastForecastUpdate = 0;
const unsigned long WEATHER_INTERVAL_MS  = 600000UL;  // 10 min
const unsigned long FORECAST_INTERVAL_MS = 1800000UL; // 30 min

// ====== Control & Thresholds ======
bool watering = false;
bool autoMode = true;
float moistureThreshold = 30.0f;
float humidityThreshold = 50.0f;

// P2-7: Moisture calibration (now runtime-configurable, not const)
int MOISTURE_DRY = 4095;
int MOISTURE_WET = 1500;

// ====== DHT11 Reading Cache =====================================
float  cachedTemp = NAN;
float  cachedHum  = NAN;
unsigned long lastDhtRead = 0;
const unsigned long DHT_INTERVAL_MS = 2500UL;

// ====== P2-4: Sensor Fault Detection ========================================
// DHT fault: triggered after 4 consecutive NaN reads (~10s at 2.5s cadence)
// Moisture fault: triggered after 10 consecutive reads where raw ADC == 0 or
//                 raw ADC == 4095 (indicates sensor disconnect or short)
int  dhtFailCount = 0;
bool dhtFault = false;
int  moistureFailCount = 0;
bool moistureFault = false;
const int DHT_FAULT_THRESHOLD = 4;
const int MOISTURE_FAULT_THRESHOLD =  10;

// ====== P2-5: Max Watering Safety (from P1-5) ===============================
unsigned long wateringStartTime = 0;
const unsigned long MAX_WATER_TIME_MS = 300000UL; // 5 minutes
bool safetyTrip = false;

// ====== P2-3: Scheduled Watering Window =====================================
// wateringStartHour and wateringEndHour are 0-23. If start == end, watering is
// always allowed. Otherwise, watering is only allowed within the window.
// Handles overnight windows (e.g., start=22, end=6 means 10pm to 6am).
int wateringStartHour = 6;   // default 6 AM
int wateringEndHour   = 8;   // default 8 AM

// ====== WiFi Reconnect ======
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000UL;

// ====== P2-6: History Logging ===============================================
const char* LOG_FILE = "/log.csv";
const unsigned long LOG_INTERVAL_MS = 60000UL; // log every 60s
unsigned long lastLogTime = 0;
const size_t  MAX_LOG_SIZE = 200000; // ~200KB before truncation

// ====== P2-2: NTP / Time ====================================================
// IST = UTC + 5:30 = 19800 seconds
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
.btn{padding:10px 16px;border:none;border-radius:8px;cursor:pointer;font-weight:700;transition:transform .1s ease,opacity .1s ease}
.btn:active{transform:scale(.98)}
.btn-primary{background:var(--secondary);color:#fff}
.btn-danger{background:var(--accent);color:#fff}
.btn-success{background:var(--success);color:#fff}
.btn-warn{background:var(--warn);color:#fff}
.form-row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.input{width:100%;padding:10px;border:1px solid #ddd;border-radius:8px}
.notice{font-size:12px;opacity:.7}
.kv{display:flex;justify-content:space-between;margin:6px 0}
.badge{display:inline-block;background:#eef;border-radius:999px;padding:6px 10px;font-weight:700}
.badge-rain{background:#aed6f1;color:#1b4f72}
.badge-ok{background:#d5f5e3;color:#1e8449}
.small{font-size:12px}
.warn-box{background:#fff3cd;border:1px solid #ffe69c;color:#664d03;padding:10px;border-radius:8px;margin:8px 0;font-size:13px}
.danger-box{background:#f8d7da;border:1px solid #f1aeb5;color:#58151c;padding:10px;border-radius:8px;margin:8px 0;font-size:13px}
.info-box{background:#d1ecf1;border:1px solid #bee5eb;color:#0c5460;padding:10px;border-radius:8px;margin:8px 0;font-size:13px}
.chart-container{position:relative;height:300px}
</style>
</head>
<body>
<div class="container">

  <header>
    <h1>Smart Agricultural System</h1>
    <div class="small">ESP32-S3 • DHT11 + Soil Moisture + Relay • Live Weather + Trends + OTA</div>
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
      <div class="notice">Current conditions update every 10 min. Forecast every 30 min.</div>
    </div>
  </div>

  <div class="card">
    <h2>Trends (last 24 hours)</h2>
    <div class="chart-container">
      <canvas id="trends-chart"></canvas>
    </div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-primary" id="refresh-chart">Refresh Chart</button>
    </div>
    <div class="notice">Data is logged every 60 seconds on the device.</div>
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
    <div class="notice">If start == end, watering is allowed any time. Set both to 0 to disable scheduling.</div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-success" id="save-settings">Save</button>
    </div>
    <div class="notice">Thresholds and schedule persist in device memory.</div>
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
      <div></div>
    </div>
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-success" id="save-config">Save Configuration</button>
      <button class="btn btn-warn" id="reset-wifi">Reset WiFi Configuration</button>
      <a class="btn btn-primary" href="/update" target="_blank">OTA Firmware Update</a>
    </div>
    <div class="notice">
      API key and credentials are stored in device memory (NVS).<br>
      OTA updates: upload a compiled .bin file at <code>/update</code>.<br>
      "Reset WiFi Configuration" clears stored WiFi credentials and reboots into captive portal.
    </div>
  </div>

</div>

<script>
const CSRF_HEADER = {'Content-Type':'application/json','X-Requested-With':'XMLHttpRequest'};

const temperatureEl = document.getElementById('temperature');
const humidityEl    = document.getElementById('humidity');
const moistureEl    = document.getElementById('moisture');
const systemStatusEl= document.getElementById('system-status');
const relayStatusEl = document.getElementById('relay-status');
const clockEl       = document.getElementById('clock');

const waterOnBtn  = document.getElementById('water-on');
const waterOffBtn = document.getElementById('water-off');
const autoModeBtn = document.getElementById('auto-mode');

const wName = document.getElementById('w-name');
const wDesc = document.getElementById('w-desc');
const wTemp = document.getElementById('w-temp');
const wHum  = document.getElementById('w-hum');
const wRain = document.getElementById('w-rain');
const wRefresh = document.getElementById('w-refresh');
const rainWarn = document.getElementById('rain-warn');
const rainDetail = document.getElementById('rain-detail');

const moistInp = document.getElementById('moisture-threshold');
const humInp   = document.getElementById('humidity-threshold');
const waterStartInp = document.getElementById('water-start-hour');
const waterEndInp   = document.getElementById('water-end-hour');
const saveBtn  = document.getElementById('save-settings');

const cfgApiKey = document.getElementById('cfg-api-key');
const cfgCity   = document.getElementById('cfg-city');
const cfgCountry= document.getElementById('cfg-country');
const cfgAuthUser = document.getElementById('cfg-auth-user');
const cfgAuthPass = document.getElementById('cfg-auth-pass');
const saveCfgBtn = document.getElementById('save-config');
const resetWifiBtn = document.getElementById('reset-wifi');

const safetyWarn = document.getElementById('safety-warn');
const defaultCredsWarn = document.getElementById('default-creds-warn');
const faultWarn = document.getElementById('fault-warn');
const faultDetail = document.getElementById('fault-detail');
const tempFaultBadge = document.getElementById('temp-fault');
const humFaultBadge  = document.getElementById('hum-fault');
const moistFaultBadge= document.getElementById('moist-fault');

const calibRaw  = document.getElementById('calib-raw');
const calibDry  = document.getElementById('calib-dry');
const calibWet  = document.getElementById('calib-wet');
const calibDryBtn = document.getElementById('calib-dry-btn');
const calibWetBtn = document.getElementById('calib-wet-btn');
const calibRefresh = document.getElementById('calib-refresh');

const refreshChartBtn = document.getElementById('refresh-chart');
let trendsChart = null;

let moistureThreshold = 30;
let humidityThreshold = 50;

function updateSensorData(){
  fetch('/sensor-data').then(r=>r.json()).then(d=>{
    temperatureEl.textContent = (typeof d.temperature==='number') ? d.temperature.toFixed(1) : '--';
    humidityEl.textContent    = (typeof d.humidity==='number')    ? d.humidity.toFixed(1)    : '--';
    moistureEl.textContent    = (typeof d.moisture==='number')    ? d.moisture.toFixed(1)    : '--';
    calibRaw.textContent = (typeof d.raw_moisture==='number') ? d.raw_moisture : '—';
  }).catch(()=>{});
}

function updateSystemStatus(){
  fetch('/system-status').then(r=>r.json()).then(d=>{
    systemStatusEl.textContent = `System Status: ${d.status}`;
    systemStatusEl.className = `status ${d.status==='Auto'?'status-auto':'status-manual'}`;
    relayStatusEl.textContent = `Motor Status: ${d.relay?'ON':'OFF'}`;
    relayStatusEl.className   = `status ${d.relay?'status-on':'status-off'}`;
    updateAutoModeButton(d.status==='Auto');
    safetyWarn.style.display = d.safety_trip ? 'block' : 'none';

    // Fault indicators
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
    wHum.textContent  = (typeof w.hum==='number')  ? `${w.hum.toFixed(0)} %`   : '—';
    if (typeof w.rain_3h==='number') {
      wRain.textContent = `${w.rain_3h.toFixed(1)} mm`;
      if (w.rain_expected) { rainDetail.textContent = w.rain_3h.toFixed(1); rainWarn.style.display = 'block'; }
      else { rainWarn.style.display = 'none'; }
    } else {
      wRain.textContent = '—';
      rainWarn.style.display = 'none';
    }
  }).catch(()=>{});
}

function loadSettings(){
  fetch('/settings').then(r=>r.json()).then(s=>{
    moistureThreshold = s.moisture_threshold ?? 30;
    humidityThreshold = s.humidity_threshold ?? 50;
    moistInp.value = moistureThreshold;
    humInp.value = humidityThreshold;
    waterStartInp.value = s.watering_start_hour ?? 6;
    waterEndInp.value   = s.watering_end_hour ?? 8;
  }).catch(()=>{});
}

function saveSettings(){
  const body = {
    moisture_threshold: parseFloat(moistInp.value||30),
    humidity_threshold: parseFloat(humInp.value||50),
    watering_start_hour: parseInt(waterStartInp.value||0),
    watering_end_hour: parseInt(waterEndInp.value||0)
  };
  fetch('/settings',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify(body)})
    .then(r=>r.json()).then(_=>{ loadSettings(); }).catch(()=>{});
}

function sendCommand(command){
  fetch('/control',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify({command})})
    .then(r=>r.json()).then(_=>{ updateSystemStatus(); }).catch(()=>{});
}

function updateAutoModeButton(autoOn){ autoModeBtn.textContent = autoOn ? 'Auto Mode: ON' : 'Auto Mode: OFF'; }

waterOnBtn.onclick = ()=>sendCommand('water_on');
waterOffBtn.onclick= ()=>sendCommand('water_off');
autoModeBtn.onclick= ()=>sendCommand('auto_mode');
wRefresh.onclick   = ()=>fetchWeather();
saveBtn.onclick    = ()=>saveSettings();

function loadConfig(){
  fetch('/config').then(r=>r.json()).then(c=>{
    cfgApiKey.value    = c.api_key || '';
    cfgCity.value      = c.city || '';
    cfgCountry.value   = c.country || '';
    cfgAuthUser.value  = c.auth_user || 'admin';
    cfgAuthPass.value  = '';
    if (c.auth_user === 'admin' && c.using_default_pass) defaultCredsWarn.style.display = 'block';
    else defaultCredsWarn.style.display = 'none';
  }).catch(()=>{});
}

function saveConfig(){
  const body = { api_key: cfgApiKey.value, city: cfgCity.value, country: cfgCountry.value.toUpperCase(), auth_user: cfgAuthUser.value };
  if (cfgAuthPass.value) body.auth_pass = cfgAuthPass.value;
  fetch('/config',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{
      if (d.success) {
        alert('Configuration saved. If you changed the API key or city, weather will refresh shortly.');
        if (d.rebuild_url) setTimeout(fetchWeather, 1500);
        loadConfig();
      } else alert('Failed to save: ' + (d.error || 'unknown error'));
    }).catch(()=>{ alert('Network error saving config'); });
}

function resetWifi(){
  if (!confirm('This will erase stored WiFi credentials and reboot the device into the captive portal. Continue?')) return;
  fetch('/reset-wifi',{method:'POST',headers:CSRF_HEADER})
    .then(r=>r.json()).then(d=>{ if (d.success) alert('Rebooting. Connect to the "SmartAgri-Setup" WiFi network to reconfigure.'); })
    .catch(()=>{});
}

saveCfgBtn.onclick = saveConfig;
resetWifiBtn.onclick = resetWifi;

// P2-7: Calibration
function loadCalibration(){
  fetch('/calibrate').then(r=>r.json()).then(c=>{
    calibDry.textContent = c.dry ?? '—';
    calibWet.textContent = c.wet ?? '—';
  }).catch(()=>{});
}
function setCalibration(action){
  fetch('/calibrate',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify({action})})
    .then(r=>r.json()).then(d=>{
      if (d.success) { loadCalibration(); alert(`${action === 'set_dry' ? 'Dry' : 'Wet'} calibration saved: ${d.value}`); }
      else alert('Calibration failed: ' + (d.error || 'unknown'));
    }).catch(()=>{ alert('Network error'); });
}
calibDryBtn.onclick = ()=>setCalibration('set_dry');
calibWetBtn.onclick = ()=>setCalibration('set_wet');
calibRefresh.onclick = ()=>{ updateSensorData(); loadCalibration(); };

// P2-6: Trends chart
function fetchAndRenderChart(){
  fetch('/history?hours=24').then(r=>r.json()).then(data=>{
    const labels = data.map(p => p.t);
    const tempData = data.map(p => p.temp);
    const humData  = data.map(p => p.hum);
    const moistData= data.map(p => p.moist);
    const ctx = document.getElementById('trends-chart').getContext('2d');
    if (trendsChart) trendsChart.destroy();
    trendsChart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: labels,
        datasets: [
          { label: 'Temperature (°C)', data: tempData,  borderColor: '#e74c3c', backgroundColor: 'rgba(231,76,60,0.1)', tension: 0.3, yAxisID: 'y' },
          { label: 'Humidity (%)',     data: humData,   borderColor: '#3498db', backgroundColor: 'rgba(52,152,219,0.1)', tension: 0.3, yAxisID: 'y' },
          { label: 'Soil Moisture (%)',data: moistData, borderColor: '#27ae60', backgroundColor: 'rgba(39,174,96,0.1)', tension: 0.3, yAxisID: 'y' }
        ]
      },
      options: {
        responsive: true, maintainAspectRatio: false,
        scales: { x: { ticks: { maxTicksLimit: 12 } }, y: { min: 0, max: 100, title: { display: true, text: 'Value' } } },
        plugins: { legend: { position: 'top' } }
      }
    });
  }).catch(()=>{});
}
refreshChartBtn.onclick = fetchAndRenderChart;

function updateClock(){ fetch('/time').then(r=>r.json()).then(d=>{ clockEl.textContent = d.time || '—'; }).catch(()=>{}); }

function init(){
  loadSettings();
  loadConfig();
  loadCalibration();
  updateSensorData();
  updateSystemStatus();
  fetchWeather();
  fetchAndRenderChart();
  updateClock();
  setInterval(()=>{ updateSensorData(); updateSystemStatus(); }, 5000);
  setInterval(()=>{ fetchWeather(); }, 600000);
  setInterval(()=>{ updateClock(); }, 30000);
  setInterval(()=>{ fetchAndRenderChart(); }, 300000);
}
init();
</script>
</body>
</html>
)rawliteral";

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

  // P2-4: Sensor fault detection - check for disconnected/shorted sensor
  // (raw at extreme 0 or 4095 means sensor is disconnected or shorted)
  if (raw == 0 || raw == 4095) {
    moistureFailCount++;
    if (moistureFailCount >= MOISTURE_FAULT_THRESHOLD) {
      if (!moistureFault) Serial.println(F("[FAULT] Moisture sensor fault detected"));
      moistureFault = true;
    }
  } else {
    if (moistureFault) Serial.println(F("[FAULT] Moisture sensor recovered"));
    moistureFault = false;
    moistureFailCount = 0;
  }

  float denom_map = (float)(MOISTURE_DRY - MOISTURE_WET);
  float percent_unflipped = 0.0f;
  if (denom_map == 0.0f) percent_unflipped = 0.0f;
  else percent_unflipped = (float)(raw - MOISTURE_WET) * 100.0f / denom_map;
  percent_unflipped = constrain(percent_unflipped, 0.0f, 100.0f);
  float mapped = 100.0f - percent_unflipped;

  Serial.printf("[MOIST] raw=%d min=%d max=%d mapped=%.1f%% fault=%d\n", raw, minv, maxv, mapped, moistureFault?1:0);
  return mapped;
}

int readMoistureRaw() {
  // Single averaged raw reading (used by calibration wizard)
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

  // P2-4: DHT fault detection
  if (isnan(t) || isnan(h)) {
    dhtFailCount++;
    if (dhtFailCount >= DHT_FAULT_THRESHOLD) {
      if (!dhtFault) Serial.println(F("[FAULT] DHT11 sensor fault detected"));
      dhtFault = true;
    }
  } else {
    cachedTemp = t;
    cachedHum  = h;
    if (dhtFault) Serial.println(F("[FAULT] DHT11 sensor recovered"));
    dhtFault = false;
    dhtFailCount = 0;
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
  preferences.end();
}

void buildWeatherURL() {
  weatherURL  = "https://api.openweathermap.org/data/2.5/weather?q="  + city + "," + countryCode + "&appid=" + apiKey + "&units=metric";
  forecastURL = "https://api.openweathermap.org/data/2.5/forecast?q=" + city + "," + countryCode + "&appid=" + apiKey + "&units=metric";
}

// ====== Auth + CSRF ======
bool requireAuth(AsyncWebServerRequest *request) {
  if (authUser.length() == 0 || authPass.length() == 0) return true;
  if (!request->authenticate(authUser.c_str(), authPass.c_str())) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

bool requireCsrf(AsyncWebServerRequest *request) {
  if (!request->hasHeader("X-Requested-With")) {
    request->send(403, "application/json", "{\"success\":false,\"error\":\"missing CSRF header\"}");
    return false;
  }
  return true;
}

// ====== Weather fetch (current conditions) ======
void getWeatherData() {
  if (WiFi.status() != WL_CONNECTED || apiKey.length() < 10) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
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
      Serial.println(F("[Weather] Updated OK"));
    } else Serial.println(F("[Weather] JSON parse error"));
  } else Serial.printf("[Weather] HTTPS error: %d\n", code);
  http.end();
  lastWeatherUpdate = millis();
}

// ====== P2-5: Forecast fetch (rain in next 3 hours) ======
void getForecastData() {
  if (WiFi.status() != WL_CONNECTED || apiKey.length() < 10) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(client, forecastURL);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<8192> doc;  // forecast response is larger
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      // The /forecast endpoint returns a "list" array of 3-hour slots.
      // Sum rain volume for the first slot (next 3 hours).
      float totalRain = 0.0f;
      JsonArray list = doc["list"].as<JsonArray>();
      if (list.size() > 0) {
        JsonObject first = list[0];
        if (first.containsKey("rain")) {
          totalRain = first["rain"]["3h"] | 0.0f;
        }
      }
      rainNext3h = totalRain;
      rainExpected = (totalRain >= RAIN_THRESHOLD_MM);
      Serial.printf("[Forecast] Rain next 3h: %.2f mm, expected=%d\n", totalRain, rainExpected?1:0);
    } else Serial.println(F("[Forecast] JSON parse error"));
  } else Serial.printf("[Forecast] HTTPS error: %d\n", code);
  http.end();
  lastForecastUpdate = millis();
}

// ====== WiFi reconnect ======
void checkWifiReconnect() {
  if (millis() - lastWifiCheck < WIFI_CHECK_INTERVAL_MS) return;
  lastWifiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[WiFi] Disconnected. Attempting reconnect..."));
    WiFi.reconnect();
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) delay(200);
    if (WiFi.status() == WL_CONNECTED)
      Serial.printf("[WiFi] Reconnected. IP: %s\n", WiFi.localIP().toString().c_str());
    else Serial.println(F("[WiFi] Reconnect attempt failed. Will retry."));
  }
}

// ====== P2-3: Scheduled watering window ======
bool isWithinWateringWindow() {
  if (wateringStartHour == wateringEndHour) return true;  // 0 == 0 means always allowed
  int h = getLocalHour();
  if (h < 0) return true;  // NTP not synced yet - allow (safer than blocking)
  if (wateringStartHour < wateringEndHour) {
    // Same-day window, e.g. 6 to 8
    return (h >= wateringStartHour && h < wateringEndHour);
  } else {
    // Overnight window, e.g. 22 to 6
    return (h >= wateringStartHour || h < wateringEndHour);
  }
}

// ====== P2-6: LittleFS history logging ======
void setupLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println(F("[LittleFS] Mount failed"));
    return;
  }
  Serial.printf("[LittleFS] Mounted. Used: %u bytes\n", (unsigned)LittleFS.usedBytes());
}

void logSensorData() {
  if (millis() - lastLogTime < LOG_INTERVAL_MS) return;
  lastLogTime = millis();

  // Truncate if file is too large
  if (LittleFS.exists(LOG_FILE)) {
    File f = LittleFS.open(LOG_FILE, "r");
    if (f && f.size() > MAX_LOG_SIZE) {
      f.close();
      LittleFS.remove(LOG_FILE);
      Serial.println(F("[Log] Rotated (file exceeded max size)"));
    } else if (f) {
      f.close();
    }
  }

  File logFile = LittleFS.open(LOG_FILE, "a");
  if (!logFile) {
    Serial.println(F("[Log] Failed to open log file"));
    return;
  }

  // Format: epoch,temp,hum,moist,watering
  // Use -1 for NaN so CSV parsing stays simple
  float t = isnan(cachedTemp) ? -1.0f : cachedTemp;
  float h = isnan(cachedHum)  ? -1.0f : cachedHum;
  float m = moistureFault ? -1.0f : readMoisturePercent();

  struct tm timeinfo;
  time_t now;
  time(&now);
  if (getLocalTime(&timeinfo, 2000)) {
    // Use ISO timestamp for easier JS parsing
    char ts[24];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    logFile.printf("%s,%.1f,%.1f,%.1f,%d\n", ts, t, h, m, watering?1:0);
  } else {
    logFile.printf("%ld,%.1f,%.1f,%.1f,%d\n", (long)now, t, h, m, watering?1:0);
  }
  logFile.close();
}

// Returns last N hours of history as JSON. Read entire file, filter by time.
String getHistoryJson(int hours) {
  if (!LittleFS.exists(LOG_FILE)) return "[]";
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return "[]";

  // Compute cutoff time
  time_t now;
  time(&now);
  time_t cutoff = now - (hours * 3600);

  String out = "[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Parse: ts,temp,hum,moist,watering
    // Try ISO format first, fall back to epoch
    int c1 = line.indexOf(',');
    if (c1 < 0) continue;
    String ts = line.substring(0, c1);
    String rest = line.substring(c1 + 1);

    // Parse rest
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

    // Convert ts to time_t for filtering
    // For ISO format, we trust the timestamp string and include all recent entries
    // (simple approach: include if ts contains a 4-digit year, which all ISO entries do)
    // For epoch format, parse and compare
    if (ts.length() > 0 && isdigit(ts.charAt(0)) && ts.length() > 10) {
      // epoch
      time_t t = ts.toInt();
      if (t < cutoff) continue;
    }

    if (!first) out += ",";
    first = false;
    // Format as JSON object. Convert -1 to null.
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
    out += "\"water\":" + (waterStr.toInt() ? "true" : "false") + "}";
  }
  out += "]";
  f.close();
  return out;
}

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(100);

  dht.begin();
  pinMode(MOISTURE_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  analogReadResolution(12);
  analogSetPinAttenuation(MOISTURE_PIN, ADC_11db);

  loadConfig();

  // P2-6: Mount LittleFS for history logging
  setupLittleFS();

  // P1-1: WiFiManager captive portal
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  Serial.println(F("[WiFi] Starting WiFiManager..."));
  bool res = wm.autoConnect("SmartAgri-Setup");
  if (!res) {
    Serial.println(F("[WiFi] Config portal timed out. Rebooting..."));
    delay(1000);
    ESP.restart();
  }
  Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  WiFi.setHostname("smartagri");
  if (MDNS.begin("project")) Serial.println(F("[mDNS] http://project.local/"));
  else Serial.println(F("[mDNS] failed."));

  // P2-2: NTP sync
  Serial.println(F("[NTP] Configuring time..."));
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);

  buildWeatherURL();
  getWeatherData();
  getForecastData();  // P2-5

  updateDhtCache();

  // ====== Routes ======
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    request->send_P(200, "text/html", index_html);
  });

  server.on("/sensor-data", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    int raw = readMoistureRaw();
    float m = readMoisturePercent();
    DynamicJsonDocument doc(256);
    if (!isnan(cachedTemp)) doc["temperature"] = cachedTemp; else doc["temperature"] = nullptr;
    if (!isnan(cachedHum))  doc["humidity"]    = cachedHum;  else doc["humidity"]    = nullptr;
    doc["moisture"] = m;
    doc["raw_moisture"] = raw;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/system-status", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    DynamicJsonDocument doc(256);
    doc["status"] = (autoMode ? "Auto" : "Manual");
    doc["relay"]  = watering;
    doc["safety_trip"] = safetyTrip;
    doc["dht_fault"] = dhtFault;
    doc["moisture_fault"] = moistureFault;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/control", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    String body = request->arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    String command = doc["command"] | "";
    if (command == "water_on") {
      watering = true; autoMode = false; safetyTrip = false;
      wateringStartTime = millis();
      digitalWrite(RELAY_PIN, LOW);
      saveConfigToNvs();
      request->send(200, "application/json", "{\"success\":true}");
    } else if (command == "water_off") {
      watering = false; autoMode = false; safetyTrip = false;
      digitalWrite(RELAY_PIN, HIGH);
      saveConfigToNvs();
      request->send(200, "application/json", "{\"success\":true}");
    } else if (command == "auto_mode") {
      autoMode = !autoMode;
      safetyTrip = false;
      if (!autoMode) { watering = false; digitalWrite(RELAY_PIN, HIGH); }
      saveConfigToNvs();
      DynamicJsonDocument res(128);
      res["success"] = true; res["auto_mode"] = autoMode;
      String out; serializeJson(res, out);
      request->send(200, "application/json", out);
    } else {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"unknown command\"}");
    }
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    DynamicJsonDocument doc(256);
    doc["moisture_threshold"] = moistureThreshold;
    doc["humidity_threshold"] = humidityThreshold;
    doc["watering_start_hour"] = wateringStartHour;
    doc["watering_end_hour"] = wateringEndHour;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/settings", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    String body = request->arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      request->send(400, "application/json", "{\"success\":false}");
      return;
    }
    float m = doc["moisture_threshold"].as<float>();
    float h = doc["humidity_threshold"].as<float>();
    moistureThreshold = constrain(m, 0.0f, 100.0f);
    humidityThreshold = constrain(h, 0.0f, 100.0f);
    if (doc.containsKey("watering_start_hour")) {
      int sh = doc["watering_start_hour"].as<int>();
      int eh = doc["watering_end_hour"].as<int>();
      wateringStartHour = constrain(sh, 0, 23);
      wateringEndHour   = constrain(eh, 0, 23);
    }
    saveConfigToNvs();
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    DynamicJsonDocument doc(256);
    doc["api_key"]    = apiKey;
    doc["city"]       = city;
    doc["country"]    = countryCode;
    doc["auth_user"]  = authUser;
    doc["using_default_pass"] = (authPass == "admin");
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    String body = request->arg("plain");
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    bool urlNeedsRebuild = false;
    bool authChanged = false;
    if (doc.containsKey("api_key")) {
      String newKey = doc["api_key"].as<String>();
      if (newKey != apiKey) { apiKey = newKey; urlNeedsRebuild = true; }
    }
    if (doc.containsKey("city")) {
      String newCity = doc["city"].as<String>();
      if (newCity != city) { city = newCity; urlNeedsRebuild = true; }
    }
    if (doc.containsKey("country")) {
      String newCountry = doc["country"].as<String>();
      newCountry.toUpperCase();
      if (newCountry != countryCode) { countryCode = newCountry; urlNeedsRebuild = true; }
    }
    if (doc.containsKey("auth_user")) {
      String newUser = doc["auth_user"].as<String>();
      if (newUser.length() > 0 && newUser != authUser) { authUser = newUser; authChanged = true; }
    }
    if (doc.containsKey("auth_pass")) {
      String newPass = doc["auth_pass"].as<String>();
      if (newPass.length() > 0) { authPass = newPass; authChanged = true; }
    }
    if (urlNeedsRebuild) {
      buildWeatherURL();
      lastWeatherUpdate = 0;
      lastForecastUpdate = 0;
    }
    saveConfigToNvs();
    DynamicJsonDocument res(128);
    res["success"] = true;
    res["rebuild_url"] = urlNeedsRebuild;
    if (authChanged) res["auth_changed"] = true;
    String out; serializeJson(res, out);
    request->send(200, "application/json", out);
  });

  // P2-7: Calibration endpoints
  server.on("/calibrate", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    DynamicJsonDocument doc(128);
    doc["dry"] = MOISTURE_DRY;
    doc["wet"] = MOISTURE_WET;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/calibrate", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    String body = request->arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    String action = doc["action"] | "";
    int raw = readMoistureRaw();
    DynamicJsonDocument res(128);
    if (action == "set_dry") {
      MOISTURE_DRY = raw;
      saveConfigToNvs();
      res["success"] = true; res["value"] = raw;
      Serial.printf("[Calib] Dry set to %d\n", raw);
    } else if (action == "set_wet") {
      MOISTURE_WET = raw;
      saveConfigToNvs();
      res["success"] = true; res["value"] = raw;
      Serial.printf("[Calib] Wet set to %d\n", raw);
    } else {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"unknown action\"}");
      return;
    }
    String out; serializeJson(res, out);
    request->send(200, "application/json", out);
  });

  // P2-6: History endpoint
  server.on("/history", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    int hours = 24;
    if (request->hasParam("hours")) {
      hours = request->getParam("hours")->value().toInt();
      if (hours < 1) hours = 24;
      if (hours > 168) hours = 168; // cap at 1 week
    }
    String json = getHistoryJson(hours);
    request->send(200, "application/json", json);
  });

  // P2-2: Time endpoint
  server.on("/time", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    DynamicJsonDocument doc(128);
    doc["time"] = getLocalTimeStr();
    doc["hour"] = getLocalHour();
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/reset-wifi", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    if (!requireCsrf(request)) return;
    WiFiManager wm;
    wm.resetSettings();
    DynamicJsonDocument res(64);
    res["success"] = true;
    String out; serializeJson(res, out);
    request->send(200, "application/json", out);
    delay(500);
    ESP.restart();
  });

  server.on("/weather", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
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
    request->send(200, "application/json", out);
  });

  // P2-1: AsyncElegantOTA - serves /update route
  AsyncElegantOTA.begin(&server);

  server.begin();
  Serial.println(F("[HTTP] server started"));
  Serial.println(F("[OTA]  Firmware update UI at /update"));
}

// ====== Loop ======
void loop() {
  updateDhtCache();
  checkWifiReconnect();
  AsyncElegantOTA.loop();  // P2-1

  // Auto watering logic with all P2 conditions:
  //   1. Within scheduled watering window
  //   2. No rain expected in next 3 hours
  //   3. No sensor faults
  //   4. Moisture below threshold (with 5% hysteresis)
  //   5. Max watering duration not exceeded (P1-5)
  if (autoMode) {
    bool inWindow = isWithinWateringWindow();
    bool canWater = inWindow && !rainExpected && !moistureFault;

    float moisture = readMoisturePercent();
    if (canWater && moisture < moistureThreshold && !watering) {
      watering = true; safetyTrip = false;
      wateringStartTime = millis();
      digitalWrite(RELAY_PIN, LOW);
      Serial.printf("[Auto] Water ON (Moisture: %.1f%% < %.1f%%, inWindow=%d, rain=%d)\n",
                    moisture, moistureThreshold, inWindow, rainExpected?1:0);
    } else if (!canWater && watering) {
      // Conditions changed while watering - turn off
      watering = false;
      digitalWrite(RELAY_PIN, HIGH);
      Serial.println(F("[Auto] Water OFF (conditions no longer met)"));
    } else if (moisture >= (moistureThreshold + 5.0f) && watering) {
      watering = false;
      digitalWrite(RELAY_PIN, HIGH);
      Serial.printf("[Auto] Water OFF (Moisture: %.1f%% >= %.1f%%)\n", moisture, moistureThreshold + 5.0f);
    }
  }

  // P1-5: Max watering duration safety cutoff
  if (watering && (millis() - wateringStartTime > MAX_WATER_TIME_MS)) {
    watering = false;
    autoMode = false;
    safetyTrip = true;
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println(F("[SAFETY] Max watering time exceeded! Pump OFF, auto-mode disabled."));
    saveConfigToNvs();
  }

  // P2-6: Periodic history logging
  logSensorData();

  // Periodic weather + forecast refresh
  if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastWeatherUpdate > WEATHER_INTERVAL_MS) getWeatherData();
    if (millis() - lastForecastUpdate > FORECAST_INTERVAL_MS) getForecastData();
  }

  delay(100);
}
