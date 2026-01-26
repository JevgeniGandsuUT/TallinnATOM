#define USE_STUBS 0

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
#include "esp_camera.h"

#define DIN 33
#define CLK 19
#define CS  23

static const char* CAPTURE_CSV_PATH = "/capture_events.csv";


static const uint32_t SOL_CLOSE_DELAY_US = 60000000UL;

int pinG25 = 25;
double g25Value = 0;
volatile bool g_armed = true;
uint32_t g_lastEventUs = 0;
static const uint32_t EVENT_DEADTIME_US = 300000;
volatile bool g_printing = false;
uint32_t g_eventId = 0;

// ===== G25 ANALOG (rule: <2V => pressure rising) =====
static const float    G25_VREF = 3.3f;
static const uint16_t G25_ADC_MAX = 4095;
static const float    G25_THR_V = 2.0f;
static const uint16_t G25_THR_ADC = (uint16_t)((G25_THR_V / G25_VREF) * G25_ADC_MAX + 0.5f); // ~2482

// hysteresis
static const uint16_t G25_HYST_ADC = 80; // ~0.064V
static const uint16_t G25_LOW_ADC  = (G25_THR_ADC > G25_HYST_ADC) ? (G25_THR_ADC - G25_HYST_ADC) : 0;
static const uint16_t G25_HIGH_ADC = (G25_THR_ADC + G25_HYST_ADC);

// poll + stable confirm for end-of-rise
static const uint32_t G25_POLL_US   = 500;   // 0.5ms
static const uint32_t G25_STABLE_US = 3000;  // 3ms above => end-of-rise
static const float    G25_FILT_A    = 0.20f;

static float    g25Filt = 0.0f;
static bool     g25Rising = false;
static uint32_t g25AboveSinceUs = 0;
static uint32_t g25LastPollUs = 0;

struct Sample {
  uint32_t seq;
  uint64_t ts_ms;
  float pressure_30ms_ago;
  float pressure_now;
  bool valve_open;
};

LedController<1,1> lc;
const unsigned int NUMBER_OF_DIGITS = 4;

// =================== Sensor / bar state =================
int pinSolenoid = 26;
int pinSensorBar = 32;
double lastBar = 0.0;
unsigned long lastSensorReadMs = 0;
const uint32_t SENSOR_INTERVAL_MS = 100;

// =================== Lab3 Buffer (last 100) + ACK ===================
String deviceId = "ESP32_TallinnAtom_01";
String teamId   = "TallinnAtom";
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
String topicStatus;
String topicInit;
unsigned long lastPublishMs = 0;
const uint32_t PUBLISH_INTERVAL_MS = 1000;
const char* MQTT_HOST = "10.8.0.1";
const int   MQTT_PORT = 1883;

static const int BUF_SIZE = 100;

// ===== NTP time sync (epoch ms) =====
static bool g_timeSynced = false;

void syncTimeNtp() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("[NTP] syncing");
  time_t now = 0;
  int tries = 0;

  while (tries < 30) {
    time(&now);
    if (now > 1700000000) {
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
uint64_t lastAcceptedTs = 0;

// =================== Configuration =====================
constexpr uint8_t  DATA_PIN = 27;
constexpr uint8_t  NUM_LEDS = 1;

//CSV Button
int csvButton = 22;
bool logEnabled = false;
bool lastButtonState = HIGH;
unsigned long lastButtonChangeMs = 0;
const uint32_t BUTTON_DEBOUNCE_MS = 50;
int makePhotoPin = 21;

IPAddress  apIP(192, 168, 4, 1);
DNSServer  dns;
WebServer  server(80);
Preferences prefs;
CRGB leds[NUM_LEDS];

File myFile;
int optimalHz = 2000;

void loadOptimalHz(int& hz) {
  prefs.begin("ui", true);
  hz = prefs.getInt("opt_hz", 2000);
  prefs.end();
}

// ===== IRQ + high-rate sampling (thread) =====
TaskHandle_t g_sampleTaskHandle = nullptr;

static const uint16_t SAMPLE_US = 500; // 0.5ms
static const uint32_t WINDOW_MS =
    (SOL_CLOSE_DELAY_US / 1000UL) + 200;
static const uint16_t MAX_SAMPLES =
    (WINDOW_MS * 1000UL) / SAMPLE_US;

volatile bool g_sampling = false;
volatile bool g_captureDone = false;

volatile uint32_t g_t0Us = 0;    // capture start = SOLENOID OPEN
volatile uint32_t g_tStopUs = 0; // capture stop  = SOLENOID CLOSE
volatile uint16_t g_idx = 0;

uint16_t g_adc[MAX_SAMPLES];
uint32_t g_dtUs[MAX_SAMPLES];

// ===== G25 rise/fall tracking + close schedule =====
volatile bool     g_g25RiseFired = false;
volatile bool     g_g25FallFired = false;
volatile uint32_t g_g25RiseUs = 0;
volatile uint32_t g_g25FallUs = 0;

volatile uint32_t g_stopAtUs = 0;       // "close time" target
volatile bool     g_closeArmed = false; 
volatile uint32_t g_lastDelayUs = 0;

// track solenoid edges
static const int SOL_OPEN_LEVEL  = HIGH;
static const int SOL_CLOSE_LEVEL = LOW;
static int g_lastSolLevel = SOL_CLOSE_LEVEL;

// ===== G25 analog polling =====
void pollG25Analog() {
  uint32_t nowUs = (uint32_t)micros();
  if ((uint32_t)(nowUs - g25LastPollUs) < G25_POLL_US) return;
  g25LastPollUs = nowUs;

  uint16_t raw = (uint16_t)analogRead(pinG25);

  // IIR filter
  if (g25Filt == 0.0f) g25Filt = (float)raw;
  g25Filt = g25Filt * (1.0f - G25_FILT_A) + (float)raw * G25_FILT_A;

  uint16_t v = (uint16_t)(g25Filt + 0.5f);
  g25Value = (double)v * 3.3 / 4095.0;



  // ===== DETECT START OF PRESSURE RISE =====
  if (!g25Rising) {
    if (v < G25_LOW_ADC) {
      g25Rising = true;

      g_g25FallUs = nowUs;
      g_g25FallFired = true;

      if (g_stopAtUs == 0) {
        g_stopAtUs = nowUs + SOL_CLOSE_DELAY_US;
        g_closeArmed = true;

        Serial.printf(
          "[G25 RISE] adc=%u V=%.2f  ==> SCHEDULE CLOSE at %lu (in %.1f ms)\n",
          v, g25Value,
          (unsigned long)g_stopAtUs,
          (double)SOL_CLOSE_DELAY_US / 1000.0
        );
      }
    }
    return;
  }

  // ===== OPTIONAL: END OF RISE (LOG ONLY) =====
  if (v > G25_HIGH_ADC) {
    if (g25AboveSinceUs == 0) {
      g25AboveSinceUs = nowUs;
      Serial.printf("[G25 ABOVE] adc=%u V=%.2f (checking stable)\n", v, g25Value);
    }

    if ((uint32_t)(nowUs - g25AboveSinceUs) >= G25_STABLE_US) {
      g_g25RiseUs = nowUs;
      g_g25RiseFired = true;

      Serial.printf("[G25 END] stable >2V for %lu us (adc=%u V=%.2f)\n",
                    (unsigned long)G25_STABLE_US, v, g25Value);

      g25Rising = false;
      g25AboveSinceUs = 0;
    }
  } else {
    g25AboveSinceUs = 0;
  }
}

void sampleTask(void* pv) {
  (void)pv;
  for (;;) {
    if (!g_sampling) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    uint32_t t0 = g_t0Us;
    uint32_t nextUs = t0;
    uint16_t i = 0;

    while (g_sampling && i < MAX_SAMPLES) {

      // stop when solenoid actually closed (tStop set)
      uint32_t tStop = g_tStopUs;
      if (tStop != 0 && (int32_t)((uint32_t)micros() - tStop) >= 0) {
        break;
      }

      while ((int32_t)((uint32_t)micros() - nextUs) < 0) {
        // busy wait
      }

      uint32_t nowUs = (uint32_t)micros();
      uint16_t raw = (uint16_t)analogRead(pinSensorBar);

      g_adc[i] = raw;
      g_dtUs[i] = nowUs - t0;

      i++;
      g_idx = i;

      nextUs += SAMPLE_US;
    }

    g_sampling = false;
    g_captureDone = true;
  }
}

// ===== Forward declarations =====
void displayBarOn7Seg(double bar);
void debugScanWifi();
void setupWifiMode();
void loadCryptoKey();
void registerRoutes();
void handleSolenoidButton();
void updateSensorAndDisplay();
String readFileToString(const char* path);
void writeEventToCsv(uint32_t eventId, uint16_t n);

// ===== MQTT =====
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("[MQTT RX] ");
  Serial.print(topic);
  Serial.print(" -> ");
  for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
  Serial.println();
}

bool publishInitHtmlIfSta();

void ensureMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  String clientId = deviceId + "_" + String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.print("[MQTT] Connecting as ");
  Serial.println(clientId);

  if (mqtt.connect(clientId.c_str())) {
    Serial.println("[MQTT] Connected");
    mqtt.subscribe(("sensors/" + deviceId + "/ack").c_str());
    publishInitHtmlIfSta();
  } else {
    Serial.print("[MQTT] Failed, rc=");
    Serial.println(mqtt.state());
  }
}

bool publishOldestSample() {
  Sample* o = bufPeekOldest();
  if (!o) return true;

  String payload = "{";
  payload.reserve(256);
  payload += "\"device_id\":\"" + deviceId + "\",";
  payload += "\"team\":\"" + teamId + "\",";
  if (g_timeSynced) payload += "\"timestamp_ms\":" + String((unsigned long long)o->ts_ms) + ",";
  else payload += "\"timestamp_ms\":0,";

  payload += "\"valve_state\":\"" + String(o->valve_open ? "open" : "closed") + "\",";
  payload += "\"pressure_30ms_ago\":" + String(o->pressure_30ms_ago, 3) + ",";
  payload += "\"pressure_now\":" + String(o->pressure_now, 3);
  payload += "}";

  bool ok = mqtt.publish(topicStatus.c_str(), payload.c_str());
  if (ok) {
    Serial.println("[MQTT] sent sample " + payload);
    bufHead = (bufHead + 1) % BUF_SIZE;
    bufCount--;
  } else {
    Serial.println("[MQTT] publish failed");
  }
  return ok;
}

bool publishInitHtmlIfSta() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!mqtt.connected()) return false;

  const char* path = "/sensorbar.html";
  String html = readFileToString(path);
  if (html.length() == 0) return false;

  bool ok = mqtt.publish(topicInit.c_str(), html.c_str(), true);

  Serial.printf("[INIT] publish %s size=%d retain=true => %s\n",
                topicInit.c_str(), (int)html.length(), ok ? "OK" : "FAIL");
  return ok;
}

// ===== CSV meta append (optional) =====
void appendEventMetaToCsv(uint32_t eventId, uint16_t n) {
  File f = LittleFS.open(CAPTURE_CSV_PATH, FILE_APPEND);
  if (!f) return;

  uint32_t durUs = 0;
  if (g_tStopUs >= g_t0Us) durUs = g_tStopUs - g_t0Us;

  f.printf("#meta,event=%lu,n=%u,sol_open_us=%lu,sol_close_us=%lu,sol_dur_us=%lu,t_delay_us=%lu\n",
           (unsigned long)eventId,
           (unsigned)n,
           (unsigned long)g_t0Us,
           (unsigned long)g_tStopUs,
           (unsigned long)durUs,
           (unsigned long)g_lastDelayUs);
  f.close();
}

// ===== Close solenoid at scheduled stopAt =====
void closeSolenoidAtStop() {
  uint32_t stopAt = g_stopAtUs;
  if (stopAt == 0) return;

  if ((int32_t)((uint32_t)micros() - stopAt) >= 0) {

    // CLOSE
    digitalWrite(pinSolenoid, SOL_CLOSE_LEVEL);

    if (g_sampling && g_tStopUs == 0) {
      g_tStopUs = (uint32_t)micros();
    }

    g_stopAtUs = 0;
    g_closeArmed = false;

    leds[0] = CRGB(255, 0, 0);
    FastLED.show();

    Serial.printf("[SOLENOID] CLOSE at %lu us (scheduled)\n", (unsigned long)stopAt);
  }
}

void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetPinAttenuation(pinSensorBar, ADC_11db);

  // ===== G25 NOW ANALOG =====
  analogSetPinAttenuation(pinG25, ADC_11db);
  pinMode(pinG25, INPUT);
  // attachInterrupt(...) 

  xTaskCreatePinnedToCore(sampleTask, "sampleTask", 4096, nullptr, 1, &g_sampleTaskHandle, 1);
  Serial.println("[THREAD] sampleTask started");

  lc = LedController<1,1>(DIN, CLK, CS);
  lc.setIntensity(8);
  lc.clearMatrix();
  displayBarOn7Seg(0.0);

  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);

  pinMode(csvButton, INPUT_PULLUP);

  pinMode(pinSolenoid, OUTPUT);
  digitalWrite(pinSolenoid, SOL_CLOSE_LEVEL); // CLOSED
  g_lastSolLevel = digitalRead(pinSolenoid);  // init tracker

  pinMode(makePhotoPin, OUTPUT);
  digitalWrite(makePhotoPin, LOW);

  leds[0] = CRGB::Red;
  FastLED.show();

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");

  loadOptimalHz(optimalHz);
  Serial.printf("Optimal Hz loaded: %d\n", optimalHz);

  debugScanWifi();
  setupWifiMode();
  if (WiFi.status() == WL_CONNECTED) syncTimeNtp();

  loadCryptoKey();
  Serial.print("Loaded crypto key: ");
  Serial.println(cryptoKey);

  registerRoutes();

  server.begin();
  Serial.println("HTTP server started");

  topicStatus = "sensors/" + deviceId + "/status";
  topicInit  = "sensors/" + deviceId + "/init";
  mqtt.setBufferSize(22000);
  Serial.printf("[MQTT] buffer size=%d\n", mqtt.getBufferSize());
  ensureMQTT();

  Serial.printf("[G25] thr=%.2fV adc=%u low=%u high=%u stable_us=%lu\n",
                (double)G25_THR_V, (unsigned)G25_THR_ADC,
                (unsigned)G25_LOW_ADC, (unsigned)G25_HIGH_ADC,
                (unsigned long)G25_STABLE_US);
}

void loop() {
  dns.processNextRequest();
  server.handleClient();

  handleSolenoidButton();
  updateSensorAndDisplay();

  // ===== START/STOP capture by solenoid edges =====
  int solLevel = digitalRead(pinSolenoid);
  if (solLevel != g_lastSolLevel) {
    uint32_t nowUs = (uint32_t)micros();

    if (solLevel == SOL_OPEN_LEVEL) {
      if (g_armed && !g_sampling && !g_printing) {
        g_armed = false;
        g_lastEventUs = nowUs;
        g_eventId++;

        g_t0Us = nowUs;
        g_tStopUs = 0;
        g_idx = 0;
        g_captureDone = false;
        g_sampling = true;

        g_stopAtUs = 0;
        g_closeArmed = false;
        g_lastDelayUs = 0;

        // reset G25 state per event
        g25Rising = false;
        g25AboveSinceUs = 0;

        Serial.printf("[SOL OPEN] event=%lu t0=%lu us -> CAPTURE START\n",
                      (unsigned long)g_eventId, (unsigned long)g_t0Us);
      } else {
        Serial.println("[SOL OPEN] ignored (not armed / already sampling / printing)");
      }
    }

    if (solLevel == SOL_CLOSE_LEVEL) {
      if (g_sampling) {
        g_tStopUs = nowUs;
        Serial.printf("[SOL CLOSE] event=%lu tStop=%lu us -> CAPTURE STOP\n",
                      (unsigned long)g_eventId, (unsigned long)g_tStopUs);
      }
    }

    g_lastSolLevel = solLevel;
  }

  ensureMQTT();
  mqtt.loop();

  unsigned long now = millis();
  if (mqtt.connected() && bufCount > 0 && now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    int burst = 5;
    while (burst-- > 0 && mqtt.connected() && bufCount > 0) {
      if (!publishOldestSample()) break;
      mqtt.loop();
      delay(2);
    }
  }

  uint32_t nowUs = (uint32_t)micros();
  if (!g_armed && (uint32_t)(nowUs - g_lastEventUs) > EVENT_DEADTIME_US) {
    g_armed = true;
  }

  // ===== NEW: G25 analog logic =====
  pollG25Analog();

  // ===== Optional: compute delay logs (kept compatible) =====
  if (g_g25FallFired) {
    noInterrupts();
    g_g25FallFired = false;
    uint32_t tFall = g_g25FallUs;
    uint32_t tRise = g_g25RiseUs;
    interrupts();

    if (tFall >= tRise) g_lastDelayUs = tFall - tRise;
    else g_lastDelayUs = 0;

    Serial.printf("[G25 EVENT] t_delay=%lu us, stopAt=%lu us (V=%.2f)\n",
                  (unsigned long)g_lastDelayUs,
                  (unsigned long)g_stopAtUs,
                  g25Value);
  }

  closeSolenoidAtStop();

  // ===== After capture: write CSV =====
  if (g_captureDone) {
    g_captureDone = false;

    uint16_t n = g_idx;
    uint32_t eventId = g_eventId;

    if (n == 0) {
      Serial.printf("[CAPTURE] event=%lu empty\n", (unsigned long)eventId);
      return;
    }

    uint16_t mn = 65535, mx = 0;
    for (uint16_t i = 0; i < n; i++) {
      if (g_adc[i] < mn) mn = g_adc[i];
      if (g_adc[i] > mx) mx = g_adc[i];
    }

    float vMin = (mn * 3.3f) / 4095.0f;
    float vMax = (mx * 3.3f) / 4095.0f;

    Serial.printf("[CAPTURE] event=%lu n=%u dt=[%lu..%lu]us V=[%.3f..%.3f]\n",
      (unsigned long)eventId,
      (unsigned)n,
      (unsigned long)g_dtUs[0],
      (unsigned long)g_dtUs[n-1],
      vMin, vMax
    );

    if (n >= 2) {
      uint32_t dtTotal = g_dtUs[n-1] - g_dtUs[0];
      float avgUs = (float)dtTotal / (float)(n - 1);
      Serial.printf("          avg_period=%.1fus target=%u  sol_dur=%.3fms\n",
        avgUs, SAMPLE_US, (double)(g_tStopUs - g_t0Us) / 1000.0);
    }

    g_printing = true;
    writeEventToCsv(eventId, n);
    appendEventMetaToCsv(eventId, n);
    g_printing = false;
  }
}

#if USE_STUBS

#endif
