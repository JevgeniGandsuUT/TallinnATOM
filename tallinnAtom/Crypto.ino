
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

