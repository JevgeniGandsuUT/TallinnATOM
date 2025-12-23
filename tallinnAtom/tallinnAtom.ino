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
#include <PubSubClient.h>
#include <time.h>
#include <sys/time.h>
//Led Display Settings

#define DIN 33
#define CLK 19
#define CS  23

LedController<1,1> lc;
const unsigned int NUMBER_OF_DIGITS = 4;  

// =================== Sensor / bar state =================
int pinSolenoid = 26;
int pinSensorBar = 32;  // GPIO32 is ADC1_CH4
double lastBar = 0.0;
unsigned long lastSensorReadMs = 0;
const uint32_t SENSOR_INTERVAL_MS = 250;   

// =================== Lab3 Buffer (last 100) + ACK ===================
String deviceId = "ESP32_TallinnAtom_01";
String teamId   = "TallinnAtom";
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
String topicStatus; // sensors/<deviceId>/status
String topicInit; // sensors/<deviceId>/init
unsigned long lastPublishMs = 0;
const uint32_t PUBLISH_INTERVAL_MS = 1000; // ms
const char* MQTT_HOST = "10.8.0.1";   // broker (droplet / VPN)
const int   MQTT_PORT = 1883;

static const int BUF_SIZE = 100;

struct Sample {
  uint32_t seq;
  uint64_t ts_ms;
  float pressure_30ms_ago;
  float pressure_now;
  bool valve_open;
};

// ===== NTP time sync (epoch ms) =====
static bool g_timeSynced = false;

void syncTimeNtp() {
  // Важно: вызывать после того, как STA реально подключился к интернету
  // (если ты в AP-only без выхода — NTP не сработает)

  // Можно указать твой TZ, но для epoch это не важно. Всё равно UTC внутри.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("[NTP] syncing");
  time_t now = 0;
  int tries = 0;

  while (tries < 30) { // ~30 секунд
    time(&now);
    if (now > 1700000000) { // грубая проверка "не 1970", 2023+
      g_timeSynced = true;
      Serial.println("\n[NTP] synced OK");
      return;
    }
    Serial.print(".");
    delay(1000);
    tries++;
  }

  Serial.println("\n[NTP] sync FAILED (no internet?)");
  g_timeSynced = false;
}

uint64_t epochMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  // tv_sec = seconds since epoch, tv_usec = microseconds
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000ULL);
}

Sample buf[BUF_SIZE];
int bufHead = 0;
int bufCount = 0;

uint32_t nextSeq = 1;
uint32_t lastAckSeq = 0;

bool bufIsEmpty() { return bufCount == 0; }
bool bufIsFull()  { return bufCount >= BUF_SIZE; }

Sample* bufPeekOldest() {
  if (bufIsEmpty()) return nullptr;
  return &buf[bufHead];
}

void bufPush(const Sample& s) {
  if (bufIsFull()) {
    bufHead = (bufHead + 1) % BUF_SIZE;
    bufCount--;
  }
  int tail = (bufHead + bufCount) % BUF_SIZE;
  buf[tail] = s;
  bufCount++;
}

void bufDropConfirmed(uint32_t ackSeq) {
  while (!bufIsEmpty()) {
    Sample* o = bufPeekOldest();
    if (!o) break;
    if (o->seq <= ackSeq) {
      bufHead = (bufHead + 1) % BUF_SIZE;
      bufCount--;
    } else break;
  }
}


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

  pinMode(pinSolenoid, OUTPUT);
  digitalWrite(pinSolenoid, LOW); // 


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
  if (WiFi.status() == WL_CONNECTED) {
    syncTimeNtp();
  }
  // Laeme salajase HMAC võtme (kui eelnevalt salvestatud)
  loadCryptoKey();
  Serial.print("Loaded crypto key: ");
  Serial.println(cryptoKey);

  // Registreerime kõik HTTP teekonnad ja API endpointid
  registerRoutes();

  // Käivitame HTTP serveri
  server.begin();
  Serial.println("HTTP server started");
  topicStatus = "sensors/" + deviceId + "/status";
  topicInit  = "sensors/" + deviceId + "/init";
  mqtt.setBufferSize(16384);   // если твоя PubSubClient поддерживает
  Serial.printf("[MQTT] buffer size=%d\n", mqtt.getBufferSize());
  ensureMQTT();
}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("[MQTT RX] ");
  Serial.print(topic);
  Serial.print(" -> ");

  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

void ensureMQTT() {
  if (mqtt.connected()) return;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  String clientId = deviceId + "_" + String((uint32_t)ESP.getEfuseMac(), HEX);


  Serial.print("[MQTT] Connecting as ");
  Serial.println(clientId);

  if (mqtt.connect(clientId.c_str())) {
    Serial.println("[MQTT] Connected");
    // ACK пригодится позже, уже можешь подписаться
    mqtt.subscribe(("sensors/" + deviceId + "/ack").c_str());
    // ВАЖНО: init шлём только когда STA подключен
    publishInitHtmlIfSta();
  } else {
    Serial.print("[MQTT] Failed, rc=");
    Serial.println(mqtt.state());
  }
}

bool publishOldestSample() {
  Sample* o = bufPeekOldest();
  if (!o) return true;

  // JSON payload
  String payload = "{";
  payload += "\"device_id\":\"" + deviceId + "\",";
  payload += "\"team\":\"" + teamId + "\",";
  if (g_timeSynced) {
  payload += "\"timestamp_ms\":" + String((unsigned long long)o->ts_ms) + ",";
  } else {
    // если NTP не синхронизирован — пусть listener поставит now_utc()
    payload += "\"timestamp_ms\":0,";
  }

  payload += "\"valve_state\":\"" + String(o->valve_open ? "open" : "closed") + "\",";
  payload += "\"pressure_30ms_ago\":" + String(o->pressure_30ms_ago, 3) + ",";
  payload += "\"pressure_now\":" + String(o->pressure_now, 3);
  payload += "}";

  bool ok = mqtt.publish(topicStatus.c_str(), payload.c_str());
  if (ok) {
    Serial.println("[MQTT] sent sample");
    // временно чистим сразу (ПОКА БЕЗ ACK)
    bufHead = (bufHead + 1) % BUF_SIZE;
    bufCount--;
  } else {
    Serial.println("[MQTT] publish failed");
  }
  return ok;
}
bool publishInitHtmlIfSta() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[INIT] STA not connected -> skip init publish");
    return false;
  }
  if (!mqtt.connected()) {
    Serial.println("[INIT] MQTT not connected -> skip init publish");
    return false;
  }

  // твой файл в LittleFS
  const char* path = "/sensorbar.html";   // или "/init.html"
  String html = readFileToString(path);
  if (html.length() == 0) {
    Serial.printf("[INIT] %s empty or not found\n", path);
    return false;
  }

  // retain=true чтобы сервер получил сразу после подписки
  bool ok = mqtt.publish(topicInit.c_str(), html.c_str(), true);

  Serial.printf("[INIT] publish %s size=%d retain=true => %s\n",
                topicInit.c_str(), (int)html.length(), ok ? "OK" : "FAIL");

  return ok;
}

void loop() {
  dns.processNextRequest();   // captive-portali DNS teenus
  server.handleClient();      // HTTP päringud (UI + API)
  handleSolenoidButton(); 
  updateSensorAndDisplay();   // а

  ensureMQTT();
  mqtt.loop();

  unsigned long now = millis();
  if (mqtt.connected() && bufCount > 0 && now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    publishOldestSample();
  }
}
