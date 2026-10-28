"""Print the energy meter's MQTT messages.

    pip install paho-mqtt
    python scripts/mqtt_monitor.py                      # broker from include/AppConfig.h
    python scripts/mqtt_monitor.py --host 192.168.100.3 --topic "D1MiniProEnergyMeterV1/#"
"""
import argparse
import datetime
import pathlib
import re

import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion

CONFIG = pathlib.Path(__file__).resolve().parent.parent / "include" / "AppConfig.h"


def config_value(name, default):
    try:
        match = re.search(rf'#define\s+{name}\s+"?([^"\s]+)"?', CONFIG.read_text())
        return match.group(1) if match else default
    except OSError:
        return default


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=config_value("MQTT_HOST", "192.168.100.3"))
    parser.add_argument("--port", type=int, default=int(config_value("MQTT_PORT", "1883")))
    parser.add_argument("--topic", default=config_value("MQTT_BASE_TOPIC", "D1MiniProEnergyMeterV1") + "/#")
    parser.add_argument("--user")
    parser.add_argument("--password")
    args = parser.parse_args()

    client = mqtt.Client(CallbackAPIVersion.VERSION2)
    if args.user:
        client.username_pw_set(args.user, args.password)

    def on_connect(c, userdata, flags, reason, properties):
        print(f"connected to {args.host}:{args.port} ({reason}), subscribing to {args.topic}")
        c.subscribe(args.topic, qos=1)

    def on_message(c, userdata, msg):
        now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        retained = " (retained)" if msg.retain else ""
        print(f"{now} {msg.topic}{retained} {msg.payload.decode(errors='replace')}", flush=True)

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.host, args.port, 60)
    client.loop_forever()


if __name__ == "__main__":
    main()
