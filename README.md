# D1MiniProEnergyMeterV1

[![PlatformIO](https://img.shields.io/badge/PlatformIO-espressif8266%404.2.1-orange?logo=platformio)](https://platformio.org/)
[![Board](https://img.shields.io/badge/board-LOLIN%20D1%20mini-blue)](https://www.wemos.cc/en/latest/d1/d1_mini.html)
[![Framework](https://img.shields.io/badge/framework-Arduino%20(core%203.1.2)-teal?logo=arduino)](https://github.com/esp8266/Arduino)

Firmware for a WiFi energy meter built on a **LOLIN (WEMOS) D1 mini** (ESP8266) and a
**Peacefair PZEM-004T v3.0** power measurement module. The device reads voltage, current, active
power, energy, frequency and power factor from the PZEM over Modbus-RTU and exposes them as JSON
over a small HTTP API, with over-the-air firmware updates. Energy consumed per local hour, day,
ISO week and month is published to an MQTT broker, timed by NTP.

## Features

- Live electrical readings from the PZEM-004T v3.0 as JSON (`GET /data`)
- Energy per hour / day / ISO week / month pushed to MQTT at each period end, using NTP time
  (Europe/Chisinau), queued in RAM while the broker is unreachable. See [docs/MQTT.md](docs/MQTT.md)
- Health endpoint for monitoring (`GET /health`)
- Device info page with flash, chip and WiFi details, plus a firmware upload form (`GET /`)
- OTA firmware updates over WiFi, from the browser or with one `pio` command
- Static IP and a WiFi watchdog that reconnects automatically and shows the link state on the on-board LED
- Structured JSON logging over UDP, mirrored to the USB serial console

## Hardware

| Component        | Details                                                     |
|------------------|-------------------------------------------------------------|
| MCU board        | LOLIN (WEMOS) D1 mini (clone), ESP8266, 4 MB flash          |
| Energy meter     | Peacefair PZEM-004T v3.0 (TTL UART, Modbus-RTU, 9600 baud) |
| Status LED       | On-board LED (`LED_BUILTIN`, GPIO2)                         |

### Wiring

| PZEM-004T | D1 mini         | Notes                                   |
|-----------|-----------------|-----------------------------------------|
| `TX`      | `D5` (GPIO14)   | SoftwareSerial RX                       |
| `RX`      | `D6` (GPIO12)   | SoftwareSerial TX                       |
| `5V`      | `5V`            |                                         |
| `GND`     | `GND`           |                                         |

The PZEM uses a software serial port, so the hardware UART stays free for USB flashing and debug
output at 115200 baud.

> [!WARNING]
> The PZEM-004T measures mains voltage. Wire the AC side and the current transformer with the
> power off, and keep the low-voltage side insulated from mains.

## HTTP API

| Method | Path      | Description                                         |
|--------|-----------|-----------------------------------------------------|
| GET    | `/`       | Device, flash and WiFi info, firmware upload form   |
| GET    | `/data`   | Current readings from the PZEM                      |
| GET    | `/health` | Liveness check: `status`, `time_synced`, `mqtt`, `queued`, `dropped`, `heap` |
| GET    | `/mqtt/test[?msg=text]` | Publishes a test message to `D1MiniProEnergyMeterV1/test` |
| POST   | `/update` | OTA firmware update (multipart form, field `update`) |

Example response from `GET /data`:

```json
{
  "success": true,
  "voltage": 229.8,
  "current": 1.274,
  "power": 271.3,
  "energy": 1532.418,
  "frequency": 50.0,
  "pf": 0.93
}
```

| Field       | Unit |
|-------------|------|
| `voltage`   | V    |
| `current`   | A    |
| `power`     | W    |
| `energy`    | kWh  |
| `frequency` | Hz   |
| `pf`        | power factor, 0 to 1 |

If the PZEM does not answer, the response is `{"success": false, "error": "Error reading voltage"}`,
where the message names the first value that could not be read.

## MQTT

Broker, client ID, base topic, time zone and NTP servers are in
[`include/AppConfig.h`](include/AppConfig.h) (broker `192.168.100.3:1883`, topics
`D1MiniProEnergyMeterV1/...`). Topics, payload and delivery guarantees: [docs/MQTT.md](docs/MQTT.md).

```sh
pip install paho-mqtt
python scripts/mqtt_monitor.py
```

## Getting started

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) (CLI) or the
  PlatformIO IDE extension for VS Code

The ESP8266 platform, toolchain and libraries are downloaded on the first build.

### Configure WiFi

```sh
cp include/WiFiConfig.example.h include/WiFiConfig.h
```

Edit `include/WiFiConfig.h` and set `LOCAL2_SSID` and `LOCAL2_KEY`. The file is git-ignored.
Without it, the default build falls back to placeholder credentials and prints a warning. The OTA
build refuses to compile without it, so you cannot push firmware that would never reconnect.

The static IP (`192.168.100.21`) is set in `setup()` in [`src/main.cpp`](src/main.cpp), and the
UDP log target (`192.168.100.134:5000` / `:5010`) in [`lib/Logger`](lib/Logger/src/Logger.cpp).

### Build

```sh
pio run
```

### Flash

| Method          | Command                             | Notes                                     |
|-----------------|-------------------------------------|-------------------------------------------|
| USB (serial)    | `pio run -e d1_mini -t upload`      | Port set by `upload_port` (default `COM5`) |
| OTA over WiFi   | `pio run -e d1_mini_ota -t upload`  | Posts the firmware to `http://192.168.100.21/update` |

You can also open `http://192.168.100.21/` in a browser and upload
`.pio/build/d1_mini_ota/firmware.bin` from the form.

The OTA script ([`scripts/ota_upload.py`](scripts/ota_upload.py)) checks the device reply for
`Update Success`, because the ESP8266 updater answers HTTP 200 even when flashing fails.

### Serial monitor

```sh
pio device monitor
```

## Build configuration

The board settings match the original Arduino IDE configuration
([screenshot](docs/ArduinoBoardConfigurationNew4MB.png)):

| Setting        | Value                                      |
|----------------|--------------------------------------------|
| Board          | `d1_mini`                                  |
| CPU frequency  | 80 MHz                                     |
| Flash          | 4 MB, no filesystem (`eagle.flash.4m.ld`), sketch up to ~1019 KB |
| Flash mode     | DIO, 40 MHz                                |
| lwIP variant   | v2 Lower Memory                            |

### Dependencies

| Library                                                                 | Version |
|-------------------------------------------------------------------------|---------|
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson)                 | 7.2.0   |
| [MQTT (256dpi)](https://github.com/256dpi/arduino-mqtt)                 | 2.5.2   |
| [PZEM-004T-v30](https://github.com/mandulaj/PZEM-004T-v30)              | 1.2.1   |

## Project structure

```
├── src/main.cpp              Application: HTTP routes, PZEM readout
├── lib/
│   ├── WiFiHelper/           WiFi connect, static IP, status LED, reconnect watchdog
│   ├── ServerHelper/         Index page and 404 handler
│   ├── Logger/               JSON logger over UDP (mirrored to Serial)
│   ├── EnergyPeriods/        PZEM counter -> per-period records + RAM queue (host-tested)
│   └── MqttPublisher/        MQTT connection with backoff, drains the queue with QoS 1
├── include/
│   ├── AppConfig.h           MQTT broker, topic, time zone, NTP servers
│   └── WiFiConfig.example.h  WiFi credentials template
├── scripts/ota_upload.py     PlatformIO custom upload target for OTA
├── scripts/mqtt_monitor.py   Prints the MQTT topics
├── test/host/                Host unit test for lib/EnergyPeriods
├── docs/                     MQTT contract, board configuration screenshots
├── sloeber_arduino_eclipse/  Original Eclipse Sloeber project (legacy, not built)
└── platformio.ini            Build environments: d1_mini (USB), d1_mini_ota (WiFi)
```

## Related projects

- [ESP01EnergyMeterV2.1](https://github.com/aformusatii/ESP01EnergyMeterV2.1): the same meter on an
  ESP-01S. The MQTT period publishing here is ported from it.

## History

This project started as an Eclipse Sloeber (Arduino IDE) sketch and was migrated to PlatformIO.
The original sources are kept unchanged in [`sloeber_arduino_eclipse/`](sloeber_arduino_eclipse/)
for reference.
