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

uint64_t lastAcceptedTs = 0;  // last received timestamp
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
String cryptoKey = "";
String cryptoSalt = "SkyNET1985!";

int pinG32 = 32;  // GPIO32 is ADC1_CH4
File myFile;
// UI prefs
int optimalHz = 2000;  // Default value if nothing stored

// =================== Crypto ============================

String sha256Hex(const String& notSaltedInput) {

  // Lisame soola – kaitseb rainbow-tabelite vastu
  String input = notSaltedInput + cryptoSalt;
  uint8_t hash[32];

  // Initsialiseerime SHA256 konteksti
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);

  // 0 = SHA-256 (mitte SHA-224)
  mbedtls_sha256_starts(&ctx, 0);

  // Lisame räsi allikale sisendi
  mbedtls_sha256_update(&ctx,
                        (const unsigned char*)input.c_str(),
                        input.length());

  // Lõpetame arvutuse ja saame 32-baidise tulemuse
  mbedtls_sha256_finish(&ctx, hash);

  // Puhastame konteksti
  mbedtls_sha256_free(&ctx);

  // Konverteerime 32 baiti → 64-kohaline HEX (inimloetav)
  char buf[65];
  for (int i = 0; i < 32; i++) {
    sprintf(&buf[i * 2], "%02x", hash[i]);
  }
  buf[64] = '\0';

  return String(buf);
}


// ================= HMAC-SHA256 (turvaline allkiri) =========================
// Tagastab: HMAC_SHA256(key, message) hex-kujul
// Kasutus: API päringute ehtsuse kontroll, volitamata ligipääsu vältimine

String hmacSha256Hex(const String& key, const String& msg) {

  // Saame SHA-256 HMAC algoritmi kirjelduse
  const mbedtls_md_info_t* mdInfo =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  if (mdInfo == nullptr) {
    return "";
  }

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);

  // 1 = HMAC režiim (mitte tavaline hash)
  if (mbedtls_md_setup(&ctx, mdInfo, 1) != 0) {
    mbedtls_md_free(&ctx);
    return "";
  }

  // Käivitame HMAC sisemise oleku, kasutades salajast võtit
  // (tekitab pad-i: key XOR 0x36 ja key XOR 0x5C)
  mbedtls_md_hmac_starts(&ctx,
      (const unsigned char*)key.c_str(),
      key.length());

  // Lisame allkirjastatava sõnumi
  mbedtls_md_hmac_update(&ctx,
      (const unsigned char*)msg.c_str(),
      msg.length());

  // Lõplik HMAC 32-baidise bufferina
  unsigned char hmac[32];
  mbedtls_md_hmac_finish(&ctx, hmac);

  // Puhastame konteksti
  mbedtls_md_free(&ctx);

  // Konverteerime tulemuse HEX-kujule (64 sümbolit)
  char buf[65];
  for (int i = 0; i < 32; i++) {
    sprintf(&buf[i * 2], "%02x", hmac[i]);
  }
  buf[64] = '\0';

  return String(buf);
}

// ================= Parameetrite sõnumi kokkupanek =========================
// Tagastab: "name=value&name2=value2" vormis stringi,
//            kuhu EI kaasata 'signature' parameetrit.
// Seda kasutatakse HMAC allkirja kontrollimiseks serveri poolel,
// et taastada täpselt sama sõnum, millele brauser lõi HMAC-i.

String buildSignedMessageFromRequest() {

  // Lõplik parameetrite ühendatud string (GET päringust)
  String paramStr;

  // Läbime kõik päringu parameetrid
  for (int i = 0; i < server.args(); i++) {

    String name = server.argName(i);

    // Jätame 'signature' välja — seda ei allkirjastata
    if (name == "signature") continue;

    // Kui string pole tühi, lisame "&"
    if (paramStr.length() > 0) {
      paramStr += "&";
    }

    // Lisame kujul: nimi=väärtus
    paramStr += name;
    paramStr += "=";
    paramStr += server.arg(i);
  }

  // Tagastame kõik parameetrid ühe stringina
  return paramStr;
}
// =================== Preferences: Wi-Fi =================

void saveWifi(const String& ssid, const String& pass) {
  prefs.begin("sta", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void loadWifi(String& ssid, String& pass) {
  prefs.begin("sta", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
}

// =================== Preferences: Crypto Key ===========

void saveCryptoKey(const String& key) {
  prefs.begin("crypto", false);
  prefs.putString("key", key);
  prefs.end();
}

void loadCryptoKey() {
  prefs.begin("crypto", true);
  cryptoKey = prefs.getString("key", "");
  prefs.end();
}

// =================== Preferences: UI Hz =================

void loadOptimalHz(int& hz) {
  prefs.begin("ui", true);
  hz = prefs.getInt("opt_hz", 2000);
  prefs.end();
}

// =================== Auth Helpers (Autentimine ja turvakontroll) ======================

// Kontrollib, kas seade töötab *ainult AP režiimis*
// AP režiimis HMAC kontrolli ei tehta (võib-olla pole veel võtit)
bool isApOnlyMode() {
  wifi_mode_t mode = WiFi.getMode();
  return (mode == WIFI_AP);
}

// Kõik režiimid, mis EI OLE puhas AP → käsitletakse kliendirežiimina
// (siis peab API päringutel olema HMAC allkiri)
bool isClientMode() {
  return !isApOnlyMode();
}


// Autentimiskontroll API päringutele (STA / AP_STA režiimid)
// Kontrollid: signature, HMAC, timestamp replays
bool ensureAuthorized() {

  // AP-only režiimis turvakontrolli ei rakendata
  // (siin seadistatakse WiFi ja võtmeid)
  if (isApOnlyMode()) {
    return true;
  }

  // Kui salajane võti puudub → API keelatud
  if (cryptoKey.isEmpty()) {
    server.send(503, "text/plain", "Crypto key is not set");
    return false;
  }

  // Loeme signature parameetri
  String sig = server.arg("signature");
  if (sig.isEmpty()) {
    server.send(401, "text/plain", "Missing 'signature' parameter");
    return false;
  }

  // Ootame vormi "<ts>.<mac>" — timestamp + HMAC
  int dotPos = sig.indexOf('.');
  if (dotPos <= 0) {
    server.send(400, "text/plain", "Invalid signature format");
    return false;
  }

  // Eraldame timestamp'i ja MAC-i
  String ts = sig.substring(0, dotPos);
  String mac = sig.substring(dotPos + 1);

  // Koostame päringu parameetrite stringi ilma 'signature'-ta
  String paramStr = buildSignedMessageFromRequest();

  // Koostame täpselt selle sõnumi, millele brauser lõi HMAC-i:
  // "<path>|<paramStr>|<ts>"
  String msg = server.uri();
  msg += "|";
  msg += paramStr;
  msg += "|";
  msg += ts;

  // Genereerime oodatud HMAC-i
  String expected = hmacSha256Hex(cryptoKey, msg);

  // Kui HMAC ei klapi → päring on võltsitud
  if (!mac.equalsIgnoreCase(expected)) {
    server.send(403, "text/plain", "Invalid signature");
    return false;
  }

  // ======================= Anti-replay kaitse ==========================
  // Kaitseb korduspäringute ja sõnumite ümbermängimise vastu.
  // Lubame 5-sekundilise akna, et kompenseerida kella erinevusi.

  uint64_t tsVal = strtoull(ts.c_str(), nullptr, 10);

  // Esimene korrektne päring — lihtsalt salvestame
  if (lastAcceptedTs == 0) {
    lastAcceptedTs = tsVal;
    return true;
  }

  const uint64_t WINDOW_MS = 5000; // 5 sekundi lubatud aken

  // Kui timestamp on VANEM kui viimane lubatud - 5 s → replay
  if (tsVal + WINDOW_MS < lastAcceptedTs) {
    server.send(409, "text/plain", "Replay detected (too old ts)");
    return false;
  }

  // Kui timestamp on natuke väiksem, aga jääb lubatud aknasse → OK
  // Kui uuem — uuendame lastAcceptedTs
  if (tsVal > lastAcceptedTs) {
    lastAcceptedTs = tsVal;
  }

  return true;
}

// =================== LittleFS / Static Files (statiliste failide teenindamine) ============

// Määrab MIME-tüübi vastavalt faililaiendile
// (vajalik, et brauser kuvaks faile õigesti)
String contentType(const String& path) {
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".csv"))  return "text/html";  // CSV kuvatakse HTML-na
  return "text/plain";
}


// Loeb faili LittleFS-ist ja saadab selle kliendile stream'ina
// Tagastab true kui fail leiti ja saadeti
bool streamFromFS(const String& inPath) {

  String path = inPath;

  // Kui küsitakse kataloogi (nt "/") → serveerime index.html
  if (path.endsWith("/"))
    path += "index.html";

  // Kui faili ei ole olemas, katkestame
  if (!LittleFS.exists(path))
    return false;

  // Avame faili lugemiseks
  File f = LittleFS.open(path, "r");
  if (!f)
    return false;

  // Edastame faili brauserile õige MIME-tüübiga
  server.streamFile(f, contentType(path));
  f.close();
  return true;
}


// Töötleb kõik staatiliste failide päringud
// Access rules:
//  • AP režiim – kõik failid lubatud
//  • Client režiim – lubatud AINULT "/" ja "/index.html"
void handleFileRequest() {

  const String path = server.uri();

  // Kliendirežiimis piirame ligipääsu failidele
  if (isClientMode()) {
    // Lubatud ainult: "/" ja "/index.html"
    if (!(path == "/" || path == "/index.html")) {
      server.send(403, "text/plain", "Forbidden in client mode");
      return;
    }
  }

  // Proovime teenindada faili
  if (!streamFromFS(path)) {
    // Faili pole olemas → 404
    server.send(404, "text/plain", "Not found");
  }
}

// =================== Captive Portal (võrgu suunamised) =====================

// Suunab kõik captive-portaali päringud juurlehele ("/")
// Kasutavad Android, iOS, Windows jt, et tuvastada "hotspot login page"
void handleCaptivePortalRedirect() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Redirecting to /");
}


// Kontrollib, kas päringutee on API funktsioon
// Kui on API path → töötleme autoriseerimise ja tagastame JSON/txt
// Kui EI OLE API ja seade arvab, et tegemist on "captive portaliga"
// → suuname kasutaja /-le
bool isApiPath(const String& u) {
  return u.startsWith("/get")                 // LED värvi lugemine
      || u.startsWith("/set")                // LED värvi muutmine
      || u.startsWith("/setKey")             // HMAC salajase võtme seadmine
      || u.startsWith("/change")             // WiFi muutmine (changewifi)
      || u.startsWith("/getoptimal")         // UI sagedus (Hz)
      || u.startsWith("/getsensorvalueinbar")// Rõhuanduri väärtus
      || u.startsWith("/eraseDataCSV")       // Logifaili tühjendamine
      || u.startsWith("/generate_204")       // Android captive portal check
      || u.startsWith("/hotspot-detect.html")// Apple captive portal check
      || u.startsWith("/connectivitycheck.gstatic.com") // Chrome/Android check
      || u.startsWith("/captive.apple.com"); // iOS captive portal suunamine
}

// =================== API: LED Color (LED värvi lugemine ja muutmine) =====================

// Tagastab praeguse LED värvi HEX-kujul: "#RRGGBB"
// Lubatud ainult juhul, kui HMAC-autentimine õnnestub
void getCurrentLedColorInHEX() {
  if (!ensureAuthorized()) return; // turvakontroll

  char colorHex[8]; // "#RRGGBB" + '\0'
  sprintf(colorHex, "#%02X%02X%02X", leds[0].r, leds[0].g, leds[0].b);

  // Saadame värvi tekstina
  server.send(200, "text/plain", colorHex);
}


// Muudab LED värvi kasutaja saadetud väärtuse järgi
// Sisend võib olla: "#RRGGBB" või "RRGGBB"
void setCurrentLedColorInHEX() {
  if (!ensureAuthorized()) return; // HMAC kontroll

  // Loeme GET-päringu parameetri 'value'
  String incomingHex = server.arg("value");

  // Kui kasutaja lisas "#", eemaldame selle
  if (incomingHex.startsWith("#"))
    incomingHex = incomingHex.substring(1);

  // HEX peab olema täpselt 6 sümbolit (RRGGBB)
  if (incomingHex.length() != 6) {
    server.send(400, "text/plain", "Invalid HEX format");
    return;
  }

  // Teisendame HEX → arv → RGB komponendid
  long number = strtol(incomingHex.c_str(), nullptr, 16);
  uint8_t r = (number >> 16) & 0xFF;  // punane
  uint8_t g = (number >>  8) & 0xFF;  // roheline
  uint8_t b =  number        & 0xFF;  // sinine

  // Paneme uue värvi LED-ile
  leds[0] = CRGB(r, g, b);
  FastLED.show(); // kuvame muudatuse füüsiliselt

  // Tagastame kinnituseks uue värvi "#RRGGBB" kujul
  char colorHex[8];
  sprintf(colorHex, "#%02X%02X%02X", r, g, b);
  server.send(200, "text/plain", colorHex);
}

// =================== API: Optimal Hz (UI värskendamise sagedus) ====================

// Tagastab brauserile salvestatud "optimalHz" väärtuse.
// Seda kasutab JS, et piirata kui tihti värviväärtust serverisse saadetakse.
void getOptimalHzHandler() {
  if (!ensureAuthorized()) return; // HMAC kontroll
  server.send(200, "text/plain", String(optimalHz));
}

// =================== API: Sensor Value (rõhuanduri lugemine + CSV logimine) ====================

// Loeb rõhuanduri toorväärtuse (0–4095), teisendab selle bar-ideks,
// logib tulemuse CSV faili ja tagastab selle brauserile.
void getSensorValueInBar() {
  if (!ensureAuthorized()) return; // turvakontroll

  int val = analogRead(pinG32);  // ESP32 ADC lugemine (0–4095)

  double dividerCoefficient = 1.48809; // kalibreerimiskoefitsient (pinge jagaja)
  double sensorVoltage = val * dividerCoefficient; // arvutame tegeliku pinge

  // Teisendus valem → surve bar'ides (MPX5700AP füüsiline karakteristika)
  double bar = ((((sensorVoltage / 5.0) - 0.04) / 0.0012858) / 100000) - 1;

  // Logime väärtuse CSV faili (append)
  myFile = LittleFS.open("/data.csv", "a"); // 'a' = lisamisrežiim
  if (myFile) {
    myFile.println(bar); // kirjutame ühe rea
    myFile.print(",");  // lisame eraldaja (soovi korral)
    myFile.close();
  } else {
    Serial.println("Failed to open file for appending!");
  }

  // Tagastame brauserile sama väärtuse
  server.send(200, "text/plain", String(bar));
}

// =================== API: CSV puhastamine ====================

// Tühjendab /data.csv faili (kirjutame selle üle tühja sisuga).
void eraseDataCSV() {
  if (!ensureAuthorized()) return; // kaitse, ainult autenditud päringud

  myFile = LittleFS.open("/data.csv", "w"); // 'w' = overwrite
  if (myFile) {
    myFile.close(); // kohe sulgeme, fail on nüüd tühi
  } else {
    Serial.println("Failed to open file for appending!");
  }

  server.send(200, "text/plain", "erased");
}

// =================== API: Wi-Fi Change (STA võrgu seadistamine) ==================

// Lubab muuta Atomi STA Wi-Fi võrku (SSID + parool).
// Seda saab teha *ainult puhtas AP režiimis*, sest just siis kasutaja ühendub
// Atomi loodud võrku (TallinnAtom) ja seadistab uue koduse Wi-Fi.
// AP režiimis HMACi ei kasutata – võtme pole veel olemas.
void changeWifi() {

  // WiFi muutmine on lubatud ainult siis, kui seade töötab AP-only režiimis.
  // Kui Atom on juba STA või AP_STA režiimis, siis keelame selle – turvalisuse mõttes.
  if (!isApOnlyMode()) {
    server.send(403, "text/plain", "WiFi change allowed only in AP mode");
    return;
  }

  // Loeme päringust uue SSID ja parooli (GET parameetrid)
  const String newSsid = server.arg("ssid");
  const String newPass = server.arg("password");

  // SSID ei tohi olla tühi
  if (newSsid.isEmpty()) {
    server.send(400, "text/plain", "SSID must not be empty");
    return;
  }

  // WPA2 minimaalne pikkus = 8 märki
  if (newPass.length() < 8) {
    server.send(400, "text/plain", "Password must be >= 8 chars");
    return;
  }

  // Salvestame uue Wi-Fi STA profiili ESP32 NVS-i
  saveWifi(newSsid, newPass);

  // Teavitame kasutajat ja taaskäivitame seadme
  server.send(200, "text/plain", "Saved, rebooting...");
  delay(500);
  ESP.restart();
}

// =================== Setup Helpers (WiFi töörežiimi seadistamine) ======================

// Valib, kas Atom töötab:
//  • ainult AP režiimis (kui STA andmeid pole)
//  • AP+STA režiimis (kui STA SSID/parool on salvestatud)
//
// Loogika:
// 1) Kui STA seadistust pole → käivitame ainult AP (konfiguratsioonirežiim).
// 2) Kui STA on olemas → käivitame AP_STA, ühendame koduvõrku + jätame oma AP alles.

void setupWifiMode() {
  String staSsid, staPass;
  loadWifi(staSsid, staPass); // loeme NVS-ist salvestatud STA SSID ja parooli

  // Kui STA andmeid pole või parool liiga lühike → ainult AP režiim
  if (staSsid.isEmpty() || staPass.length() < 8) {
    Serial.println("No STA WiFi stored -> starting AP only");

    // Seadistame WiFi ainult AP režiimi (Atom loob oma võrgu)
    WiFi.mode(WIFI_AP);

    String apSsid = DEFAULT_SSID;
    String apPass = DEFAULT_PASS;

    // AP IP-konfiguratsioon (staatiline IP Atomi AP-võrgus)
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(apSsid.c_str(), apPass.c_str());

    // DNS server – kõigi domeenide suunamine Atomi IP-le (captive portal)
    dns.start(53, "*", apIP);

    Serial.print("AP SSID: ");
    Serial.println(apSsid);
    Serial.print("AP IP: ");
    Serial.println(apIP);

  } else {
    // STA andmed olemas → proovime ühendada kodusesse WiFi võrku
    Serial.print("STA WiFi stored -> connecting to: ");
    Serial.println(staSsid);

    // AP_STA – seade on korraga nii klient (STA) kui AP
    WiFi.mode(WIFI_AP_STA);

    String apSsid = DEFAULT_SSID;
    String apPass = DEFAULT_PASS;

    // Loome jätkuvalt oma AP (nt "TallinnAtom") lokaalsete seadistuste jaoks
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(apSsid.c_str(), apPass.c_str());
    dns.start(53, "*", apIP);

    // Ühendume salvestatud STA võrku (nt MAX-AP ehk Raspberry)
    WiFi.begin(staSsid.c_str(), staPass.c_str());

    unsigned long start = millis();
    const unsigned long timeoutMs = 15000;

    // Ootame kuni 15 sekundit ühenduse loomist
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      // STA ühendus õnnestus → seade on nüüd võrgu sees
      Serial.print("STA connected, IP: ");
      Serial.println(WiFi.localIP());
    } else {
      // STA ühendus ebaõnnestus → fallback ainult AP režiimile
      Serial.print("STA connect FAILED, status = ");
      Serial.println(WiFi.status());
      WiFi.disconnect(true);
      WiFi.mode(WIFI_AP);
    }
  }
}

// =================== Routes (HTTP teekonnad ja API endpointid) =============================

void handleNewKey();

void registerRoutes() {
  // --- Staatilised failid LittleFS-ist (UI) ---
  // Põhileht – index.html
  server.on("/", HTTP_GET, handleFileRequest);
  server.on("/index.html", HTTP_GET, handleFileRequest);

  // wifi.html on lubatud AINULT AP režiimis (seadistamise leht).
  // Kui seade töötab kliendirežiimis (STA / AP_STA), siis blokeerime selle.
  server.on("/wifi.html", HTTP_GET, []() {
    if (isClientMode()) {
      server.send(403, "text/plain", "wifi.html not available in client mode");
      return;
    }
    handleFileRequest();
  });

  // Universaalne handler kõikidele "mitte-leitud" teedele
  // Siin otsustame:
  //  • kas tegu on API-ga
  //  • kas vaja captive-portali suunamist
  //  • või lihtsalt keelame ligipääsu
  server.onNotFound([]() {
    const String u = server.uri();

    // Kliendirežiimis:
    // kõik, mis EI OLE API, "/" või "/index.html" → keelatud
    if (isClientMode() && !isApiPath(u) && !(u == "/" || u == "/index.html")) {
      server.send(403, "text/plain", "Forbidden");
      return;
    }

    // Kui tegu on API või captive-portali kontrolliga → suuname /
    if (isApiPath(u)) {
      handleCaptivePortalRedirect();
      return;
    }

    // Proovime serveerida faili LittleFS-ist, kui ei leia → suuname /
    if (!streamFromFS(u))
      handleCaptivePortalRedirect();
  });

  // --- API endpointid (turvatud HMAC + Auth abil, v.a AP-only režiimis) ---

  // LED värvi lugemine ja muutmine
  server.on("/get",              HTTP_GET, getCurrentLedColorInHEX);
  server.on("/set",              HTTP_GET, setCurrentLedColorInHEX);

  // STA WiFi seadistamine (ainult AP režiimis)
  server.on("/changewifi",       HTTP_GET, changeWifi);

  // UI värskendussagedus (optimalHz)
  server.on("/getoptimal",       HTTP_GET, getOptimalHzHandler);

  // Rõhuanduri logifaili puhastamine
  server.on("/eraseDataCSV",     HTTP_GET, eraseDataCSV);

  // Rõhuanduri hetkväärtus bar'ides + logimine CSV-sse
  server.on("/getsensorvalueinbar", HTTP_GET, getSensorValueInBar);

  // --- Captive-portal "tuntud" endpointid (Android, Apple, Chrome jne) ---

  // Android captive portal kontroll (204 tähendab "internet olemas")
  server.on("/generate_204", []() { server.send(204); });

  // Apple seadmete hotspot-check
  server.on("/hotspot-detect.html", []() {
    server.send(200, "text/html", "OK");
  });

  // Chrome/Android Google connectivity check → suuname /
  server.on("/connectivitycheck.gstatic.com", handleCaptivePortalRedirect);

  // iOS captive portal kontroll → samuti suuname /
  server.on("/captive.apple.com",             handleCaptivePortalRedirect);

  // --- HMAC salajase võtme seadmine (ainult puhas AP režiim) ---
  server.on("/setKey", HTTP_GET, handleNewKey);
}


// Seadistab HMAC salajase võtme.
// Lubatud ainult siis, kui seade töötab puhtas AP režiimis (tehase / setup mode).
void handleNewKey() {
  // Kliendirežiimis võtme muutmine on keelatud – turvarisk
  if (!isApOnlyMode()) {
    server.send(403, "text/plain", "Key can only be set in pure AP mode");
    return;
  }

  // Loeme GET parameetri 'key'
  String key = server.arg("key");
  if (key.isEmpty()) {
    server.send(400, "text/plain", "Missing 'key' parameter");
    return;
  }

  // Salvestame võtme NVS-i ja hoiame ka RAM-is.
  // Seda kasutatakse HMAC allkirjade arvutamiseks.
  saveCryptoKey(key);
  cryptoKey = key;

  server.send(200, "text/plain", "Key saved");
}

// =================== Arduino Setup/Loop =================
// Siin toimub kogu seadme käivitamine, WiFi seadistamine
// ja HTTP serveri töö tsükliline käitlemine.


// --- WiFi võrkude skaneerimine (diagnostika) ---
// Käivitab lühiajalise STA režiimi ja otsib kõik nähtavad WiFi võrgud.
// Kasulik debuggimiseks, et näha:
//   • mis võrgud on lähedal
//   • signaali tugevus (RSSI)
//   • krüptotüüp (WEP/WPA/WPA2 jne)
void debugScanWifi() {
  Serial.println("Scanning WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(1000);

  int n = WiFi.scanNetworks();
  Serial.printf("Found %d networks:\n", n);
  for (int i = 0; i < n; i++) {
    Serial.printf("%2d: %s  RSSI=%d  enc=%d\n",
                  i,
                  WiFi.SSID(i).c_str(),
                  WiFi.RSSI(i),
                  WiFi.encryptionType(i));
  }
  Serial.println("Scan done.");
}


// --- Peamine käivitusrutiin ---
// Setup() käivitatakse täpselt üks kord pärast ESP32 bootimist.
// Siin initsialiseerime kõik komponendid:
//   • Serial logimine
//   • LED driver (FastLED)
//   • Failisüsteem (LittleFS)
//   • WiFi töörežiim (AP / AP_STA)
//   • Krüptovõti (HMAC key)
//   • HTTP server + routing
void setup() {
  Serial.begin(9600);

  // LED-de initsialiseerimine ja esialgne värv (punane)
  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
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


// --- Peatsükliline töö ---
// ESP32 teeb siin kahte põhitoimingut iga tsükliga:
//   1) DNS serveri käitlemine (captive portal redirect)
//   2) HTTP klientide teenindamine
//
// loop() töötab ~1000 korda sekundis, seega kõik jääb sujuvaks.
void loop() {
  dns.processNextRequest();   // captive-portali DNS teenus
  server.handleClient();      // HTTP päringud (UI + API)
}
