"""
stress.py – ESP32 ülekoormuse ja stabiilsuse test.

- Eskaleeruv koormustest: alustab 10 SET käsuga sekundis,
  suurendab järk-järgult 20, 50, 75, 100, 125, 150 käsuni sekundis.
- Mõõdab vastuse aegu ja kontrollib GET kaudu, kas värvid reaalselt jõuavad LED-ile.
- Märgib kriitilise punkti, kus latentsus > 500ms või hakkavad käsud kaduma.
- Taastumistest: peatame koormuse, ootame ja kontrollime,
  kas seade taastub ja viimane värv püsib.
- Lõpus salvestab raporti stress_report.json.
"""

import os
import time
import json
import random
import requests

# --- Seadistatavad parameetrid ---
BASE_URL = os.getenv("BASE_URL", "http://192.168.4.1")
RATES = [10, 20, 50, 75, 100, 125, 150]  # käske sekundis
STEP_DURATION = float(os.getenv("STEP_DURATION", "5"))  # sek igal tasemel
SAMPLE_EVERY = int(os.getenv("SAMPLE_EVERY", "5"))      # iga N-set kontrollime GET-iga
LATENCY_SLOW_MS = int(os.getenv("LATENCY_SLOW_MS", "500"))
MAX_ERR_RATE = float(os.getenv("MAX_ERR_RATE", "0.05"))  # 5% errorid
MAX_MISS_RATE = float(os.getenv("MAX_MISS_RATE", "0.05"))# 5% missid
RECOVERY_PAUSE = float(os.getenv("RECOVERY_PAUSE", "3.0"))


def _hex6():
    return f"{random.randint(0, 0xFFFFFF):06X}"


def set_color(session, hex_no_hash, timeout=1.5):
    t0 = time.perf_counter()
    r = session.get(f"{BASE_URL}/set", params={"value": hex_no_hash}, timeout=timeout)
    dt = (time.perf_counter() - t0) * 1000.0
    return r.status_code, r.text.strip(), dt


def get_color(session, timeout=1.0):
    t0 = time.perf_counter()
    r = session.get(f"{BASE_URL}/get", timeout=timeout)
    dt = (time.perf_counter() - t0) * 1000.0
    return r.status_code, r.text.strip(), dt


def _norm_hash(hex_no_hash: str) -> str:
    return "#" + hex_no_hash.upper()


def run_step(session, rate_hz, duration_s):
    """Jookseb ühe koormuse astme."""
    interval = 1.0 / rate_hz
    t_end = time.perf_counter() + duration_s
    next_tick = time.perf_counter()
    sent = 0
    errors = 0
    slow = 0
    latencies = []
    checked = 0
    mismatches = 0
    last_set = None

    while time.perf_counter() < t_end:
        now = time.perf_counter()
        if now >= next_tick:
            hex6 = _hex6()
            try:
                code, body, dt_ms = set_color(session, hex6)
            except Exception:
                errors += 1
            else:
                if code != 200:
                    errors += 1
                if dt_ms > LATENCY_SLOW_MS:
                    slow += 1
                latencies.append(dt_ms)
                last_set = hex6
                # perioodiline kontroll
                if sent % SAMPLE_EVERY == 0:
                    try:
                        gcode, gbody, _ = get_color(session)
                        if gcode == 200:
                            checked += 1
                            if gbody.upper() != _norm_hash(hex6).upper():
                                mismatches += 1
                        else:
                            errors += 1
                    except Exception:
                        errors += 1
            sent += 1
            next_tick += interval
        time.sleep(min(0.0015, interval * 0.2))

    avg_lat = sum(latencies) / len(latencies) if latencies else 0.0
    err_rate = errors / sent if sent else 1.0
    miss_rate = mismatches / checked if checked else 0.0
    slow_rate = slow / sent if sent else 0.0

    metrics = {
        "rate_hz": rate_hz,
        "sent": sent,
        "errors": errors,
        "err_rate": round(err_rate, 4),
        "checked": checked,
        "mismatches": mismatches,
        "miss_rate": round(miss_rate, 4),
        "slow": slow,
        "slow_rate": round(slow_rate, 4),
        "avg_latency_ms": round(avg_lat, 2),
        "last_set": last_set,
    }

    unstable = (
        err_rate > MAX_ERR_RATE
        or miss_rate > MAX_MISS_RATE
        or avg_lat > LATENCY_SLOW_MS
    )

    return metrics, unstable


def recovery_check(session, last_color):
    time.sleep(RECOVERY_PAUSE)
    gcode, gbody, _ = get_color(session)
    api_ok = (gcode == 200)
    stays_ok = None
    if last_color and api_ok:
        stays_ok = (gbody.upper() == _norm_hash(last_color).upper())
    new_hex = _hex6()
    try:
        scode, _, _ = set_color(session, new_hex)
        time.sleep(0.05)
        g2code, g2body, _ = get_color(session)
        set_ok = (scode == 200 and g2code == 200 and g2body.upper() == _norm_hash(new_hex).upper())
    except Exception:
        set_ok = False
    return {
        "api_ok": api_ok,
        "stays_ok": stays_ok,
        "set_ok": set_ok,
        "after_pause_s": RECOVERY_PAUSE,
    }


def main():
    session = requests.Session()
    report = {
        "base_url": BASE_URL,
        "rates": RATES,
        "step_duration_s": STEP_DURATION,
        "thresholds": {
            "latency_slow_ms": LATENCY_SLOW_MS,
            "max_err_rate": MAX_ERR_RATE,
            "max_miss_rate": MAX_MISS_RATE,
        },
        "steps": [],
        "critical_point": None,
        "recovery": None,
    }

    print(f"\n=== ESP32 stress & stability test ===")
    print(f"Target: {BASE_URL}")
    print(f"Rates: {RATES} Hz, step {STEP_DURATION}s")
    last_color = None
    for rate in RATES:
        metrics, unstable = run_step(session, rate, STEP_DURATION)
        report["steps"].append(metrics)
        last_color = metrics.get("last_set") or last_color
        print(
            f"[{rate} Hz] sent={metrics['sent']} "
            f"errors={metrics['errors']} ({metrics['err_rate']*100:.1f}%) "
            f"mism={metrics['mismatches']} ({metrics['miss_rate']*100:.1f}%) "
            f"avg={metrics['avg_latency_ms']}ms slow={metrics['slow']}"
        )
        if unstable and report["critical_point"] is None:
            report["critical_point"] = {
                "rate_hz": rate,
                "metrics": metrics,
            }
            print(f"⚠️ Critical point reached at {rate} Hz")
            break

    print("\n=== Recovery check ===")
    rec = recovery_check(session, last_color)
    report["recovery"] = rec
    print(f"API OK: {rec['api_ok']} | stays: {rec['stays_ok']} | set_ok: {rec['set_ok']}")

    with open("stress_report.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("Report saved -> stress_report.json")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped by user.")
