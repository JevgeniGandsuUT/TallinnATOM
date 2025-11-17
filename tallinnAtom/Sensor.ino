void displayBarOn7Seg(double bar) {
  // --- Piirame väärtuse lubatud vahemikku ---
  if (bar < 0.0) bar = 0.0;
  if (bar > 9.99) bar = 9.99;

  // --- Teisendame bar väärtuse sajandikeks ---
  int v = (int)round(bar * 100.0);

  int d0 = v % 10;         // sajandikud
  int d1 = (v / 10) % 10;  // kümnendikud
  int d2 = (v / 100) % 10; // täisarv

  // --- Kuvamispaigutus MAX7219 jaoks ---
  // DIG0 = ' '      (vasakpoolne tühi koht)
  // DIG1 = d2       (täisarv + koma)
  // DIG2 = d1       (kümnendikud)
  // DIG3 = d0       (sajandikud)

  lc.setChar (0, 0, ' ', false);   // vasak on tühi
  lc.setDigit(0, 1, d2, true);     // täisarv + koma
  lc.setDigit(0, 2, d1, false);    // kümnendikud
  lc.setDigit(0, 3, d0, false);    // sajandikud
}


void handleLogButton() {
  unsigned long now = millis();
  int state = digitalRead(csvButton);

  // --- Nupu detsibees: väldime valeimpulsse ---
  if (state != lastButtonState && (now - lastButtonChangeMs) > BUTTON_DEBOUNCE_MS) {
    lastButtonChangeMs = now;
    lastButtonState = state;

    // --- Reageerime ainult nupu vajutamisele (HIGH → LOW) ---
    if (state == LOW) {

      // --- Lülitame logimise sisse/välja ---
      logEnabled = !logEnabled;

      Serial.print("Logging is now: ");
      Serial.println(logEnabled ? "ENABLED" : "DISABLED");

      // --- LED annab märku logimisrežiimist ---
      if (logEnabled) {
        digitalWrite(csvLED, HIGH);   // logimine sees
      } else {
        digitalWrite(csvLED, LOW);    // logimine väljas
      }
    }
  }
}


void updateSensorAndDisplay() {
  unsigned long now = millis();

  // --- 1) Loe andurit kindla intervalliga ---
  if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = now;

    int val = analogRead(pinSensorBar);

    // Pingejaguri koefitsient (sinu skeemi järgi)
    double dividerCoefficient = 1.48809;
    double sensorVoltage = val * dividerCoefficient;

    // Teisendame pinge bar väärtuseks (MPX5700 valem)
    double bar = ((((sensorVoltage / 5.0) - 0.04) / 0.0012858) / 100000) - 1;

    lastBar = bar;

    // --- Kuvame uue väärtuse 7-segmendis ---
    displayBarOn7Seg(lastBar);
  }

  // --- 2) Logime CSV faili ainult siis, kui logimine on sisse lülitatud ---
  if (logEnabled && now - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = now;

    myFile = LittleFS.open("/data.csv", "a");
    if (myFile) {
      myFile.println(lastBar);   // kirjutame väärtuse
      myFile.print(",");         // eraldaja
      myFile.close();
    } else {
      Serial.println("Failed to open file for appending!");
    }
  }
}
