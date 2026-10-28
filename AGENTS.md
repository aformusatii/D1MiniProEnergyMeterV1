# AGENTS.md

Guidance for coding agents working on this repository.

## Project

LOLIN (WEMOS) D1 mini (ESP8266, 4 MB flash) firmware that reads a Peacefair PZEM-004T v3.0 energy
meter over SoftwareSerial and exposes the values over HTTP. PlatformIO project (Arduino framework,
`espressif8266@4.2.1`, core 3.1.2), migrated from an Eclipse Sloeber project
(`sloeber_arduino_eclipse/` is legacy, do not build from it).

- Device: static IP `192.168.100.21`
- Logs: JSON over UDP to `192.168.100.134:5000` / `:5010` (`lib/Logger`), also printed on `Serial`
- HTTP API: `GET /` (info + upload form), `GET /data` (readings), `GET /health`, `GET /mqtt/test` (test MQTT publish), `POST /update` (OTA)
- MQTT: energy per hour/day/ISO week/month to Mosquitto `192.168.100.3:1883`, topics
  `D1MiniProEnergyMeterV1/...`, client ID `D1MiniProEnergyMeterV1`, local time Europe/Chisinau from NTP.
  Contract: [docs/MQTT.md](docs/MQTT.md). Ported from `ESP01EnergyMeterV2.1` (topics `energy-meter/...`);
  client ID and base topic must stay different from that device, which uses the same broker.

```
src/main.cpp          application: HTTP routes, PZEM readout
lib/WiFiHelper        WiFi connect, static IP, status LED, reconnect watchdog
lib/ServerHelper      index page / 404 handler
lib/Logger            JSON UDP logger
lib/EnergyPeriods     PZEM counter -> per-period records + RAM queue (no Arduino deps, host-tested)
lib/MqttPublisher     MQTT connection with backoff, drains the queue with QoS 1
include/AppConfig.h   MQTT broker, topic, timezone, NTP servers
include/WiFiConfig.h  local WiFi credentials (git-ignored; copy from WiFiConfig.example.h)
scripts/ota_upload.py PlatformIO custom upload: POSTs firmware.bin to /update
scripts/mqtt_monitor.py  prints the MQTT topics (pip install paho-mqtt)
test/host/            host unit test for lib/EnergyPeriods
platformio.ini        envs: d1_mini (serial, COM5) and d1_mini_ota (WiFi)
```

Hardware notes:
- The PZEM is on SoftwareSerial, RX `D5` (GPIO14) and TX `D6` (GPIO12). The hardware UART (USB)
  is free, so `Serial` debug output at 115200 baud is fine.
- The status LED is the on-board LED (`LED_BUILTIN`, GPIO2).
- Flash is 4 MB with no filesystem (`eagle.flash.4m.ld`).
- RTC user memory: blocks 0-31 belong to the OTA bootloader; the energy tracker state lives at block 64.

## Build

The PlatformIO CLI is at `~/.platformio/penv/bin/pio` (Windows: `~/.platformio/penv/Scripts/pio`).

```sh
pio run -e d1_mini_ota          # build only (the OTA env refuses placeholder WiFi credentials)
```

Host test for the period logic (needs g++ with POSIX TZ support):

```sh
g++ -std=gnu++17 -Wall -I lib/EnergyPeriods/src test/host/test_energy_periods.cpp \
    lib/EnergyPeriods/src/EnergyPeriods.cpp -o .pio/test_energy_periods && .pio/test_energy_periods
```

## OTA upload

```sh
pio run -e d1_mini_ota -t upload
```

This builds the firmware and POSTs `.pio/build/d1_mini_ota/firmware.bin` to
`http://192.168.100.21/update` (`custom_ota_url` in `platformio.ini`). The updater answers
HTTP 200 even when flashing fails, so the script checks the reply for `Update Success`.

A successful run ends with:

```
OTA update successful, device is rebooting
```

After uploading, confirm that the device came back. It takes a few seconds to reboot and rejoin WiFi:

```sh
for i in 1 2 3 4 5 6; do curl -s -m 5 -o /dev/null -w "%{http_code}\n" http://192.168.100.21/health && break; done
curl -s http://192.168.100.21/data
```

If OTA fails and the device is unreachable, the only recovery is serial flashing over USB on COM5
(`pio run -e d1_mini -t upload`), which needs physical access. Do not take OTA risks.

## Firmware size limit

With `eagle.flash.4m.ld` the sketch region is limited to **1,044,464 bytes** (`irom0_0_seg`,
"OTA:~1019KB"). PlatformIO checks this limit at link time ("Flash: ... from 1044464 bytes").

OTA staging is not a constraint on this board: the new image is written into the free flash above
the running sketch, up to `_FS_start` = `0x3FB000` (~4 MB), so any two images that each fit the
sketch region also fit together. As of 2026-10-28 `firmware.bin` is 361,536 bytes (with MQTT).

In your report, include the firmware size so that size growth is visible.

## Conventions

- Keep the board settings in `platformio.ini` (80 MHz, DIO 40 MHz, lwIP2 low memory,
  `eagle.flash.4m.ld`) unless the user asks to change them.
- Never commit `include/WiFiConfig.h`.
