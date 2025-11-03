#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --- FIREBASE CONFIG ---
#define FIREBASE_HOST "enter-database-api"
#define FIREBASE_API_KEY "enter-database-api-key"

// --- WIFI SETTINGS ---
// AP Mode (for phone connection)
const char* ap_ssid = "ESP32";
const char* ap_password = "12345678";

// Station Mode (for internet connection - UPDATE THESE!)
const char* wifi_ssid = "iPhone";      // Your home/hotspot WiFi name
const char* wifi_password = "12345678"; // Your WiFi password

// --- CYCLIST INFO (UPDATE THESE) ---
const float CYCLIST_WEIGHT_KG = 70.0; // Update with actual weight
const int WHEEL_CIRCUMFERENCE_MM = 2105;

// DNS and Web Server
DNSServer dnsServer;
WebServer server(80);

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// MPU6050 Sensor
Adafruit_MPU6050 mpu;

// Hall Effect Sensor
const int hallPin = 15;

// Speed variables
volatile unsigned long lastMagnetTime = 0;
volatile unsigned long rotationCount = 0;
float speedKph = 0.0;
float imuSpeedKph = 0.0;
const int SPEED_TIMEOUT_MS = 2000;

// IMU variables
float pitch = 0.0;      // X-axis tilt (forward/backward)
float roll = 0.0;       // Y-axis tilt
float zIncline = 0.0;   // Z-axis incline (vertical component)
float accelY = 0.0;
float prevVelocity = 0.0;
unsigned long lastIMUUpdate = 0;

// Trip tracking variables
unsigned long tripStartTime = 0;
unsigned long tripDuration = 0;
float tripDistance = 0.0;
float totalSpeed = 0.0;
float totalIncline = 0.0;
unsigned int speedSamples = 0;
float caloriesBurned = 0.0;
bool tripActive = false;
String magnetState = "Standby";

// Firebase timing
unsigned long lastFirebaseUpdate = 0;
const unsigned long FIREBASE_UPDATE_INTERVAL = 10000; // Update every 10 seconds

// Interrupt for Hall sensor
void IRAM_ATTR onMagnetDetect() {
  unsigned long now = millis();
  unsigned long timeElapsed = now - lastMagnetTime;

  if (timeElapsed > 20) { // Debounce
    speedKph = ((float)WHEEL_CIRCUMFERENCE_MM / (float)timeElapsed) * 3.6;
    lastMagnetTime = now;
    rotationCount++;
  }
}

// Calculate MET value based on speed and incline
float calculateMET(float speed, float incline) {
  // Base MET values for cycling
  float met = 0.0;
  
  if (speed < 1.0) {
    met = 1.0; // Resting
  } else if (speed < 16.0) {
    met = 4.0; // Light effort
  } else if (speed < 19.0) {
    met = 6.8; // Moderate effort
  } else if (speed < 22.0) {
    met = 8.0; // Vigorous effort
  } else {
    met = 10.0; // Very vigorous
  }
  
  // Adjust for incline (add 0.5 MET per 5 degrees of incline)
  if (incline > 0) {
    met += (incline / 5.0) * 0.5;
  }
  
  return met;
}

// Calculate calories burned
void updateCalories() {
  if (!tripActive || speedSamples == 0) return;
  
  float avgSpeed = totalSpeed / speedSamples;
  float avgIncline = totalIncline / speedSamples;
  float tripHours = tripDuration / 3600000.0; // Convert ms to hours
  
  float met = calculateMET(avgSpeed, avgIncline);
  caloriesBurned = met * CYCLIST_WEIGHT_KG * tripHours;
}

// Update Firebase with trip data
void updateFirebase() {
  // Check if we have internet connection
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
  json.set("avgSpeed", speedSamples > 0 ? totalSpeed / speedSamples : 0.0);
  json.set("avgIncline", speedSamples > 0 ? totalIncline / speedSamples : 0.0);
  json.set("calories", caloriesBurned);
  json.set("currentSpeed", speedKph);
  json.set("currentIncline", zIncline);
  json.set("active", tripActive);
  
  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    Serial.println("Firebase update success!");
  } else {
    Serial.println("Firebase update failed: " + fbdo.errorReason());
  }
}

// HTML Page
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 Bike Module</title>
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
  <link href="https://fonts.googleapis.com/css2?family=Roboto+Mono:wght@300;700&family=Roboto:wght@400&display=swap" rel="stylesheet">
  <style>
    html, body {
      height: 100%; margin: 0; padding: 0; background-color: #000; color: #f0f0f0; 
      font-family: 'Roboto Mono', monospace; overflow: hidden;
    }
    .main-container {
      display: flex; justify-content: space-between; align-items: center;
      height: 100%; padding: 0 3vw; box-sizing: border-box;
    }
    .column {
      flex: 1; height: 90vh; display: flex; flex-direction: column;
      justify-content: center; align-items: center; padding: 0 1vw;
    }
    .center-column { flex: 1.5; justify-content: space-around; }
    
    .trip-stats {
      width: 100%; padding: 20px; background: #111; border-radius: 15px;
      border: 2px solid #333; text-align: center; margin-bottom: 20px;
    }
    .stat-item { margin: 10px 0; font-size: 1.2em; }
    .stat-label { color: #666; font-size: 0.8em; }
    .stat-value { color: #00ff88; font-size: 1.5em; font-weight: 700; }
    .calories { color: #ff8800; }
    
    .trip-controls {
      display: flex; gap: 15px; margin-top: 20px;
    }
    .trip-btn {
      flex: 1; padding: 15px; font-size: 1.2em; font-weight: 700;
      border: 2px solid; border-radius: 10px; cursor: pointer;
      font-family: 'Roboto Mono', monospace; transition: all 0.3s;
    }
    .start-btn { background: #00ff88; color: #000; border-color: #00ff88; }
    .start-btn:active { background: #00cc66; }
    .stop-btn { background: #ff4444; color: #fff; border-color: #ff4444; }
    .stop-btn:active { background: #cc0000; }
    
    .placeholder-box {
      width: 100%; height: 45%; border: 2px dashed #444; border-radius: 15px;
      display: flex; flex-direction: column; justify-content: center;
      align-items: center; text-align: center; color: #666;
    }
    .icon { font-size: 3em; margin-bottom: 15px; }
    
    .magnet-status { font-size: 2.0vh; font-weight: 700; color: #00ff88; 
      height: 4vh; letter-spacing: 2px; margin-bottom: 2vh;}
    .speed-display { font-size: 18vh; color: #FFFFFF; font-weight: 700; line-height: 0.9; }
    .unit-kmh { font-size: 3vh; color: #888; font-weight: 300; margin-left: 10px; }
    .incline-display { font-size: 6vh; color: #999999; font-weight: 700; margin-top: 1vh; }
    .unit-deg { font-size: 2.5vh; color: #666; font-weight: 300; }
    
    @import url('https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css');
  </style>
</head>
<body>
  <div class="main-container">
    
    <!-- Left Column: Trip Stats -->
    <div class="column">
      <div class="trip-stats">
        <div class="stat-item">
          <div class="stat-label">DURATION</div>
          <div class="stat-value" id="duration">00:00:00</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">DISTANCE</div>
          <div class="stat-value" id="distance">0.00 km</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">AVG SPEED</div>
          <div class="stat-value" id="avgSpeed">0.0 km/h</div>
        </div>
        <div class="stat-item">
          <div class="stat-label">CALORIES</div>
          <div class="stat-value calories" id="calories">0 kcal</div>
        </div>
        <div class="trip-controls">
          <button class="trip-btn start-btn" onclick="startTrip()">START</button>
          <button class="trip-btn stop-btn" onclick="stopTrip()">STOP</button>
        </div>
      </div>
    </div>

    <!-- Center Column: Live Data -->
    <div class="column center-column">
      <div id="magnet" class="magnet-status">SYSTEM READY</div>
      <div class="speed-display">
        <span id="speed">0.0</span><span class="unit-kmh">km/h</span>
      </div>
      <div class="incline-display">
        <span id="incline">0.0</span><span class="unit-deg">&deg;</span>
      </div>
    </div>

    <!-- Right Column: Firebase Status -->
    <div class="column">
      <div class="placeholder-box">
        <span class="icon"><i class="fas fa-cloud"></i></span>
        <h3>Firebase Sync</h3>
        <div id="fbStatus" style="font-size: 1.2em; margin: 10px 0;">Connecting...</div>
        <div style="margin-top: 15px; font-size: 0.9em; color: #888;">
          Last Update:<br><span id="lastUpdate" style="color: #00ff88;">--</span>
        </div>
      </div>
    </div>
  </div>

  <script>
    function formatDuration(ms) {
      let totalSec = Math.floor(ms / 1000);
      let hours = Math.floor(totalSec / 3600);
      let minutes = Math.floor((totalSec % 3600) / 60);
      let seconds = totalSec % 60;
      return hours.toString().padStart(2,'0') + ':' + 
             minutes.toString().padStart(2,'0') + ':' + 
             seconds.toString().padStart(2,'0');
    }

    function startTrip() {
      fetch('/trip?action=start').then(r => r.text()).then(msg => console.log(msg));
    }

    function stopTrip() {
      fetch('/trip?action=stop').then(r => r.text()).then(msg => console.log(msg));
    }

    setInterval(function() {
      fetch('/data').then(r => r.text()).then(data => {
        let parts = data.split(',');
        document.getElementById("speed").innerHTML = parts[0];
        document.getElementById("incline").innerHTML = parts[1];
        document.getElementById("duration").innerHTML = formatDuration(parseInt(parts[2]));
        document.getElementById("distance").innerHTML = parseFloat(parts[3]).toFixed(2) + ' km';
        document.getElementById("avgSpeed").innerHTML = parts[4] + ' km/h';
        document.getElementById("calories").innerHTML = Math.round(parseFloat(parts[5])) + ' kcal';
        document.getElementById("fbStatus").innerHTML = parts[6];
        document.getElementById("lastUpdate").innerHTML = parts[7];
        
        let magnetEl = document.getElementById("magnet");
        if (parts[8] == "DETECTED") {
          magnetEl.innerHTML = "!! MAGNET DETECTED !!";
          magnetEl.style.color = "#ff4444";
        } else {
          magnetEl.innerHTML = parts[9] == "true" ? "TRIP ACTIVE" : "SYSTEM READY";
          magnetEl.style.color = parts[9] == "true" ? "#ffaa00" : "#00ff88";
        }
      });
    }, 500);
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
  
  String avgSpeed = speedSamples > 0 ? String(totalSpeed / speedSamples, 1) : "0.0";
  
  String data = String(speedKph, 1) + "," + 
                String(zIncline, 1) + "," +
                String(tripDuration) + "," +
                String(tripDistance, 2) + "," +
                avgSpeed + "," +
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
      totalSpeed = 0.0;
      totalIncline = 0.0;
      speedSamples = 0;
      caloriesBurned = 0.0;
      rotationCount = 0;
      server.send(200, "text/plain", "Trip started");
    } 
    else if (action == "stop" && tripActive) {
      tripActive = false;
      updateFirebase(); // Final update
      server.send(200, "text/plain", "Trip stopped");
    }
    else {
      server.send(200, "text/plain", "Invalid action");
    }
  }
}

void setup() {
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

  // Connect to WiFi (Station Mode) for internet access
  Serial.print("Connecting to WiFi: ");
  Serial.println(wifi_ssid);
  WiFi.mode(WIFI_AP_STA); // Enable both AP and Station mode
  WiFi.begin(wifi_ssid, wifi_password);
  
  // Wait for WiFi connection (timeout after 15 seconds)
  int wifi_attempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_attempts < 30) {
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

  // Start Access Point (for phone connection)
  Serial.print("Creating Access Point: ");
  Serial.println(ap_ssid);
  WiFi.softAP(ap_ssid, ap_password);

  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);

  // Start DNS for captive portal
  dnsServer.start(53, "*", myIP);

  // Configure Firebase (only if WiFi connected)
  if (WiFi.status() == WL_CONNECTED) {
    config.api_key = FIREBASE_API_KEY;
    config.database_url = "https://" + String(FIREBASE_HOST);
    config.token_status_callback = tokenStatusCallback;
    
    // Sign up (anonymous)
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

  // Setup Web Server
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/trip", handleTrip);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("HTTP server started.");

  lastMagnetTime = millis();
  lastIMUUpdate = millis();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // Read MPU6050
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  
  // Calculate angles
  pitch = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
  roll = atan2(-a.acceleration.x, a.acceleration.z) * 180.0 / PI;
  
  // Calculate Z-axis incline (vertical component)
  // This represents how much the bike is tilted from vertical
  zIncline = asin(a.acceleration.z / 9.81) * 180.0 / PI;
  if (isnan(zIncline)) zIncline = 0.0;
  
  accelY = a.acceleration.y;

  // Check for speed timeout
  if (millis() - lastMagnetTime > SPEED_TIMEOUT_MS) {
    speedKph = 0.0;
  }

  // Magnet detection state
  if (millis() - lastMagnetTime < 250) {
    magnetState = "DETECTED";
  } else {
    magnetState = "Standby";
  }

  // Update trip data if active
  if (tripActive) {
    tripDuration = millis() - tripStartTime;
    tripDistance = (rotationCount * WHEEL_CIRCUMFERENCE_MM) / 1000000.0; // Convert to km
    
    if (speedKph > 0.1) { // Only count when moving
      totalSpeed += speedKph;
      totalIncline += abs(zIncline);
      speedSamples++;
    }
    
    updateCalories();
    
    // Update Firebase periodically
    if (millis() - lastFirebaseUpdate >= FIREBASE_UPDATE_INTERVAL) {
      updateFirebase();
      lastFirebaseUpdate = millis();
      Serial.println("Firebase updated");
    }
  }
}
