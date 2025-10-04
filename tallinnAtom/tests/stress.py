"""
ESP32 overload test: ramp Hz until failure; stop on first errors.

- Steps through RATES (Hz), sending random RGB via /set and periodically verifying /get.
- Stops immediately when any errors appear on a step (HTTP != 200, exceptions, etc.).
- Tracks max_safe_rate_hz: last step with ZERO errors/mismatches and avg latency <= LATENCY_SLOW_MS.
- Saves stress_report.json with details.
"""

import os
import time
import json
import random
import requests

# -------- Config --------
BASE_URL = os.getenv("BASE_URL", "http://192.168.4.1")
RATES = [10, 20, 50, 75, 100, 125, 150]  # extend if needed
STEP_DURATION = float(os.getenv("STEP_DURATION", "5"))
SAMPLE_EVERY = int(os.getenv("SAMPLE_EVERY", "5"))
LATENCY_SLOW_MS = int(os.getenv("LATENCY_SLOW_MS", "500"))
RECOVERY_PAUSE = float(os.getenv("RECOVERY_PAUSE", "2.0"))

# -------- Helpers --------
def _hex6():
    return f"{random.randint(0, 0xFFFFFF):06X}"

def _hash(hex_no_hash: str) -> str:
    return "#" + hex_no_hash.upper()

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

def run_step(session, rate_hz, duration_s):
    """Run one step; returns (metrics, has_errors)."""
    interval = 1.0 / rate_hz
    t_end = time.perf_counter() + duration_s
    next_tick = time.perf_counter()

    sent = errors = slow = checked = mismatches = 0
    latencies = []
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
                else:
                    if dt_ms > LATENCY_SLOW_MS:
                        slow += 1
                    latencies.append(dt_ms)
                    last_set = hex6

                    # periodic GET verification
                    if sent % SAMPLE_EVERY == 0:
                        try:
                            gcode, gbody, _ = get_color(session)
                            if gcode == 200:
                                checked += 1
                                if gbody.upper() != _hash(hex6).upper():
                                    mismatches += 1
                            else:
                                errors += 1
                        except Exception:
                            errors += 1

            sent += 1
            next_tick += interval

        # tiny sleep to reduce busy-waiting
        time.sleep(min(0.0015, interval * 0.2))

    avg_lat = sum(latencies) / len(latencies) if latencies else 0.0

    metrics = {
        "rate_hz": rate_hz,
        "sent": sent,
        "errors": errors,
        "checked": checked,
        "mismatches": mismatches,
        "slow": slow,
        "avg_latency_ms": round(avg_lat, 2),
        "last_set": last_set,
    }

    has_errors = errors > 0  # stop condition as requested
    return metrics, has_errors

def recovery_check(session, last_color):
    time.sleep(RECOVERY_PAUSE)
    try:
        gcode, gbody, _ = get_color(session)
        api_ok = (gcode == 200)
    except Exception:
        api_ok = False
        gbody = None

    stays_ok = None
    if api_ok and last_color:
        stays_ok = (gbody or "").upper() == _hash(last_color).upper()

    # quick sanity set after recovery pause
    new_hex = _hex6()
    try:
        scode, _, _ = set_color(session, new_hex)
        time.sleep(0.05)
        g2code, g2body, _ = get_color(session)
        set_ok = (scode == 200 and g2code == 200 and (g2body or "").upper() == _hash(new_hex).upper())
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
        "thresholds": {"latency_slow_ms": LATENCY_SLOW_MS},
        "steps": [],
        "critical_point": None,
        "max_safe_rate_hz": None,
        "recovery": None,
        "stop_reason": None,
    }

    print("\n=== ESP32 Overload Test (stop on first errors) ===")
    print(f"Target: {BASE_URL}")
    print(f"Rates: {RATES} Hz, step {STEP_DURATION}s, latency limit {LATENCY_SLOW_MS} ms\n")

    max_safe = None
    last_color = None

    for rate in RATES:
        metrics, has_errors = run_step(session, rate, STEP_DURATION)
        report["steps"].append(metrics)
        last_color = metrics.get("last_set") or last_color

        # safe iff no errors, no mismatches, and latency acceptable
        safe = (metrics["errors"] == 0 and metrics["mismatches"] == 0 and metrics["avg_latency_ms"] <= LATENCY_SLOW_MS)
        if safe:
            max_safe = rate

        print(
            f"[{rate:>3} Hz] sent={metrics['sent']} "
            f"errors={metrics['errors']} "
            f"mismatches={metrics['mismatches']} "
            f"avg={metrics['avg_latency_ms']}ms slow={metrics['slow']}"
        )

        if has_errors:
            report["critical_point"] = {"rate_hz": rate, "metrics": metrics}
            report["stop_reason"] = f"errors_detected_at_{rate}_hz"
            print(f"!! Stopping: errors detected at {rate} Hz")
            break

    report["max_safe_rate_hz"] = max_safe

    print(f"\nMax safe rate (no errors/mismatches, latency OK): {max_safe} Hz")
    print("\n=== Recovery check ===")
    report["recovery"] = recovery_check(session, last_color)
    print(f"API OK: {report['recovery']['api_ok']} | stays: {report['recovery']['stays_ok']} | new SET ok: {report['recovery']['set_ok']}")

    with open("stress_report.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print("\nReport saved -> stress_report.json")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped by user.")
