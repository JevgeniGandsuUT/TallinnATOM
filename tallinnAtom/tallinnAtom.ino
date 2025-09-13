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
  <title>Color Picker and Fetch Example</title>
</head>
 
<body>
 
  <label for="colorPicker">Выберите цвет: </label>
  <input type="color" id="colorPicker" />
  <br>
  <button id="getButton">Получить данные</button>
 
  <script>
    const colorInput = document.getElementById('colorPicker');
    const getButton = document.getElementById('getButton');
 
    let currentColor;
 
    const getCurrentColor = () => {
      fetch('http://192.168.4.1/get')
        .then(response => response.text())
        .then(data => {
          currentColor = data;
          colorInput.value = currentColor;
        })
        .catch(error => {
          console.error('Ошибка при запросе:', error);
        });
    }
 
    window.onload = function () {
      getCurrentColor();
      colorInput.value = currentColor;
    }
 
    getButton.addEventListener('click', () => {
      fetch('http://192.168.4.1/get')
        .then(response => response.text())
        .then(data => {
          console.log('Ответ от сервера:', data);
        })
        .catch(error => {
          console.error('Ошибка при запросе:', error);
        });
    });
 
    colorInput.addEventListener('change', () => {
      let selectedColor = colorInput.value;
      selectedColor = selectedColor.replace('#', '');
      console.log(selectedColor)
      fetch(`http://192.168.4.1/set?value=${selectedColor}`)
        .then(response => response.text())
        .then(data => {
          console.log('Ответ от сервера:', data);
        })
        .catch(error => {
          console.error('Ошибка при запросе:', error);
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
