# TallinnAtom ESP32 Firmware (M5Atom / ESP32)

## Purpose of the Project

This repository contains the **ESP32 (M5Atom) firmware** developed as part of an **academic laboratory project**.

The project was created within the courses **Andmehõive** and **Nutilahendused**, where the goal is to design and implement a realistic embedded data acquisition and IoT system.

The firmware is intended to demonstrate practical skills in embedded programming, signal acquisition, timing-critical logic, secure communication, and data handling on real hardware.

Although this is a study project, it is implemented as a **fully functional engineering system**, not as a simplified example or mock-up.

---

## What the Firmware Does

The device:

* operates as an IoT node with AP / STA / AP+STA Wi‑Fi modes
* serves a web user interface directly from ESP32 flash memory using LittleFS
* protects HTTP API endpoints with HMAC‑SHA256 authentication and anti‑replay checks
* reads an analog pressure sensor (MPX5700)
* controls a solenoid valve
* performs high‑frequency signal capture (~2 kHz) using a dedicated FreeRTOS task
* stores measurement data in CSV format with microsecond timestamps
* publishes summarized sensor data via MQTT with buffering and reconnect handling
* displays pressure values on a MAX7219 7‑segment display
* controls a WS2812 RGB LED for status indication

---

## High‑Level Architecture

The firmware is divided into several logical subsystems:

1. **Wi‑Fi and Network Modes**
   The device automatically selects its operating mode:

   * AP‑only mode when no STA credentials are stored (initial configuration)
   * AP+STA mode when STA credentials are available

2. **Web User Interface (LittleFS)**
   Static HTML, CSS and JavaScript files are stored in flash and served directly by the ESP32.

   * `index.html` – main interface
   * `wifi.html` – Wi‑Fi configuration (AP‑only mode)

3. **HTTP API**
   Used by the web UI and external clients for control and monitoring.
   Most endpoints are protected by HMAC authentication.

4. **Security Layer**

   * HMAC‑SHA256 request signing
   * timestamp‑based anti‑replay protection
   * security checks disabled only in AP‑only setup mode

5. **Measurement and Control Logic**

   * analog pressure sampling
   * solenoid control
   * G25 analog trigger logic
   * precise timing using `micros()` and a dedicated sampling task

6. **Data Transport (MQTT)**

   * circular buffer for the last 100 samples
   * reliable publishing with reconnect handling
   * designed to tolerate temporary network outages

---

## Hardware Overview

### Main Components

The project is built around the following hardware components:

* **ESP32 (M5Atom)**
  Main microcontroller. Responsible for networking, control logic, data acquisition, storage, and UI serving.

* **Pressure sensor: MPX5700**
  Analog pressure sensor used for measurement experiments. Connected to ESP32 ADC input. The firmware converts raw ADC values into pressure values in bar.

* **Solenoid valve**
  Electrically controlled valve used to manipulate pressure during experiments. Timing of opening and closing is critical and tightly coupled with measurement logic.

* **MAX7219 7-segment display**
  Used to display current pressure values in real time.

* **WS2812 RGB LED**
  Single addressable LED used for visual status indication (idle, active, valve open/closed).

* **Control button**
  Used to manually toggle solenoid state and trigger capture sequences.

---

## Software Dependencies and Libraries

### Core Framework

* **Arduino framework for ESP32**
  Provides basic runtime, GPIO, ADC, Wi-Fi, and timing APIs.

* **FreeRTOS (ESP32 built-in)**
  Used for running a dedicated high-frequency sampling task in parallel with the main loop.

### Networking and Web

* **WiFi.h**
  ESP32 Wi-Fi control (AP, STA, AP+STA modes).

* **WebServer.h**
  Lightweight HTTP server for API endpoints and static file serving.

* **DNSServer.h**
  Captive portal support (redirecting all DNS requests to the device).

* **PubSubClient**
  MQTT client library used to publish sensor data to a central server.

### File System and Storage

* **LittleFS**
  Flash-based file system used for:

  * Web UI files (HTML/CSS/JS)
  * CSV measurement logs

* **Preferences**
  ESP32 NVS wrapper used to store:

  * Wi-Fi credentials
  * HMAC secret key

### Cryptography and Security

* **mbedTLS (ESP32 built-in)**
  Used for:

  * SHA-256 hashing
  * HMAC-SHA256 request signing

### LEDs and Display

* **FastLED**
  Controls the WS2812 RGB LED.

* **LedController / MAX7219 library**
  Drives the 7-segment display via SPI-like interface.

### Miscellaneous

* **time.h / sys/time.h**
  Used for NTP time synchronization and timestamp generation.

--------|------|
| Solenoid control | 26 |
| Pressure sensor (ADC) | 32 |
| G25 analog trigger | 25 |
| WS2812 RGB LED | 27 |
| Control button | 22 |
| MAX7219 DIN | 33 |
| MAX7219 CLK | 19 |
| MAX7219 CS | 23 |

---

## Data Capture and CSV Format

High‑frequency capture data is stored in:

```
/capture_events.csv
```

Format:

```
event_id,i,dt_us,adc_raw,volts
12,0,0,512,0.4123
12,1,500,518,0.4171
...
END_EVENT,12
```

Each event may also include a metadata line describing timing and solenoid duration.

The CSV file is considered the **primary source of truth** for measurements.

---

## Web UI Upload (LittleFS)

The web interface files are located in the `data/` directory and must be uploaded separately from the firmware.

Using PlatformIO:

1. Open the project in VS Code
2. Open PlatformIO Project Tasks
3. Select the target environment (m5stack‑atom)
4. Run **Build Filesystem Image**
5. Run **Upload Filesystem Image**

This flashes the HTML/CSS/JS files into ESP32 flash memory.

---

## Security Model Summary

* AP‑only mode: authentication disabled (initial setup only)
* Client modes (STA / AP+STA):

  * all API requests must include a valid HMAC signature
  * signature format: `<timestamp>.<hmac>`
  * message signed as: `HMAC(key, path | params | timestamp)`
  * replay protection window: 5 seconds

---

## Design Intent

This firmware was written with the following priorities:

* deterministic timing over convenience
* raw data preservation over abstraction
* robustness against network instability
* clear separation between measurement, transport, and UI logic

If you are reading this months later: the complexity is intentional and driven by measurement requirements.

---

## Notes for Future Maintenance

* Sampling and solenoid timing are sensitive to changes
* Do not simplify the G25 logic without verifying with real signals
* CSV data is the authoritative measurement record
* MQTT is a transport layer, not the primary data store

---

TallinnAtom project
Platform: ESP32 / M5Atom

