#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h> 


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

}

void getCurrentLedColorInHEX(){  
    // Get color in HEX
    char colorHex[7];  
    sprintf(colorHex, "%02X%02X%02X", leds[0].r, leds[0].g, leds[0].b);
    String response = "#"+String(colorHex);
    server.send(200, "text/html", response);
}
