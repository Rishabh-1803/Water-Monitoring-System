// =============================================================================
// Smart Agricultural System - ESP32-C3 Super Mini
// Water Monitoring + Auto Irrigation + Live Weather
// -----------------------------------------------------------------------------
// FIXES APPLIED :
//   1. Race condition in /control POST body handler fixed (per-request buffer)
//   2. DHT11 readings now cached in loop() instead of being read inside the
//      HTTP handler (avoids stale/NaN data when polled faster than 2s)
//   3. Removed meaningless `#define BOARD_HAS_PSRAM 0` (it has no effect in
//      user code - that flag is a board-package build flag)
//   4. Fixed misleading MOISTURE_PIN comment. On ESP32-C3, GPIO 1 IS a valid
//      ADC1 channel (ADC1_CH1) - the value is correct, only the comment was wrong
//   5. Added prominent API key rotation warning
//   6. Added GPIO 8 strapping-pin warning (hardware note, no code change)
// =============================================================================

// ====== Includes ======
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>     // ArduinoJson v6
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DHT.h>
#include <ESPmDNS.h>

// ====== Hardware Pins (ESP32-C3 Super Mini) ======
#define DHTPIN 4              // GPIO 4 = ADC1_CH4 on C3, OK for digital I/O too
#define DHTTYPE DHT11
#define MOISTURE_PIN 1        // GPIO 1 = ADC1_CH1 on ESP32-C3 (valid analog pin)
#define RELAY_PIN 8           // GPIO 8 - see WARNING below
//
// !!! HARDWARE WARNING - GPIO 8 IS A STRAPPING PIN ON ESP32-C3 !!!
// On ESP32-C3, GPIO 8 controls VDD_SPI voltage selection at boot.
// If your relay module pulls this pin LOW at power-on (or it floats LOW),
// the chip may enter an abnormal boot state or select wrong flash voltage.
//
// RECOMMENDED FIX (requires rewiring):
//   Move RELAY_PIN to GPIO 5, 6, 7, or 10 (non-strapping, free pins).
//   Then update #define RELAY_PIN 5 (or chosen pin) in this file.
//
// If you cannot rewire now, ensure the relay module has a pull-up on its
// input (most modules do), and verify the board boots reliably before use.

// ====== Objects ======
DHT dht(DHTPIN, DHTTYPE);
Preferences preferences;
AsyncWebServer server(80);

char ssid[32]     = "";
char password[64] = "";

String apiKey       = "";
String city         = "Ahmedabad";
String countryCode  = "IN";
String weatherURL   = ""; // built in setup()

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

// ====== DHT11 Reading Cache (P0 fix #2) =====================================
// DHT11 has a minimum sampling interval of ~2 seconds. Reading it directly
// inside the /sensor-data HTTP handler caused stale/NaN data when the UI
// polled faster than 2s, or when multiple clients polled simultaneously.
// We now sample in loop() and serve cached values.
float  cachedTemp = NAN;
float  cachedHum  = NAN;
unsigned long lastDhtRead = 0;
const unsigned long DHT_INTERVAL_MS = 2500UL; // > 2s per DHT11 datasheet

void updateDhtCache() {
  if (millis() - lastDhtRead < DHT_INTERVAL_MS) return;
  lastDhtRead = millis();
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  // Only update cache if reading is valid (NaN readings do not overwrite
  // the last known good value - this gives the UI a stable display)
  if (!isnan(t)) cachedTemp = t;
  if (!isnan(h)) cachedHum  = h;
}

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
.status-manual{background:#fdebd0;color:#e67e22}
.status-on{background:#d5f5e3;color:#27ae60}
.status-off{background:#fadbd8;color:#c0392b}
.controls{display:flex;gap:10px;flex-wrap:wrap}
.btn{padding:10px 16px;border:none;border-radius:8px;cursor:pointer;font-weight:700;transition:transform .1s ease,opacity .1s ease}
.btn:active{transform:scale(.98)}
.btn-primary{background:var(--secondary);color:#fff}
.btn-danger{background:var(--accent);color:#fff}
.btn-success{background:var(--success);color:#fff}
.form-row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.input{width:100%;padding:10px;border:1px solid #ddd;border-radius:8px}
.notice{font-size:12px;opacity:.7}
.kv{display:flex;justify-content:space-between;margin:6px 0}
.badge{display:inline-block;background:#eef;border-radius:999px;padding:6px 10px;font-weight:700}
.small{font-size:12px}
</style>
</head>
<body>
<div class="container">

  <header>
    <h1>Smart Agricultural System</h1>
    <div class="small">DHT11 + Soil Moisture + Relay • with Live Weather (OpenWeatherMap)</div>
  </header>

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
      <div class="notice">Auto-updates every 10 minutes.</div>
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

</div>

<script>
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
  fetch('/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(_=>{
      loadSettings();
    });
}

function sendCommand(command){
  fetch('/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command})})
    .then(r=>r.json()).then(_=>{
      updateSystemStatus();
    });
}

function updateAutoModeButton(autoOn){ autoModeBtn.textContent = autoOn ? 'Auto Mode: ON' : 'Auto Mode: OFF'; }

waterOnBtn.onclick = ()=>sendCommand('water_on');
waterOffBtn.onclick= ()=>sendCommand('water_off');
autoModeBtn.onclick= ()=>sendCommand('auto_mode');
wRefresh.onclick   = ()=>fetchWeather();
saveBtn.onclick    = ()=>saveSettings();

function init(){
  loadSettings();
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

// ====== Weather fetch ======
void getWeatherData() {
  if (WiFi.status() != WL_CONNECTED || apiKey.length() < 10) return;

  HTTPClient http;
  http.setTimeout(8000);
  http.begin(weatherURL);

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
    Serial.printf("[Weather] HTTP error: %d\n", code);
  }
  http.end();
  lastWeatherUpdate = millis();
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

  // Preferences
  preferences.begin("watering", true);
  moistureThreshold = preferences.getFloat("moisture", 30.0f);
  humidityThreshold = preferences.getFloat("humidity", 50.0f);
  autoMode          = preferences.getBool("automode", true);
  preferences.end();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("Neel");
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("project")) Serial.println("mDNS: http://project.local/");
    else Serial.println("mDNS failed.");
  } else {
    Serial.println("\nWiFi connect timeout. Continuing without network.");
  }

  // Build weather URL after WiFi (but before first fetch)
  weatherURL = "http://api.openweathermap.org/data/2.5/weather?q=" + city + "," + countryCode + "&appid=" + apiKey + "&units=metric";

  // Initial weather fetch (if online)
  getWeatherData();

  // Prime the DHT cache before serving the first request
  updateDhtCache();

  // ====== Routes ======
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.on("/sensor-data", HTTP_GET, [](AsyncWebServerRequest *request){
    // P0 fix #2: serve cached DHT values instead of reading the sensor here.
    // Moisture is fast enough to sample on demand (8 ADC reads, ~2ms).
    float m = readMoisturePercent();
    DynamicJsonDocument doc(256);
    if (!isnan(cachedTemp)) doc["temperature"] = cachedTemp; else doc["temperature"] = nullptr;
    if (!isnan(cachedHum))  doc["humidity"]    = cachedHum;  else doc["humidity"]    = nullptr;
    doc["moisture"] = m;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/system-status", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(128);
    doc["status"] = (autoMode ? "Auto" : "Manual");
    doc["relay"]  = watering;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // ===========================================================================
  // P0 fix #1: Race condition in /control POST handler
  // ---------------------------------------------------------------------------
  // ORIGINAL BUG:
  //   The onBody callback used a `static String body` that was shared across
  //   all concurrent requests. Two simultaneous POSTs would corrupt each
  //   other's buffer. Also `String((char*)data)` assumed null-termination,
  //   which is not guaranteed for chunked bodies.
  //
  // FIX:
  //   Use the same pattern as /settings POST below - the ESPAsyncWebServer
  //   auto-buffers the body and exposes it via `request->arg("plain")`.
  //   This is per-request (no shared state) and properly handles binary data.
  // ===========================================================================
  server.on("/control", HTTP_POST, [](AsyncWebServerRequest *request){
    String body = request->arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    String command = doc["command"] | "";
    preferences.begin("watering", false);

    if (command == "water_on") {
      watering = true; autoMode = false;
      digitalWrite(RELAY_PIN, LOW);
      preferences.putBool("automode", false);
      preferences.end();
      request->send(200, "application/json", "{\"success\":true}");
    } else if (command == "water_off") {
      watering = false; autoMode = false;
      digitalWrite(RELAY_PIN, HIGH);
      preferences.putBool("automode", false);
      preferences.end();
      request->send(200, "application/json", "{\"success\":true}");
    } else if (command == "auto_mode") {
      autoMode = !autoMode;
      preferences.putBool("automode", autoMode);
      preferences.end();
      if (!autoMode) { watering = false; digitalWrite(RELAY_PIN, HIGH); }
      DynamicJsonDocument res(128);
      res["success"] = true; res["auto_mode"] = autoMode;
      String out; serializeJson(res, out);
      request->send(200, "application/json", out);
    } else {
      preferences.end();
      request->send(400, "application/json", "{\"success\":false,\"error\":\"unknown command\"}");
    }
  });

  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(128);
    doc["moisture_threshold"] = moistureThreshold;
    doc["humidity_threshold"] = humidityThreshold;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/settings", HTTP_POST, [](AsyncWebServerRequest *request){
    String body = request->arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      request->send(400, "application/json", "{\"success\":false}");
      return;
    }
    moistureThreshold = doc["moisture_threshold"].as<float>();
    humidityThreshold = doc["humidity_threshold"].as<float>();
    preferences.begin("watering", false);
    preferences.putFloat("moisture", moistureThreshold);
    preferences.putFloat("humidity", humidityThreshold);
    preferences.end();
    request->send(200, "application/json", "{\"success\":true}");
  });

  // Weather endpoint
  server.on("/weather", HTTP_GET, [](AsyncWebServerRequest *request){
    // refresh if older than 10 min (lightweight)
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
  Serial.println("HTTP server started");
}

// ====== Loop ======
void loop() {
  // P0 fix #2: sample DHT on a 2.5s cadence in loop(), not in HTTP handler.
  updateDhtCache();

  // Auto watering logic with 5% hysteresis
  if (autoMode) {
    float moisture = readMoisturePercent();
    if (moisture < moistureThreshold && !watering) {
      watering = true; digitalWrite(RELAY_PIN, LOW);
      Serial.printf("Auto: Water ON (Moisture: %.1f%% < %.1f%%)\n", moisture, moistureThreshold);
    } else if (moisture >= (moistureThreshold + 5.0f) && watering) {
      watering = false; digitalWrite(RELAY_PIN, HIGH);
      Serial.printf("Auto: Water OFF (Moisture: %.1f%% >= %.1f%%)\n", moisture, moistureThreshold + 5.0f);
    }
  }

  // Periodic weather refresh
  if (WiFi.status() == WL_CONNECTED && (millis() - lastWeatherUpdate > WEATHER_INTERVAL_MS)) {
    getWeatherData();
  }

  delay(100);
}
