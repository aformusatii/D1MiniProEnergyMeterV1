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
- Wake-on-LAN for the ThinkPad T580 (Proxmox host `.2`) from a Telegram bot or `POST /wol`, with a
  message once it is up. See [Wake-on-LAN and Telegram bot](#wake-on-lan-and-telegram-bot)
- Daily energy report in a family Telegram group: yesterday's consumption and cost at 07:00, on the
  1st also the previous month, plus `/today`, `/yesterday`, `/month`. See
  [Family energy reports](#family-energy-reports)
- Day/week/month totals survive power cuts (kept in flash), so the monthly total stays exact
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
| GET    | `/health` | Liveness check: `status`, `time_synced`, `mqtt`, `queued`, `dropped`, `heap`, `telegram`, `telegram_error`, `telegram_unknown_chat`, `report_due` |
| GET    | `/mqtt/test[?msg=text]` | Publishes a test message to `D1MiniProEnergyMeterV1/test` |
| GET    | `/wol`    | Wake-on-LAN target and `online` (its SSH port answers) |
| POST   | `/wol`    | Sends the Wake-on-LAN magic packet to the target    |
| GET    | `/report` | Preview of the daily energy report (`text`), target `chat_id`, `due` |
| POST   | `/report[?to=admin]` | Sends the daily report now to the family chat (or your private chat), without counting it as the 07:00 message |
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

## Wake-on-LAN and Telegram bot

The Proxmox host (ThinkPad T580, `192.168.100.2`) does not power on by itself after a power cut.
The board can wake it with a Wake-on-LAN magic packet, sent 3 times to the subnet broadcast
`192.168.100.255`, UDP port 9. The target is set in [`include/AppConfig.h`](include/AppConfig.h)
(`WOL_TARGET_*`).

```sh
curl -X POST http://192.168.100.21/wol    # send the magic packet
curl http://192.168.100.21/wol            # {"online": true} once SSH on the T580 answers
```

### Telegram commands

| Command (or button)   | What it does |
|-----------------------|--------------|
| `/wol` (`Wake T580`)  | Sends the magic packet, then checks every 20 s for 5 min and reports when the T580 is up. If the T580 is already up, it only says so. |
| `/status` (`Status`)  | T580 up or down, mains voltage and power from the PZEM, board uptime, heap and WiFi signal |
| anything else, `/start` | Help text and the reply keyboard |

When the board starts, it sends a message with the reset reason. `Power On` means mains power came
back, which is the moment to send `/wol`.

The bot uses long polling (`getUpdates`) over one kept-alive TLS connection, so no port has to be
open to the internet. Only messages from `TELEGRAM_CHAT_ID` are handled. Messages from any other
chat are logged with their chat id and ignored. Commands older than 5 minutes, for example sent
while the board had no power, are ignored too.

### Setup

1. In Telegram, open [@BotFather](https://t.me/BotFather), send `/newbot`, and pick a name and a
   username ending in `bot`. BotFather replies with the token (`123456789:AA...`). Use a **new**
   bot. Two devices polling with the same token take updates from each other (HTTP 409).
2. Open the new bot and send it any message, for example `/start`.
3. Find your chat id. Open `https://api.telegram.org/bot<TOKEN>/getUpdates` in a browser and read
   `result[0].message.chat.id`. Do this before flashing, because while the board polls, the browser
   gets nothing.
4. Add both values to `include/WiFiConfig.h` (git-ignored):

   ```c
   #define TELEGRAM_BOT_TOKEN  "123456789:AA..."
   #define TELEGRAM_CHAT_ID    123456789LL
   ```

5. Flash: `pio run -e d1_mini_ota -t upload`. The bot sends a "started" message with the
   keyboard.
6. Optional: in @BotFather, `/setcommands` for the bot, then:

   ```
   wol - Wake the ThinkPad T580
   status - Is the T580 up, mains voltage and power
   ```

`GET /health` shows `"telegram": true` once requests succeed. `telegram_error` is the last
Telegram HTTP status: 401 means a wrong token, 409 means another poller uses the same token,
-1 is a connection error and -2 is low heap.

## Family energy reports

The same bot also posts to a family group, so others can follow the consumption without access to
Wake-on-LAN. One bot serves both chats over its single TLS connection (a second polling bot would
need a second connection, about 16 KB of heap, which the board does not have). Messages are routed
by chat id:

| Chat | Set by | Gets |
|------|--------|------|
| Private chat | `TELEGRAM_CHAT_ID` | `/wol`, `/status`, the start message, and the energy commands |
| Family group | `TELEGRAM_FAMILY_CHAT_ID` | the daily report and the energy commands. `/wol` is not available here |

Every day at 07:00 (Europe/Chisinau) the group gets yesterday's consumption. On the 1st the same
message also covers the month that just ended:

```
⚡ Yesterday, Mon 30 Nov: 11.0 kWh = 39.49 lei
Previous 7 days: 11.0 kWh/day on average (+0%)

📅 November 2026: 352.1 kWh = 1264.04 lei
11.7 kWh/day on average
October 2026: 330.0 kWh (November +7%)
November 2025: 300.0 kWh (November +17%)
```

On other days the second part shows the month so far and a forecast:

```
November so far: 42.0 kWh = 150.78 lei
Forecast for November: ~420 kWh = ~1508 lei
```

| Command      | Answer |
|--------------|--------|
| `/today`     | used since midnight and its cost, power right now, yesterday's total |
| `/yesterday` | the daily report for the newest finished day |
| `/month`     | this month so far, forecast for the month, last month's total |

The price (`ENERGY_PRICE_PER_KWH`, 3.59 lei), the currency and the report time are in
[`include/AppConfig.h`](include/AppConfig.h). Costs are kWh × price, rounded to bani.

Delivery:

- If the board or the internet is down at 07:00, the report goes out as soon as it is back. Each day
  is reported once, even across reboots. Only the newest day is reported: if several days were
  missed, they are not sent one by one.
- Day, week and month in progress and the report history (last 8 days, last 13 months) are kept in
  flash, written about twice a day. After a power cut the totals continue from the PZEM counter,
  which keeps its value. Nothing is consumed while the mains is off, so the totals stay exact.
- If the board itself was off while the mains was on, the day is marked incomplete, or shown as a
  range of several days.

### Setup

1. In Telegram, create a group (for example "Home energy"), add your wife and the bot. Optional:
   rename the bot in @BotFather (`/setname`), because the group sees its name.
2. In the group, send `/start`. The board ignores it, but it logs the group id and shows it in
   `GET /health` as `telegram_unknown_chat` (group ids are negative, e.g. `-1001234567890`):

   ```sh
   curl -s http://192.168.100.21/health
   ```

3. Add it to `include/WiFiConfig.h` and flash:

   ```c
   #define TELEGRAM_FAMILY_CHAT_ID  -1001234567890LL
   ```

4. Check: `curl -s http://192.168.100.21/report` shows the text and `"family_chat":true`, and
   `curl -X POST http://192.168.100.21/report` posts it to the group now.
5. Optional: show only the matching commands in each chat's `/` menu, once, from any computer:

   ```sh
   T=<bot token>
   curl -s "https://api.telegram.org/bot$T/setMyCommands" -H 'Content-Type: application/json' -d '{"commands":[
     {"command":"today","description":"Energy used today so far"},
     {"command":"yesterday","description":"Yesterday'"'"'s consumption"},
     {"command":"month","description":"This month so far and forecast"}],
     "scope":{"type":"chat","chat_id":-1001234567890}}'
   curl -s "https://api.telegram.org/bot$T/setMyCommands" -H 'Content-Type: application/json' -d '{"commands":[
     {"command":"wol","description":"Wake the ThinkPad T580"},
     {"command":"status","description":"Is the T580 up, mains voltage and power"},
     {"command":"today","description":"Energy used today so far"},
     {"command":"month","description":"This month so far and forecast"}],
     "scope":{"type":"chat","chat_id":<your chat id>}}'
   ```

Until `TELEGRAM_FAMILY_CHAT_ID` is set, the daily report goes to your private chat. In the group the
bot answers commands only, so ordinary chatting there gets no replies. The buttons it offers are
commands (`/today`), because with privacy mode on (the default) a bot in a group only sees commands.
If you change the group's type (for example make the chat history visible to new members), Telegram
can turn it into a supergroup with a new id. The send then fails with HTTP 400 in the logs. Set the
new id.

### Target requirements (T580)

- BIOS: `Config > Network > Wake On LAN` set to `AC Only` or `AC and Battery` (currently
  `ACandBattery`, readable on Proxmox under `/sys/class/firmware-attributes/thinklmi/attributes/`)
- OS: `ethtool enp0s31f6 | grep Wake-on` must show `g` (it does, set by the e1000e driver)
- The T580 must be wired. Wake-on-LAN does not work over Wi-Fi.

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

TLS for the Telegram API is BearSSL from the ESP8266 core. It negotiates a 1 KB maximum fragment
length with `api.telegram.org`, so a connection needs about 16 KB of heap instead of about 30 KB.

## Project structure

```
├── src/main.cpp              Application: HTTP routes, PZEM readout
├── lib/
│   ├── WiFiHelper/           WiFi connect, static IP, status LED, reconnect watchdog
│   ├── ServerHelper/         Index page and 404 handler
│   ├── Logger/               JSON logger over UDP (mirrored to Serial)
│   ├── EnergyPeriods/        PZEM counter -> per-period records + RAM queue (host-tested)
│   ├── EnergyReport/         Daily/monthly report: history, 07:00 schedule, message texts (host-tested)
│   ├── MqttPublisher/        MQTT connection with backoff, drains the queue with QoS 1
│   ├── WakeOnLan/            MAC parsing and magic packet (host-tested)
│   └── TelegramBot/          Non-blocking Telegram long polling client, several chats (getUpdates / sendMessage)
├── include/
│   ├── AppConfig.h           MQTT broker, topic, time zone, NTP servers, Wake-on-LAN target, price, report time
│   └── WiFiConfig.example.h  WiFi credentials, Telegram bot token and chat ids template
├── scripts/ota_upload.py     PlatformIO custom upload target for OTA
├── scripts/mqtt_monitor.py   Prints the MQTT topics
├── test/host/                Host unit tests for lib/EnergyPeriods, lib/EnergyReport and lib/WakeOnLan
├── docs/                     MQTT contract, board configuration screenshots
├── sloeber_arduino_eclipse/  Original Eclipse Sloeber project (legacy, not built)
└── platformio.ini            Build environments: d1_mini (USB), d1_mini_ota (WiFi)
```

## Related projects

- [ESP01EnergyMeterV2.1](https://github.com/aformusatii/ESP01EnergyMeterV2.1): the same meter on an
  ESP-01S. The MQTT period publishing here is ported from it.
- PowerManager V1 (`aformusatii/SloeberArduino`, `D1MiniPro_PowerManagerV1/`): the Telegram bot here
  follows its long polling approach.

## History

This project started as an Eclipse Sloeber (Arduino IDE) sketch and was migrated to PlatformIO.
The original sources are kept unchanged in [`sloeber_arduino_eclipse/`](sloeber_arduino_eclipse/)
for reference.
