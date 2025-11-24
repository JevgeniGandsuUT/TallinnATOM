// =================== Setup Helpers (WiFi töörežiimi seadistamine) ======================

// Valib, kas Atom töötab:
//  • ainult AP režiimis (kui STA andmeid pole)
//  • AP+STA režiimis (kui STA SSID/parool on salvestatud)
//
// Loogika:
// 1) Kui STA seadistust pole → käivitame ainult AP (konfiguratsioonirežiim).
// 2) Kui STA on olemas → käivitame AP_STA, ühendame koduvõrku + jätame oma AP alles.
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

void setupWifiMode() {
  String staSsid, staPass;
  loadWifi(staSsid, staPass); // loeme NVS-ist salvestatud STA SSID ja parooli
  Serial.println(staSsid);
  Serial.println(staPass);
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

