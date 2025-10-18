#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <Preferences.h>

// =================== Configuration =====================

constexpr uint8_t  DATA_PIN       = 27;
constexpr uint8_t  NUM_LEDS       = 1;
constexpr char     DEFAULT_SSID[] = "TallinnAtom";
constexpr char     DEFAULT_PASS[] = "12345678";

IPAddress  apIP(192, 168, 4, 1);
DNSServer  dns;
WebServer  server(80);
Preferences prefs;
CRGB leds[NUM_LEDS];



int pinG32 = 32;  // GPIO32 is ADC1_CH4
File myFile;
// UI prefs
int optimalHz = 10;  // Default value if nothing stored

// =================== Preferences: Wi-Fi =================

void saveWifi(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
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

// =================== Preferences: UI Hz =================


void loadOptimalHz(int& hz) {
  prefs.begin("ui", true);
  hz = prefs.getInt("opt_hz", 10);
  prefs.end();
}

// =================== LittleFS / Static Files ============

String contentType(const String& path) {
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".csv")) return "text/html";
  return "text/plain";
}

bool streamFromFS(const String& inPath) {
  String path = inPath;
  // Map folder requests to index.html
  if (path.endsWith("/")) path += "index.html";
  if (!LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  server.streamFile(f, contentType(path));
  f.close();
  return true;
}

void handleFileRequest() {
  const String path = server.uri();
  if (!streamFromFS(path)) {
    server.send(404, "text/plain", "Not found");
  }
}

// =================== Captive Portal =====================

void handleCaptivePortalRedirect() {
  // For unknown paths (or OS captive checks), push client to root
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Redirecting to /");
}

bool isApiPath(const String& u) {
  return u.startsWith("/get")
      || u.startsWith("/set")
      || u.startsWith("/change")     // matches /changewifi
      || u.startsWith("/getoptimal")
      || u.startsWith("/getsensorvalueinbar")
      || u.startsWith("/generate_204")
      || u.startsWith("/hotspot-detect.html")
      || u.startsWith("/connectivitycheck.gstatic.com")
      || u.startsWith("/captive.apple.com");
}

// =================== API: LED Color =====================

void getCurrentLedColorInHEX() {
  char colorHex[8]; // "#RRGGBB" + '\0'
  sprintf(colorHex, "#%02X%02X%02X", leds[0].r, leds[0].g, leds[0].b);
  server.send(200, "text/plain", colorHex);
}

void setCurrentLedColorInHEX() {
  // Accepts "#RRGGBB" or "RRGGBB"
  String incomingHex = server.arg("value");
  if (incomingHex.startsWith("#")) incomingHex = incomingHex.substring(1);

  if (incomingHex.length() != 6) {
    server.send(400, "text/plain", "Invalid HEX format");
    return;
  }

  long number = strtol(incomingHex.c_str(), nullptr, 16);
  uint8_t r = (number >> 16) & 0xFF;
  uint8_t g = (number >>  8) & 0xFF;
  uint8_t b =  number        & 0xFF;

  leds[0] = CRGB(r, g, b);
  FastLED.show();

  char colorHex[8];
  sprintf(colorHex, "#%02X%02X%02X", r, g, b);
  server.send(200, "text/plain", colorHex);
}

// =================== API: Optimal Hz ====================

void getOptimalHzHandler() {
  server.send(200, "text/plain", String(optimalHz));
}

void getSensorValueInBar() {
  int val = analogRead(pinG32);  // 0–4095
  double dividerCoefficient =  1.674;
  double sensorVoltage = val * dividerCoefficient;
  double bar = (((sensorVoltage / 5.0)-0.04)/0.0012858)/100000;


   // Append data
  myFile = LittleFS.open("/data.csv", "a"); // append mode
  if (myFile) {
    myFile.println(bar);
     myFile.print(",");
    myFile.close();
  } else {
    Serial.println("Failed to open file for appending!");
  }
  server.send(200, "text/plain", String(bar));
}

void eraseDataCSV() {
   // Append data
  myFile = LittleFS.open("/data.csv", "w"); 
  if (myFile) {
    myFile.close();
  } else {
    Serial.println("Failed to open file for appending!");
  }
  server.send(200, "text/plain", "erased");
}


// =================== API: Wi-Fi Change ==================

void changeWifi() {
  const String newSsid = server.arg("ssid");
  const String newPass = server.arg("password");

  if (newSsid.isEmpty()) {
    server.send(400, "text/plain", "SSID must not be empty");
    return;
  }
  if (newPass.length() < 8) {
    server.send(400, "text/plain", "Password must be >= 8 chars");
    return;
  }

  saveWifi(newSsid, newPass);
  server.send(200, "text/plain", "Saved, rebooting...");
  delay(500);
  ESP.restart();
}

// =================== Setup Helpers ======================

void setupAccessPoint() {
  WiFi.mode(WIFI_AP);

  String ssid, pass;
  loadWifi(ssid, pass);
  if (ssid.isEmpty()) {
    ssid = DEFAULT_SSID;
    pass = DEFAULT_PASS;
  }

  // IMPORTANT: set AP IP before enabling SoftAP
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid.c_str(), pass.c_str());

  // DNS to catch all hostnames and point them to AP IP
  dns.start(53, "*", apIP);
}

void registerRoutes() {
  // --- Static files from LittleFS ---
  server.on("/", HTTP_GET, handleFileRequest);      // serves /index.html by default
  server.on("/index.html", HTTP_GET, handleFileRequest);
  server.on("/wifi.html",   HTTP_GET, handleFileRequest);

  // Universal static / captive handler for "not found"
  server.onNotFound([]() {
    const String u = server.uri();
    if (isApiPath(u)) {
      // For captive probes or unexpected API-like paths -> redirect to root
      handleCaptivePortalRedirect();
      return;
    }
    // Otherwise try to serve from FS; if missing -> redirect to root
    if (!streamFromFS(u)) handleCaptivePortalRedirect();
  });

  // --- APIs ---
  server.on("/get",         HTTP_GET, getCurrentLedColorInHEX);
  server.on("/set",         HTTP_GET, setCurrentLedColorInHEX);
  server.on("/changewifi",  HTTP_GET, changeWifi);
  server.on("/getoptimal",  HTTP_GET, getOptimalHzHandler);
  server.on("/eraseDataCSV",  HTTP_GET, eraseDataCSV);
  server.on("/getsensorvalueinbar",  HTTP_GET, getSensorValueInBar);

  // --- Captive-portal well-known endpoints ---
  server.on("/generate_204", []() { server.send(204); });                    // Android
  server.on("/hotspot-detect.html", []() { server.send(200, "text/html", "OK"); }); // Apple
  server.on("/connectivitycheck.gstatic.com", handleCaptivePortalRedirect);
  server.on("/captive.apple.com",             handleCaptivePortalRedirect);
}

// =================== Arduino Setup/Loop =================

void setup() {
  Serial.begin(115200);

  // LED init (turn the pixel red by default)
  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
  leds[0] = CRGB::Red;
  FastLED.show();

  // Mount filesystem (format if mount fails)
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  // Load UI prefs
  loadOptimalHz(optimalHz);
  Serial.printf("Optimal Hz loaded: %d\n", optimalHz);

  // Bring up AP + DNS
  setupAccessPoint();

  // HTTP routes
  registerRoutes();

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  // Process DNS requests for captive portal
  dns.processNextRequest();

  // Process HTTP requests
  server.handleClient();
}
