import json
import os
import time
from pathlib import Path
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point, WritePrecision

# ===================== MQTT CONFIG =====================
MQTT_HOST = os.getenv("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

# слушаем два топика (status + init)
MQTT_TOPIC_STATUS = os.getenv("MQTT_TOPIC_STATUS", "sensors/+/status")
MQTT_TOPIC_INIT = os.getenv("MQTT_TOPIC_INIT", "sensors/+/init")

# ===================== INFLUX CONFIG =====================
INFLUX_URL = os.getenv("INFLUX_URL", "http://127.0.0.1:8086")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN", "")
INFLUX_ORG = os.getenv("INFLUX_ORG", "TallinnAtom")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "TallinnAtom")

MEASUREMENT = os.getenv("INFLUX_MEASUREMENT", "device_status")

# ===================== FILE STORAGE =====================
BASE_DIR = Path(__file__).resolve().parent
TEMPL_DIR = BASE_DIR / "device_templates"
TEMPL_DIR.mkdir(parents=True, exist_ok=True)

DEVICES_JSON = BASE_DIR / "devices.json"

# защита: чтобы кто-то не прислал 10MB html и не убил диск/память
MAX_INIT_BYTES = int(os.getenv("MAX_INIT_BYTES", "200000"))  # 200KB


def now_utc():
    return datetime.now(timezone.utc)


def to_int(x, default=0):
    try:
        return int(x)
    except Exception:
        return default


def to_float(x, default=None):
    try:
        return float(x)
    except Exception:
        return default


def parse_topic(topic: str):
    # sensors/<uid>/status OR sensors/<uid>/init
    parts = topic.split("/")
    if len(parts) != 3:
        return None, None
    if parts[0] != "sensors":
        return None, None
    uid = parts[1].strip()
    kind = parts[2].strip()
    if not uid or kind not in ("status", "init"):
        return None, None
    return uid, kind


def load_devices():
    if not DEVICES_JSON.exists():
        return {}
    try:
        return json.loads(DEVICES_JSON.read_text(encoding="utf-8"))
    except Exception:
        # если файл битый — не падаем
        return {}


def save_devices(devs: dict):
    tmp = DEVICES_JSON.with_suffix(".tmp")
    tmp.write_text(json.dumps(devs, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, DEVICES_JSON)


def update_device_meta(device_id: str, **kwargs):
    devs = load_devices()
    d = devs.get(device_id, {})
    d["device_id"] = device_id
    for k, v in kwargs.items():
        d[k] = v
    devs[device_id] = d
    save_devices(devs)


def save_init_html(device_id: str, html: str) -> str:
    # сохраняем как utf-8 файл: device_templates/<device_id>.html
    safe_name = "".join(c for c in device_id if c.isalnum() or c in ("-", "_", "."))
    if not safe_name:
        safe_name = "unknown"

    path = TEMPL_DIR / f"{safe_name}.html"
    tmp = path.with_suffix(".tmp")
    tmp.write_text(html, encoding="utf-8")
    os.replace(tmp, path)
    return str(path)


def main():
    if not INFLUX_TOKEN:
        raise SystemExit("INFLUX_TOKEN missing. Export it first.")

    influx = InfluxDBClient(
        url=INFLUX_URL,
        token=INFLUX_TOKEN,
        org=INFLUX_ORG
    )
    writer = influx.write_api()

    def on_connect(client, userdata, flags, rc, properties=None):
        print(f"[MQTT] connected rc={rc}")

        # подписка на status + init
        client.subscribe(MQTT_TOPIC_STATUS)
        client.subscribe(MQTT_TOPIC_INIT)

        print(f"[MQTT] subscribed: {MQTT_TOPIC_STATUS}")
        print(f"[MQTT] subscribed: {MQTT_TOPIC_INIT}")

    def handle_init(uid: str, raw: str):
        # лимит по размеру
        if len(raw.encode("utf-8", errors="replace")) > MAX_INIT_BYTES:
            print(f"[INIT] too large, skip uid={uid} bytes~{len(raw)}")
            update_device_meta(uid, has_init=False, init_error="too_large", init_updated_utc=int(time.time()))
            return

        saved_path = save_init_html(uid, raw)
        update_device_meta(
            uid,
            has_init=True,
            init_path=saved_path,
            init_bytes=len(raw),
            init_updated_utc=int(time.time())
        )
        print(f"[INIT] saved uid={uid} bytes={len(raw)} -> {saved_path}")

    def handle_status(uid: str, raw: str):
        try:
            data = json.loads(raw)
        except Exception as e:
            print("[MQTT] bad json:", e, raw[:200])
            update_device_meta(uid, last_seen_utc=int(time.time()), last_error="bad_json")
            return

        device_id = str(data.get("device_id") or uid or "unknown")
        team = str(data.get("team", "unknown"))
        valve_state = str(data.get("valve_state", "unknown"))

        ts_ms = to_int(data.get("timestamp_ms") or data.get("timestamp"))
        if 0 < ts_ms < 10_000_000_000:
            ts_ms *= 1000

        pressure_now = to_float(data.get("pressure_now"))
        pressure_prev = to_float(data.get("pressure_30ms_ago") or data.get("pressure_prev"))

        point = (
            Point(MEASUREMENT)
            .tag("device_id", device_id)
            .tag("team", team)
            .tag("valve_state", valve_state)
        )

        if pressure_now is not None:
            point = point.field("pressure_now", pressure_now)
        if pressure_prev is not None:
            point = point.field("pressure_prev", pressure_prev)
        if pressure_now is not None and pressure_prev is not None:
            point = point.field("pressure_delta", pressure_now - pressure_prev)

        if ts_ms:
            point = point.time(ts_ms, WritePrecision.MS)
        else:
            point = point.time(now_utc(), WritePrecision.NS)

        writer.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)

        # мета для overview/offline
        update_device_meta(
            device_id,
            team=team,
            valve_state=valve_state,
            pressure_now=pressure_now,
            pressure_prev=pressure_prev,
            last_seen_utc=int(time.time()),
            last_status_raw=raw[:4000]  # защита
        )

        print(f"[OK] {device_id} valve={valve_state} now={pressure_now} prev={pressure_prev}")

    def on_message(client, userdata, msg):
        uid, kind = parse_topic(msg.topic)
        if not uid:
            # fallback: если topic не стандартный — пробуем json device_id
            raw = msg.payload.decode("utf-8", errors="replace")
            print("[MQTT] unknown topic:", msg.topic, "payload:", raw[:120])
            return

        raw = msg.payload.decode("utf-8", errors="replace")

        if kind == "init":
            handle_init(uid, raw)
        elif kind == "status":
            handle_status(uid, raw)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_forever()


if __name__ == "__main__":
    main()
