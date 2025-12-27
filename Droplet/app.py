import os
import json
import time
import threading
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, render_template, jsonify, Response, request
from influxdb_client import InfluxDBClient
from dotenv import load_dotenv

# ===================== CONFIG =====================
load_dotenv()

INFLUX_URL = os.getenv("INFLUX_URL")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN")
INFLUX_ORG = os.getenv("INFLUX_ORG")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET")

TEAM_FILTER = os.getenv("TEAM_FILTER", "TallinnAtom")
MEASUREMENT = os.getenv("INFLUX_MEASUREMENT", "device_status")
PORT = int(os.getenv("PORT", "5000"))

OFFLINE_SECONDS = int(os.getenv("OFFLINE_SECONDS", "15"))
SSE_INTERVAL_MS = int(os.getenv("SSE_INTERVAL_MS", "2000"))
INFLUX_RANGE = os.getenv("INFLUX_RANGE", "-10m")
INFLUX_TIMEOUT_MS = int(os.getenv("INFLUX_TIMEOUT_MS", "60000"))

CACHE_TTL_SEC = float(os.getenv("CACHE_TTL_SEC", "1.0"))
_latest_cache = {"ts": 0.0, "data": None, "error": None}
_cache_lock = threading.Lock()

BASE_DIR = Path(__file__).resolve().parent
DEVICE_TEMPLATES_DIR = Path(os.getenv(
    "DEVICE_TEMPLATES_DIR",
    str(BASE_DIR / "device_templates")
))

app = Flask(__name__)


# ===================== HELPERS =====================
def influx_client():
    return InfluxDBClient(
        url=INFLUX_URL,
        token=INFLUX_TOKEN,
        org=INFLUX_ORG,
        timeout=INFLUX_TIMEOUT_MS,
        enable_gzip=True,
    )


def utc_now():
    return datetime.now(timezone.utc)


def is_offline(last_time):
    if not last_time:
        return True
    return (utc_now() - last_time.astimezone(timezone.utc)).total_seconds() > OFFLINE_SECONDS


def fmt_float(v):
    try:
        return float(v)
    except Exception:
        return None


def template_path_for(uid: str) -> Path:
    safe = "".join(c for c in uid if c.isalnum() or c in ("_", "-", "."))
    return DEVICE_TEMPLATES_DIR / f"{safe}.html"


def has_init_template(uid: str) -> bool:
    return template_path_for(uid).exists()


# ===================== INFLUX =====================
def _load_latest_devices_from_influx():
    q = f"""
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {INFLUX_RANGE})
  |> filter(fn: (r) => r._measurement == "{MEASUREMENT}")
  |> filter(fn: (r) => r.team == "{TEAM_FILTER}")
  |> last()
"""

    devices = {}

    with influx_client() as c:
        for table in c.query_api().query(q):
            for r in table.records:
                uid = r.values.get("device_id")
                if not uid:
                    continue

                d = devices.setdefault(uid, {
                    "device_id": uid,
                    "valve_state": None,
                    "pressure_now": None,
                    "pressure_prev": None,
                    "_time": None
                })

                ts = r.get_time()
                if ts and (d["_time"] is None or ts > d["_time"]):
                    d["_time"] = ts
                    d["valve_state"] = r.values.get("valve_state")

                if r.get_field() == "pressure_now":
                    d["pressure_now"] = fmt_float(r.get_value())
                elif r.get_field() in ("pressure_prev", "pressure_30ms_ago"):
                    d["pressure_prev"] = fmt_float(r.get_value())

    out = []
    for d in devices.values():
        if d["pressure_now"] is not None and d["pressure_prev"] is not None:
            d["delta"] = d["pressure_now"] - d["pressure_prev"]
        else:
            d["delta"] = None

        t = d.pop("_time")
        d["offline"] = is_offline(t)

        if t:
            t = t.astimezone(timezone.utc)
            d["time_utc"] = t.strftime("%Y-%m-%d %H:%M:%S")
            d["time_ms"] = int(t.timestamp() * 1000)
        else:
            d["time_utc"] = "-"
            d["time_ms"] = None

        d["has_view"] = has_init_template(d["device_id"])
        out.append(d)

    return sorted(out, key=lambda x: x["device_id"])


def load_latest_devices():
    now = time.time()
    with _cache_lock:
        if _latest_cache["data"] and now - _latest_cache["ts"] < CACHE_TTL_SEC:
            return _latest_cache["data"]

    data = _load_latest_devices_from_influx()
    with _cache_lock:
        _latest_cache["data"] = data
        _latest_cache["ts"] = now
        _latest_cache["error"] = None
    return data


# ===================== ROUTES =====================
@app.get("/")
def index():
    return render_template(
        "index.html",
        devices=load_latest_devices(),
        team=TEAM_FILTER,
        bucket=INFLUX_BUCKET,
        sse_ms=SSE_INTERVAL_MS,
        server_time=utc_now().strftime("%Y-%m-%d %H:%M:%S"),
    )


@app.get("/device/<uid>")
def device_view(uid: str):
    p = template_path_for(uid)
    if not p.exists():
        return render_template("no_init.html", uid=uid)

    fragment = p.read_text(encoding="utf-8", errors="replace")
    return render_template("device_wrapper.html", uid=uid, fragment=fragment)


@app.get("/api/devices/latest")
def api_devices_latest():
    return jsonify({
        "server_time_utc": utc_now().strftime("%Y-%m-%d %H:%M:%S"),
        "devices": load_latest_devices()
    })


@app.get("/events/devices")
def events_devices():
    def gen():
        yield "retry: 2000\n\n"
        while True:
            payload = {
                "server_time_utc": utc_now().strftime("%Y-%m-%d %H:%M:%S"),
                "devices": load_latest_devices()
            }
            yield "event: devices\n"
            yield f"data: {json.dumps(payload, ensure_ascii=False)}\n\n"
            time.sleep(max(0.3, SSE_INTERVAL_MS / 1000))
    return Response(gen(), mimetype="text/event-stream")


# ===================== MAIN =====================
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=PORT, threaded=True, use_reloader=False)
