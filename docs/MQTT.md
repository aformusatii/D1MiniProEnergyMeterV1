# MQTT energy reports

The meter publishes the energy consumed per local calendar period to the Mosquitto broker at
`192.168.100.3:1883` (settings in [include/AppConfig.h](../include/AppConfig.h)).

## Topics

| Topic                        | Retained | QoS | When                                              |
|------------------------------|----------|-----|---------------------------------------------------|
| `D1MiniProEnergyMeterV1/availability`  | yes      | 1   | `{"state":"online"}` on connect, `{"state":"offline"}` as last will |
| `D1MiniProEnergyMeterV1/energy/hour`   | no       | 1   | every full hour                                   |
| `D1MiniProEnergyMeterV1/energy/day`    | no       | 1   | local midnight                                    |
| `D1MiniProEnergyMeterV1/energy/week`   | no       | 1   | Monday 00:00 (ISO week)                           |
| `D1MiniProEnergyMeterV1/energy/month`  | no       | 1   | 1st of the month 00:00                            |
| `D1MiniProEnergyMeterV1/test`          | no       | 1   | on demand: `GET http://192.168.100.21/mqtt/test[?msg=text]` |

`D1MiniProEnergyMeterV1/test` is for checking the device -> broker -> consumer path. The payload is
`{"test":true,"time":"...","time_synced":true,"uptime_ms":...,"heap":...,"msg":"text"}`
(`msg` only if given, cut to 100 characters). The HTTP reply has `success` (PUBACK received),
`topic` and `payload`; 503 means no MQTT connection (it opens only after NTP sync).

When boundaries coincide (every midnight closes an hour and a day; a Monday or the 1st of a
month also closes a week or month), the records arrive together, shortest period first.

## Payload

```json
{"period":"hour","start":"2026-10-18T13:00:00+03:00","end":"2026-10-18T14:00:00+03:00","energy_wh":412,"meter_wh":810775,"complete":true}
```

| Field       | Meaning |
|-------------|---------|
| `period`    | `hour`, `day`, `week` or `month` (same as the last topic level) |
| `start`     | period start, ISO 8601 local time with offset (Europe/Chisinau). Parses directly into PostgreSQL `timestamptz` |
| `end`       | period end = start of the next period (exclusive) |
| `energy_wh` | energy consumed in `[start, end)`, Wh (integer) |
| `meter_wh`  | PZEM cumulative counter at `end`, Wh. Lets the consumer rebuild any gap: consumption between two records = difference of `meter_wh` |
| `complete`  | `false` if the record does not cover exactly one whole period: tracking started mid-period (first record after a power cycle), the device was off across one or more boundaries (record then spans from `start` to `end`), or the PZEM counter was reset |

Days are 23 or 25 hours on DST change days; the 03:00 hour on the last Sunday of October occurs
twice and is reported as two records (`+03:00` and `+02:00`).

## Delivery guarantees

- No time, no data: until NTP has synced after boot nothing is measured or published.
- Once synced, boundaries are detected even without WiFi/broker. Finished records wait in a RAM
  queue (36 records, about 1.5 days of hourly data) and are published with QoS 1 after reconnect;
  a record is removed only after the broker acknowledged it. When the queue is full the oldest
  record is dropped (`dropped` counter in `GET /health`).
- A record may be delivered twice (if the acknowledgement is lost). Make the consumer idempotent:
  use `(period, start)` as the unique key and upsert.
- The period in progress survives a reset/OTA reboot (kept in RTC memory). After a power cycle
  it starts over and the first record per period has `complete: false`.
- The RAM queue does not survive a reboot. Records lost that way can still be derived from `meter_wh`.

For Node-RED: subscribe to `D1MiniProEnergyMeterV1/energy/+` with QoS 1 and a fixed client ID and
**Clean session off**, so Mosquitto keeps queuing records while Node-RED is down.

Example table:

```sql
CREATE TABLE energy_report (
    period      text        NOT NULL,
    period_start timestamptz NOT NULL,
    period_end  timestamptz NOT NULL,
    energy_wh   integer     NOT NULL,
    meter_wh    bigint      NOT NULL,
    complete    boolean     NOT NULL,
    PRIMARY KEY (period, period_start)
);
-- INSERT ... ON CONFLICT (period, period_start) DO UPDATE SET ...
```

## Watching the topics

```sh
pip install paho-mqtt
python scripts/mqtt_monitor.py
```
