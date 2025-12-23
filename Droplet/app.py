import os
import json
import time
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, render_template_string, jsonify, Response
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
SSE_INTERVAL_MS = int(os.getenv("SSE_INTERVAL_MS", "2000"))

# where init html from ESP will be stored later
DEVICE_TEMPLATES = Path("./device_templates")
DEVICE_TEMPLATES.mkdir(exist_ok=True)

# ===================== APP =====================
app = Flask(__name__)

# ===================== HELPERS =====================
def influx_client():
    return InfluxDBClient(
        url=INFLUX_URL,
        token=INFLUX_TOKEN,
        org=INFLUX_ORG
    )

def utc_now():
    return datetime.now(timezone.utc)

def is_offline(last_seen_utc: datetime | None):
    if not last_seen_utc:
        return True
    return (utc_now() - last_seen_utc).total_seconds() > OFFLINE_SECONDS

def load_latest_devices():
    """
    Returns list of dicts WITHOUT datetime objects
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
                uid = r.values.get("device_id")
                if not uid:
                    continue

                dev = devices.setdefault(uid, {
                    "device_id": uid,
                    "valve_state": r.values.get("valve_state"),
                    "pressure_now": None,
                    "pressure_prev": None,
                    "delta": None,
                    "_time": None,
                })

                ts = r.get_time()
                if ts and (dev["_time"] is None or ts > dev["_time"]):
                    dev["_time"] = ts
                    dev["valve_state"] = r.values.get("valve_state")

                if r.get_field() == "pressure_now":
                    dev["pressure_now"] = r.get_value()
                elif r.get_field() in ("pressure_prev", "pressure_30ms_ago"):
                    dev["pressure_prev"] = r.get_value()

    out = []
    for d in devices.values():
        if d["pressure_now"] is not None and d["pressure_prev"] is not None:
            d["delta"] = d["pressure_now"] - d["pressure_prev"]

        t = d["_time"]
        d["offline"] = is_offline(t)

        if t:
            t_utc = t.astimezone(timezone.utc)
            d["time_utc"] = t_utc.strftime("%Y-%m-%d %H:%M:%S")
            d["time_ms"] = int(t_utc.timestamp() * 1000)
        else:
            d["time_utc"] = "-"
            d["time_ms"] = None

        d.pop("_time", None)
        out.append(d)

    return sorted(out, key=lambda x: x["device_id"])

# ===================== ROUTES =====================
@app.get("/health")
def health():
    try:
        with influx_client() as c:
            ok = c.ping()
        return jsonify(ok=ok)
    except Exception as e:
        return jsonify(ok=False, error=str(e)), 500


@app.get("/events/devices")
def events_devices():
    def gen():
        yield "retry: 2000\n\n"
        while True:
            try:
                payload = {
                    "server_time_utc": utc_now().strftime("%Y-%m-%d %H:%M:%S"),
                    "devices": load_latest_devices()
                }
                yield "event: devices\n"
                yield f"data: {json.dumps(payload, ensure_ascii=False)}\n\n"
            except Exception as e:
                yield "event: error\n"
                yield f"data: {json.dumps({'error': str(e)})}\n\n"

            time.sleep(SSE_INTERVAL_MS / 1000.0)

    return Response(gen(), mimetype="text/event-stream", headers={
        "Cache-Control": "no-cache",
        "X-Accel-Buffering": "no",
        "Connection": "keep-alive",
    })


@app.get("/")
def index():
    devices = load_latest_devices()

    html = r"""
<!doctype html>
<html lang="et">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>TallinnAtomHub</title>

<!-- Chart.js globally available -->
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chartjs-plugin-annotation"></script>

<style>
body{
  margin:0;
  padding:32px;
  font-family:system-ui,-apple-system,"Segoe UI",sans-serif;
  background:radial-gradient(circle at top,#0f172a 0,#020617 55%,#000 100%);
  color:#e5e7eb;
}
h1{ margin:0 0 6px 0; }
small{ color:#94a3b8; }
table{
  width:100%;
  border-collapse:collapse;
  margin-top:18px;
}
th, td{
  padding:10px;
  border-bottom:1px solid rgba(30,64,175,.25);
}
th{
  text-align:left;
  color:#a5b4fc;
  font-size:11px;
  text-transform:uppercase;
  letter-spacing:.12em;
}
.mono{
  font-family:"JetBrains Mono","Fira Code",Consolas,monospace;
}
a{ color:#8ab4ff; text-decoration:none; }
.pill{
  padding:4px 10px;
  border-radius:999px;
  background:#020617;
  border:1px solid rgba(30,64,175,.35);
}
.offline{ background:rgba(127,29,29,.5); }
</style>
</head>

<body>
<h1>TallinnAtomHub</h1>
<small>
Team: {{ team }} · Bucket: {{ bucket }} ·
<span id="server-time">{{ server_time }}</span> ·
<span id="conn">connecting…</span>
</small>

<table>
<thead>
<tr>
  <th>Device</th>
  <th>Status</th>
  <th>Valve</th>
  <th>Now</th>
  <th>Prev</th>
  <th>Δ</th>
  <th>Last seen</th>
</tr>
</thead>
<tbody id="tbody">
{% for d in devices %}
<tr data-uid="{{ d.device_id }}">
  <td class="mono"><a href="/device/{{ d.device_id }}">{{ d.device_id }}</a></td>
  <td>
    <span class="pill {{ 'offline' if d.offline else '' }}">
      {{ 'Offline' if d.offline else 'Online' }}
    </span>
  </td>
  <td>{{ d.valve_state or '-' }}</td>
  <td class="mono">{{ "%.3f"|format(d.pressure_now) if d.pressure_now is not none else '-' }}</td>
  <td class="mono">{{ "%.3f"|format(d.pressure_prev) if d.pressure_prev is not none else '-' }}</td>
  <td class="mono">{{ "%.3f"|format(d.delta) if d.delta is not none else '-' }}</td>
  <td class="mono">{{ d.time_utc }}</td>
</tr>
{% endfor %}
</tbody>
</table>

<script>
const tbody = document.getElementById("tbody");
const serverTime = document.getElementById("server-time");
const conn = document.getElementById("conn");

function upsert(d){
  let row = tbody.querySelector(`tr[data-uid="${d.device_id}"]`);
  if(!row){
    row = document.createElement("tr");
    row.dataset.uid = d.device_id;
    row.innerHTML = `
      <td class="mono"><a></a></td>
      <td></td><td></td><td class="mono"></td><td class="mono"></td><td class="mono"></td><td class="mono"></td>
    `;
    tbody.appendChild(row);
  }
  row.children[0].querySelector("a").textContent = d.device_id;
  row.children[0].querySelector("a").href = "/device/" + d.device_id;
  row.children[1].innerHTML = `<span class="pill ${d.offline?'offline':''}">${d.offline?'Offline':'Online'}</span>`;
  row.children[2].textContent = d.valve_state || "-";
  row.children[3].textContent = d.pressure_now?.toFixed?.(3) ?? "-";
  row.children[4].textContent = d.pressure_prev?.toFixed?.(3) ?? "-";
  row.children[5].textContent = d.delta?.toFixed?.(3) ?? "-";
  row.children[6].textContent = d.time_utc || "-";
}

const es = new EventSource("/events/devices");
es.onopen = ()=> conn.textContent = "connected";
es.onerror = ()=> conn.textContent = "reconnecting…";
es.addEventListener("devices", e=>{
  const snap = JSON.parse(e.data);
  serverTime.textContent = snap.server_time_utc;
  snap.devices.forEach(upsert);
});
</script>

</body>
</html>
"""
    return render_template_string(
        html,
        devices=devices,
        team=TEAM_FILTER,
        bucket=INFLUX_BUCKET,
        server_time=utc_now().strftime("%Y-%m-%d %H:%M:%S"),
    )


@app.get("/device/<uid>")
def device_view(uid):
    return f"<h2>{uid}</h2><p>device view will be injected here (next step)</p><p><a href='/'>back</a></p>"


# ===================== MAIN =====================
if __name__ == "__main__":
    app.run(host="0.0.0.0", port=PORT)
