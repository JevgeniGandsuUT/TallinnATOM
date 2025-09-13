#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h> 
#include "SPIFFS.h"
#include <Preferences.h>
Preferences prefs;

IPAddress IP;
WebServer server(80);

#define DATA_PIN 27    
#define NUM_LEDS 1   
CRGB leds[NUM_LEDS];

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
  
  IP = WiFi.softAPIP(); // Needed IP adress to get to Server from another device
  Serial.print("AP IP address: ");
  Serial.println(IP);

  server.on("/", handleRoot);
  server.on("/get", getCurrentLedColorInHEX);
  server.on("/set", setCurrentLedColorInHEX);
  server.on("/wifi", wifi);
  server.on("/changewifi", changeWifi);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  // put your main code here, to run repeatedly:
  // Handle incoming client requests
  server.handleClient();

  Serial.println(IP);
  delay(1000);
}


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
      overflow: hidden; /* убираем белые края */
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


//server.serveStatic("/", SPIFFS, "/")
   //   .setDefaultFile("index.html")
     // .setCacheControl("max-age=31536000, immutable");
server.send(200, "text/html", html);
}

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
                .then(data => {
                    console.log('Response:', data);
                })
                .catch(error => {
                    console.error('Error:', error);
                });
        });
    </script>
 
</body>
 
</html>)rawliteral";


//server.serveStatic("/", SPIFFS, "/")
   //   .setDefaultFile("index.html")
     // .setCacheControl("max-age=31536000, immutable");
server.send(200, "text/html", html);
}

void getCurrentLedColorInHEX(){  
    // Get color in HEX
    char colorHex[7];  
    sprintf(colorHex, "%02X%02X%02X", leds[0].r, leds[0].g, leds[0].b);
    String response = "#"+String(colorHex);
    server.send(200, "text/html", response);
}

void changeWifi(){  
    // Get color in HEX
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

void setCurrentLedColorInHEX(){  
    String incomingHex = server.arg("value");  // например "#FF00FF" или "FF00FF"
    
    // уберём решётку если есть
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
        // Сформировать ответ с подтверждением
        char colorHex[8];
        sprintf(colorHex, "#%02X%02X%02X", r, g, b);
        server.send(200, "text/html", colorHex);
    } else {
        server.send(400, "text/html", "Invalid HEX format");
    }
    // Get color in HEX
   //char colorHex[7];  
    //sprintf(colorHex, "%02X%02X%02X", leds[0].r, leds[0].g, leds[0].b);
    //String response = "#"+String(colorHex);
    //server.send(200, "text/html", response);
}
