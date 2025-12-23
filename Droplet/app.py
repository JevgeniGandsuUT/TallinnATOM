import os
from datetime import timezone
from flask import Flask, render_template_string, jsonify

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

# ===================== APP =====================
app = Flask(__name__)


# ===================== HELPERS =====================
def influx_client():
    if not all([INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET]):
        raise RuntimeError("InfluxDB config missing in .env")
    return InfluxDBClient(
        url=INFLUX_URL,
        token=INFLUX_TOKEN,
        org=INFLUX_ORG,
    )


def load_latest_devices():
    """
    Loads last known state per device from InfluxDB
    """
    query = f"""
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "{MEASUREMENT}")
  |> filter(fn: (r) => r.team == "{TEAM_FILTER}")
  |> last()
"""

    devices = {}

    with influx_client() as client:
        tables = client.query_api().query(query)

        for table in tables:
            for r in table.records:
                device_id = r.values.get("device_id")
                if not device_id:
                    continue

                dev = devices.setdefault(device_id, {
                    "device_id": device_id,
                    "team": r.values.get("team"),
                    "valve_state": r.values.get("valve_state"),
                    "pressure_now": None,
                    "pressure_prev": None,
                    "delta": None,
                    "time": None
                })

                # keep newest timestamp
                ts = r.get_time()
                if ts and (dev["time"] is None or ts > dev["time"]):
                    dev["time"] = ts
                    dev["valve_state"] = r.values.get("valve_state")

                field = r.get_field()
                if field == "pressure_now":
                    dev["pressure_now"] = r.get_value()
                elif field in ("pressure_30ms_ago", "pressure_prev"):
                    dev["pressure_prev"] = r.get_value()

        # compute delta
        for dev in devices.values():
            if dev["pressure_now"] is not None and dev["pressure_prev"] is not None:
                try:
                    dev["delta"] = float(dev["pressure_now"]) - float(dev["pressure_prev"])
                except Exception:
                    dev["delta"] = None

    return sorted(devices.values(), key=lambda x: x["device_id"])


# ===================== ROUTES =====================
@app.get("/health")
def health():
    try:
        with influx_client() as c:
            ok = c.ping()
        return jsonify({
            "ok": ok,
            "bucket": INFLUX_BUCKET,
            "org": INFLUX_ORG,
            "team": TEAM_FILTER,
            "measurement": MEASUREMENT
        })
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500


@app.get("/")
def index():
    devices = load_latest_devices()

    html = """
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>TallinnAtomHub</title>
<style>
body { font-family: Arial, sans-serif; background:#0f1115; color:#e8e8e8; padding:20px; }
h1 { margin-bottom:5px; }
small { color:#aaa; }
table { width:100%; border-collapse: collapse; margin-top:15px; }
th, td { padding:8px 10px; border-bottom:1px solid #333; }
th { text-align:left; background:#161b2a; font-size:12px; text-transform:uppercase; }
td { background:#141823; }
.badge { padding:2px 8px; border-radius:10px; border:1px solid #444; }
.right { text-align:right; }
</style>
</head>
<body>

<h1>TallinnAtomHub</h1>
<small>
Team: {{ team }} · Bucket: {{ bucket }} ·
<a href="/health" style="color:#8ab4ff">health</a>
</small>

<table>
<tr>
  <th>Device</th>
  <th>Valve</th>
  <th class="right">Pressure now (bar)</th>
  <th class="right">Pressure prev (bar)</th>
  <th class="right">Δ (bar)</th>
  <th>Last seen (UTC)</th>
</tr>

{% for d in devices %}
<tr>
  <td><b>{{ d.device_id }}</b></td>
  <td><span class="badge">{{ d.valve_state or "-" }}</span></td>
  <td class="right">{{ "%.3f"|format(d.pressure_now) if d.pressure_now is not none else "-" }}</td>
  <td class="right">{{ "%.3f"|format(d.pressure_prev) if d.pressure_prev is not none else "-" }}</td>
  <td class="right">
    {% if d.delta is not none %}
      {{ "%.3f"|format(d.delta) }}
    {% else %}
      -
    {% endif %}
  </td>
  <td>{{ d.time.astimezone(timezone.utc).strftime("%Y-%m-%d %H:%M:%S") if d.time else "-" }}</td>
</tr>
{% endfor %}
</table>

</body>
</html>
"""
    return render_template_string(
        html,
        devices=devices,
        team=TEAM_FILTER,
        bucket=INFLUX_BUCKET,
        timezone=timezone
    )


# ===================== MAIN =====================
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=PORT)