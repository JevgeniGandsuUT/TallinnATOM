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
      || u.startsWith("/sensor")// Rõhuanduri väärtus
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
  if (!ensureAuthorized()) return;  
  server.send(200, "text/plain", String(lastBar, 4)); 
}


void getSensor() {
  server.send(200, "text/plain", String(lastBar, 4)); 
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
