/*
 * AtomS3R-CAM Capture-on-request (no GPIO trigger, no storing)
 * Sensor does NOT support PIXFORMAT_JPEG -> use RGB565 + frame2jpg()
 *
 * Endpoints:
 *   GET /         -> HTML page with preview + button
 *   GET /capture  -> returns a fresh snapshot JPEG (always current)
 */

#include <WiFi.h>
#include "esp_camera.h"

// ================= CAMERA PINS =================
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  21
#define SIOD_GPIO_NUM  12
#define SIOC_GPIO_NUM  9
#define Y9_GPIO_NUM    13
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    17
#define Y6_GPIO_NUM    4
#define Y5_GPIO_NUM    48
#define Y4_GPIO_NUM    46
#define Y3_GPIO_NUM    42
#define Y2_GPIO_NUM    3
#define VSYNC_GPIO_NUM 10
#define HREF_GPIO_NUM  14
#define PCLK_GPIO_NUM  40
#define POWER_GPIO_NUM 18   // POWER_N (LOW = ON)

// ================= WIFI =================
const char* ssid     = "Zombiland";
const char* password = "mukrstik";

WiFiServer server(80);

// ================= CAMERA CONFIG =================
static camera_config_t camera_config = {
  .pin_pwdn     = PWDN_GPIO_NUM,
  .pin_reset    = RESET_GPIO_NUM,
  .pin_xclk     = XCLK_GPIO_NUM,
  .pin_sscb_sda = SIOD_GPIO_NUM,
  .pin_sscb_scl = SIOC_GPIO_NUM,

  .pin_d7       = Y9_GPIO_NUM,
  .pin_d6       = Y8_GPIO_NUM,
  .pin_d5       = Y7_GPIO_NUM,
  .pin_d4       = Y6_GPIO_NUM,
  .pin_d3       = Y5_GPIO_NUM,
  .pin_d2       = Y4_GPIO_NUM,
  .pin_d1       = Y3_GPIO_NUM,
  .pin_d0       = Y2_GPIO_NUM,

  .pin_vsync    = VSYNC_GPIO_NUM,
  .pin_href     = HREF_GPIO_NUM,
  .pin_pclk     = PCLK_GPIO_NUM,

  .xclk_freq_hz = 20000000,
  .ledc_timer   = LEDC_TIMER_0,
  .ledc_channel = LEDC_CHANNEL_0,

  // ✅ для GC0308: JPEG нельзя, берём RGB565
  .pixel_format = PIXFORMAT_RGB565,
  .frame_size   = FRAMESIZE_QVGA,   // можно поднять до VGA (см. ниже)
  .jpeg_quality = 12,              // тут не используется при RGB565, оставляем как есть
  .fb_count     = 1,
  .fb_location  = CAMERA_FB_IN_PSRAM,
  .grab_mode    = CAMERA_GRAB_LATEST,
  .sccb_i2c_port = 0,
};

// ================= HTTP helpers =================
static bool readLine(WiFiClient& c, String& line) {
  uint32_t t = millis();
  while (!c.available() && (millis() - t) < 1500) delay(1);
  if (!c.available()) return false;
  line = c.readStringUntil('\n');
  line.trim();
  return true;
}

static void drainHeaders(WiFiClient& c) {
  uint32_t t = millis();
  while (c.connected() && (millis() - t) < 300) {
    while (c.available()) {
      String l = c.readStringUntil('\n');
      if (l == "\r" || l.length() == 0) return;
    }
    delay(1);
  }
}

static String parsePath(const String& reqLine) {
  int sp1 = reqLine.indexOf(' ');
  if (sp1 < 0) return "/";
  int sp2 = reqLine.indexOf(' ', sp1 + 1);
  if (sp2 < 0) return "/";
  String path = reqLine.substring(sp1 + 1, sp2);
  int q = path.indexOf('?');
  if (q >= 0) path = path.substring(0, q);
  return path;
}

static void send404(WiFiClient& c) {
  c.println("HTTP/1.1 404 Not Found");
  c.println("Connection: close");
  c.println();
  c.stop();
}

static void sendIndex(WiFiClient& c) {
  c.println("HTTP/1.1 200 OK");
  c.println("Content-Type: text/html; charset=utf-8");
  c.println("Cache-Control: no-store");
  c.println("Connection: close");
  c.println();

  c.println(
    "<html><body style='background:#111;color:#eee;font-family:Arial'>"
    "<h3>AtomS3R-CAM Capture</h3>"
    "<button onclick='cap()' style='padding:10px 14px;font-size:16px'>Capture</button>"
    "<div style='margin-top:12px'>"
      "<img id='img' src='/capture?ts=0' width='320' style='border:1px solid #333'>"
    "</div>"
    "<script>"
      "function cap(){ document.getElementById('img').src='/capture?ts='+Date.now(); }"
      "setInterval(cap, 2000);"
    "</script>"
    "</body></html>"
  );

  c.stop();
}

static void sendCapture(WiFiClient& c) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("esp_camera_fb_get() FAILED");
    c.println("HTTP/1.1 500 Camera FB get failed");
    c.println("Connection: close");
    c.println();
    c.stop();
    return;
  }

  // ✅ Конвертируем RGB565 -> JPEG на лету (и сразу отправляем)
  uint8_t* jpg = nullptr;
  size_t jpgLen = 0;

  // качество: меньше число = лучше качество, больше размер
  // 10 = заметно лучше, 12 = норм, 8 = ещё лучше/тяжелее
  const int JPG_Q = 10;

  bool ok = frame2jpg(fb, JPG_Q, &jpg, &jpgLen);
  esp_camera_fb_return(fb);

  if (!ok || !jpg || jpgLen == 0) {
    if (jpg) free(jpg);
    Serial.println("frame2jpg() FAILED");
    c.println("HTTP/1.1 500 frame2jpg failed");
    c.println("Connection: close");
    c.println();
    c.stop();
    return;
  }

  c.println("HTTP/1.1 200 OK");
  c.println("Content-Type: image/jpeg");
  c.printf("Content-Length: %u\r\n", (unsigned)jpgLen);
  c.println("Cache-Control: no-store");
  c.println("Connection: close");
  c.println();

  size_t sent = 0;
  while (sent < jpgLen && c.connected()) {
    size_t n = min((size_t)4096, jpgLen - sent);
    sent += c.write(jpg + sent, n);
    delay(0);
  }

  free(jpg);
  c.stop();
}

// ================= optional sensor tune =================
static void tuneSensor() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  // мягкий “апгрейд” картинки (без рискованных сеттеров)
  s->set_brightness(s, 0); // -2..2
  s->set_contrast(s, 1);   // -2..2
  s->set_saturation(s, 0); // -2..2
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(POWER_GPIO_NUM, OUTPUT);
  digitalWrite(POWER_GPIO_NUM, LOW);   // POWER ON
  delay(200);

  esp_err_t err = esp_camera_init(&camera_config);
  Serial.printf("esp_camera_init: 0x%x\n", err);

  if (err == ESP_OK) tuneSensor();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  server.begin();
}

// ================= LOOP =================
void loop() {
  WiFiClient c = server.available();
  if (!c) return;

  String reqLine;
  if (!readLine(c, reqLine)) { c.stop(); return; }
  drainHeaders(c);

  String path = parsePath(reqLine);

  if (path == "/capture") sendCapture(c);
  else if (path == "/") sendIndex(c);
  else send404(c);
}
