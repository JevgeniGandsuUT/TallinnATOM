import os
from datetime import timezone, datetime
from flask import Flask, render_template_string, jsonify, url_for
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
OFFLINE_SECONDS = int(os.getenv("OFFLINE_SECONDS", "120"))

# ===================== APP =====================
app = Flask(__name__)


# ===================== HELPERS =====================
def influx_client():
    if not all([INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET]):
        raise RuntimeError("InfluxDB config missing in .env")
    return InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)


def utc_now():
    return datetime.now(timezone.utc)


def is_offline(last_time):
    if not last_time:
        return True
    return (utc_now() - last_time.astimezone(timezone.utc)).total_seconds() > OFFLINE_SECONDS


def load_latest_devices():
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
                uid = r.values.get("device_id")
                if not uid:
                    continue

                dev = devices.setdefault(uid, {
                    "device_id": uid,
                    "valve_state": r.values.get("valve_state"),
                    "pressure_now": None,
                    "pressure_prev": None,
                    "delta": None,
                    "time": None
                })

                ts = r.get_time()
                if ts and (dev["time"] is None or ts > dev["time"]):
                    dev["time"] = ts
                    dev["valve_state"] = r.values.get("valve_state")

                if r.get_field() == "pressure_now":
                    dev["pressure_now"] = r.get_value()
                elif r.get_field() in ("pressure_prev", "pressure_30ms_ago"):
                    dev["pressure_prev"] = r.get_value()

    for d in devices.values():
        if d["pressure_now"] is not None and d["pressure_prev"] is not None:
            d["delta"] = float(d["pressure_now"]) - float(d["pressure_prev"])
        d["offline"] = is_offline(d["time"])
        d["time_utc"] = d["time"].astimezone(timezone.utc).strftime("%Y-%m-%d %H:%M:%S") if d["time"] else "-"

    return sorted(devices.values(), key=lambda x: x["device_id"])


# ===================== ROUTES =====================
@app.get("/health")
def health():
    with influx_client() as c:
        return jsonify({
            "ok": c.ping(),
            "bucket": INFLUX_BUCKET,
            "team": TEAM_FILTER
        })


@app.get("/")
def index():
    devices = load_latest_devices()

    html = r"""
<!doctype html>
<html lang="et">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TallinnAtomHub</title>

<style>
*{box-sizing:border-box}
body{
  margin:0;
  padding:36px 14px;
  font-family:system-ui,-apple-system,"Segoe UI",sans-serif;
  background:radial-gradient(circle at top,#0f172a 0,#020617 55%,#000 100%);
  color:#e5e7eb;
  display:flex;
  justify-content:center;
}
.card{
  width:100%;
  max-width:1200px;
  background:#020617;
  border-radius:26px;
  padding:26px;
  box-shadow:0 0 0 1px rgba(15,23,42,.9),0 35px 120px rgba(15,23,42,.95);
}
.title{
  font-size:26px;
  font-weight:800;
  letter-spacing:.08em;
  text-transform:uppercase;
  background:linear-gradient(120deg,#60a5fa,#a855f7,#22d3ee);
  -webkit-background-clip:text;
  color:transparent;
}
.subtitle{
  margin-top:6px;
  font-size:12px;
  color:#94a3b8;
  text-transform:uppercase;
  letter-spacing:.16em;
}
.subtitle a{color:#8ab4ff;text-decoration:none}

.table-wrap{
  margin-top:18px;
  overflow:hidden;
  border-radius:18px;
  border:1px solid rgba(30,64,175,.25);
}

table{
  width:100%;
  border-collapse:collapse;
  table-layout:fixed;
}

th,td{
  padding:12px 10px;
  font-size:13px;
  border-bottom:1px solid rgba(30,64,175,.25);
  overflow:hidden;
  text-overflow:ellipsis;
}

th{
  background:rgba(2,6,23,.95);
  color:#a5b4fc;
  font-size:11px;
  text-transform:uppercase;
  letter-spacing:.14em;
}

.mono{font-family:"JetBrains Mono","Fira Code",monospace}

.pill{
  display:inline-block;
  padding:4px 10px;
  border-radius:999px;
  background:rgba(15,23,42,.9);
  border:1px solid rgba(30,64,175,.35);
  box-shadow:0 0 12px rgba(37,99,235,.35);
}

.pill.offline{
  background:rgba(127,29,29,.4);
  border-color:rgba(248,113,113,.4);
  box-shadow:none;
}

.pill.open{
  border-color:rgba(52,211,153,.4);
  box-shadow:0 0 14px rgba(52,211,153,.35);
}

.pill.alarm{
  background:rgba(127,29,29,.9);
  color:#fee2e2;
  border-color:#f87171;
  animation:pulse .8s infinite;
}

@keyframes pulse{
  0%{transform:scale(1)}
  50%{transform:scale(1.05)}
  100%{transform:scale(1)}
}

.actions a{
  color:#8ab4ff;
  font-size:12px;
  text-decoration:none;
}
</style>
</head>

<body>
<div class="card">
  <div class="title">TallinnAtomHub</div>
  <div class="subtitle">
    Team: {{ team }} · Bucket: {{ bucket }} · <a href="/health">health</a>
  </div>

  <div class="table-wrap">
    <table>
      <tr>
        <th>Device</th>
        <th>Status</th>
        <th>Valve</th>
        <th>Now</th>
        <th>Prev</th>
        <th>Δ</th>
        <th>Last seen</th>
      </tr>

      {% for d in devices %}
      <tr>
        <td class="mono">{{ d.device_id }}</td>
        <td>
          <span class="pill {{ 'offline' if d.offline else 'open' }}">
            {{ 'Offline' if d.offline else 'Online' }}
          </span>
        </td>
        <td class="mono">{{ d.valve_state or '-' }}</td>
        <td>
          <span class="pill {{ 'alarm' if d.pressure_now and d.pressure_now > 5 else '' }}">
            {{ "%.3f"|format(d.pressure_now) if d.pressure_now is not none else '-' }}
          </span>
        </td>
        <td>{{ "%.3f"|format(d.pressure_prev) if d.pressure_prev is not none else '-' }}</td>
        <td>{{ "%.3f"|format(d.delta) if d.delta is not none else '-' }}</td>
        <td class="mono">{{ d.time_utc }}</td>
      </tr>
      {% endfor %}
    </table>
  </div>
</div>
</body>
</html>
"""
    return render_template_string(
        html,
        devices=devices,
        team=TEAM_FILTER,
        bucket=INFLUX_BUCKET
    )


# ===================== MAIN =====================
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=PORT)
