#!/usr/bin/env python3
"""
led_freq_probe.py – LED värvimuutuse sageduse test ESP32 kontrolleril.

Tsükliliselt muudetakse värvi (punane -> roheline -> sinine -> punane).
Iga sagedus (1Hz, 2Hz, 5Hz, 10Hz, 20Hz, 50Hz) jookseb 5 sekundit.
Pärast iga perioodi küsitakse kasutajalt:
  "Kas värvimuutused olid sujuvad ja selgelt eristatavad? (jah/ei)"

Kui vastus on "ei", test peatub ja salvestab viimase "hea"
sageduse kui optimaalne.
"""

import os
import sys
import time
import requests
from itertools import cycle

# Baasaadress ESP32 jaoks – vajadusel saab muuta keskkonnamuutujaga BASE_URL
BASE_URL = os.getenv("BASE_URL", "http://192.168.4.1")

# Testitavad sagedused Hz
FREQS_HZ = [1, 2, 5, 10, 20, 50]

# Iga sageduse kestus sekundites
DURATION_SEC = 5

# Kasutatavad värvid (punane, roheline, sinine)
COLORS = ["FF0000", "00FF00", "0000FF"]

color_cycle = cycle(COLORS)


def set_color(hex_no_hash: str, timeout=2.0):
    """Saadab ESP32-le värvi käsu /set?value=RRGGBB kujul."""
    r = requests.get(f"{BASE_URL}/set", params={"value": hex_no_hash}, timeout=timeout)
    r.raise_for_status()
    return r.text.strip()


def run_period(freq_hz: float, duration_s: float):
    """
    Käivitab värvivahetuse antud sagedusega freq_hz.
    Kestab duration_s sekundit, vahetab värvi punane→roheline→sinine tsüklis.
    """
    interval = 1.0 / freq_hz
    t_end = time.perf_counter() + duration_s
    next_tick = time.perf_counter()
    changes = 0

    while time.perf_counter() < t_end:
        now = time.perf_counter()
        if now >= next_tick:
            hex_color = next(color_cycle)
            try:
                set_color(hex_color)
            except Exception as ex:
                print(f"[HOIATUS] Viga /set käsus @ {freq_hz}Hz: {ex}")
            changes += 1
            next_tick += interval
        # väike paus, et protsessorit mitte üle koormata
        time.sleep(min(0.002, interval * 0.2))
    return changes


def ask_user():
    """
    Küsib kasutajalt, kas värvimuutused olid sujuvad ja eristatavad.
    Positiivsed vastused: jah/да/yes/y/ok
    """
    ans = input("Kas värvimuutused olid sujuvad ja selgelt eristatavad? (jah/ei) ").strip().lower()
    return ans in ("jah", "да", "yes", "y", "ok")


def main():
    print(f"BASE_URL = {BASE_URL}")
    print("Alustame tsüklilise värvimuutuse testiga: punane → roheline → sinine → punane ...")
    print("Iga sagedus kestab 5 s. Pärast iga perioodi vasta 'jah' või 'ei'.")

    last_ok = None
    for f in FREQS_HZ:
        print(f"\n==> Sagedus: {f} Hz — {DURATION_SEC} sekundit")
        changes = run_period(f, DURATION_SEC)
        print(f"Tehtud värvivahetusi: ~{changes} (eesmärk: ~{int(f*DURATION_SEC)})")

        if ask_user():
            last_ok = f
            continue
        else:
            print(f"\n⚠️  Kasutaja märkis, et {f} Hz juures ei ole enam sujuv/eristatav.")
            print(f"Viimane OK sagedus: {last_ok if last_ok is not None else '—'} Hz")

            opt = last_ok if last_ok is not None else 1
            with open("optimal_hz.json", "w", encoding="utf-8") as f:
                f.write('{"optimal_hz": %s}\n' % opt)
            print(f"✅ Salvestatud optimal_hz.json (optimal_hz={opt})")
            print(f'👉 Brauseris saab seada: localStorage.setItem("led_optimal_hz","{opt}");')
            sys.exit(0)

    # Kui kõik sagedused olid OK
    opt = last_ok if last_ok is not None else FREQS_HZ[-1]
    print(f"\n✅ Kõik sagedused OK. Viimane OK: {opt} Hz")
    with open("optimal_hz.json", "w", encoding="utf-8") as f:
        f.write('{"optimal_hz": %s}\n' % opt)
    print(f"Salvestatud optimal_hz.json (optimal_hz={opt})")
    print(f'👉 Brauseris saab seada: localStorage.setItem("led_optimal_hz","{opt}");')


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nKatkestatud.")
        
import requests
BASE_URL = "http://192.168.4.1"
opt = 20  # пример
r = requests.get(f"{BASE_URL}/setoptimal", params={"hz": opt}, timeout=3)
print("Device stored:", r.text)