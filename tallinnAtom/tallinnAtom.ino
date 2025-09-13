#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h> 
#include "SPIFFS.h"


const char* ssid = "TallinnAtom";
const char* password = "12345678";
IPAddress IP;
WebServer server(80);

#define DATA_PIN 27    
#define NUM_LEDS 1   
CRGB leds[NUM_LEDS];

void setup() {

  Serial.begin(9600);
  Serial.print("Setting as access point ");
  
  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
  leds[0] = CRGB::Red;  
  FastLED.show();


  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);

  
  IP = WiFi.softAPIP(); // Needed IP adress to get to Server from another device
  Serial.print("AP IP address: ");
  Serial.println(IP);

  server.on("/", handleRoot);
  server.on("/get", getCurrentLedColorInHEX);
  server.on("/set", setCurrentLedColorInHEX);
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


void getCurrentLedColorInHEX(){  
    // Get color in HEX
    char colorHex[7];  
    sprintf(colorHex, "%02X%02X%02X", leds[0].r, leds[0].g, leds[0].b);
    String response = "#"+String(colorHex);
    server.send(200, "text/html", response);
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
