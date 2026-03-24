/* ============================
   PART 1 of 3 (CLEANED, NO FLASK)
   ESP8266 EEG Receiver - MAIN + SMS + UART + WiFi + Local Web Server
   ============================ */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecure.h>
#include <SoftwareSerial.h>

/* ---------- USER CONFIG - EDIT THESE ---------- */
/* WiFi */
const char* WIFI_SSID = "AITTEST";
const char* WIFI_PASSWORD = "12345678";

/* Twilio SMS credentials */
const char* TWILIO_SID   = "YOUR_TWILIO_SID_HERE";      
const char* TWILIO_AUTH  = "YOUR_TWILIO_AUTH_TOKEN_HERE";
const char* TWILIO_FROM  = "+1YOUR_TWILIO_NUMBER";
const char* TARGET_NUMBER= "+91TARGET_PHONE_NUMBER";

/* ---------- UART Pins & Alert Pins ---------- */
SoftwareSerial STM32_UART(4, 5);   // RX=D2(GPIO4), TX=D1(GPIO5)

#define ALERT_LED_PIN 2       // D4 onboard LED
#define ALERT_BUZZER_PIN 14   // GPIO14 buzzer

/* ---------- Web server ---------- */
ESP8266WebServer server(80);

/* ---------- Local circular buffer ---------- */
const uint8_t MAX_MSGS = 60;
String msgBuffer[MAX_MSGS];
unsigned long msgTimestamps[MAX_MSGS];
uint8_t msgHead = 0;
uint8_t msgCount = 0;

/* ---------- Thresholds (ESP-only defaults) ---------- */
struct Thresholds {
  float rms = 50.0f;
  float delta = 1000.0f;
  float entropy = 3.5f;
  float variance = 200.0f;
  float activity = 0.25f;
} thresholds;

/* ---------- Forward declarations ---------- */
void handleRoot();
void handleLocalData();
void handleSetThresholds();
void handleGetThresholds();
void handleTriggerAlert();
void handleClearLogs();

void WiFi_Init();
bool Check_WiFi();
bool Handshake_With_STM32();
void Process_Incoming_Data(const String &msg);
void Local_Alert(const String &msg);
void LED_Constant_On();
float parseFloatFromJson(const String &json, const String &field);
bool stateIsSeizure(const String &json);

/* ---------- Twilio SMS forward decl ---------- */
void Send_SMS_Alert(const String &alertMsg);

/* ---------- Base64 encode used for Twilio auth ---------- */
String base64Encode(const String &input) {
  static const char PROGMEM table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  String output;
  int len = input.length();
  const uint8_t *bytes = (const uint8_t*)input.c_str();

  for (int i = 0; i < len; i += 3) {
    uint32_t triple = 0;
    int remain = len - i;

    triple |= bytes[i] << 16;
    if (remain > 1) triple |= bytes[i + 1] << 8;
    if (remain > 2) triple |= bytes[i + 2];

    output += table[(triple >> 18) & 0x3F];
    output += table[(triple >> 12) & 0x3F];
    output += (remain > 1) ? table[(triple >> 6) & 0x3F] : '=';
    output += (remain > 2) ? table[triple & 0x3F] : '=';
  }

  return output;
}

/* ===================== SETUP ===================== */
void setup() {
  pinMode(ALERT_LED_PIN, OUTPUT);
  pinMode(ALERT_BUZZER_PIN, OUTPUT);
  digitalWrite(ALERT_LED_PIN, LOW);
  digitalWrite(ALERT_BUZZER_PIN, LOW);

  Serial.begin(115200);
  delay(300);

  STM32_UART.begin(38400);
  delay(300);

  Serial.println();
  Serial.println("--- ESP8266 EEG Receiver (Clean No-Flask Version) ---");

  /* ---- Handshake ---- */
  Serial.println("Waiting for STM32 handshake...");
  bool hs = Handshake_With_STM32();
  if (hs) {
    Serial.println("Handshake OK");
    digitalWrite(ALERT_LED_PIN, HIGH);
  } else {
    Serial.println("Handshake failed");
    Local_Alert("Handshake Failed");
  }

  /* ---- WiFi ---- */
  Serial.println("Connecting to WiFi...");
  WiFi_Init();
  if (Check_WiFi()) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }

  /* ---- Register Web Routes ---- */
  server.on("/", HTTP_GET, handleRoot);
  server.on("/localdata", HTTP_GET, handleLocalData);
  server.on("/set_thresholds", HTTP_POST, handleSetThresholds);
  server.on("/get_thresholds", HTTP_GET, handleGetThresholds);
  server.on("/trigger_alert", HTTP_POST, handleTriggerAlert);
  server.on("/clear_logs", HTTP_POST, handleClearLogs);

  server.begin();
  Serial.println("Web server started on port 80.");
}

/* ===================== MAIN LOOP ===================== */
void loop() {
  server.handleClient();

  // UART data from STM32
  if (STM32_UART.available()) {
    static String msg = "";

    while (STM32_UART.available()) {
      char c = STM32_UART.read();

      if (c == '<') msg = "";
      else if (c == '>') {
        msg.trim();
        if (msg.length() > 0) Process_Incoming_Data(msg);
        msg = "";
      }
      else msg += c;
    }
  }
}

/* ===================== HANDSHAKE ===================== */
bool Handshake_With_STM32() {
  for (int attempt = 1; attempt <= 5; attempt++) {
    Serial.printf("Handshake attempt %d\n", attempt);
    STM32_UART.println("HELLO_ESP");

    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
      if (STM32_UART.available()) {
        String r = STM32_UART.readStringUntil('\n');
        r.trim();
        if (r == "Response") return true;
      }
    }
  }
  return false;
}

/* ===================== WIFI ===================== */
void WiFi_Init() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    Serial.print(".");
    delay(250);
  }
  Serial.println();
}

bool Check_WiFi() {
  return WiFi.status() == WL_CONNECTED;
}

/* ===================== PROCESS JSON FROM STM32 ===================== */
void Process_Incoming_Data(const String &msg) {

  Serial.println("STM32 JSON:");
  Serial.println(msg);

  // store in circular buffer
  msgBuffer[msgHead] = msg;
  msgTimestamps[msgHead] = millis();
  msgHead = (msgHead + 1) % MAX_MSGS;
  if (msgCount < MAX_MSGS) msgCount++;

  // direct seizure string check
  if (stateIsSeizure(msg)) {
    Local_Alert("SEIZURE DETECTED!");
    Send_SMS_Alert("⚠️ SEIZURE DETECTED - Immediate attention required.");
  }
}

/* ===================== ALERT ===================== */
void Local_Alert(const String &msg) {
  Serial.println("ALERT: " + msg);

  for (int i = 0; i < 3; i++) {
    digitalWrite(ALERT_LED_PIN, LOW);
    digitalWrite(ALERT_BUZZER_PIN, HIGH);
    delay(250);
    digitalWrite(ALERT_LED_PIN, HIGH);
    digitalWrite(ALERT_BUZZER_PIN, LOW);
    delay(250);
  }
}

void LED_Constant_On() {
  digitalWrite(ALERT_LED_PIN, HIGH);
}

/* ===================== SEIZURE STRING DETECTOR ===================== */
bool stateIsSeizure(const String &json) {
  String up = json;
  up.toUpperCase();
  return (up.indexOf("SEIZURE") != -1);
}

/* ===================== TWILIO SMS (HTTPS) ===================== */
void Send_SMS_Alert(const String &alertMsg) {

  if (!Check_WiFi()) {
    Serial.println("SMS not sent: WiFi down");
    return;
  }

  String auth = String(TWILIO_SID) + ":" + String(TWILIO_AUTH);
  String auth_b64 = base64Encode(auth);

  WiFiClientSecure client;
  client.setInsecure();

  const char* host = "api.twilio.com";
  const uint16_t httpsPort = 443;

  if (!client.connect(host, httpsPort)) {
    Serial.println("Twilio connect fail");
    return;
  }

  String url = String("/2010-04-01/Accounts/") + TWILIO_SID + "/Messages.json";
  String body = "To=" + String(TARGET_NUMBER) +
                "&From=" + String(TWILIO_FROM) +
                "&Body=" + urlencode(alertMsg);

  client.print(String("POST ") + url + " HTTP/1.1\r\n");
  client.print(String("Host: ") + host + "\r\n");
  client.print(String("Authorization: Basic ") + auth_b64 + "\r\n");
  client.print("Content-Type: application/x-www-form-urlencoded\r\n");
  client.print(String("Content-Length: ") + body.length() + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  client.stop();
  Serial.println("SMS sent request → Twilio");
}

String millisToTimeStr(unsigned long ms) {
  unsigned long sec = ms / 1000;
  unsigned long min = sec / 60;
  unsigned long hr  = min / 60;
  sec %= 60;
  min %= 60;

  char buf[20];
  sprintf(buf, "%02lu:%02lu:%02lu", hr, min, sec);
  return String(buf);
}

/* ===================== URL ENCODE ===================== */
String urlencode(const String &str) {
  String encoded = "";
  for (uint16_t i = 0; i < str.length(); i++) {
    char c = str[i];
    if (isalnum(c)) encoded += c;
    else if (c == ' ') encoded += '+';
    else {
      char buf[4];
      sprintf(buf, "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

/* ===================== END OF CLEANED PART 1 =====================
   Part 2 contains the dashboard HTML/CSS/JS (handleRoot body).
   Part 3 contains the remaining endpoints (/localdata, /set_thresholds, etc.)
   ==================================================== */
/* ============================
  REPLACE handleRoot() (Part 2)
  Adds:
    - Spectrogram (4-band heatmap: delta/theta/alpha/beta)
    - Synthesized EEG waveform view (from band powers + dominant_freq)
    - Light / Dark mode toggle (saved in localStorage)
    - Retains seizure & dominant frequency charts + log + threshold UI
  Paste this function in place of your previous handleRoot().
  ============================ */
void handleRoot() {
  String page = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>ESP EEG — Local Dashboard (Spectrogram + Wave)</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600&display=swap" rel="stylesheet">
<style>
:root{
  --bg:#f5f7fb; --card:#fff; --muted:#6b7280; --accent:#0f62fe; --danger:#e11d48;
  --mono-bg:#071218; --mono-text:#9df2b6;
}
/* Dark theme overrides */
:root[data-theme='dark']{
  --bg:#0b1220; --card:#071223; --muted:#99a1b3; --accent:#6fb8ff; --danger:#ff6b7a;
  --mono-bg:#071218; --mono-text:#9df2b6;
}

html,body{height:100%;margin:0;background:var(--bg);font-family:Inter,system-ui,Arial;color: #111}
.container{max-width:1150px;margin:14px auto;padding:14px}
.header{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px;gap:12px}
.header h1{font-size:20px;margin:0}
.header .controls{display:flex;gap:8px;align-items:center}
.small{font-size:13px;color:var(--muted)}
.row{display:grid;grid-template-columns:1fr 380px;gap:12px}
.card{background:var(--card);border-radius:12px;padding:14px;box-shadow:0 6px 18px rgba(15,23,42,0.06)}
.kv{display:flex;align-items:center;justify-content:space-between;margin-bottom:6px}
.label{min-width:120px;font-size:13px;color:#333}
.slider{flex:1}
.value{width:64px;text-align:right;font-weight:600}
.btn{background:var(--accent);color:white;padding:8px 12px;border-radius:8px;border:0;cursor:pointer}
.btn-danger{background:var(--danger)}
.log{background:var(--mono-bg);color:var(--mono-text);padding:10px;border-radius:8px;height:240px;overflow:auto;font-family:monospace;font-size:12px}
@media (max-width:900px){ .row{grid-template-columns:1fr;} .header{flex-direction:column;align-items:flex-start} }

/* Small layout for spectrogram and waveform */
.spectro-wrap{display:flex;gap:8px;align-items:flex-start}
.spectro{width:100%;height:120px;border-radius:8px;overflow:hidden;background:#000}
.waveform{width:100%;height:140px;border-radius:8px;background:#071218}
.legend{display:flex;gap:8px;align-items:center;margin-top:6px}
.legend .item{display:flex;gap:6px;align-items:center;font-size:12px}
.swatch{width:18px;height:8px;border-radius:3px}
.theme-toggle{background:transparent;border:1px solid var(--muted);padding:6px 8px;border-radius:8px;color:var(--muted);cursor:pointer}
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <div>
      <h1>ESP EEG — Local Dashboard</h1>
      <div class="small">ESP IP: <span id="espip">loading...</span></div>
    </div>

    <div class="controls">
      <button id="themeBtn" class="theme-toggle">Toggle Theme</button>
      <div class="small">Mode</div>
    </div>
  </div>

  <div class="row">
    <div class="card">
      <div style="display:flex;gap:12px;align-items:center;flex-direction:column">
        <div style="width:100%;display:flex;gap:12px;">
          <div style="flex:1">
            <canvas id="seizureChart" height="140"></canvas>
          </div>
          <div style="width:220px">
            <canvas id="domChart" height="140"></canvas>
          </div>
        </div>

        <div style="width:100%;margin-top:12px">
          <div style="display:flex;gap:12px;">
            <div style="flex:1">
              <div class="spectro-wrap">
                <div style="flex:1">
                  <div style="font-size:13px;margin-bottom:6px">Spectrogram (delta / theta / alpha / beta)</div>
                  <canvas id="spectrogram" class="spectro"></canvas>
                  <div class="legend">
                    <div class="item"><div class="swatch" style="background:rgb(5,150,105)"></div>Delta</div>
                    <div class="item"><div class="swatch" style="background:rgb(7,125,255)"></div>Theta</div>
                    <div class="item"><div class="swatch" style="background:rgb(250,200,0)"></div>Alpha</div>
                    <div class="item"><div class="swatch" style="background:rgb(255,80,80)"></div>Beta</div>
                  </div>
                </div>
                <div style="width:320px">
                  <div style="font-size:13px;margin-bottom:6px">EEG Waveform (synthesized)</div>
                  <canvas id="waveform" class="waveform"></canvas>
                  <div class="small" style="margin-top:6px">Wave generated from band powers + dominant frequency — approximates EEG appearance when raw samples aren't available.</div>
                </div>
              </div>
            </div>
          </div>
        </div>

        <div style="width:100%;margin-top:10px;display:flex;gap:8px;">
          <button class="btn" id="btnClear">Clear Logs</button>
          <button class="btn btn-danger" id="btnTrigger">Trigger SEIZURE</button>
          <button class="btn" id="btnReset">Reset LED</button>
          <button class="btn" id="btnDownload">Download CSV</button>
        </div>


        <div style="width:100%;margin-top:12px" class="small">Recent messages (newest at top)</div>
        <div class="log" id="log">Loading...</div>
      </div>
    </div>

    <div class="card">
      <h3 style="margin-top:0">Threshold Controls (ESP-only)</h3>
      <div class="small" style="margin-bottom:8px">Adjust thresholds and press <strong>Save Thresholds</strong></div>

      <div class="kv">
        <div class="label">RMS threshold</div>
        <div style="display:flex;gap:8px;align-items:center">
          <input type="range" id="rms" min="1" max="200" step="1" class="slider">
          <div class="value" id="rmsVal"></div>
        </div>
      </div>

      <div class="kv">
        <div class="label">Delta power</div>
        <div style="display:flex;gap:8px;align-items:center">
          <input type="range" id="delta" min="0" max="50000" step="10" class="slider">
          <div class="value" id="deltaVal"></div>
        </div>
      </div>

      <div class="kv">
        <div class="label">Entropy (max)</div>
        <div style="display:flex;gap:8px;align-items:center">
          <input type="range" id="entropy" min="0" max="8" step="0.1" class="slider">
          <div class="value" id="entropyVal"></div>
        </div>
      </div>

      <div class="kv">
        <div class="label">Variance</div>
        <div style="display:flex;gap:8px;align-items:center">
          <input type="range" id="variance" min="0" max="2000" step="1" class="slider">
          <div class="value" id="varianceVal"></div>
        </div>
      </div>

      <div class="kv">
        <div class="label">Activity (min)</div>
        <div style="display:flex;gap:8px;align-items:center">
          <input type="range" id="activity" min="0" max="5" step="0.01" class="slider">
          <div class="value" id="activityVal"></div>
        </div>
      </div>

      <div style="display:flex;gap:8px;margin-top:8px">
        <button class="btn" id="btnSave">Save Thresholds</button>
        <button class="btn" id="btnLoad">Reload</button>
      </div>
    </div>
  </div>
</div>

<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<script>
/* -------------------------
  Helper: fetch /localdata and update everything
  Uses polling (1s) to keep backward compatibility with existing ESP endpoints.
  When you want, we can upgrade to WebSockets — tell me and I'll switch.
--------------------------*/
async function fetchData() {
  try {
    const res = await fetch('/localdata');
    if (!res.ok) return;
    const json = await res.json();
    updateFromLocalData(json);
  } catch (e) { console.error(e); }
}

/* -------------------------
  UI Elements & Charts
--------------------------*/
const seizureCtx = document.getElementById('seizureChart').getContext('2d');
const domCtx = document.getElementById('domChart').getContext('2d');
const seizureChart = new Chart(seizureCtx, {
  type: 'line',
  data: { labels: [], datasets: [{ label: 'Seizure', data: [], fill:false, tension:0.2, borderWidth:2 }]},
  options: { responsive:true, maintainAspectRatio:false, scales:{ y:{ min:0, max:1 }}}
});
const domChart = new Chart(domCtx, {
  type:'line', data:{ labels:[], datasets:[{label:'Dominant Hz', data:[], fill:false, tension:0.2, borderWidth:2}]},
  options:{ responsive:true, maintainAspectRatio:false, scales:{ y:{ beginAtZero:true } } }
});

/* Spectrogram: we'll maintain an offscreen buffer as image data.
   Spectrogram canvas width = time columns; each update we shift left by 1 and draw a new column representing 4 bands stacked vertically.
*/
const specCanvas = document.getElementById('spectrogram');
const specCtx = specCanvas.getContext('2d');
const SPEC_WIDTH = 300;
const SPEC_HEIGHT = 120;
specCanvas.width = SPEC_WIDTH;
specCanvas.height = SPEC_HEIGHT;
// row height per band (4 bands)
const BAND_ROWS = 4;
const ROW_H = Math.floor(SPEC_HEIGHT / BAND_ROWS);
let specImage = specCtx.createImageData(SPEC_WIDTH, SPEC_HEIGHT);
for (let i=0;i<specImage.data.length;i++) specImage.data[i]=0;
specCtx.putImageData(specImage,0,0);

/* Waveform canvas */
const waveCanvas = document.getElementById('waveform');
const waveCtx = waveCanvas.getContext('2d');
const WAVE_W = waveCanvas.width = 320;
const WAVE_H = waveCanvas.height = 140;
waveCtx.fillStyle = '#071218';
waveCtx.fillRect(0,0,WAVE_W,WAVE_H);

/* Latest feature holder */
let latestFeatures = null; // parsed JSON object from latest message
let timestamps = []; // for charts and labels

/* Update UI given /localdata JSON */
function updateFromLocalData(data) {
  // data.messages is an array of JSON strings (old->new chronological)
  const msgs = data.messages || [];
  const times = data.timestamps || [];

  // update logs (newest first)
  const reversed = msgs.slice().reverse();
  document.getElementById('log').innerText = reversed.join('\\n\\n');

  // labels for charts (use times)
  seizureChart.data.labels = times;
  domChart.data.labels = times;

  // fill seizure and dominant arrays
  seizureChart.data.datasets[0].data = data.seizure || [];
  domChart.data.datasets[0].data = data.dominant || [];

  seizureChart.update();
  domChart.update();

  // parse latest JSON message for bandpowers etc.
  if (msgs.length > 0) {
    try {
      latestFeatures = JSON.parse(msgs[msgs.length-1]);
      // update spectrogram and waveform
      pushSpectrogramColumn(latestFeatures);
      updateWaveform(latestFeatures);
    } catch (e) {
      console.error('JSON parse error:', e);
    }
  }
}

/* -------------------------
  Spectrogram helpers
  - band order: delta, theta, alpha, beta
  - we map each band's power to intensity (0..255) after log-normalization
  - color mapping chosen for visibility
--------------------------*/
function mapPowerToIntensity(p, maxP=1e4) {
  // log scale mapping; avoid negative/inf
  const val = Math.log10(1 + Math.abs(p));
  const m = Math.log10(1 + maxP);
  return Math.max(0, Math.min(255, Math.floor((val / m) * 255)));
}

function bandColor(idx, intensity) {
  // idx 0: delta (green), 1: theta (blue), 2: alpha (yellow), 3: beta (red)
  if (idx === 0) return [5,150,105, intensity];
  if (idx === 1) return [7,125,255, intensity];
  if (idx === 2) return [250,200,0, intensity];
  return [255,80,80, intensity];
}

function pushSpectrogramColumn(feat) {
  // get band values
  const delta = parseFloat(feat.delta_power || feat.delta || 0);
  const theta = parseFloat(feat.theta_power || 0);
  const alpha = parseFloat(feat.alpha_power || 0);
  const beta  = parseFloat(feat.beta_power || 0);

  // choose a normalization max dynamically from typical values
  const maxP = Math.max(1e2, delta, theta, alpha, beta);

  // shift image 1 pixel left
  const img = specCtx.getImageData(1,0,SPEC_WIDTH-1,SPEC_HEIGHT);
  specCtx.clearRect(0,0,SPEC_WIDTH,SPEC_HEIGHT);
  specCtx.putImageData(img,0,0);

  // draw new rightmost column
  const x = SPEC_WIDTH - 1;
  const bands = [delta, theta, alpha, beta];
  for (let b=0;b<BAND_ROWS;b++) {
    const intensity = mapPowerToIntensity(bands[b], maxP);
    const col = bandColor(b, intensity);
    // fill vertical area ROW_H pixels tall
    const y0 = b * ROW_H;
    specCtx.fillStyle = `rgba(${col[0]},${col[1]},${col[2]},${col[3]/255})`;
    specCtx.fillRect(x, y0, 1, ROW_H);
    // also draw a brighter pixel center for readability
    specCtx.fillRect(x, y0 + Math.max(0, Math.floor(ROW_H/3)), 1, Math.max(1, Math.floor(ROW_H/3)));
  }
}

/* -------------------------
  Waveform synthesis
  We'll synthesize a short segment (WAVE_W samples) by summing 4 band-limited sinusoids.
  Frequencies: delta=2Hz, theta=6Hz, alpha=10.5Hz, beta=20Hz (centers).
  Amplitude derived from sqrt(bandpower) normalized by a factor.
--------------------------*/
let wavePhase = 0;
function updateWaveform(feat) {
  const delta = parseFloat(feat.delta_power || feat.delta || 0);
  const theta = parseFloat(feat.theta_power || feat.theta_power || feat.theta || 0);
  const alpha = parseFloat(feat.alpha_power || feat.alpha_power || 0);
  const beta  = parseFloat(feat.beta_power || feat.beta_power || 0);
  const rms   = parseFloat(feat.rms || 0);
  const domF  = parseFloat(feat.dominant_freq || feat.dominant || 0);

  // compute amplitudes (sqrt of power, scaled)
  const aDelta = Math.sqrt(Math.max(0, delta)) * 0.01;
  const aTheta = Math.sqrt(Math.max(0, theta)) * 0.01;
  const aAlpha = Math.sqrt(Math.max(0, alpha)) * 0.01;
  const aBeta  = Math.sqrt(Math.max(0, beta)) * 0.01;

  const ampScale = 12 + (rms * 0.5); // scale factor to make waveform visible

  // clear
  waveCtx.fillStyle = getComputedStyle(document.documentElement).getPropertyValue('--mono-bg').trim() || '#071218';
  waveCtx.fillRect(0,0,WAVE_W,WAVE_H);

  // draw midline
  const mid = WAVE_H/2;
  waveCtx.strokeStyle = 'rgba(150,150,150,0.08)';
  waveCtx.beginPath();
  waveCtx.moveTo(0, mid);
  waveCtx.lineTo(WAVE_W, mid);
  waveCtx.stroke();

  // synthesize samples
  const fs = 256; // synthetic sampling for display
  const dt = 1 / fs;
  waveCtx.lineWidth = 1.5;
  waveCtx.strokeStyle = '#7be495';
  waveCtx.beginPath();
  for (let x=0; x < WAVE_W; x++) {
    const t = (x / WAVE_W) * 1.0; // 1 second window visual
    // base frequencies
    const fDelta = (domF > 0 && domF < 4) ? domF : 2.0;
    const fTheta = 6.0;
    const fAlpha = 10.5;
    const fBeta  = 20.0;

    // phase increments influenced by domF to make pattern vary with dominant frequency
    const val =
       aDelta * Math.sin(2*Math.PI*(fDelta)* (t + wavePhase*0.001))
      + aTheta * Math.sin(2*Math.PI*(fTheta)* (t + wavePhase*0.001))
      + aAlpha * Math.sin(2*Math.PI*(fAlpha)* (t + wavePhase*0.001))
      + aBeta  * Math.sin(2*Math.PI*(fBeta)*  (t + wavePhase*0.001));

    const y = mid - val * ampScale;
    if (x===0) waveCtx.moveTo(x, y);
    else waveCtx.lineTo(x, y);
  }
  waveCtx.stroke();

  // small envelope overlay to show RMS
  waveCtx.fillStyle = 'rgba(123,228,149,0.08)';
  const envelope = Math.min(WAVE_H/2 - 6, rms * 0.6);
  waveCtx.fillRect(0, mid - envelope, WAVE_W, envelope*2);

  // increment phase for animation
  wavePhase += 1;
}

/* -------------------------
  Theme toggle (light/dark)
--------------------------*/
const themeBtn = document.getElementById('themeBtn');
function setTheme(theme) {
  if (theme === 'dark') document.documentElement.setAttribute('data-theme','dark');
  else document.documentElement.removeAttribute('data-theme');
  localStorage.setItem('esp_theme', theme);
}
themeBtn.addEventListener('click', ()=>{
  const cur = localStorage.getItem('esp_theme') || 'light';
  const next = cur === 'dark' ? 'light' : 'dark';
  setTheme(next);
});
const savedTheme = localStorage.getItem('esp_theme') || 'light';
setTheme(savedTheme);

/* -------------------------
  Thresholds UI hooks (same as before)
--------------------------*/
function setSliderVal(id,val){ document.getElementById(id).value = val; document.getElementById(id+'Val').innerText = val; }
async function loadThresholds(){
  try { let r = await fetch('/get_thresholds'); if(!r.ok) return; let j = await r.json();
    setSliderVal('rms', Number(j.rms).toFixed(0));
    setSliderVal('delta', Number(j.delta).toFixed(0));
    setSliderVal('entropy', Number(j.entropy).toFixed(1));
    setSliderVal('variance', Number(j.variance).toFixed(0));
    setSliderVal('activity', Number(j.activity).toFixed(2));
  } catch(e){ console.error(e); }
}
async function saveThresholds(){
  const body = new URLSearchParams();
  body.append('rms', document.getElementById('rms').value);
  body.append('delta', document.getElementById('delta').value);
  body.append('entropy', document.getElementById('entropy').value);
  body.append('variance', document.getElementById('variance').value);
  body.append('activity', document.getElementById('activity').value);
  try { await fetch('/set_thresholds', {method:'POST', body: body}); alert('Saved'); loadThresholds(); } catch(e){ console.error(e); }
}

/* -------------------------
   DOWNLOAD CSV FILE
--------------------------*/
async function downloadCSV() {
  try {
    const res = await fetch('/localdata');
    if (!res.ok) {
      alert("Failed to fetch data.");
      return;
    }

    const data = await res.json();
    const msgs = data.messages || [];
    const times = data.timestamps || [];

    if (msgs.length === 0) {
      alert("No data available!");
      return;
    }

    // CSV Header
    let csv = "timestamp,rms,variance,activity,mobility,complexity,delta_power,theta_power,alpha_power,beta_power,entropy,kurtosis,skewness,zero_crossings,p2p,crest,spec_flatness,dominant_freq,spec_centroid,state\n";

    // Convert messages to CSV
    for (let i = 0; i < msgs.length; i++) {
      try {
        const obj = JSON.parse(msgs[i]);

        const row = [
          times[i] || "",
          obj.rms || "",
          obj.variance || "",
          obj.activity || "",
          obj.mobility || "",
          obj.complexity || "",
          obj.delta_power || "",
          obj.theta_power || "",
          obj.alpha_power || "",
          obj.beta_power || "",
          obj.entropy || "",
          obj.kurtosis || "",
          obj.skewness || "",
          obj.zero_crossings || "",
          obj.p2p || "",
          obj.crest || "",
          obj.spec_flatness || "",
          obj.dominant_freq || "",
          obj.spec_centroid || "",
          obj.state || "",
        ].join(",");

        csv += row + "\n";
      } catch (e) {
        console.error("Error parsing JSON row:", e);
      }
    }

    // Trigger download
    const blob = new Blob([csv], { type: "text/csv" });
    const url = URL.createObjectURL(blob);

    const a = document.createElement("a");
    a.href = url;
    a.download = "EEG_data.csv";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);

    URL.revokeObjectURL(url);

  } catch (e) {
    console.error(e);
    alert("CSV download failed.");
  }
}

/* Attach button handler */
document.getElementById("btnDownload").addEventListener("click", downloadCSV);

/* Button handlers */
document.getElementById('btnSave').addEventListener('click', saveThresholds);
document.getElementById('btnLoad').addEventListener('click', loadThresholds);
document.getElementById('btnClear').addEventListener('click', async ()=>{ await fetch('/clear_logs', {method:'POST'}); fetchData(); });
document.getElementById('btnTrigger').addEventListener('click', async ()=>{ await fetch('/trigger_alert', {method:'POST'}); fetchData(); });
document.getElementById('btnReset').addEventListener('click', async ()=>{ await fetch('/clear_logs', {method:'POST'}); fetchData(); });

['rms','delta','entropy','variance','activity'].forEach(id=>{
  const el=document.getElementById(id); el.addEventListener('input', ()=>{ document.getElementById(id+'Val').innerText = el.value; });
});

/* Initial load and periodic poll (1s) */
document.getElementById('espip').innerText = window.location.hostname;
loadThresholds();
fetchData();
setInterval(fetchData, 1000);
</script>
</body>
</html>
  )rawliteral";

  server.send(200, "text/html", page);
}

/* ===================== END OF PART 2 =====================
   Now request PART 3 to complete the sketch (localdata, thresholds endpoints, helpers).
   Paste PART 3 after this part in the same .ino file.
   ==================================================== */
/* ============================
   PART 3 of 3
   Final endpoint implementations and threshold logic
   Append after Part 2 in same .ino file
   ============================ */

/* ============================================================
   /localdata
   Returns JSON:
   {
     "messages": [ ... ],
     "timestamps": [ ... ],
     "seizure": [0/1...],
     "dominant": [ ... ]
   }
   Used by dashboard JavaScript
   ============================================================ */
void handleLocalData() {
  String out = "{";

  /* -------- messages -------- */
  out += "\"messages\":[";
  int start = (msgCount == MAX_MSGS) ? msgHead : 0;
  int count = msgCount;

  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_MSGS;
    String msg = msgBuffer[idx];

    // Escape quotes & backslashes
    String esc = "";
    for (unsigned int k = 0; k < msg.length(); k++) {
      char c = msg[k];
      if (c == '"' || c == '\\') { esc += '\\'; esc += c; }
      else esc += c;
    }

    out += "\"" + esc + "\"";
    if (i < count - 1) out += ",";
  }
  out += "],";

  /* -------- timestamps -------- */
  out += "\"timestamps\":[";
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_MSGS;
    out += "\"" + millisToTimeStr(msgTimestamps[idx]) + "\"";
    if (i < count - 1) out += ",";
  }
  out += "],";

  /* -------- seizure flag -------- */
  out += "\"seizure\":[";
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_MSGS;
    bool seizure = stateIsSeizure(msgBuffer[idx]);
    out += (seizure ? "1" : "0");
    if (i < count - 1) out += ",";
  }
  out += "],";

  /* -------- dominant frequency -------- */
  out += "\"dominant\":[";
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_MSGS;
    float df = parseFloatFromJson(msgBuffer[idx], "dominant_freq");
    out += String(df, 2);
    if (i < count - 1) out += ",";
  }
  out += "]";

  out += "}";

  server.send(200, "application/json", out);
}

/* ============================================================
   /set_thresholds
   Updates ESP-only seizure thresholds via form POST
   ============================================================ */
void handleSetThresholds() {
  if (server.hasArg("rms"))      thresholds.rms = server.arg("rms").toFloat();
  if (server.hasArg("delta"))    thresholds.delta = server.arg("delta").toFloat();
  if (server.hasArg("entropy"))  thresholds.entropy = server.arg("entropy").toFloat();
  if (server.hasArg("variance")) thresholds.variance = server.arg("variance").toFloat();
  if (server.hasArg("activity")) thresholds.activity = server.arg("activity").toFloat();

  Serial.printf("Updated thresholds:\n RMS=%.2f\n Delta=%.2f\n Entropy=%.2f\n Variance=%.2f\n Activity=%.2f\n",
                thresholds.rms, thresholds.delta, thresholds.entropy,
                thresholds.variance, thresholds.activity);

  server.send(200, "text/plain", "Thresholds updated");
}

/* ============================================================
   /get_thresholds
   Returns current thresholds as JSON
   ============================================================ */
void handleGetThresholds() {
  String out = "{";
  out += "\"rms\":"+String(thresholds.rms,3)+",";
  out += "\"delta\":"+String(thresholds.delta,3)+",";
  out += "\"entropy\":"+String(thresholds.entropy,3)+",";
  out += "\"variance\":"+String(thresholds.variance,3)+",";
  out += "\"activity\":"+String(thresholds.activity,3);
  out += "}";

  server.send(200, "application/json", out);
}

/* ============================================================
   /trigger_alert
   Manual seizure button -> runs buzzer + LED alert + SMS message
   ============================================================ */
void handleTriggerAlert() {
  Local_Alert("MANUAL SEIZURE TRIGGER");
  Send_SMS_Alert("⚠️ MANUAL ALERT: Seizure triggered from ESP dashboard!");
  server.send(200, "text/plain", "Triggered");
}

/* ============================================================
   /clear_logs
   Clears the circular buffer storing messages
   ============================================================ */
void handleClearLogs() {
  msgHead = 0;
  msgCount = 0;
  for (int i = 0; i < MAX_MSGS; i++) msgBuffer[i] = "";
  server.send(200, "text/plain", "Logs cleared");
}

/* ============================================================
   Threshold alerting logic (ESP-ONLY)
   This is called only when state != "SEIZURE"
   ============================================================ */
void applyThresholdChecksAndAlert(const String &json) {

  float rms      = parseFloatFromJson(json, "rms");
  float delta    = parseFloatFromJson(json, "delta_power");
  float ent      = parseFloatFromJson(json, "entropy");
  float var      = parseFloatFromJson(json, "variance");
  float activity = parseFloatFromJson(json, "activity");

  Serial.printf("[ESP threshold check] RMS:%.2f Delta:%.2f Entropy:%.2f Var:%.2f Act:%.3f\n",
                rms, delta, ent, var, activity);

  bool alarm = false;

  if (rms >= thresholds.rms) alarm = true;
  if (delta >= thresholds.delta) alarm = true;
  if (ent <= thresholds.entropy) alarm = true;     // Lower entropy = abnormal
  if (var >= thresholds.variance) alarm = true;
  if (activity <= thresholds.activity) alarm = true;

  if (alarm) {
    Serial.println("ESP threshold logic → SEIZURE ALERT");
    Local_Alert("Threshold-based alert");
    Send_SMS_Alert("⚠️ Threshold-based seizure detection triggered!");
  }
}

/* ============================================================
   JSON helper — extract floating-point values from simple JSON
   ============================================================ */
float parseFloatFromJson(const String &json, const String &field) {
  int idx = json.indexOf(field);
  if (idx == -1) return 0.0f;

  int colon = json.indexOf(':', idx);
  if (colon == -1) return 0.0f;

  int i = colon + 1;
  while (i < json.length() && isSpace(json.charAt(i))) i++;

  int start = i;

  // Accept digits, signs, decimals, exponent
  while (i < json.length()) {
    char c = json.charAt(i);
    if ((c >= '0' && c <= '9') || c=='+' || c=='-' || c=='.' || c=='e' || c=='E')
      i++;
    else break;
  }

  if (i == start) return 0.0f;

  return json.substring(start, i).toFloat();
}

/* ============================================================
   END OF PART 3 — FULL ESP8266 CODE COMPLETE
   ============================================================ */
