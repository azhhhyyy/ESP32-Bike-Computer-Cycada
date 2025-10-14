#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>

WebServer server(80);
Adafruit_MPU6050 mpu;

// Hall Sensor
const int hallPin = 2;

// Sensor values
float pitch = 0.0, roll = 0.0, accelX = 0.0, accelY = 0.0, accelZ = 0.0;
String magnetState = "No";

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 Bike Module</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; background-color: #282c34; color: white; }
    .card { background-color: #1e2228; padding: 15px; border-radius: 10px; display: inline-block; margin: 10px; min-width: 180px; vertical-align: top;}
    h1 { color: #61dafb; }
    h2 { margin-bottom: 5px; }
    p { font-size: 1.8em; margin-top: 5px; color: #98c379; }
    .unit { font-size: 0.6em; color: #abb2bf;}
  </style>
</head>
<body>
  <h1>ESP32 Bike Module Data</h1>
  <div class="card">
    <h2>Incline (Pitch)</h2>
    <p><span id="pitch">0.00</span><span class="unit"> &deg;</span></p>
  </div>
  <div class="card">
    <h2>Tilt (Roll)</h2>
    <p><span id="roll">0.00</span><span class="unit"> &deg;</span></p>
  </div>
  <div class="card">
    <h2>Magnet Detected</h2>
    <p><span id="magnet">No</span></p>
  </div>
  <br>
  <div class="card">
    <h2>Force X</h2>
    <p><span id="accelX">0.00</span><span class="unit"> m/s&sup2;</span></p>
  </div>
  <div class="card">
    <h2>Force Y</h2>
    <p><span id="accelY">0.00</span><span class="unit"> m/s&sup2;</span></p>
  </div>
  <div class="card">
    <h2>Force Z</h2>
    <p><span id="accelZ">0.00</span><span class="unit"> m/s&sup2;</span></p>
  </div>

<script>
setInterval(function() {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      var data = this.responseText.split(',');
      document.getElementById("pitch").innerHTML = data[0];
      document.getElementById("roll").innerHTML = data[1];
      document.getElementById("magnet").innerHTML = data[2];
      document.getElementById("accelX").innerHTML = data[3];
      document.getElementById("accelY").innerHTML = data[4];
      document.getElementById("accelZ").innerHTML = data[5];
    }
  };
  xhttp.open("GET", "/data", true);
  xhttp.send();
}, 1000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleData() {
  String data = String(pitch, 2) + "," + String(roll, 2) + "," + magnetState +
                "," + String(accelX, 2) + "," + String(accelY, 2) + "," + String(accelZ, 2);
  server.send(200, "text/plain", data);
}

void setup() {
  Serial.begin(115200);
  pinMode(hallPin, INPUT_PULLUP);

  // Initialize MPU6050
  if (!mpu.begin()) {
    Serial.println("MPU6050 not found");
    while (1) delay(10);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  // --- Wi-Fi Access Point Mode ---
  const char* ap_ssid = "BikeModule";
  const char* ap_password = "12345678";
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("Access Point IP: ");
  Serial.println(IP);
  Serial.println("Connect your phone to Wi-Fi: BikeModule (password: 12345678)");
  Serial.println("Then open http://192.168.4.1 in a browser.");

  // Start server
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  accelX = a.acceleration.x;
  accelY = a.acceleration.y;
  accelZ = a.acceleration.z;
  pitch = atan2(accelY, accelZ) * 180 / PI;
  roll = atan2(-accelX, accelZ) * 180 / PI;
  magnetState = (digitalRead(hallPin) == LOW) ? "Yes" : "No";

  server.handleClient();
}
