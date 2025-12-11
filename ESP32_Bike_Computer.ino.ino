#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- FIREBASE CONFIG ---
#define FIREBASE_HOST "firebase-database-host-api"
#define FIREBASE_API_KEY "firebase-app-api"

// --- WIFI SETTINGS ---
// Station Mode (for internet connection - UPDATE THESE!)
const char* wifi_ssid = "Mobile-Hotspot-SSID";   
const char* wifi_password = "Mobile-Hotspot-Password";

// --- CYCLIST INFO (UPDATE THESE) ---
const float CYCLIST_WEIGHT_KG = 70.0; // Update with actual weight
const int WHEEL_CIRCUMFERENCE_MM = 2105; // update with actual circumference of wheel (2*(3.14*r))

// Web Server
WebServer server(80);

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// MPU6050 Sensor
Adafruit_MPU6050 mpu;

// Hall Effect Sensor
const int hallPin = 15;

// Speed variables (hall and IMU separated)
volatile unsigned long lastMagnetTime = 0;
volatile unsigned long rotationCount = 0;
volatile float hallSpeedKph = 0.0;   // updated in ISR

// IMU velocity integration
float imuVelocityMS = 0.0; // m/s
float imuSpeedKph = 0.0;

// Fused speed and animated display
float fusedSpeedKph = 0.0;
float displaySpeedKph = 0.0;
unsigned long lastAnimationTime = 0;
const unsigned long ANIMATION_STEP_MS = 200;
const float ANIMATION_STEP_VALUE = 0.5;

// Timing
const int SPEED_TIMEOUT_MS = 2000;
unsigned long lastIMUUpdate = 0;
unsigned long lastSampleTime = 0; // for 1Hz buffering

// IMU variables
float pitch = 0.0;
float roll = 0.0;
float zIncline = 0.0;
float accelY = 0.0;

// Trip tracking variables (kept mostly for compatibility)
unsigned long tripStartTime = 0;
unsigned long tripDuration = 0;
float tripDistance = 0.0; // km
float caloriesBurned = 0.0;
bool tripActive = false;
String magnetState = "Standby";

// Buffering: per-spec arrays of 30 samples
const int BUFFER_SIZE = 30;
float speedArray[BUFFER_SIZE];
float inclineArray[BUFFER_SIZE];
float caloriesArray[BUFFER_SIZE];
int bufferIndex = 0;
bool bufferPending = false; // true while a full buffer is awaiting send

// Pending batches queue (store up to 4 pending batches)
struct Batch {
  float speed[BUFFER_SIZE];
  float incline[BUFFER_SIZE];
  float calories[BUFFER_SIZE];
};
const int MAX_PENDING = 4;
Batch pendingBatches[MAX_PENDING];
int pendingHead = 0;
int pendingTail = 0;
int pendingCount = 0;

// Firebase update timing
unsigned long lastFirebaseUpdate = 0;
const unsigned long FIREBASE_UPDATE_INTERVAL = 10000; // keep for compatibility

// ISR for Hall sensor
void IRAM_ATTR onMagnetDetect() {
  unsigned long now = millis();
  unsigned long timeElapsed = now - lastMagnetTime;
if (timeElapsed > 20) { // debounce
    // calculate instantaneous hall speed (kph) based on delta ms
    hallSpeedKph = ((float)WHEEL_CIRCUMFERENCE_MM / (float)timeElapsed) * 3.6;
    lastMagnetTime = now;
    rotationCount++;
  }
}

// Calculate MET value based on speed and incline
float calculateMET(float speed, float incline) {
  float met = 0.0;
  if (speed < 1.0) {
    met = 1.0;
  } else if (speed < 16.0) {
    met = 4.0;
  } else if (speed < 19.0) {
    met = 6.8;
  } else if (speed < 22.0) {
    met = 8.0;
  } else {
    met = 10.0;
  }
  if (incline > 0) {
    met += (incline / 5.0) * 0.5;
  }
  return met;
}

// Internal helper: push current filled buffers into pending queue
void pushPendingBatch(float *s, float *i, float *c) {
  if (pendingCount >= MAX_PENDING) {
    // Queue full. We will not overwrite. Log error and stop accepting new buffers until space clears.
    Serial.println("Pending queue full. Pausing buffer acceptance until cleared.");
    return;
  }
  // copy into pendingTail
  Batch &b = pendingBatches[pendingTail];
  for (int k = 0; k < BUFFER_SIZE; ++k) {
    b.speed[k] = s[k];
    b.incline[k] = i[k];
    b.calories[k] = c[k];
  }
  pendingTail = (pendingTail + 1) % MAX_PENDING;
  pendingCount++;
  Serial.printf("Pushed batch to pending queue. pendingCount=%d\n", pendingCount);
}

// Internal helper: attempt to send one pending batch. returns true on success.
bool sendPendingBatch() {
  if (pendingCount == 0) return true; // nothing pending
  Batch &b = pendingBatches[pendingHead];

  FirebaseJson json;
  FirebaseJsonArray speedArr;
  FirebaseJsonArray inclineArr;
  FirebaseJsonArray calArr;

  for (int k = 0; k < BUFFER_SIZE; ++k) {
    speedArr.add(b.speed[k]);
    inclineArr.add(b.incline[k]);
    calArr.add(b.calories[k]);
  }
  json.set("speed", speedArr);
  json.set("incline", inclineArr);
  json.set("calories", calArr);

  String path = "/batches/" + String(millis());
  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    Serial.println("Batch push success. Clearing pending batch.");
    // remove from queue
    pendingHead = (pendingHead + 1) % MAX_PENDING;
    pendingCount--;
    return true;
  } else {
    Serial.println("Batch push failed: " + fbdo.errorReason());
    return false;
  }
}

// Update Firebase with a single trip summary (kept for compatibility)
void updateFirebaseSummary() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No internet connection for Firebase");
    return;
  }
  if (!Firebase.ready()) {
    Serial.println("Firebase not ready");
    return;
  }

  String path = "/trips/" + String(tripStartTime);
  FirebaseJson json;
  json.set("startTime", (int)tripStartTime);
  json.set("duration", (int)tripDuration);
  json.set("distance", tripDistance);
  json.set("avgSpeed", bufferIndex > 0 ? (totalSpeed() / bufferIndex) : 0.0);
  json.set("avgIncline", bufferIndex > 0 ? (totalIncline() / bufferIndex) : 0.0);
  json.set("calories", caloriesBurned);
  json.set("currentSpeed", fusedSpeedKph);
  json.set("currentIncline", zIncline);
  json.set("active", tripActive);

  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    Serial.println("Firebase summary update success!");
  } else {
    Serial.println("Firebase summary update failed: " + fbdo.errorReason());
  }
}

// Helper to compute totalSpeed and totalIncline for summary (simple sums)
float totalSpeed() {
  float s = 0;
  for (int i = 0; i < bufferIndex; ++i) s += speedArray[i];
  return s;
}
float totalIncline() {
  float s = 0;
  for (int i = 0; i < bufferIndex; ++i) s += inclineArray[i];
  return s;
}

// HTML Page (same UI but fetch '/data' for displaySpeed)
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>

<head>
  <title>ESP32 Bike Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
  <style>
    :root {
      --bg-color: #050505;
      --card-bg: rgba(255, 255, 255, 0.04);
      --card-border: rgba(255, 255, 255, 0.08);
      --highlight-orange: #ff6b35;
      --highlight-green: #00ff88;
      --highlight-blue: #00d9ff;
      --text-main: #ffffff;
      --text-muted: #888888;
      /* System font stack for best performance */
      --font-main: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
      --font-mono: 'Courier New', Courier, monospace;
      /* Simple mono stack */
    }

    html,
    body {
      height: 100%;
      margin: 0;
      padding: 0;
      background-color: var(--bg-color);
      color: var(--text-main);
      font-family: var(--font-main);
      overflow: hidden;
      display: flex;
      justify-content: center;
      align-items: center;
    }

    /* SVG Icons Utility */
    .icon {
      width: 1em;
      height: 1em;
      fill: currentColor;
      display: inline-block;
      vertical-align: middle;
    }

    .icon-lg {
      width: 1.5em;
      height: 1.5em;
    }

    .icon-xl {
      width: 2em;
      height: 2em;
    }

    /* Dashboard Layout */
    .dashboard-frame {
      width: 96vw;
      height: 92vh;
      background: #111;
      border-radius: 40px;
      padding: 10px;
      box-shadow: 0 0 0 2px #333, 0 0 50px rgba(0, 0, 0, 0.8);
      position: relative;
      display: flex;
    }

    .screen-surface {
      width: 100%;
      height: 100%;
      background: radial-gradient(circle at 50% 120%, #1a1a1a, #000);
      border-radius: 30px;
      padding: 20px;
      box-sizing: border-box;
      display: grid;
      grid-template-columns: 280px 1fr 280px;
      grid-template-rows: 40px 1fr;
      gap: 20px;
      position: relative;
      overflow: hidden;
    }

    /* Glare */
    .screen-surface::after {
      content: '';
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 40%;
      background: linear-gradient(180deg, rgba(255, 255, 255, 0.03) 0%, rgba(255, 255, 255, 0) 100%);
      pointer-events: none;
      border-radius: 30px 30px 0 0;
    }

    /* Header */
    .header-bar {
      grid-column: 1 / -1;
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 0 10px;
      z-index: 10;
    }

    .clock {
      font-size: 1.5rem;
      font-weight: 600;
      letter-spacing: 1px;
    }

    .status-area {
      display: flex;
      gap: 15px;
      align-items: center;
      font-size: 0.9rem;
      color: var(--highlight-blue);
    }

    .battery {
      display: flex;
      align-items: center;
      gap: 5px;
      color: var(--highlight-green);
      background: rgba(0, 255, 136, 0.1);
      padding: 2px 8px;
      border-radius: 5px;
    }

    /* Widgets */
    .widget-col {
      display: flex;
      flex-direction: column;
      gap: 20px;
      justify-content: space-between;
      height: 100%;
    }

    .card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 20px;
      padding: 20px;
      backdrop-filter: blur(10px);
      display: flex;
      flex-direction: column;
    }

    /* Media */
    .media-widget {
      flex: 0 0 auto;
      min-height: 140px;
      justify-content: center;
    }

    .media-content {
      display: flex;
      align-items: center;
      gap: 15px;
      margin-bottom: 20px;
    }

    .album-art {
      width: 50px;
      height: 50px;
      border-radius: 10px;
      background: #333;
      display: flex;
      align-items: center;
      justify-content: center;
      color: #1DB954;
    }

    .song-details span {
      display: block;
      font-size: 1.1em;
      font-weight: 600;
    }

    .song-details small {
      color: var(--text-muted);
      font-size: 0.8em;
    }

    .media-controls {
      display: flex;
      justify-content: space-around;
      width: 80%;
      margin: 0 auto;
    }

    .media-btn {
      background: none;
      border: none;
      color: #fff;
      font-size: 1.2rem;
      cursor: pointer;
      opacity: 0.8;
      transition: opacity 0.2s;
      padding: 5px;
    }

    .media-btn:hover {
      opacity: 1;
    }

    /* Incline */
    .incline-widget {
      flex: 1;
      position: relative;
      overflow: hidden;
    }

    .incline-value {
      font-size: 2.5rem;
      font-weight: 600;
      color: var(--text-main);
      z-index: 2;
    }

    .incline-graph {
      margin-top: auto;
      height: 60px;
      display: flex;
      align-items: flex-end;
      gap: 5px;
      opacity: 0.5;
    }

    .bar {
      flex: 1;
      background: var(--highlight-blue);
      border-radius: 2px 2px 0 0;
      transition: height 0.5s;
    }

    /* Gauge */
    .center-stage {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      position: relative;
    }

    .gauge-container {
      width: 380px;
      height: 380px;
      position: relative;
      display: flex;
      justify-content: center;
      align-items: center;
    }

    .gauge-ring {
      position: absolute;
      width: 100%;
      height: 100%;
      border-radius: 50%;
      background: conic-gradient(var(--highlight-orange) 0deg, var(--highlight-orange) var(--speed-deg, 0deg), rgba(255, 255, 255, 0.05) var(--speed-deg, 0deg), rgba(255, 255, 255, 0.05) 280deg, transparent 280deg);
      transform: rotate(-140deg);
      mask: radial-gradient(transparent 65%, black 66%);
      -webkit-mask: radial-gradient(transparent 65%, black 66%);
      transition: --speed-deg 0.5s ease-out;
    }

    .gauge-ticks {
      position: absolute;
      width: 85%;
      height: 85%;
      border-radius: 50%;
      border: 1px dashed rgba(255, 255, 255, 0.1);
      transform: rotate(-140deg);
    }

    .main-speed {
      text-align: center;
      z-index: 5;
      margin-bottom: 20px;
    }

    .speed-val {
      font-size: 5.5rem;
      font-weight: 700;
      line-height: 1;
      text-shadow: 0 0 30px rgba(0, 0, 0, 0.5);
    }

    .speed-unit {
      font-size: 1.2rem;
      color: var(--text-muted);
      font-weight: 500;
      letter-spacing: 2px;
    }

    /* Bottom Info */
    .bottom-info {
      margin-top: -40px;
      text-align: center;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 15px;
    }

    .info-pill {
      background: rgba(255, 255, 255, 0.1);
      padding: 8px 16px;
      border-radius: 20px;
      font-size: 1rem;
      color: var(--text-main);
      display: inline-flex;
      gap: 8px;
      align-items: center;
    }

    .center-controls {
      display: flex;
      gap: 20px;
      margin-top: 10px;
      z-index: 10;
    }

    /* Old ctrl-btn removed, updated below */

    .btn-start {
      background: var(--highlight-green);
      color: #000;
      box-shadow: 0 0 15px rgba(0, 255, 136, 0.3);
    }

    .btn-stop {
      background: rgba(255, 68, 68, 0.2);
      color: #ff4444;
      border: 1px solid #ff4444;
    }

    .ctrl-btn:active {
      transform: scale(0.95);
    }

    /* Stats */
    .stats-grid {
      display: grid;
      grid-template-columns: 1fr;
      gap: 15px;
      height: 100%;
    }

    .stat-row {
      background: rgba(255, 255, 255, 0.02);
      padding: 15px;
      border-radius: 12px;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }

    .stat-row small {
      color: var(--text-muted);
      font-size: 0.8rem;
      text-transform: uppercase;
    }

    .stat-row strong {
      font-size: 1.2rem;
      font-family: var(--font-mono);
      color: var(--highlight-blue);
    }

    /* Force Landscape Layout on Mobile */
    @media (max-height: 500px) and (orientation: landscape) {
      .dashboard-frame {
        width: 98vw;
        height: 98vh;
        padding: 5px;
        border-radius: 20px;
      }

      .screen-surface {
        display: grid;
        grid-template-columns: 1fr 1.5fr 1fr;
        /* Flexible columns for all mobile widths */
        grid-template-rows: 30px 1fr;
        gap: 10px;
      }

      .header-bar {
        grid-column: 1 / -1;
      }

      .main-content-row {
        display: contents;
      }

      .widget-col {
        display: flex;
        flex-direction: column;
        justify-content: space-between;
        gap: 5px;
      }

      .center-stage {
        grid-column: 2;
        grid-row: 2;
        min-height: 0;
      }

      /* Scale down gauge for short screens */
      .gauge-container {
        width: 70vh;
        height: 70vh;
        max-width: 300px;
        max-height: 300px;
      }

      .speed-val {
        font-size: 15vh;
      }

      /* Adjust stats for space */
      .stats-grid {
        grid-template-columns: 1fr;
        gap: 5px;
      }

      .stat-row {
        padding: 5px 10px;
      }

      .media-widget {
        min-height: 0;
        padding: 5px;
      }

      .media-content {
        margin-bottom: 5px;
      }
    }

    /* Shared Button/Text Styles */
    #magnet {
      font-family: var(--font-main);
      font-weight: 700;
      letter-spacing: 1px;
    }

    .ctrl-btn {
      padding: 10px 24px;
      border-radius: 30px;
      border: none;
      /* MATCHING FONT WITH SYSTEM READY (#magnet) */
      font-family: var(--font-main);
      font-weight: 700;
      letter-spacing: 1px;
      cursor: pointer;
      text-transform: uppercase;
      font-size: 0.9rem;
    }

    .magnet-alert {
      color: #ff4444;
      font-weight: bold;
      text-align: center;
      margin-top: 10px;
      display: none;
      animation: blink 1s infinite;
      font-size: 1.2rem;
    }

    @keyframes blink {
      50% {
        opacity: 0.5;
      }
    }
  </style>
</head>

<body>
  <div class="dashboard-frame">
    <div class="screen-surface">
      <!-- HEADER -->
      <div class="header-bar">
        <div class="clock" id="clock">09:34</div>
        <div class="status-area">
          <svg class="icon" viewBox="0 0 24 24">
            <path
              d="M17.71,7.29l-4.71-4.71V11.59L8.29,6.88L6.88,8.29L12.59,14l-5.71,5.71l1.41,1.41L13,16.41V25.41l4.71-4.71L14.41,17.41L17.71,14.12C18.1,13.73,18.1,13.1,17.71,12.71L14.41,9.41L17.71,6.12L17.71,7.29z M15,5.41L16.29,6.71L15,8V5.41z M15,18.59l-1.29,1.29L15,21.17V18.59z M13,13.59l-2.29-2.29L13,9V13.59z M13,15l-2.29,2.29L13,19.59V15z" />
          </svg>
          <span id="fbStatus">BLE Connected</span>
          <div class="battery">
            <svg class="icon" viewBox="0 0 24 24">
              <path
                d="M16,20H8V6H16M16.67,4H15V2H9V4H7.33A1.33,1.33,0,0,0,6,5.33V20.67C6,21.4,6.6,22,7.33,22H16.67A1.33,1.33,0,0,0,18,20.67V5.33C18,4.6,17.4,4,16.67,4Z" />
            </svg>
            67%
          </div>
        </div>
      </div>

      <!-- LEFT -->
      <div class="widget-col">
        <div class="card media-widget">
          <div class="media-content">
            <div class="album-art">
              <svg class="icon-xl" viewBox="0 0 24 24">
                <path d="M12,3V13.55A4,4,0,1,0,14,17V7H18V3M10,19A2,2,0,1,1,12,17A2,2,0,0,1,10,19Z" />
              </svg>
            </div>
            <div class="song-details"><span>Blinding Lights</span><small>The Weeknd</small></div>
          </div>
          <div class="media-controls">
            <button class="media-btn" onclick="controlMedia('prev')">
              <svg class="icon-lg" viewBox="0 0 24 24">
                <path d="M6,18V6H8V18H6M9.5,12L18,6V18L9.5,12Z" />
              </svg>
            </button>
            <button class="media-btn" id="playBtn" onclick="controlMedia('toggle')">
              <svg class="icon-lg" viewBox="0 0 24 24">
                <path d="M8,5.14V19.14L19,12.14L8,5.14Z" />
              </svg>
            </button>
            <button class="media-btn" onclick="controlMedia('next')">
              <svg class="icon-lg" viewBox="0 0 24 24">
                <path d="M16,18H18V6H16M6,18L14.5,12L6,6V18Z" />
              </svg>
            </button>
          </div>
        </div>
        <div class="card incline-widget">
          <div
            style="color: var(--text-muted); font-size: 0.8rem; margin-bottom: 10px; border-left: 3px solid var(--highlight-blue); padding-left: 10px;">
            INCLINE</div>
          <div class="incline-value" id="incline">+ 0.0 %</div>
          <div class="incline-graph">
            <div class="bar" style="height: 20%"></div>
            <div class="bar" style="height: 40%"></div>
            <div class="bar" style="height: 30%"></div>
            <div class="bar" style="height: 60%"></div>
            <div class="bar" style="height: 45%"></div>
            <div class="bar" style="height: 70%"></div>
          </div>
        </div>
      </div>

      <!-- CENTER -->
      <div class="center-stage">
        <div class="gauge-container">
          <div class="gauge-ring" id="gaugeRing"></div>
          <div class="gauge-ticks"></div>
          <div class="main-speed">
            <div class="speed-val" id="speed">0.0</div>
            <div class="speed-unit">KM/H</div>
          </div>
        </div>
        <div class="bottom-info">
          <div id="magnet" style="color: var(--highlight-green); font-weight:700;">SYSTEM READY</div>
          <div class="center-controls">
            <button class="ctrl-btn btn-start" onclick="startTrip()">Start</button>
            <button class="ctrl-btn btn-stop" onclick="stopTrip()">Stop</button>
          </div>
          <div class="info-pill" style="margin-top: 5px;">
            <svg class="icon" viewBox="0 0 24 24">
              <path
                d="M16,6L18.29,8.29L13.41,13.17L9.41,9.17L2,16.59L3.41,18L9.41,12L13.41,16L19.71,9.71L22,12V6H16Z" />
            </svg>
            <span id="inclinePill">0.0 %</span>
          </div>
        </div>
      </div>

      <!-- RIGHT -->
      <div class="widget-col">
        <div class="card" style="height: 100%;">
          <div style="color: var(--text-muted); font-size: 0.8rem; margin-bottom: 15px;">TRIP METRICS</div>
          <div class="stats-grid">
            <div class="stat-row"><small>Distance</small><strong id="distance">0.00 Km</strong></div>
            <div class="stat-row"><small>Duration</small><strong id="duration">00:00:00</strong></div>
            <div class="stat-row"><small>Avg Speed</small><strong id="avgSpeed">0.0</strong></div>
            <div class="stat-row"><small>Calories</small><strong id="calories">0</strong></div>
            <div class="stat-row"><small>System</small><strong id="lastUpdate" style="font-size: 0.8rem;">--</strong>
            </div>
            <!-- Removed external image, just a placeholder block -->
            <div
              style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 10px; margin-top: 10px; display:flex; align-items:center; justify-content:center; color:var(--text-muted); font-size:0.8rem;">
              MAP VIEW</div>
          </div>
        </div>
      </div>
    </div>
  </div>

  <script>
    function fT(ms) {
      let s = Math.floor(ms / 1000), h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
      return h.toString().padStart(2, '0') + ':' + m.toString().padStart(2, '0') + ':' + (s % 60).toString().padStart(2, '0');
    }
    function uC() {
      const n = new Date();
      document.getElementById('clock').innerText = n.getHours().toString().padStart(2, '0') + ":" + n.getMinutes().toString().padStart(2, '0');
    }
    setInterval(uC, 1000); uC();

    const isSim = window.location.protocol === 'file:';

    function api(ep) {
      if (isSim) console.log("Sim req:", ep);
      else fetch(ep).catch(console.error);
    }

    function startTrip() {
      if (isSim) {
        let m = document.getElementById("magnet");
        if (m.innerText === "SYSTEM READY") { m.innerText = "TRIP ACTIVE"; m.style.color = "#ffaa00"; }
      } else api('/trip?action=start');
    }
    function stopTrip() {
      if (isSim) {
        let m = document.getElementById("magnet");
        if (m.innerText === "TRIP ACTIVE") { m.innerText = "SYSTEM READY"; m.style.color = "#00ff88"; }
      } else api('/trip?action=stop');
    }
    function controlMedia(a) {
      if (isSim && a === 'toggle') {
        // Toggle play/pause icon logic
        // This is a simplified toggle for visual feedback
        let btn = document.getElementById("playBtn");
        let path = btn.querySelector("path");
        // Simple heuristic: if d starts with M8 (Play), switch to Pause d="M14,19H18V5H14M6,19H10V5H6V19Z"
        if (path.getAttribute("d").startsWith("M8")) path.setAttribute("d", "M14,19H18V5H14M6,19H10V5H6V19Z");
        else path.setAttribute("d", "M8,5.14V19.14L19,12.14L8,5.14Z");
      }
      api('/media?action=' + a);
    }

    setInterval(() => {
      if (isSim) {
        uUI((Math.random() * 45).toFixed(1), (Math.random() * 10).toFixed(1), Date.now() % 1000000, "48.75", "35.2", "450", "BLE OK", "Now", Math.random() > 0.9 ? "DETECTED" : "Standby", "true");
      } else {
        fetch('/data').then(r => r.text()).then(d => {
          let p = d.split(',');
          if (p.length >= 10) uUI(p[0], p[1], parseInt(p[2]), p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
        }).catch(() => { });
      }
    }, 800);

    function uUI(spd, inc, dur, dst, avg, cal, stat, last, mag, act) {
      document.getElementById("speed").innerHTML = spd;
      document.getElementById("incline").innerHTML = "+ " + inc + " %";
      document.getElementById("inclinePill").innerHTML = "+ " + inc + " %";
      document.getElementById("duration").innerHTML = fT(dur);
      document.getElementById("distance").innerHTML = parseFloat(dst).toFixed(2) + ' <span style="font-size:0.6em">Km</span>';
      document.getElementById("avgSpeed").innerHTML = avg + " <span style='font-size:0.6em'>KM/H</span>";
      document.getElementById("calories").innerHTML = Math.round(parseFloat(cal)) + " <span style='font-size:0.6em'>KCAL</span>";
      document.getElementById("fbStatus").innerHTML = stat;
      document.getElementById("lastUpdate").innerHTML = last;

      let deg = Math.min(spd / 60, 1) * 280;
      document.getElementById("gaugeRing").style.setProperty('--speed-deg', deg + 'deg');

      let m = document.getElementById("magnet");
      if (mag == "DETECTED") {
        m.innerHTML = "!! MAGNET !!"; m.style.color = "#ff4444";
        m.classList.add('magnet-alert'); m.style.display = "block";
      } else {
        m.classList.remove('magnet-alert');
        m.innerHTML = act == "true" ? "TRIP ACTIVE" : "SYSTEM READY";
        m.style.color = act == "true" ? "#ffaa00" : "#00ff88";
      }
    }
  </script>
</body>

</html>
)rawliteral";
  server.send(200, "text/html", html);
}

// Send sensor data
void handleData() {
  String lastUpdate = lastFirebaseUpdate > 0 ? String((millis() - lastFirebaseUpdate) / 1000) + "s ago" : "Never";
  String fbStatus = "Offline";
  if (WiFi.status() == WL_CONNECTED) {
    fbStatus = Firebase.ready() ? "Connected" : "Connecting...";
  } else {
    fbStatus = "No Internet";
  }

  // provide animated display speed
  String disp = String(displaySpeedKph, 1);
  String data = disp + "," +
                String(zIncline, 1) + "," +
                String(tripDuration) + "," +
                String(tripDistance, 2) + "," +
                String(bufferIndex > 0 ? (totalSpeed() / bufferIndex) : 0.0, 1) + "," +
                String(caloriesBurned, 1) + "," +
                fbStatus + "," +
                lastUpdate + "," +
                magnetState + "," +
                (tripActive ? "true" : "false");

  server.send(200, "text/plain", data);
}

// Handle trip control
void handleTrip() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    if (action == "start" && !tripActive) {
      tripActive = true;
      tripStartTime = millis();
      tripDuration = 0;
      tripDistance = 0.0;
      caloriesBurned = 0.0;
      rotationCount = 0;
      bufferIndex = 0;
      pendingHead = pendingTail = pendingCount = 0;
      server.send(200, "text/plain", "Trip started");
    } else if (action == "stop" && tripActive) {
      tripActive = false;
      // attempt to send any partial buffer now by packing it if it has >0 samples
      if (bufferIndex > 0 && pendingCount < MAX_PENDING) {
        pushPendingBatch(speedArray, inclineArray, caloriesArray);
        bufferIndex = 0;
      }
      // try to flush pending batches immediately
      while (pendingCount > 0) {
        if (!sendPendingBatch()) break;
      }
      updateFirebaseSummary();
      server.send(200, "text/plain", "Trip stopped");
    } else {
      server.send(200, "text/plain", "Invalid action");
    }
  }
}

void handleMedia() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    Serial.println("Media Action: " + action);
    // You can add Bluetooth HID logic here later to actually control phone music!
    server.send(200, "text/plain", "OK");
  }
}

void setup() {

  server.on("/media", handleMedia);
  Serial.begin(115200);
  delay(1000);

  // Initialize Hall Sensor
  pinMode(hallPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(hallPin), onMagnetDetect, FALLING);

  // Initialize MPU6050
  Serial.println("Initializing MPU6050...");
  if (!mpu.begin()) {
    Serial.println("Failed to find MPU6050 chip");
    while (1) { delay(10); }
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  Serial.println("MPU6050 initialized!");

  // Connect to WiFi (Station Mode only)
  Serial.print("Connecting to WiFi: ");
  Serial.println(wifi_ssid);
  WiFi.mode(WIFI_STA); // FORCE station-only
  WiFi.begin(wifi_ssid, wifi_password);

  int wifi_attempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_attempts < 60) {
    delay(500);
    Serial.print(".");
    wifi_attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("Station IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed - Firebase will not work!");
  }

  // Configure Firebase (only if WiFi connected)
  if (WiFi.status() == WL_CONNECTED) {
    config.api_key = FIREBASE_API_KEY;
    config.database_url = "https://" + String(FIREBASE_HOST);
    config.token_status_callback = tokenStatusCallback;
    Serial.println("Signing up to Firebase...");
    if (Firebase.signUp(&config, &auth, "", "")) {
      Serial.println("Firebase sign up success!");
    } else {
      Serial.printf("Sign up error: %s\n", config.signer.signupError.message.c_str());
    }
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    Serial.println("Firebase initialized!");
  } else {
    Serial.println("Skipping Firebase - no internet connection");
  }

  // Start Web Server
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/trip", handleTrip);
  server.begin();
  Serial.println("HTTP server started.");

  lastMagnetTime = millis();
  lastIMUUpdate = millis();
  lastSampleTime = millis();
  lastAnimationTime = millis();
}

// Main loop
void loop() {
  server.handleClient();

  // Read MPU6050
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  // Calculate angles
  pitch = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
  roll = atan2(-a.acceleration.x, a.acceleration.z) * 180.0 / PI;

  // Z-axis incline (vertical component)
  zIncline = asin(constrain(a.acceleration.z / 9.81, -1.0, 1.0)) * 180.0 / PI;
  if (isnan(zIncline)) zIncline = 0.0;

  accelY = a.acceleration.y; // forward/backwards acceleration in m/s^2

  // Compute time delta for IMU integration
  unsigned long now = millis();
  float dt = (now - lastIMUUpdate) / 1000.0;
  if (dt <= 0) dt = 0.001;
  lastIMUUpdate = now;

  // Integrate forward acceleration to estimate velocity (very simple)
  // apply small decay to limit drift
  imuVelocityMS += accelY * dt;
  imuVelocityMS *= 0.998; // decay to limit long-term drift
  if (imuVelocityMS < 0) imuVelocityMS = 0.0;
  imuSpeedKph = imuVelocityMS * 3.6;

  // Read hall speed safely (disable interrupts briefly)
  float hallLocal;
  noInterrupts();
  hallLocal = hallSpeedKph;
  interrupts();

  // Speed timeout if no magnet detected recently
  if (millis() - lastMagnetTime > SPEED_TIMEOUT_MS) {
    hallLocal = 0.0;
    // also reset hallSpeedKph variable under interrupt protection
    noInterrupts();
    hallSpeedKph = 0.0;
    interrupts();
  }
 // Fuse speed: average of hall and imu (if both zero, fused zero)
  fusedSpeedKph = (hallLocal + imuSpeedKph) / 2.0;

  // Trip distance update
  if (tripActive) {
    tripDuration = millis() - tripStartTime;
    tripDistance = (rotationCount * WHEEL_CIRCUMFERENCE_MM) / 1000000.0; // km

    // accumulate running calories using MET formula in continuous manner
    float met = calculateMET(fusedSpeedKph, fabs(zIncline));
    // calories per second
    float caloriesThisSecond = met * CYCLIST_WEIGHT_KG * (1.0/3600.0);
    caloriesBurned += caloriesThisSecond * dt; // dt fraction of a second
  }

  // Animation: step displaySpeed towards fusedSpeed in 0.5 increments every 200ms
  if (millis() - lastAnimationTime >= ANIMATION_STEP_MS) {
    lastAnimationTime = millis();
    float target = fusedSpeedKph;
    // round steps to 0.5
    if (fabs(displaySpeedKph - target) < 0.25) {
      displaySpeedKph = target; // close enough
    } else if (displaySpeedKph < target) {
      displaySpeedKph += ANIMATION_STEP_VALUE;
      if (displaySpeedKph > target) displaySpeedKph = target;
    } else if (displaySpeedKph > target) {
      displaySpeedKph -= ANIMATION_STEP_VALUE;
      if (displaySpeedKph < target) displaySpeedKph = target;
    }
  }

  // Buffering at 1 Hz
  if (millis() - lastSampleTime >= 1000) {
    lastSampleTime += 1000;
    // If pending queue is full and buffer already waiting to be pushed, pause acceptance.
    if (pendingCount >= MAX_PENDING && bufferIndex == BUFFER_SIZE) {
      Serial.println("All queues full. Pausing data collection to avoid overwrite.");
    } else {
      // store sample
      if (bufferIndex < BUFFER_SIZE) {
        speedArray[bufferIndex] = fusedSpeedKph;
        inclineArray[bufferIndex] = fabs(zIncline);
        // compute per-second calories sample
        float met = calculateMET(fusedSpeedKph, fabs(zIncline));
        float caloriesPerSec = met * CYCLIST_WEIGHT_KG / 3600.0;
        caloriesArray[bufferIndex] = caloriesPerSec;
        bufferIndex++;
      }
      // when buffer full, attempt sending
      if (bufferIndex >= BUFFER_SIZE) {
        // If Firebase ready and no pending batches, attempt to send directly.
        if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
          // attempt to send direct batch
          FirebaseJson json;
          FirebaseJsonArray speedArr;
          FirebaseJsonArray inclineArr;
          FirebaseJsonArray calArr;
          for (int k = 0; k < BUFFER_SIZE; ++k) {
            speedArr.add(speedArray[k]);
            inclineArr.add(inclineArray[k]);
            calArr.add(caloriesArray[k]);
          }
          json.set("speed", speedArr);
          json.set("incline", inclineArr);
          json.set("calories", calArr);
          String path = "/batches/" + String(millis());
          if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
            Serial.println("Direct batch push success.");
            bufferIndex = 0; // clear primary buffer
            // After direct success attempt to flush any pending batches too
            while (pendingCount > 0) {
              if (!sendPendingBatch()) break;
            }
          } else {
            Serial.println("Direct batch push failed. Queuing batch for retry.");
            // push current filled buffer into pending queue (if space)
            pushPendingBatch(speedArray, inclineArray, caloriesArray);
            bufferIndex = 0;
          }
        } else {
          // No internet or Firebase not ready: push to pending queue
          Serial.println("No Firebase connectivity, queuing batch for later.");
          pushPendingBatch(speedArray, inclineArray, caloriesArray);
          bufferIndex = 0;
        }
      }
    }
  }

  // retry pending batches periodically (every FIREBASE_UPDATE_INTERVAL)
  if (millis() - lastFirebaseUpdate >= FIREBASE_UPDATE_INTERVAL) {
    lastFirebaseUpdate = millis();
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      while (pendingCount > 0) {
        if (!sendPendingBatch()) break; // stop on first failure
      }
    }
  }
}

