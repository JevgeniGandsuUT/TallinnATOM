#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include <stdlib.h>   
#include <inttypes.h> 
#include "LedController.hpp"

//Led Display Settings

#define DIN 33
#define CLK 19
#define CS  23

LedController<1,1> lc;
const unsigned int NUMBER_OF_DIGITS = 4;  

// =================== Sensor / bar state =================
int pinSensorBar = 32;  // GPIO32 is ADC1_CH4
double lastBar = 0.0;
unsigned long lastSensorReadMs = 0;
const uint32_t SENSOR_INTERVAL_MS = 250;   

unsigned long lastLogMs = 0;
const uint32_t LOG_INTERVAL_MS = 250;     // logimine interval CSV (ms)

// Security configuration
constexpr char     DEFAULT_SSID[] = "TallinnAtom";
constexpr char     DEFAULT_PASS[] = "12345678";
String cryptoKey = "";
String cryptoSalt = "SkyNET1985!";
uint64_t lastAcceptedTs = 0;  // last received timestamp

// =================== Configuration =====================

constexpr uint8_t  DATA_PIN       = 27;
constexpr uint8_t  NUM_LEDS       = 1;

//CSV Button
int csvButton = 22;  
bool logEnabled = false;
bool lastButtonState = HIGH;
unsigned long lastButtonChangeMs = 0;
const uint32_t BUTTON_DEBOUNCE_MS = 50;
int csvLED = 21; 

IPAddress  apIP(192, 168, 4, 1);
DNSServer  dns;
WebServer  server(80);
Preferences prefs;
CRGB leds[NUM_LEDS];

File myFile;
// UI prefs
int optimalHz = 2000;  // Default value if nothing stored
// =================== Preferences: UI Hz =================

void loadOptimalHz(int& hz) {
  prefs.begin("ui", true);
  hz = prefs.getInt("opt_hz", 2000);
  prefs.end();
}


void setup() {
  Serial.begin(9600);
// --- MAX7219 init ---
  lc = LedController<1,1>(DIN, CLK, CS);
  lc.setIntensity(8);     // brightness 0..15
  lc.clearMatrix();       
  displayBarOn7Seg(0.0);  
  // LED-de initsialiseerimine ja esialgne värv (punane)
  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);

  pinMode(csvButton, INPUT_PULLUP); 

  pinMode(csvLED,OUTPUT);
  digitalWrite(csvLED,LOW);

  leds[0] = CRGB::Red;
  FastLED.show();

  // Failisüsteemi käivitamine (LittleFS)
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  // Laeme UI konfiguratsiooni (nt optimalHz)
  loadOptimalHz(optimalHz);
  Serial.printf("Optimal Hz loaded: %d\n", optimalHz);

  // WiFi võrkude skannimine (diagnoos, mitte kohustuslik)
  debugScanWifi();

  // WiFi töörežiimi määramine (AP või AP_STA)
  setupWifiMode();

  // Laeme salajase HMAC võtme (kui eelnevalt salvestatud)
  loadCryptoKey();
  Serial.print("Loaded crypto key: ");
  Serial.println(cryptoKey);

  // Registreerime kõik HTTP teekonnad ja API endpointid
  registerRoutes();

  // Käivitame HTTP serveri
  server.begin();
  Serial.println("HTTP server started");
}



void loop() {
  dns.processNextRequest();   // captive-portali DNS teenus
  server.handleClient();      // HTTP päringud (UI + API)
  handleLogButton(); 
  updateSensorAndDisplay();   // а
}
