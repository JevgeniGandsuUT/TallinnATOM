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


void handleSolenoidButton() {
  unsigned long now = millis();
  int state = digitalRead(csvButton);

  // Debounce
  if (state != lastButtonState && (now - lastButtonChangeMs) > BUTTON_DEBOUNCE_MS) {
    lastButtonChangeMs = now;
    lastButtonState = state;

    // реагируем только на нажатие
    if (state == LOW) {

      bool solenoidOn = digitalRead(pinSolenoid) == HIGH;

      // toggle solenoid
      solenoidOn = !solenoidOn;

      digitalWrite(pinSolenoid, solenoidOn ? HIGH : LOW);
      digitalWrite(csvLED, solenoidOn ? HIGH : LOW);

      Serial.print("Solenoid is now: ");
      Serial.println(solenoidOn ? "OPEN" : "CLOSED");
    }
  }
}


double readPressureBar() {
  int val = analogRead(pinSensorBar);

  // Pingejaguri koefitsient (sinu skeemi järgi)
  double dividerCoefficient = 1.48809;
  double sensorVoltage = val * dividerCoefficient;

  // Teisendame pinge bar väärtuseks (MPX5700 valem)
  double bar = ((((sensorVoltage / 5.0) - 0.04) / 0.0012858) / 100000) - 1;
  return bar;
}

void updateSensorAndDisplay() {
  unsigned long now = millis();
  static double prevBar = 0.0;

  if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = now;

    double bar = readPressureBar();
    lastBar = bar;

    // display
    displayBarOn7Seg(lastBar);

    // buffer push
    Sample s;
    s.seq = nextSeq++;
    s.ts_ms = g_timeSynced ? epochMs() : 0;
    s.pressure_30ms_ago = (float)prevBar;
    s.pressure_now = (float)lastBar;
    s.valve_open = (digitalRead(pinSolenoid) == HIGH);

    bufPush(s);
    prevBar = lastBar;
  }
}