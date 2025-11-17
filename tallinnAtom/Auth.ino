
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