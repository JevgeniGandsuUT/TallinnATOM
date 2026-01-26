// =================== Routes (HTTP teekonnad ja API endpointid) =============================



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
  server.on("/sensor", HTTP_GET, getSensor);
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

  server.on("/downloadCaptureCsv", HTTP_GET, []() {
  if (!LittleFS.exists(CAPTURE_CSV_PATH)) {
    server.send(404, "text/plain", "capture_events.csv not found");
    return;
  }

  File f = LittleFS.open(CAPTURE_CSV_PATH, "r");
  if (!f) {
    server.send(500, "text/plain", "open failed");
    return;
  }

  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "attachment; filename=\"capture_events.csv\"");
  server.streamFile(f, "text/csv");
  f.close();
});

server.on("/eraseCaptureCsv", HTTP_GET, []() {
  if (LittleFS.exists(CAPTURE_CSV_PATH)) LittleFS.remove(CAPTURE_CSV_PATH);
  server.send(200, "text/plain", "OK");
});


}