# app.py
import os
import json
import time
import threading
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, render_template_string, jsonify, Response, request
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

# Cache: prevents Influx from being hammered by multiple SSE clients/tabs
CACHE_TTL_SEC = float(os.getenv("CACHE_TTL_SEC", "1.0"))
_latest_cache = {"ts": 0.0, "data": None, "error": None}
_cache_lock = threading.Lock()

# Where listener saves init-html fragments (one file per device)
BASE_DIR = Path(__file__).resolve().parent
TEMPLATES_DIR = Path(os.getenv("DEVICE_TEMPLATES_DIR", str(BASE_DIR / "device_templates")))
DEVICES_JSON_PATH = Path(os.getenv("DEVICES_JSON_PATH", str(BASE_DIR / "devices.json")))  # kept for future use

app = Flask(__name__)

# make sure dir exists
TEMPLATES_DIR.mkdir(parents=True, exist_ok=True)


# ===================== HELPERS =====================
def influx_client():
    if not all([INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET]):
        raise RuntimeError("InfluxDB config missing in .env")
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
    if v is None:
        return None
    try:
        return float(v)
    except Exception:
        return None


def _influx_query(query: str):
    with influx_client() as client:
        return client.query_api().query(query)


def load_device_history(uid: str, hours: int = 24, limit_events: int = 50):
    hours = max(1, min(int(hours), 168))
    limit_events = max(1, min(int(limit_events), 500))

    range_expr = f"-{hours}h"

    # 🔑 разные окна: мелкое для событий, крупное для графика
    WIN_EVENTS = os.getenv("HISTORY_WINDOW_EVERY", "1s")
    WIN_CHART  = "10s"

    # ---------- 1) chart: pressure_now ----------
    q_pressure = f"""
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {range_expr})
  |> filter(fn: (r) => r._measurement == "{MEASUREMENT}")
  |> filter(fn: (r) => r.team == "{TEAM_FILTER}")
  |> filter(fn: (r) => r.device_id == "{uid}")
  |> filter(fn: (r) => r._field == "pressure_now")
  |> group(columns: ["device_id","_field"])
  |> aggregateWindow(every: {WIN_CHART}, fn: last, createEmpty: false)
  |> keep(columns: ["_time","_value"])
  |> sort(columns: ["_time"], desc: false)
"""

    # ---------- 2) valve timeline ----------
    q_valve = f"""
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {range_expr})
  |> filter(fn: (r) => r._measurement == "{MEASUREMENT}")
  |> filter(fn: (r) => r.team == "{TEAM_FILTER}")
  |> filter(fn: (r) => r.device_id == "{uid}")
  |> filter(fn: (r) => r._field == "pressure_now")
  |> group(columns: ["device_id","_field"])
  |> aggregateWindow(every: {WIN_EVENTS}, fn: last, createEmpty: false)
  |> keep(columns: ["_time","valve_state"])
  |> sort(columns: ["_time"], desc: false)
"""

    # ---------- 3) EVENTS = ONLY valve flips ----------
    q_events = f"""
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {range_expr})
  |> filter(fn: (r) => r._measurement == "{MEASUREMENT}")
  |> filter(fn: (r) => r.team == "{TEAM_FILTER}")
  |> filter(fn: (r) => r.device_id == "{uid}")
  |> filter(fn: (r) => r._field == "pressure_now" or r._field == "pressure_prev" or r._field == "pressure_30ms_ago")
  |> group(columns: ["device_id","_field"])
  |> aggregateWindow(every: {WIN_EVENTS}, fn: last, createEmpty: false)
  |> keep(columns: ["_time","_field","_value","valve_state"])
  |> pivot(rowKey: ["_time","valve_state"], columnKey: ["_field"], valueColumn: "_value")
  |> sort(columns: ["_time"], desc: false)
"""

    # ---------- EXEC ----------
    pressure_points = []
    for t in _influx_query(q_pressure):
        for r in t.records:
            ts = r.get_time()
            if ts:
                pressure_points.append({
                    "t": int(ts.timestamp() * 1000),
                    "v": float(r.get_value())
                })

    valve_points = []
    for t in _influx_query(q_valve):
        for r in t.records:
            ts = r.get_time()
            if ts:
                valve_points.append({
                    "t": int(ts.timestamp() * 1000),
                    "state": _norm_valve_state(r.values.get("valve_state"))
                })

    # dedupe valve timeline
    vp, last = [], None
    for p in valve_points:
        if p["state"] != last:
            vp.append(p)
            last = p["state"]
    valve_points = vp

    # ---- ONLY


def _norm_valve_state(st) -> str:
    st = (st or "").lower()
    if st in ("lahti", "open", "opened", "on", "1", "true"):
        return "open"
    if st in ("kinni", "closed", "off", "0", "false"):
        return "closed"
    return st or "?"



def template_path_for(uid: str) -> Path:
    safe = "".join(ch for ch in uid if ch.isalnum() or ch in ("_", "-", "."))
    return TEMPLATES_DIR / f"{safe}.html"


def has_init_template(uid: str) -> bool:
    return template_path_for(uid).exists()


def _load_latest_devices_from_influx():
    """
    FIX:
      valve_state is a TAG => open/closed creates different series.
      last() works per-series, so we must group by device_id+_field to glue.
    """
    query = f"""
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {INFLUX_RANGE})
  |> filter(fn: (r) => r._measurement == "{MEASUREMENT}")
  |> filter(fn: (r) => r.team == "{TEAM_FILTER}")
  |> group(columns: ["device_id","_field"])      // FIX: glue open/closed series
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
                    dev["pressure_now"] = fmt_float(r.get_value())
                elif r.get_field() in ("pressure_prev", "pressure_30ms_ago"):
                    dev["pressure_prev"] = fmt_float(r.get_value())

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

        d["has_view"] = has_init_template(d["device_id"])

        d.pop("_time", None)
        out.append(d)

    return sorted(out, key=lambda x: x["device_id"])


def load_latest_devices():
    now = time.time()

    with _cache_lock:
        if _latest_cache["data"] is not None and (now - _latest_cache["ts"]) < CACHE_TTL_SEC:
            return _latest_cache["data"]

    try:
        data = _load_latest_devices_from_influx()
        with _cache_lock:
            _latest_cache["ts"] = now
            _latest_cache["data"] = data
            _latest_cache["error"] = None
        return data
    except Exception as e:
        with _cache_lock:
            _latest_cache["error"] = str(e)
            if _latest_cache["data"] is not None:
                return _latest_cache["data"]
        raise


# ===================== ROUTES =====================
@app.get("/api/device/<uid>/history")
def api_device_history(uid: str):
    hours = int(request.args.get("hours", "24"))
    limit = int(request.args.get("limit", "50"))
    data = load_device_history(uid, hours=hours, limit_events=limit)
    return jsonify(data)


@app.get("/api/export")
def api_export():
    """
    Download CSV/JSON of time range.
    Query:
      uid=... (optional)
      hours=24 (optional, default 24)
      format=csv|json (default csv)
      limit=5000 (optional)
    """
    uid = request.args.get("uid")
    hours = int(request.args.get("hours", "24"))
    fmt = (request.args.get("format", "csv") or "csv").lower()
    limit = int(request.args.get("limit", "5000"))

    hours = max(1, min(hours, 168))
    limit = max(1, min(limit, 50000))

    range_expr = f"-{hours}h"
    uid_filter = f'|> filter(fn: (r) => r.device_id == "{uid}")' if uid else ""

    q = f"""
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {range_expr})
  |> filter(fn: (r) => r._measurement == "{MEASUREMENT}")
  |> filter(fn: (r) => r.team == "{TEAM_FILTER}")
  {uid_filter}
  |> filter(fn: (r) => r._field == "pressure_now" or r._field == "pressure_prev" or r._field == "pressure_30ms_ago")
  |> group(columns: ["device_id","_field"])     // FIX: glue open/closed series
  |> keep(columns: ["_time","device_id","valve_state","_field","_value"])
  |> pivot(rowKey: ["_time","device_id"], columnKey: ["_field"], valueColumn: "_value") // FIX: no valve_state in rowKey
  |> sort(columns: ["_time"], desc: false)
  |> limit(n: {limit})
"""

    rows = []
    for t in _influx_query(q):
        for r in t.records:
            ts = r.values.get("_time") or r.get_time()
            if not ts:
                continue

            device_id = r.values.get("device_id")
            valve_state = r.values.get("valve_state")

            p_now = fmt_float(r.values.get("pressure_now"))
            p_prev = r.values.get("pressure_prev")
            if p_prev is None:
                p_prev = r.values.get("pressure_30ms_ago")
            p_prev = fmt_float(p_prev)

            delta = (p_now - p_prev) if (p_now is not None and p_prev is not None) else None

            t_utc = ts.astimezone(timezone.utc)
            rows.append({
                "device_id": device_id,
                "timestamp": int(t_utc.timestamp()),      # seconds
                "timestamp_ms": int(t_utc.timestamp() * 1000),
                "valve_state": valve_state,
                "pressure_30ms_ago": p_prev,
                "pressure_now": p_now,
                "pressure_delta": delta
            })

    if fmt == "json":
        return Response(
            json.dumps(rows, ensure_ascii=False),
            mimetype="application/json",
            headers={"Content-Disposition": "attachment; filename=export.json"}
        )

    # CSV default
    import csv
    from io import StringIO

    buf = StringIO()
    w = csv.DictWriter(buf, fieldnames=[
        "device_id", "timestamp", "timestamp_ms", "valve_state",
        "pressure_30ms_ago", "pressure_now", "pressure_delta"
    ])
    w.writeheader()
    for r in rows:
        w.writerow(r)

    return Response(
        buf.getvalue(),
        mimetype="text/csv",
        headers={"Content-Disposition": "attachment; filename=export.csv"}
    )


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
            "range": INFLUX_RANGE,
            "timeout_ms": INFLUX_TIMEOUT_MS,
            "templates_dir": str(TEMPLATES_DIR),
            "sse_interval_ms": SSE_INTERVAL_MS,
            "cache_ttl_sec": CACHE_TTL_SEC,
        })
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)}), 500


@app.get("/api/devices/latest")
def api_devices_latest():
    return jsonify({
        "server_time_utc": utc_now().strftime("%Y-%m-%d %H:%M:%S"),
        "devices": load_latest_devices()
    })


@app.get("/events/devices")
def events_devices():
    """
    Server-Sent Events stream. Pushes device snapshot JSON.
    """
    def gen():
        yield "retry: 2000\n\n"
        while True:
            try:
                with _cache_lock:
                    cache_ts = _latest_cache["ts"]
                    cache_error = _latest_cache["error"]

                payload = {
                    "server_time_utc": utc_now().strftime("%Y-%m-%d %H:%M:%S"),
                    "cache_age_ms": int((time.time() - cache_ts) * 1000) if cache_ts else None,
                    "cache_error": cache_error,
                    "devices": load_latest_devices()
                }

                yield ": keepalive\n\n"
                yield "event: devices\n"
                yield f"data: {json.dumps(payload, ensure_ascii=False)}\n\n"
            except Exception as e:
                err = {"error": str(e)}
                yield "event: error\n"
                yield f"data: {json.dumps(err, ensure_ascii=False)}\n\n"

            time.sleep(max(0.3, SSE_INTERVAL_MS / 1000.0))

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
<html lang="en">
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
  overflow:hidden;
  position:relative;
}

.card::before{
  content:"";
  position:absolute;
  inset:-2px;
  border-radius:30px;
  background:conic-gradient(from 140deg,
    rgba(56,189,248,0.0),
    rgba(56,189,248,0.55),
    rgba(129,140,248,0.7),
    rgba(56,189,248,0.0)
  );
  opacity:.22;
  z-index:-1;
  filter:blur(12px);
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
  margin-top:8px;
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
.subtitle a:hover{ text-decoration:underline; }

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

col.device { width:22%; }
col.status { width:10%; }
col.valve  { width:12%; }
col.now    { width:10%; }
col.prev   { width:10%; }
col.delta  { width:10%; }
col.time   { width:18%; }
col.view   { width:8%; }

th, td{
  padding:12px 12px;
  border-bottom:1px solid rgba(30,64,175,.25);
  overflow:hidden;
  text-overflow:ellipsis;
  vertical-align:middle;
  white-space:nowrap;
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
th:nth-child(6), td:nth-child(6){
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

.view-link{
  display:inline-block;
  padding:6px 10px;
  border-radius:999px;
  text-decoration:none;
  color:#e5e7eb;
  background:rgba(15,23,42,.9);
  border:1px solid rgba(30,64,175,.35);
}
.view-link:hover{ border-color: rgba(56,189,248,.65); }

.view-link.disabled{
  opacity:.45;
  pointer-events:none;
}

@media (max-width:900px){
  col.time { width:0; }
  th:nth-child(7), td:nth-child(7){ display:none; }
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
        <col class="status">
        <col class="valve">
        <col class="now">
        <col class="prev">
        <col class="delta">
        <col class="time">
        <col class="view">
      </colgroup>

      <thead>
        <tr>
          <th>Device</th>
          <th>Status</th>
          <th>Valve</th>
          <th>Now</th>
          <th>Prev</th>
          <th>Δ</th>
          <th>Last seen (UTC)</th>
          <th>View</th>
        </tr>
      </thead>

      <tbody id="devices-body">
        {% for d in devices %}
        <tr data-uid="{{ d.device_id }}">
          <td class="mono" data-k="device_id">{{ d.device_id }}</td>

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

          <td data-k="view">
            <a class="view-link {{ '' if d.has_view else 'disabled' }}" href="/device/{{ d.device_id }}">
              {{ 'Open' if d.has_view else 'No init' }}
            </a>
          </td>
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
      <td data-k="status"></td>
      <td data-k="valve"></td>
      <td data-k="now"></td>
      <td class="mono" data-k="prev"></td>
      <td class="mono" data-k="delta"></td>
      <td class="mono" data-k="time"></td>
      <td data-k="view"></td>
    `;
    tbody.appendChild(row);
    return row;
  }

  function updateRow(d){
    const uid = d.device_id;
    const row = ensureRow(uid);

    row.querySelector('[data-k="device_id"]').textContent = uid;

    const statusCell = row.querySelector('[data-k="status"]');
    statusCell.innerHTML = `<span class="pill ${d.offline ? "offline" : "open"}">${d.offline ? "Offline" : "Online"}</span>`;

    const valveCell = row.querySelector('[data-k="valve"]');
    valveCell.innerHTML = `<span class="pill mono">${d.valve_state || "-"}</span>`;

    const nowCell = row.querySelector('[data-k="now"]');
    const alarm = (d.pressure_now !== null && d.pressure_now !== undefined && Number(d.pressure_now) > 5);
    nowCell.innerHTML = `<span class="pill mono ${alarm ? "alarm" : ""}">${fmt3(d.pressure_now)}</span>`;

    row.querySelector('[data-k="prev"]').textContent  = fmt3(d.pressure_prev);
    row.querySelector('[data-k="delta"]').textContent = fmt3(d.delta);
    row.querySelector('[data-k="time"]').textContent  = d.time_utc || "-";

    const viewCell = row.querySelector('[data-k="view"]');
    const hasView = !!d.has_view;
    viewCell.innerHTML = `<a class="view-link ${hasView ? "" : "disabled"}" href="/device/${encodeURIComponent(uid)}">${hasView ? "Open" : "No init"}</a>`;
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


@app.get("/device/<uid>")
def device_view(uid: str):
    p = template_path_for(uid)
    if not p.exists():
        html = r"""
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{{ uid }} · No init</title>
<style>
body{
  margin:0; padding:36px 14px;
  font-family:system-ui,-apple-system,"Segoe UI",sans-serif;
  background:radial-gradient(circle at top,#0f172a 0,#020617 55%,#000 100%);
  color:#e5e7eb;
  display:flex; justify-content:center;
}
.card{
  width:100%; max-width:900px;
  background:#020617; border-radius:26px;
  padding:26px;
  box-shadow:0 0 0 1px rgba(15,23,42,.9),
             0 35px 120px rgba(15,23,42,.95);
}
a{ color:#8ab4ff; text-decoration:none; }
</style>
</head>
<body>
  <div class="card">
    <h2 style="margin:0 0 10px 0;">{{ uid }}</h2>
    <p style="margin:0; color:#94a3b8;">
      No init template yet. ESP must publish sensors/{{ uid }}/init and listener must save it to:
      <code>{{ path }}</code>
    </p>
    <p style="margin-top:14px;"><a href="/">← back</a></p>
  </div>
</body>
</html>
"""
        return render_template_string(html, uid=uid, path=str(p))

    fragment = p.read_text(encoding="utf-8", errors="replace")

    page = r"""
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>{{ uid }} · Device</title>

  <!-- Chart.js -->
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>

  <style>
    body{
      margin:0; padding:36px 14px;
      font-family:system-ui,-apple-system,"Segoe UI",sans-serif;
      background:radial-gradient(circle at top,#0f172a 0,#020617 55%,#000 100%);
      color:#e5e7eb;
      display:flex; justify-content:center;
    }
    .wrap{ width:100%; max-width:980px; }
    a{ color:#8ab4ff; text-decoration:none; }
    .topbar{ margin-bottom:16px; display:flex; gap:14px; align-items:center; flex-wrap:wrap; }
    .muted{ color:#94a3b8; font-size:12px; letter-spacing:.14em; text-transform:uppercase; }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="topbar">
      <a href="/">← back</a>
      <div class="muted">Device: <span id="uid">{{ uid }}</span></div>
      <div class="muted" id="conn">connecting…</div>
    </div>

    {{ fragment | safe }}
  </div>

<script>
(function(){
  const uid = {{ uid|tojson }};
  const conn = document.getElementById("conn");

  function pickDevice(snapshot){
    const list = snapshot && snapshot.devices ? snapshot.devices : [];
    return list.find(d => d.device_id === uid);
  }

  function pushToFragment(d){
    if (!d) return;
    if (typeof window.handleSensorUpdate !== "function") return;

    window.handleSensorUpdate({
      uid: d.device_id,
      ts_ms: d.time_ms,
      valve_state: d.valve_state,
      pressure_prev: d.pressure_prev,
      pressure_now: d.pressure_now
    });
  }

  const es = new EventSource("/events/devices");

  es.onopen = () => { if (conn) conn.textContent = "connected"; };
  es.onerror = () => { if (conn) conn.textContent = "reconnecting…"; };

  async function loadHistory(){
    try{
      const res = await fetch(`/api/device/${encodeURIComponent(uid)}/history?hours=24&limit=50`, { cache: "no-store" });
      const snap = await res.json();
      if (typeof window.handleHistorySnapshot === "function"){
        window.handleHistorySnapshot(snap);
      }
    }catch(e){}
  }

  // load history once on open
  loadHistory();

  // optional periodic refresh:
  // setInterval(loadHistory, 20000);

  es.addEventListener("devices", (evt) => {
    try{
      const snapshot = JSON.parse(evt.data);
      const d = pickDevice(snapshot);
      pushToFragment(d);
    }catch(e){}
  });
})();
</script>
</body>
</html>
"""
    return render_template_string(page, uid=uid, fragment=fragment)


# ===================== MAIN =====================
if __name__ == "__main__":
    # threaded is important for SSE
    app.run(host="0.0.0.0", port=PORT, threaded=True, use_reloader=False)
