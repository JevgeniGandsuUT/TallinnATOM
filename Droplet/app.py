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

# Optional: when a device hasn't been seen in this many seconds -> show Offline badge
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
    try:
        delta = utc_now() - last_time.astimezone(timezone.utc)
        return delta.total_seconds() > OFFLINE_SECONDS
    except Exception:
        return True


def load_latest_devices():
    """
    Loads last known state per device from InfluxDB.
    NOTE: `last()` returns last point per series (field/tag combo),
    so we merge by device_id and keep the newest timestamp.
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
                    "time": None,
                    "offline": True
                })

                ts = r.get_time()
                if ts and (dev["time"] is None or ts > dev["time"]):
                    dev["time"] = ts
                    dev["valve_state"] = r.values.get("valve_state")

                field = r.get_field()
                if field == "pressure_now":
                    dev["pressure_now"] = r.get_value()
                elif field in ("pressure_30ms_ago", "pressure_prev"):
                    dev["pressure_prev"] = r.get_value()

        for dev in devices.values():
            if dev["pressure_now"] is not None and dev["pressure_prev"] is not None:
                try:
                    dev["delta"] = float(dev["pressure_now"]) - float(dev["pressure_prev"])
                except Exception:
                    dev["delta"] = None

            dev["offline"] = is_offline(dev["time"])

    return sorted(devices.values(), key=lambda x: x["device_id"])


def flux_time_utc_str(dt):
    if not dt:
        return "-"
    try:
        return dt.astimezone(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    except Exception:
        return "-"


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

    html = r"""
<!doctype html>
<html lang="et">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TallinnAtomHub</title>

<style>
  *{ box-sizing:border-box; }
  body{
    margin:0;
    padding:40px 16px;
    font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
    background:radial-gradient(circle at top,#0f172a 0,#020617 55%,#000 100%);
    color:#e5e7eb;
    display:flex;
    flex-direction:column;
    align-items:center;
    gap:18px;
    min-height:100vh;
  }

  .wrap{
    width:100%;
    max-width:1100px;
    display:flex;
    flex-direction:column;
    gap:14px;
  }

  .card{
    position:relative;
    background:#020617;
    border-radius:26px;
    padding:26px 28px;
    box-shadow:
      0 0 0 1px rgba(15,23,42,0.9),
      0 35px 120px rgba(15,23,42,0.95);
    overflow:hidden;
  }
  .card::before{
    content:"";
    position:absolute;
    inset:-2px;
    border-radius:30px;
    background:conic-gradient(
      from 140deg,
      rgba(56,189,248,0.0),
      rgba(56,189,248,0.65),
      rgba(129,140,248,0.8),
      rgba(56,189,248,0.0)
    );
    opacity:0.25;
    z-index:-1;
    filter:blur(12px);
  }

  .title{
    font-size:28px;
    font-weight:750;
    letter-spacing:0.06em;
    text-transform:uppercase;
    margin:0 0 6px 0;
    background:linear-gradient(120deg,#60a5fa,#a855f7,#22d3ee);
    -webkit-background-clip:text;
    color:transparent;
  }
  .subtitle{
    margin:0;
    font-size:12px;
    color:#94a3b8;
    text-transform:uppercase;
    letter-spacing:0.16em;
  }
  .subtitle a{
    color:#8ab4ff;
    text-decoration:none;
    border-bottom:1px dashed rgba(138,180,255,0.35);
  }
  .subtitle a:hover{ opacity:0.9; }

  .table-wrap{
    margin-top:16px;
    overflow:auto;
    border-radius:18px;
    border:1px solid rgba(30,64,175,0.25);
    background:rgba(2,6,23,0.6);
  }

  table{
    width:100%;
    border-collapse:separate;
    border-spacing:0;
    min-width:980px;
  }

  th, td{
    padding:12px 14px;
    border-bottom:1px solid rgba(30,64,175,0.22);
    font-size:13px;
    vertical-align:middle;
    white-space:nowrap;
  }

  th{
    position:sticky;
    top:0;
    z-index:1;
    text-align:left;
    background:linear-gradient(180deg, rgba(15,23,42,0.95), rgba(2,6,23,0.95));
    font-size:11px;
    text-transform:uppercase;
    letter-spacing:0.14em;
    color:#a5b4fc;
  }

  tr:hover td{
    background:rgba(15,23,42,0.55);
  }

  .mono{
    font-family:"JetBrains Mono","Fira Code",Consolas,monospace;
    font-variant-numeric:tabular-nums;
  }

  .pill{
    display:inline-flex;
    align-items:center;
    justify-content:center;
    padding:4px 10px;
    border-radius:999px;
    background:rgba(15,23,42,0.9);
    box-shadow:0 0 14px rgba(37,99,235,0.35);
    border:1px solid rgba(30,64,175,0.35);
    color:#e5e7eb;
    min-width:92px;
  }

  .pill.open{
    box-shadow:0 0 16px rgba(52,211,153,0.35);
    border-color:rgba(52,211,153,0.35);
    color:#bbf7d0;
  }
  .pill.closed{
    box-shadow:0 0 16px rgba(148,163,184,0.25);
    border-color:rgba(148,163,184,0.25);
    color:#e5e7eb;
  }

  .pill.offline{
    box-shadow:none;
    border-color:rgba(248,113,113,0.35);
    color:#fecaca;
    background:rgba(127,29,29,0.35);
  }

  .pill.alarm{
    background:rgba(127,29,29,0.96);
    color:#fee2e2;
    border-color:rgba(248,113,113,0.5);
    box-shadow:0 0 22px rgba(248,113,113,0.75);
    animation:alarmPulse 0.8s ease-in-out infinite;
  }

  @keyframes alarmPulse{
    0%{ transform:scale(1); box-shadow:0 0 16px rgba(248,113,113,0.55); }
    50%{ transform:scale(1.05); box-shadow:0 0 30px rgba(248,113,113,0.95); }
    100%{ transform:scale(1); box-shadow:0 0 16px rgba(248,113,113,0.55); }
  }

  .right{ text-align:right; }

  .btn{
    display:inline-flex;
    align-items:center;
    gap:8px;
    padding:7px 12px;
    border-radius:999px;
    border:1px solid rgba(56,189,248,0.28);
    background:rgba(2,6,23,0.65);
    color:#e5e7eb;
    text-decoration:none;
    font-size:12px;
    letter-spacing:0.08em;
    text-transform:uppercase;
  }
  .btn:hover{
    border-color:rgba(56,189,248,0.55);
    box-shadow:0 0 18px rgba(56,189,248,0.18);
  }

  .btn.secondary{
    border-color:rgba(129,140,248,0.25);
  }
  .btn.secondary:hover{
    border-color:rgba(129,140,248,0.55);
    box-shadow:0 0 18px rgba(129,140,248,0.18);
  }

  @media (max-width:640px){
    body{ padding:24px 8px; }
    .card{ padding:22px 18px; border-radius:22px; }
    .title{ font-size:22px; }
  }
</style>
</head>

<body>
  <div class="wrap">
    <div class="card">
      <div class="title">TallinnAtomHub</div>
      <p class="subtitle">
        Team: {{ team }} · Bucket: {{ bucket }} ·
        <a href="{{ url_for('health') }}">health</a>
      </p>

      <div class="table-wrap">
        <table>
          <tr>
            <th>Device</th>
            <th>Status</th>
            <th>Valve</th>
            <th class="right">Pressure now (bar)</th>
            <th class="right">Pressure prev (bar)</th>
            <th class="right">Δ (bar)</th>
            <th>Last seen (UTC)</th>
            <th>Actions</th>
          </tr>

          {% for d in devices %}
          {% set vs = (d.valve_state or '')|lower %}
          {% set valve_cls = 'open' if ('open' in vs or 'lahti' in vs) else ('closed' if ('closed' in vs or 'kinni' in vs) else 'closed') %}
          {% set alarm = (d.pressure_now is not none and d.pressure_now > 5) %}
          <tr>
            <td class="mono"><b>{{ d.device_id }}</b></td>

            <td>
              {% if d.offline %}
                <span class="pill offline">Offline</span>
              {% else %}
                <span class="pill open">Online</span>
              {% endif %}
            </td>

            <td>
              {% if d.offline %}
                <span class="pill offline">-</span>
              {% else %}
                <span class="pill {{ valve_cls }}">{{ d.valve_state or "-" }}</span>
              {% endif %}
            </td>

            <td class="right mono">
              {% if d.pressure_now is not none %}
                <span class="pill {{ 'alarm' if alarm else '' }}">{{ "%.3f"|format(d.pressure_now) }}</span>
              {% else %}
                <span class="pill">{{ "-" }}</span>
              {% endif %}
            </td>

            <td class="right mono">
              {% if d.pressure_prev is not none %}
                <span class="pill">{{ "%.3f"|format(d.pressure_prev) }}</span>
              {% else %}
                <span class="pill">{{ "-" }}</span>
              {% endif %}
            </td>

            <td class="right mono">
              {% if d.delta is not none %}
                <span class="pill">{{ "%.3f"|format(d.delta) }}</span>
              {% else %}
                <span class="pill">{{ "-" }}</span>
              {% endif %}
            </td>

            <td class="mono">{{ d.time_utc }}</td>

            <td>
              <a class="btn" href="{{ url_for('device_view', uid=d.device_id) }}">Open</a>
              <a class="btn secondary" href="{{ url_for('device_history', uid=d.device_id) }}">History</a>
            </td>
          </tr>
          {% endfor %}
        </table>
      </div>
    </div>
  </div>
</body>
</html>
"""
    # add computed string to avoid Jinja timezone imports inside template
    for d in devices:
        d["time_utc"] = flux_time_utc_str(d.get("time"))

    return render_template_string(
        html,
        devices=devices,
        team=TEAM_FILTER,
        bucket=INFLUX_BUCKET,
    )


@app.get("/device/<uid>")
def device_view(uid: str):
    # Placeholder until init.html-from-ESP pipeline is wired.
    # For now, show a minimal page and link back.
    html = r"""
<!doctype html>
<html lang="et">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{{ uid }} · Device</title>
<style>
  body{ margin:0; padding:30px 16px; font-family:system-ui,-apple-system,"Segoe UI",sans-serif;
        background:radial-gradient(circle at top,#0f172a 0,#020617 55%,#000 100%); color:#e5e7eb; }
  a{ color:#8ab4ff; }
  .card{ max-width:900px; margin:0 auto; background:#020617; border-radius:22px; padding:22px;
         box-shadow:0 0 0 1px rgba(15,23,42,0.9), 0 35px 120px rgba(15,23,42,0.95); }
  .mono{ font-family:"JetBrains Mono","Fira Code",Consolas,monospace; }
</style>
</head>
<body>
  <div class="card">
    <h2 style="margin:0 0 10px 0;">Device: <span class="mono">{{ uid }}</span></h2>
    <p style="margin:0 0 12px 0;color:#94a3b8;">Siia tuleb ESP32 LittleFS init.html renderdus (sensors/{{ uid }}/init).</p>
    <p style="margin:0;"><a href="{{ url_for('index') }}">← back</a></p>
  </div>
</body>
</html>
"""
    return render_template_string(html, uid=uid)


@app.get("/device/<uid>/history")
def device_history(uid: str):
    # Placeholder until history query + Chart.js is wired.
    html = r"""
<!doctype html>
<html lang="et">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{{ uid }} · History</title>
<style>
  body{ margin:0; padding:30px 16px; font-family:system-ui,-apple-system,"Segoe UI",sans-serif;
        background:radial-gradient(circle at top,#0f172a 0,#020617 55%,#000 100%); color:#e5e7eb; }
  a{ color:#8ab4ff; }
  .card{ max-width:900px; margin:0 auto; background:#020617; border-radius:22px; padding:22px;
         box-shadow:0 0 0 1px rgba(15,23,42,0.9), 0 35px 120px rgba(15,23,42,0.95); }
  .mono{ font-family:"JetBrains Mono","Fira Code",Consolas,monospace; }
</style>
</head>
<body>
  <div class="card">
    <h2 style="margin:0 0 10px 0;">History: <span class="mono">{{ uid }}</span></h2>
    <p style="margin:0 0 12px 0;color:#94a3b8;">Siia tuleb 24h pressure graafik + viimased 50 sündmust (InfluxDB päring).</p>
    <p style="margin:0;"><a href="{{ url_for('index') }}">← back</a></p>
  </div>
</body>
</html>
"""
    return render_template_string(html, uid=uid)


# ===================== MAIN =====================
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=PORT)
