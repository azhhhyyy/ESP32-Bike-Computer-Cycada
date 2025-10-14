# 🚴‍♂️ ESP32 Bike Module (Wi-Fi Dashboard Version)

A self-contained **IoT bike telemetry module** built using an **ESP32**, an **MPU6050** accelerometer/gyroscope, and a **Hall effect sensor**.  
This setup creates a **local Wi-Fi Access Point** — no router or internet required — that serves a **live web dashboard** showing real-time data such as **incline, tilt, acceleration forces, and magnet detection**.

---

## 📡 Features

- **Wi-Fi Access Point Mode** — ESP32 acts as a standalone hotspot (`BikeModule`), accessible from any phone or laptop.  
- **Live Web Dashboard** — Access real-time data at [http://192.168.4.1](http://192.168.4.1)  
- **MPU6050 Integration** — Displays incline (pitch), tilt (roll), and 3-axis acceleration (X, Y, Z).  
- **Hall Effect Sensor** — Detects magnetic field presence (for wheel rotation or speed sensing).  
- **Self-hosted** — Works entirely offline. Perfect for field use.

---

## 🧠 Hardware Used

| Component | Purpose | Notes |
|------------|----------|-------|
| **ESP32** | Microcontroller with built-in Wi-Fi | Any variant with 2+ GPIOs |
| **MPU6050** | Accelerometer + Gyroscope | I2C communication |
| **A3144 Hall Sensor** | Detects magnetic field | Digital output |
| **Neodymium Magnet** | Wheel-mounted magnet | Used for rotation/speed sensing |

---

## ⚙️ Pin Connections

| Sensor | ESP32 Pin | Notes |
|---------|------------|-------|
| **MPU6050 SDA** | GPIO21 | I2C Data |
| **MPU6050 SCL** | GPIO22 | I2C Clock |
| **Hall Sensor OUT** | GPIO2 | Digital input |
| **VCC** | 3.3V | Power |
| **GND** | GND | Common ground |

---

## 💻 Installation & Setup

1. **Clone this repository**
   ```bash
   git clone https://github.com/<yourusername>/esp32-bike-module.git
   cd esp32-bike-module
2. Open the .ino file in the Arduino IDE or PlatformIO.

3. Install required libraries:
    Adafruit MPU6050
    Adafruit Sensor
    Built-in libraries: WiFi.h, WebServer.h, Wire.h

4. Select Board:
  Tools > Board > ESP32 Dev Module
  Set upload speed to 115200

5.Upload the code.
