#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <FastLED.h>
#include "SPIFFS.h"
#include <Preferences.h>

Preferences prefs;

IPAddress apIP(192,168,4,1);
DNSServer dns;
WebServer server(80);

#define DATA_PIN 27
#define NUM_LEDS 1
CRGB leds[NUM_LEDS];

// -------- Wi-Fi creds (Preferences: "wifi") ----------
void saveWifi(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);    // namespace "wifi"
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void loadWifi(String& ssid, String& pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
}

// -------- Optimal Hz (Preferences: "ui" -> opt_hz) ---
int optimalHz = 10; // дефолт, если ещё не сохранено

void saveOptimalHz(int hz) {
  prefs.begin("ui", false);
  prefs.putInt("opt_hz", hz);
  prefs.end();
}

void loadOptimalHz(int &hz) {
  prefs.begin("ui", true);
  hz = prefs.getInt("opt_hz", 10);
  prefs.end();
}

// --------- handlers forward declarations --------------
void handleRoot();
void getCurrentLedColorInHEX();
void setCurrentLedColorInHEX();
void wifi();
void changeWifi();
void handleCaptivePortal();
void getOptimalHzHandler();
void setOptimalHzHandler();

void setup() {
  Serial.begin(9600);
  Serial.print("Setting as access point ");

  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
  leds[0] = CRGB::Red;
  FastLED.show();

  WiFi.mode(WIFI_AP);

  String ssid, pass;
  loadWifi(ssid, pass);
  if (ssid == "") {
    ssid = "TallinnAtom";
    pass = "12345678";
  }

  WiFi.softAP(ssid.c_str(), pass.c_str());
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));

  //IP = WiFi.softAPIP();
  //Serial.print("AP IP address: ");
  //Serial.println(IP);

  dns.start(53, "*", apIP);

  // Прочитать сохранённый optimal Hz
  loadOptimalHz(optimalHz);
  Serial.print("Optimal Hz loaded: ");
  Serial.println(optimalHz);

  server.on("/", handleRoot);
  server.on("/get", getCurrentLedColorInHEX);
  server.on("/set", setCurrentLedColorInHEX);
  server.on("/wifi", wifi);
  server.on("/changewifi", changeWifi);
  server.on("/getoptimal", getOptimalHzHandler);
  server.on("/setoptimal", setOptimalHzHandler);
  server.on("/generate_204", handleCaptivePortal);           // Android
  server.on("/hotspot-detect.html", handleCaptivePortal);    // Apple
  server.on("/connectivitycheck.gstatic.com", handleCaptivePortal); // Android
  server.on("/captive.apple.com", handleCaptivePortal);      // Apple
  server.onNotFound(handleCaptivePortal);                    // редиректим всё остальное
  server.begin();
  Serial.println("HTTP server started");
}

void handleCaptivePortal() {
  // Перенаправляем все запросы на корневую страницу
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Redirecting to /");
}

void loop() {
  // Handle incoming client requests
  server.handleClient();
}

// ------------------- UI: index ------------------------
void handleRoot() {
  String html = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>LED Controller</title>
  <style>
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background: #0d1117;
      color: #e6edf3;
      display: flex;
      justify-content: center;
      align-items: center;
      height: 100vh;
      margin: 0;
    }
    .container {
      background: #161b22;
      padding: 30px;
      border-radius: 16px;
      box-shadow: 0 0 20px rgba(0, 255, 200, 0.3);
      text-align: center;
      max-width: 420px;
      width: 100%;
    }
    h1 {
      font-size: 28px;
      margin-bottom: 25px;
      color: #58a6ff;
      text-shadow: 0 0 10px rgba(88,166,255,0.6);
    }
    label {
      font-size: 18px;
      margin-bottom: 15px;
      display: block;
      color: #79c0ff;
    }
    input[type="color"] {
      -webkit-appearance: none;
      border: none;
      width: 90px;
      height: 90px;
      border-radius: 50%;
      cursor: pointer;
      padding: 0;
      box-shadow: 0 0 15px rgba(0,255,200,0.5);
      overflow: hidden;
    }
    input[type="color"]::-webkit-color-swatch-wrapper {
      padding: 0;
      border-radius: 50%;
    }
    input[type="color"]::-webkit-color-swatch {
      border: none;
      border-radius: 50%;
    }
    button {
      margin-top: 20px;
      background: linear-gradient(90deg, #0ea5e9, #14b8a6);
      color: #fff;
      border: none;
      padding: 12px 25px;
      border-radius: 25px;
      font-size: 16px;
      font-weight: bold;
      cursor: pointer;
      transition: all 0.3s ease;
    }
    button:hover {
      background: linear-gradient(90deg, #38bdf8, #2dd4bf);
      transform: scale(1.05);
      box-shadow: 0 0 12px rgba(56,189,248,0.7);
    }
    .color-preview {
      margin-top: 25px;
      font-size: 18px;
      font-weight: bold;
      color: #a5d6ff;
    }
    .color-circle {
      display: inline-block;
      width: 60px;
      height: 60px;
      border-radius: 50%;
      border: 2px solid #333;
      margin-left: 12px;
      vertical-align: middle;
      box-shadow: 0 0 10px rgba(255,255,255,0.2);
      transition: background-color 0.6s ease, box-shadow 0.6s ease;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>LED Controller</h1>
    <label for="colorPicker">Choose a color:</label>
    <input type="color" id="colorPicker" />
    <br>
    <button id="getButton">Get current color</button>
    <div class="color-preview">
      Current color: <span id="currentColorText">#000000</span>
      <span class="color-circle" id="currentColorBox"></span>
    </div>
  </div>

  <script>
    const colorInput = document.getElementById('colorPicker');
    const getButton = document.getElementById('getButton');
    const currentColorText = document.getElementById('currentColorText');
    const currentColorBox = document.getElementById('currentColorBox');

    let currentColor;

    const getCurrentColor = () => {
      fetch('http://192.168.4.1/get')
        .then(response => response.text())
        .then(data => {
          currentColor = data;
          colorInput.value = currentColor;
          currentColorText.textContent = currentColor;
          currentColorBox.style.backgroundColor = currentColor;
          currentColorBox.style.boxShadow = `0 0 20px ${currentColor}`;
        })
        .catch(error => {
          console.error('Error while fetching:', error);
        });
    }

    window.onload = function () {
      getCurrentColor();
    }

    getButton.addEventListener('click', () => {
      fetch('http://192.168.4.1/get')
        .then(response => response.text())
        .then(data => {
          console.log('Response from server:', data);
          currentColorText.textContent = data;
          currentColorBox.style.backgroundColor = data;
          currentColorBox.style.boxShadow = `0 0 20px ${data}`;
        })
        .catch(error => {
          console.error('Error while fetching:', error);
        });
    });

    colorInput.addEventListener('change', () => {
      let selectedColor = colorInput.value;
      selectedColor = selectedColor.replace('#', '');
      console.log('Selected color:', selectedColor)
      fetch(`http://192.168.4.1/set?value=${selectedColor}`)
        .then(response => response.text())
        .then(data => {
          console.log('Response from server:', data);
          currentColorText.textContent = data;
          currentColorBox.style.backgroundColor = data;
          currentColorBox.style.boxShadow = `0 0 20px ${data}`;
        })
        .catch(error => {
          console.error('Error while sending color:', error);
        });
    });
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

// ------------------- Wi-Fi page -----------------------
void wifi() {
  String html = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Login and Password Form</title>
</head>
<body>
  <h2>Update Credentials</h2>
  <form id="credentialsForm">
      <label for="newLogin">New login:</label>
      <input type="text" id="newLogin" name="newLogin" required />
      <br><br>
      <label for="newPassword">New password:</label>
      <input type="password" id="newPassword" name="newPassword" required />
      <br><br>
      <button type="submit">Submit</button>
  </form>

  <script>
    const form = document.getElementById('credentialsForm');
    form.addEventListener('submit', function (event) {
      event.preventDefault();
      const newLogin = encodeURIComponent(document.getElementById('newLogin').value);
      const newPassword = encodeURIComponent(document.getElementById('newPassword').value);
      fetch(`http://192.168.4.1/changewifi?ssid=${newLogin}&password=${newPassword}`)
        .then(response => response.text())
        .then(data => { console.log('Response:', data); })
        .catch(error => { console.error('Error:', error); });
    });
  </script>
</body>
</html>)rawliteral";

  server.send(200, "text/html", html);
}

// ------------------- API: get color -------------------
void getCurrentLedColorInHEX(){
  char colorHex[7];
  sprintf(colorHex, "%02X%02X%02X", leds[0].r, leds[0].g, leds[0].b);
  String response = "#" + String(colorHex);
  server.send(200, "text/html", response);
}

// ------------------- API: change Wi-Fi ----------------
void changeWifi(){
  String newSsid = server.arg("ssid");
  String newPass = server.arg("password");
  if (newPass.length() < 8) {
    server.send(400, "text/plain", "Password must be >= 8 chars");
    return;
  }
  saveWifi(newSsid, newPass);
  server.send(200, "text/plain", "Saved, rebooting...");
  delay(500);
  ESP.restart();
}

// ------------------- API: set color -------------------
void setCurrentLedColorInHEX(){
  String incomingHex = server.arg("value");  // "#FF00FF" или "FF00FF"
  if (incomingHex.startsWith("#")) {
    incomingHex = incomingHex.substring(1);
  }

  if (incomingHex.length() == 6) {
    long number = strtol(incomingHex.c_str(), NULL, 16);
    byte r = (number >> 16) & 0xFF;
    byte g = (number >> 8) & 0xFF;
    byte b = number & 0xFF;

    leds[0] = CRGB(r, g, b);
    FastLED.show();

    char colorHex[8];
    sprintf(colorHex, "#%02X%02X%02X", r, g, b);
    server.send(200, "text/html", colorHex);
  } else {
    server.send(400, "text/html", "Invalid HEX format");
  }
}

// --------------- API: optimal Hz (get/set) ------------
void getOptimalHzHandler() {
  server.send(200, "text/plain", String(optimalHz));
}

void setOptimalHzHandler() {
  if (!server.hasArg("hz")) {
    server.send(400, "text/plain", "Missing 'hz' param");
    return;
  }
  String hzStr = server.arg("hz");
  int hz = hzStr.toInt();
  if (hz < 1 || hz > 200) {
    server.send(400, "text/plain", "hz must be between 1 and 200");
    return;
  }
  optimalHz = hz;
  saveOptimalHz(optimalHz);
  server.send(200, "text/plain", String(optimalHz));
}
