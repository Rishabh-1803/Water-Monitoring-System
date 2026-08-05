// =============================================================================
// Smart Agricultural System - ESP32-S3
// Water Monitoring + Auto Irrigation + Live Weather
// -----------------------------------------------------------------------------
// P1 FIXES APPLIED (see /CHANGELOG.md for full details):
//
//   Board:           ESP32-C3 Super Mini  ->  ESP32-S3
//                     (GPIO 1/4/8 are all safe on S3 - no strapping pin issues)
//
//   P1-1: WiFiManager captive portal (no hardcoded SSID/password in source)
//   P1-2: HTTPS for OpenWeatherMap API (WiFiClientSecure, setInsecure())
//   P1-3: HTTP Basic Auth on all routes (configurable username/password)
//   P1-4: WiFi auto-reconnect logic in loop()
//   P1-5: Max watering duration safety cutoff (5 min, then auto-mode disabled)
//   P1-6: CSRF protection via X-Requested-With header on POST endpoints
//   P1-7: API key / city / country / auth credentials stored in Preferences
//         and configurable via the web UI (no secrets in source code)
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
#include <WiFiManager.h>     // tzapu/WiFiManager (P1-1)

// ====== Hardware Pins (ESP32-S3) ======
// All three pins are safe on the ESP32-S3 dev module:
//   - GPIO 1 = ADC1_CH0 (valid analog input)
//   - GPIO 4 = ADC1_CH3 (used here as digital for DHT11)
//   - GPIO 8 = general-purpose output (NOT a strapping pin on S3)
// ESP32-S3 strapping pins are GPIO 0, 3, 45, 46 - none of which we use.
#define DHTPIN 4
#define DHTTYPE DHT11
#define MOISTURE_PIN 1
#define RELAY_PIN 8

// ====== Objects ======
DHT dht(DHTPIN, DHTTYPE);
Preferences preferences;
AsyncWebServer server(80);

// ====== Device Configuration (loaded from NVS at boot) =====================
// None of these are hardcoded in source - all configurable via web UI.
// On first boot, defaults are written to NVS.
String apiKey       = "";          // OpenWeatherMap API key (user must set)
String city         = "Ahmedabad"; // Default city
String countryCode  = "IN";        // Default country
String authUser     = "admin";     // Web UI username (CHANGE THIS)
String authPass     = "admin";     // Web UI password (CHANGE THIS)
String weatherURL   = "";          // Built dynamically in buildWeatherURL()

String weatherDescription = "";
float  weatherTemp = NAN;
float  weatherHum  = NAN;
String locationName = "—";

unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_INTERVAL_MS = 600000UL; // 10 min

// ====== Control & Thresholds ======
bool watering = false;
bool autoMode = true;
float moistureThreshold = 30.0f;  // %
float humidityThreshold = 50.0f;  // % (for alerting only)

// Calibrate these to your sensor
const int MOISTURE_DRY = 4095;   // ADC at fully dry
const int MOISTURE_WET = 1500;   // ADC at fully wet

// ====== DHT11 Reading Cache (from P0-2) =====================================
float  cachedTemp = NAN;
float  cachedHum  = NAN;
unsigned long lastDhtRead = 0;
const unsigned long DHT_INTERVAL_MS = 2500UL; // > 2s per DHT11 datasheet

// ====== P1-5: Max Watering Safety ===========================================
// If the pump runs longer than MAX_WATER_TIME_MS (e.g. due to sensor failure
// or stuck relay), force it off and disable auto-mode to prevent flooding.
unsigned long wateringStartTime = 0;
const unsigned long MAX_WATER_TIME_MS = 300000UL; // 5 minutes
bool safetyTrip = false;  // true if max-watering cutoff was triggered

// ====== P1-4: WiFi Reconnect ================================================
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000UL; // check every 10s

// ====== HTML (single page app) ======
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0"/>
<title>Smart Agricultural System</title>
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
.sensor-box{background:var(--light);border-radius:10px;padding:12px;text-align:center}
.sensor-name{font-weight:600}
.sensor-value{font-size:26px;font-weight:800;margin:8px 0}
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
.small{font-size:12px}
.warn-box{background:#fff3cd;border:1px solid #ffe69c;color:#664d03;padding:10px;border-radius:8px;margin:8px 0;font-size:13px}
.danger-box{background:#f8d7da;border:1px solid #f1aeb5;color:#58151c;padding:10px;border-radius:8px;margin:8px 0;font-size:13px}
</style>
</head>
<body>
<div class="container">

  <header>
    <h1>Smart Agricultural System</h1>
    <div class="small">ESP32-S3 • DHT11 + Soil Moisture + Relay • Live Weather (OpenWeatherMap)</div>
  </header>

  <div id="safety-warn" class="danger-box" style="display:none">
    <strong>Safety Trip:</strong> Pump was force-stopped after running longer than 5 minutes.
    Auto-mode has been disabled. Check your moisture sensor and re-enable auto-mode manually.
  </div>

  <div id="default-creds-warn" class="warn-box" style="display:none">
    <strong>Security Warning:</strong> Web UI is using default credentials (admin/admin).
    Change them in the Configuration section below.
  </div>

  <div class="grid">
    <div class="card">
      <h2>Field Sensors</h2>
      <div class="sensor-grid">
        <div class="sensor-box">
          <div class="sensor-name">Temperature</div>
          <div class="sensor-value" id="temperature">--</div>
          <div class="sensor-unit">°C</div>
        </div>
        <div class="sensor-box">
          <div class="sensor-name">Humidity</div>
          <div class="sensor-value" id="humidity">--</div>
          <div class="sensor-unit">%</div>
        </div>
        <div class="sensor-box">
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
      <div class="controls" style="margin-top:10px">
        <button class="btn btn-primary" id="w-refresh">Refresh Weather</button>
      </div>
      <div class="notice">Auto-updates every 10 minutes. Requires API key (see Configuration).</div>
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
    <div class="controls" style="margin-top:10px">
      <button class="btn btn-success" id="save-settings">Save</button>
    </div>
    <div class="notice">Thresholds persist in device memory.</div>
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
    </div>
    <div class="notice">
      API key and credentials are stored in device memory (NVS).<br>
      "Reset WiFi Configuration" clears stored WiFi credentials and reboots
      the device into the captive portal for reconfiguration.
    </div>
  </div>

</div>

<script>
// P1-6: X-Requested-With header is sent on all POST requests to prevent CSRF.
// The server rejects any POST without this header.
const CSRF_HEADER = {'Content-Type':'application/json','X-Requested-With':'XMLHttpRequest'};

const temperatureEl = document.getElementById('temperature');
const humidityEl    = document.getElementById('humidity');
const moistureEl    = document.getElementById('moisture');
const systemStatusEl= document.getElementById('system-status');
const relayStatusEl = document.getElementById('relay-status');

const waterOnBtn  = document.getElementById('water-on');
const waterOffBtn = document.getElementById('water-off');
const autoModeBtn = document.getElementById('auto-mode');

const wName = document.getElementById('w-name');
const wDesc = document.getElementById('w-desc');
const wTemp = document.getElementById('w-temp');
const wHum  = document.getElementById('w-hum');
const wRefresh = document.getElementById('w-refresh');

const moistInp = document.getElementById('moisture-threshold');
const humInp   = document.getElementById('humidity-threshold');
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

let moistureThreshold = 30;
let humidityThreshold = 50;

function updateSensorData(){
  fetch('/sensor-data').then(r=>r.json()).then(d=>{
    temperatureEl.textContent = (typeof d.temperature==='number') ? d.temperature.toFixed(1) : '--';
    humidityEl.textContent    = (typeof d.humidity==='number')    ? d.humidity.toFixed(1)    : '--';
    moistureEl.textContent    = (typeof d.moisture==='number')    ? d.moisture.toFixed(1)    : '--';
  }).catch(()=>{});
}

function updateSystemStatus(){
  fetch('/system-status').then(r=>r.json()).then(d=>{
    systemStatusEl.textContent = `System Status: ${d.status}`;
    systemStatusEl.className = `status ${d.status==='Auto'?'status-auto':'status-manual'}`;
    relayStatusEl.textContent = `Motor Status: ${d.relay?'ON':'OFF'}`;
    relayStatusEl.className   = `status ${d.relay?'status-on':'status-off'}`;
    updateAutoModeButton(d.status==='Auto');
    if (d.safety_trip) safetyWarn.style.display = 'block';
    else safetyWarn.style.display = 'none';
  }).catch(()=>{});
}

function fetchWeather(){
  fetch('/weather').then(r=>r.json()).then(w=>{
    wName.textContent = w.name || '—';
    wDesc.textContent = w.desc || '—';
    wTemp.textContent = (typeof w.temp==='number') ? `${w.temp.toFixed(1)} °C` : '—';
    wHum.textContent  = (typeof w.hum==='number')  ? `${w.hum.toFixed(0)} %`   : '—';
  }).catch(()=>{});
}

function loadSettings(){
  fetch('/settings').then(r=>r.json()).then(s=>{
    moistureThreshold = s.moisture_threshold ?? 30;
    humidityThreshold = s.humidity_threshold ?? 50;
    moistInp.value = moistureThreshold;
    humInp.value = humidityThreshold;
  }).catch(()=>{});
}

function saveSettings(){
  const body = {
    moisture_threshold: parseFloat(moistInp.value||30),
    humidity_threshold: parseFloat(humInp.value||50)
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

// P1-7: Load configuration from device and populate form
function loadConfig(){
  fetch('/config').then(r=>r.json()).then(c=>{
    cfgApiKey.value    = c.api_key || '';
    cfgCity.value      = c.city || '';
    cfgCountry.value   = c.country || '';
    cfgAuthUser.value  = c.auth_user || 'admin';
    cfgAuthPass.value  = '';  // never echo password back
    if (c.auth_user === 'admin' && c.using_default_pass) {
      defaultCredsWarn.style.display = 'block';
    } else {
      defaultCredsWarn.style.display = 'none';
    }
  }).catch(()=>{});
}

function saveConfig(){
  const body = {
    api_key:    cfgApiKey.value,
    city:       cfgCity.value,
    country:    cfgCountry.value.toUpperCase(),
    auth_user:  cfgAuthUser.value
  };
  if (cfgAuthPass.value) body.auth_pass = cfgAuthPass.value;  // only send if user entered new password
  fetch('/config',{method:'POST',headers:CSRF_HEADER,body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{
      if (d.success) {
        alert('Configuration saved. If you changed the API key or city, weather will refresh shortly.');
        if (d.rebuild_url) {
          setTimeout(fetchWeather, 1500);
        }
        loadConfig();
      } else {
        alert('Failed to save: ' + (d.error || 'unknown error'));
      }
    }).catch(()=>{ alert('Network error saving config'); });
}

// P1-1: Reset WiFi triggers captive portal on next boot
function resetWifi(){
  if (!confirm('This will erase stored WiFi credentials and reboot the device into the captive portal. Continue?')) return;
  fetch('/reset-wifi',{method:'POST',headers:CSRF_HEADER})
    .then(r=>r.json()).then(d=>{
      if (d.success) {
        alert('Rebooting. Connect to the "SmartAgri-Setup" WiFi network to reconfigure.');
      }
    }).catch(()=>{});
}

saveCfgBtn.onclick = saveConfig;
resetWifiBtn.onclick = resetWifi;

function init(){
  loadSettings();
  loadConfig();
  updateSensorData();
  updateSystemStatus();
  fetchWeather();
  setInterval(()=>{ updateSensorData(); updateSystemStatus(); }, 5000);
  setInterval(()=>{ fetchWeather(); }, 600000);
}
init();
</script>
</body>
</html>
)rawliteral";

// ====== Helpers ======
float readMoisturePercent() {
  // Read multiple samples and average to reduce noise
  const int SAMPLES = 8;
  long sum = 0;
  int minv = 4095, maxv = 0;
  for (int i = 0; i < SAMPLES; ++i) {
    int v = analogRead(MOISTURE_PIN);
    sum += v;
    if (v < minv) minv = v;
    if (v > maxv) maxv = v;
    delayMicroseconds(200); // tiny pause between samples
  }
  // Simple spike rejection: remove one min and one max if enough samples
  long adjusted = sum;
  if (SAMPLES > 2) adjusted -= (minv + maxv);
  int denom = (SAMPLES > 2) ? (SAMPLES - 2) : SAMPLES;
  int raw = (int)(adjusted / denom);

  // Safety: clamp raw within ADC range
  if (raw < 0) raw = 0;
  if (raw > 4095) raw = 4095;

  // Map raw to percentage using your calibration constants
  // Ensure denominator non-zero
  float denom_map = (float)(MOISTURE_DRY - MOISTURE_WET);
  float percent_unflipped = 0.0f;
  if (denom_map == 0.0f) {
    percent_unflipped = 0.0f;
  } else {
    percent_unflipped = (float)(raw - MOISTURE_WET) * 100.0f / denom_map;
  }
  percent_unflipped = constrain(percent_unflipped, 0.0f, 100.0f);
  float mapped = 100.0f - percent_unflipped; // higher = wetter

  // Debug print to Serial to help diagnose hardware issues
  Serial.printf("[MOIST] raw=%d min=%d max=%d avg=%d mapped=%.1f%%\n", raw, minv, maxv, (int)(adjusted/denom), mapped);

  return mapped;
}

void updateDhtCache() {
  if (millis() - lastDhtRead < DHT_INTERVAL_MS) return;
  lastDhtRead = millis();
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) cachedTemp = t;
  if (!isnan(h)) cachedHum  = h;
}

// ====== P1-7: Config load/save =============================================
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
  preferences.end();
}

// P1-2: Build HTTPS weather URL dynamically from current config
void buildWeatherURL() {
  weatherURL = "https://api.openweathermap.org/data/2.5/weather?q="
             + city + "," + countryCode
             + "&appid=" + apiKey + "&units=metric";
}

// ====== P1-3: Auth + P1-6: CSRF helpers =====================================
bool requireAuth(AsyncWebServerRequest *request) {
  if (authUser.length() == 0 || authPass.length() == 0) {
    // No credentials configured - allow all (user must configure via web UI first)
    return true;
  }
  if (!request->authenticate(authUser.c_str(), authPass.c_str())) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

bool requireCsrf(AsyncWebServerRequest *request) {
  // Browsers will not send custom headers cross-origin without CORS preflight.
  // Rejecting POSTs without X-Requested-With blocks CSRF from basic HTML forms.
  if (!request->hasHeader("X-Requested-With")) {
    request->send(403, "application/json", "{\"success\":false,\"error\":\"missing CSRF header\"}");
    return false;
  }
  return true;
}

// ====== Weather fetch (P1-2: HTTPS) =========================================
void getWeatherData() {
  if (WiFi.status() != WL_CONNECTED || apiKey.length() < 10) return;

  WiFiClientSecure client;
  client.setInsecure();  // Skip cert validation (acceptable tradeoff for ESP32;
                         // for production, bundle the root CA with setCACert())
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
    } else {
      Serial.println(F("[Weather] JSON parse error"));
    }
  } else {
    Serial.printf("[Weather] HTTPS error: %d\n", code);
  }
  http.end();
  lastWeatherUpdate = millis();
}

// ====== P1-4: WiFi reconnect logic ==========================================
void checkWifiReconnect() {
  if (millis() - lastWifiCheck < WIFI_CHECK_INTERVAL_MS) return;
  lastWifiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected. Attempting reconnect...");
    WiFi.reconnect();
    // Give it a few seconds to settle
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WiFi] Reconnected. IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("[WiFi] Reconnect attempt failed. Will retry.");
    }
  }
}

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(100);

  dht.begin();
  pinMode(MOISTURE_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // relay OFF (active LOW)

  // Configure ADC for the moisture pin
  analogReadResolution(12);                        // 12-bit resolution (0..4095)
  analogSetPinAttenuation(MOISTURE_PIN, ADC_11db); // allow full-scale up to ~3.3V

  // P1-7: Load all config from NVS
  loadConfig();

  // P1-1: WiFiManager captive portal
  // No hardcoded SSID/password anywhere. On first boot (or after WiFi reset),
  // device boots into AP mode "SmartAgri-Setup". User connects, enters WiFi
  // credentials, device reboots and connects.
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);  // 3 min timeout; reboot if no config

  Serial.println(F("[WiFi] Starting WiFiManager..."));
  bool res = wm.autoConnect("SmartAgri-Setup");
  if (!res) {
    Serial.println(F("[WiFi] Config portal timed out. Rebooting..."));
    delay(1000);
    ESP.restart();
  }

  Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
  WiFi.setHostname("smartagri");
  if (MDNS.begin("project")) {
    Serial.println(F("[mDNS] http://project.local/"));
  } else {
    Serial.println(F("[mDNS] failed."));
  }

  // P1-2: Build HTTPS weather URL after config is loaded
  buildWeatherURL();

  // Initial weather fetch (if API key is set)
  getWeatherData();

  // Prime the DHT cache
  updateDhtCache();

  // ====== Routes ======
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    request->send_P(200, "text/html", index_html);
  });

  server.on("/sensor-data", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    float m = readMoisturePercent();
    DynamicJsonDocument doc(256);
    if (!isnan(cachedTemp)) doc["temperature"] = cachedTemp; else doc["temperature"] = nullptr;
    if (!isnan(cachedHum))  doc["humidity"]    = cachedHum;  else doc["humidity"]    = nullptr;
    doc["moisture"] = m;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/system-status", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    DynamicJsonDocument doc(128);
    doc["status"] = (autoMode ? "Auto" : "Manual");
    doc["relay"]  = watering;
    doc["safety_trip"] = safetyTrip;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // P1-6: Control endpoint now requires Auth + CSRF
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
      saveConfigToNvs();  // persist autoMode
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
    DynamicJsonDocument doc(128);
    doc["moisture_threshold"] = moistureThreshold;
    doc["humidity_threshold"] = humidityThreshold;
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
    // P0 validation: clamp to valid range
    float m = doc["moisture_threshold"].as<float>();
    float h = doc["humidity_threshold"].as<float>();
    moistureThreshold = constrain(m, 0.0f, 100.0f);
    humidityThreshold = constrain(h, 0.0f, 100.0f);
    saveConfigToNvs();
    request->send(200, "application/json", "{\"success\":true}");
  });

  // P1-7: New /config endpoint for device configuration
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    DynamicJsonDocument doc(256);
    doc["api_key"]    = apiKey;
    doc["city"]       = city;
    doc["country"]    = countryCode;
    doc["auth_user"]  = authUser;
    // Never echo password - just indicate if it's the default
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
      if (newUser.length() > 0 && newUser != authUser) {
        authUser = newUser;
        authChanged = true;
      }
    }
    if (doc.containsKey("auth_pass")) {
      String newPass = doc["auth_pass"].as<String>();
      if (newPass.length() > 0) {
        authPass = newPass;
        authChanged = true;
      }
    }

    if (urlNeedsRebuild) {
      buildWeatherURL();
      // Force immediate refresh with new config
      lastWeatherUpdate = 0;
    }

    saveConfigToNvs();

    DynamicJsonDocument res(128);
    res["success"] = true;
    res["rebuild_url"] = urlNeedsRebuild;
    if (authChanged) res["auth_changed"] = true;
    String out; serializeJson(res, out);
    request->send(200, "application/json", out);
  });

  // P1-1: Reset WiFi configuration (triggers captive portal on next boot)
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

  // Weather endpoint
  server.on("/weather", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireAuth(request)) return;
    if (millis() - lastWeatherUpdate > WEATHER_INTERVAL_MS) getWeatherData();
    DynamicJsonDocument doc(256);
    doc["name"] = locationName;
    if (!isnan(weatherTemp)) doc["temp"] = weatherTemp; else doc["temp"] = nullptr;
    if (!isnan(weatherHum))  doc["hum"]  = weatherHum;  else doc["hum"]  = nullptr;
    doc["desc"] = weatherDescription;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.begin();
  Serial.println(F("[HTTP] server started"));
}

// ====== Loop ======
void loop() {
  // Sample DHT on a 2.5s cadence (P0-2)
  updateDhtCache();

  // P1-4: WiFi reconnect check
  checkWifiReconnect();

  // Auto watering logic with 5% hysteresis
  if (autoMode) {
    float moisture = readMoisturePercent();
    if (moisture < moistureThreshold && !watering) {
      watering = true; safetyTrip = false;
      wateringStartTime = millis();
      digitalWrite(RELAY_PIN, LOW);
      Serial.printf("[Auto] Water ON (Moisture: %.1f%% < %.1f%%)\n", moisture, moistureThreshold);
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
    saveConfigToNvs();  // persist autoMode = false
  }

  // Periodic weather refresh
  if (WiFi.status() == WL_CONNECTED && (millis() - lastWeatherUpdate > WEATHER_INTERVAL_MS)) {
    getWeatherData();
  }

  delay(100);
}
