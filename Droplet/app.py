import os
import json
import time
from pathlib import Path
from datetime import datetime, timezone

from flask import Flask, render_template_string, jsonify, Response, abort, url_for
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
SSE_INTERVAL_MS = int(os.getenv("SSE_INTERVAL_MS", "2000"))  # server pushes every N ms

# where listener stores init templates + devices.json (same folder as app.py by default)
BASE_DIR = Path(__file__).resolve().parent
TEMPL_DIR = Path(os.getenv("DEVICE_TEMPLATES_DIR", str(BASE_DIR / "device_templates")))
DEVICES_JSON = Path(os.getenv("DEVICES_JSON_PATH", str(BASE_DIR / "devices.json")))

# hard limit to avoid serving insane HTML
MAX_TEMPLATE_BYTES = int(os.getenv("MAX_TEMPLATE_BYTES", "250000"))  # 250KB

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


def fmt_float(v):
    if v is None:
        return None
    try:
        return float(v)
    except Exception:
        return None


def safe_uid(uid: str) -> str:
    # must match listener save_init_html() sanitizing, so we can open the same file
    return "".join(c for c in (uid or "") if c.isalnum() or c in ("-", "_", ".")) or "unknown"


def load_devices_meta():
    if not DEVICES_JSON.exists():
        return {}
    try:
        return json.loads(DEVICES_JSON.read_text(encoding="utf-8"))
    except Exception:
        return {}


def load_device_init_html(uid: str):
    suid = safe_uid(uid)
    path = TEMPL_DIR / f"{suid}.html"
    if not path.exists():
        return None, str(path)

    # limit size
    try:
        size = path.stat().st_size
        if size > MAX_TEMPLATE_BYTES:
            return f"<!-- template too large: {size} bytes -->", str(path)
    except Exception:
        pass

    try:
        html = path.read_text(encoding="utf-8", errors="replace")
        return html, str(path)
    except Exception:
        return None, str(path)


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
                    "_time": None,     # internal datetime
                })

                ts = r.get_time()
                if ts and (dev["_time"] is None or ts > dev["_time"]):
                    dev["_time"] = ts
                    dev["valve_state"] = r.values.get("valve_state")

                if r.get_field() == "pressure_now":
                    dev["pressure_now"] = fmt_float(r.get_value())
                elif r.get_field() in ("pressure_prev", "pressure_30ms_ago"):
                    dev["pressure_prev"] = fmt_float(r.get_value())

    # merge meta from devices.json (init existence)
    meta = load_devices_meta()

    out = []
    for d in devices.values():
        if d["pressure_now"] is not None and d["pressure_prev"] is not None:
            d["delta"] = d["pressure_now"] - d["pressure_prev"]

        t = d.get("_time")
        d["offline"] = is_offline(t)

        if t:
            t_utc = t.astimezone(timezone.utc)
            d["time_utc"] = t_utc.strftime("%Y-%m-%d %H:%M:%S")
            d["time_ms"] = int(t_utc.timestamp() * 1000)
        else:
            d["time_utc"] = "-"
            d["time_ms"] = None

        # attach init flag + link
        m = meta.get(d["device_id"], {}) if isinstance(meta, dict) else {}
        d["has_init"] = bool(m.get("has_init"))
        d["view_url"] = url_for("device_view", uid=d["device_id"])

        d.pop("_time", None)
        out.append(d)

    out.sort(key=lambda x: x["device_id"])

    return out


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
            "measurement": MEASUREMENT,
            "templates_dir": str(TEMPL_DIR),
            "devices_json": str(DEVICES_JSON),
        })
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500


@app.get("/events/devices")
def events_devices():
    """
    Server-Sent Events stream. Browser subscribes and receives JSON snapshots.
    """
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
                err = {"error": str(e)}
                yield "event: error\n"
                yield f"data: {json.dumps(err, ensure_ascii=False)}\n\n"

            time.sleep(max(0.2, SSE_INTERVAL_MS / 1000.0))

    return Response(gen(), mimetype="text/event-stream", headers={
        "Cache-Control": "no-cache",
        "X-Accel-Buffering": "no",  # nginx
        "Connection": "keep-alive",
    })


@app.get("/device/<uid>")
def device_view(uid: str):
    # show init html saved by listener
    html, path = load_device_init_html(uid)

    page = r"""
<!doctype html>
<html lang="et">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Device view · {{ uid }}</title>
<style>
*{ box-sizing:border-box; }
body{
  margin:0;
  padding:24px 14px;
  font-family:system-ui,-apple-system,"Segoe UI",sans-serif;
  background:radial-gradient(circle at top,#0f172a 0,#020617 55%,#000 100%);
  color:#e5e7eb;
  min-height:100vh;
}
.shell{
  max-width:1200px;
  margin:0 auto;
  background:#020617;
  border-radius:26px;
  padding:18px 18px 22px 18px;
  box-shadow:0 0 0 1px rgba(15,23,42,.9),
             0 35px 120px rgba(15,23,42,.95);
}
.top{
  display:flex;
  gap:12px;
  flex-wrap:wrap;
  align-items:baseline;
  justify-content:space-between;
  margin-bottom:14px;
}
.h1{
  font-size:18px;
  font-weight:800;
  letter-spacing:.08em;
  text-transform:uppercase;
  color:#a5b4fc;
}
.meta{
  font-size:12px;
  color:#94a3b8;
  letter-spacing:.12em;
  text-transform:uppercase;
}
a{ color:#8ab4ff; text-decoration:none; }
.note{
  margin-top:10px;
  padding:12px 14px;
  border-radius:16px;
  border:1px solid rgba(30,64,175,.25);
  background:rgba(15,23,42,.55);
  color:#cbd5e1;
  font-size:13px;
}
.frame-wrap{
  margin-top:16px;
  border-radius:18px;
  overflow:hidden;
  border:1px solid rgba(30,64,175,.25);
  background:#000;
}
iframe{
  width:100%;
  height:860px;
  border:0;
  display:block;
  background:#000;
}
</style>
</head>
<body>
  <div class="shell">
    <div class="top">
      <div>
        <div class="h1">Device view · <span style="color:#60a5fa">{{ uid }}</span></div>
        <div class="meta">init template: <span style="color:#cbd5e1">{{ path }}</span></div>
      </div>
      <div class="meta">
        <a href="{{ url_for('index') }}">← back</a>
      </div>
    </div>

    {% if not html %}
      <div class="note">
        No init template yet for this device.<br>
        ESP should publish retained message to: <b>sensors/{{ uid }}/init</b>
      </div>
    {% else %}
      <div class="frame-wrap">
        <iframe sandbox="allow-scripts allow-same-origin" srcdoc="{{ html|e }}"></iframe>
      </div>
    {% endif %}
  </div>
</body>
</html>
"""
    return render_template_string(page, uid=uid, html=html, path=path)


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
  padding:36px 14px;
  font-family:system-ui,-apple-system,"Segoe UI",sans-serif;
  background:radial-gradient(circle at top,#0f172a 0,#020617 55%,#000 100%);
  color:#e5e7eb;
  display:flex;
  justify-content:center;
}

.card{
  width:100%;
  max-width:1280px;
  background:#020617;
  border-radius:26px;
  padding:26px;
  box-shadow:0 0 0 1px rgba(15,23,42,.9),
             0 35px 120px rgba(15,23,42,.95);
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
  display:flex;
  flex-wrap:wrap;
  gap:10px;
  align-items:center;
}

.subtitle a{ color:#8ab4ff; text-decoration:none; }
.muted{ color:#94a3b8; }

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

/* === ФИКС ШИРИНЫ КОЛОНОК === */
col.device { width:20%; }
col.view   { width:8%; }
col.status { width:10%; }
col.valve  { width:12%; }
col.now    { width:10%; }
col.prev   { width:10%; }
col.delta  { width:10%; }
col.time   { width:20%; }

th, td{
  padding:12px 12px;
  border-bottom:1px solid rgba(30,64,175,.25);
  overflow:hidden;
  text-overflow:ellipsis;
  vertical-align:middle;
}

th{
  background:rgba(2,6,23,.95);
  color:#a5b4fc;
  font-size:11px;
  text-transform:uppercase;
  letter-spacing:.14em;
  text-align:left;
}

td{ font-size:13px; }

th:nth-child(2), td:nth-child(2),
th:nth-child(3), td:nth-child(3),
th:nth-child(4), td:nth-child(4),
th:nth-child(5), td:nth-child(5),
th:nth-child(6), td:nth-child(6),
th:nth-child(7), td:nth-child(7){
  text-align:center;
}

.mono{
  font-family:"JetBrains Mono","Fira Code",Consolas,monospace;
  font-variant-numeric:tabular-nums;
}

.pill{
  display:inline-block;
  padding:4px 10px;
  min-width:72px;
  text-align:center;
  border-radius:999px;
  background:rgba(15,23,42,.9);
  border:1px solid rgba(30,64,175,.35);
  box-shadow:0 0 12px rgba(37,99,235,.35);
}

.pill.open{
  border-color:rgba(52,211,153,.4);
  box-shadow:0 0 14px rgba(52,211,153,.35);
}

.pill.offline{
  background:rgba(127,29,29,.4);
  border-color:rgba(248,113,113,.4);
  box-shadow:none;
}

.pill.alarm{
  background:rgba(127,29,29,.9);
  color:#fee2e2;
  border-color:#f87171;
  animation:pulse .8s infinite;
}

@keyframes pulse{
  0%{ transform:scale(1); }
  50%{ transform:scale(1.05); }
  100%{ transform:scale(1); }
}

/* view link */
.view-link{
  display:inline-block;
  padding:6px 10px;
  border-radius:999px;
  border:1px solid rgba(30,64,175,.35);
  background:rgba(15,23,42,.75);
  color:#8ab4ff;
  text-decoration:none;
  font-weight:600;
  letter-spacing:.08em;
  text-transform:uppercase;
  font-size:11px;
}
.view-link.disabled{
  color:#64748b;
  border-color:rgba(30,64,175,.2);
  cursor:not-allowed;
  pointer-events:none;
}
</style>
</head>

<body>
<div class="card">
  <div class="title">TallinnAtomHub</div>
  <div class="subtitle">
    <span>Team: {{ team }}</span>
    <span>· Bucket: {{ bucket }}</span>
    <span>· <a href="/health">health</a></span>
    <span class="muted">· SSE: {{ sse_ms }}ms</span>
    <span class="muted">· server: <span class="mono" id="server-time">{{ server_time }}</span></span>
    <span class="muted" id="conn-state">· connecting...</span>
  </div>

  <div class="table-wrap">
    <table>
      <colgroup>
        <col class="device">
        <col class="view">
        <col class="status">
        <col class="valve">
        <col class="now">
        <col class="prev">
        <col class="delta">
        <col class="time">
      </colgroup>

      <thead>
        <tr>
          <th>Device</th>
          <th>View</th>
          <th>Status</th>
          <th>Valve</th>
          <th>Now</th>
          <th>Prev</th>
          <th>Δ</th>
          <th>Last seen</th>
        </tr>
      </thead>

      <tbody id="devices-body">
        {% for d in devices %}
        <tr data-uid="{{ d.device_id }}">
          <td class="mono" data-k="device_id">{{ d.device_id }}</td>

          <td data-k="view">
            {% if d.has_init %}
              <a class="view-link" href="{{ d.view_url }}">view</a>
            {% else %}
              <a class="view-link disabled" href="#">no init</a>
            {% endif %}
          </td>

          <td data-k="status">
            <span class="pill {{ 'offline' if d.offline else 'open' }}">
              {{ 'Offline' if d.offline else 'Online' }}
            </span>
          </td>

          <td data-k="valve">
            <span class="pill mono">{{ d.valve_state or '-' }}</span>
          </td>

          <td data-k="now">
            <span class="pill mono {{ 'alarm' if d.pressure_now and d.pressure_now > 5 else '' }}">
              {{ "%.3f"|format(d.pressure_now) if d.pressure_now is not none else '-' }}
            </span>
          </td>

          <td class="mono" data-k="prev">
            {{ "%.3f"|format(d.pressure_prev) if d.pressure_prev is not none else '-' }}
          </td>

          <td class="mono" data-k="delta">
            {{ "%.3f"|format(d.delta) if d.delta is not none else '-' }}
          </td>

          <td class="mono" data-k="time">{{ d.time_utc }}</td>
        </tr>
        {% endfor %}
      </tbody>
    </table>
  </div>
</div>

<script>
  const tbody = document.getElementById("devices-body");
  const serverTimeEl = document.getElementById("server-time");
  const connStateEl = document.getElementById("conn-state");

  function fmt3(v){
    if (v === null || v === undefined) return "-";
    const n = Number(v);
    if (Number.isNaN(n)) return "-";
    return n.toFixed(3);
  }

  function ensureRow(uid){
    let row = tbody.querySelector(`tr[data-uid="${CSS.escape(uid)}"]`);
    if (row) return row;

    row = document.createElement("tr");
    row.setAttribute("data-uid", uid);
    row.innerHTML = `
      <td class="mono" data-k="device_id"></td>
      <td data-k="view"></td>
      <td data-k="status"></td>
      <td data-k="valve"></td>
      <td data-k="now"></td>
      <td class="mono" data-k="prev"></td>
      <td class="mono" data-k="delta"></td>
      <td class="mono" data-k="time"></td>
    `;
    tbody.appendChild(row);
    return row;
  }

  function updateRow(d){
    const uid = d.device_id;
    const row = ensureRow(uid);

    row.querySelector('[data-k="device_id"]').textContent = uid;

    // View
    const viewCell = row.querySelector('[data-k="view"]');
    if (d.has_init){
      viewCell.innerHTML = `<a class="view-link" href="${d.view_url}">view</a>`;
    } else {
      viewCell.innerHTML = `<a class="view-link disabled" href="#">no init</a>`;
    }

    // Status
    const statusCell = row.querySelector('[data-k="status"]');
    statusCell.innerHTML = `<span class="pill ${d.offline ? "offline" : "open"}">${d.offline ? "Offline" : "Online"}</span>`;

    // Valve
    const valveCell = row.querySelector('[data-k="valve"]');
    const valve = d.valve_state || "-";
    valveCell.innerHTML = `<span class="pill mono">${valve}</span>`;

    // Now (alarm > 5)
    const nowCell = row.querySelector('[data-k="now"]');
    const nowVal = d.pressure_now;
    const alarm = (nowVal !== null && nowVal !== undefined && Number(nowVal) > 5);
    nowCell.innerHTML = `<span class="pill mono ${alarm ? "alarm" : ""}">${fmt3(nowVal)}</span>`;

    // Prev / Delta
    row.querySelector('[data-k="prev"]').textContent  = fmt3(d.pressure_prev);
    row.querySelector('[data-k="delta"]').textContent = fmt3(d.delta);

    // Time
    row.querySelector('[data-k="time"]').textContent = d.time_utc || "-";
  }

  function applySnapshot(snapshot){
    if (serverTimeEl && snapshot.server_time_utc){
      serverTimeEl.textContent = snapshot.server_time_utc;
    }
    const list = snapshot.devices || [];
    list.sort((a,b) => (a.device_id || "").localeCompare(b.device_id || ""));
    for (const d of list) updateRow(d);
  }

  // ===== SSE wiring =====
  const es = new EventSource("/events/devices");

  es.onopen = () => {
    if (connStateEl) connStateEl.textContent = "· connected";
  };

  es.onerror = () => {
    if (connStateEl) connStateEl.textContent = "· reconnecting...";
  };

  es.addEventListener("devices", (evt) => {
    try{
      const snapshot = JSON.parse(evt.data);
      applySnapshot(snapshot);
    }catch(e){}
  });

  es.addEventListener("error", (evt) => {
    try{
      const payload = JSON.parse(evt.data);
      console.warn("SSE error:", payload);
    }catch(e){}
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
        sse_ms=SSE_INTERVAL_MS,
        server_time=utc_now().strftime("%Y-%m-%d %H:%M:%S"),
    )


# ===================== MAIN =====================
if __name__ == "__main__":
    # note: in prod behind systemd/nginx you can keep debug off
    app.run(host="0.0.0.0", port=PORT)
