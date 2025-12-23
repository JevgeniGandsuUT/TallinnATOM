import json
import os
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point, WritePrecision

MQTT_HOST = os.getenv("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "sensors/+/status")

INFLUX_URL = os.getenv("INFLUX_URL", "http://127.0.0.1:8086")
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN", "")
INFLUX_ORG = os.getenv("INFLUX_ORG", "TallinnAtom")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "TallinnAtom")


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
        client.subscribe(MQTT_TOPIC)

    def on_message(client, userdata, msg):
        raw = msg.payload.decode("utf-8", errors="replace")
        try:
            data = json.loads(raw)
        except Exception as e:
            print("[MQTT] bad json:", e, raw[:200])
            return

        device_id = str(data.get("device_id", "unknown"))
        team = str(data.get("team", "unknown"))
        valve_state = str(data.get("valve_state", "unknown"))

        ts_ms = to_int(data.get("timestamp_ms") or data.get("timestamp"))
        if 0 < ts_ms < 10_000_000_000:
            ts_ms *= 1000

        pressure_now = to_float(data.get("pressure_now"))
        pressure_prev = to_float(
            data.get("pressure_30ms_ago") or data.get("pressure_prev")
        )

        point = (
            Point("device_status")
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

        writer.write(
            bucket=INFLUX_BUCKET,
            org=INFLUX_ORG,
            record=point
        )

        print(
            f"[OK] {device_id} valve={valve_state} "
            f"now={pressure_now} prev={pressure_prev}"
        )

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_forever()


if __name__ == "__main__":
    main()