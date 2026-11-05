#ifndef _AppConfig_H_
#define _AppConfig_H_

// MQTT broker (local Mosquitto, anonymous). For a broker with authentication, define
// MQTT_USER and MQTT_PASSWORD in include/WiFiConfig.h (git-ignored), not here.
#define MQTT_HOST        "192.168.100.3"
#define MQTT_PORT        1883
// All topics are under the sensor name, separate from the ESP-01S meter (client "esp01-energy-meter",
// topics "energy-meter/...") on the same broker. The client ID must differ too: a duplicate client ID
// makes the broker drop the other connection.
#define MQTT_CLIENT_ID   "D1MiniProEnergyMeterV1"
#define MQTT_BASE_TOPIC  "D1MiniProEnergyMeterV1"

// Local time defines the hour/day/week/month boundaries.
// Europe/Chisinau as a POSIX TZ string (EET, EEST from last Sunday of March 02:00
// to last Sunday of October 03:00).
#define TIME_ZONE        "EET-2EEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER_1     "pool.ntp.org"
#define NTP_SERVER_2     "time.cloudflare.com"

// Wake-on-LAN target: Proxmox host, Lenovo ThinkPad T580 (wired, Intel I219-V enp0s31f6).
// The magic packet goes to the subnet broadcast address, the IP is only used to check if it is up.
#define WOL_TARGET_NAME  "ThinkPad T580"
#define WOL_TARGET_MAC   "48:2A:E3:20:D6:81"
#define WOL_TARGET_IP    "192.168.100.2"
// a TCP connect to this port (SSH) tells if the target is up
#define WOL_CHECK_PORT   22

// Telegram bot: the token and the chat ids are secrets, define TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID
// (your private chat: Wake-on-LAN) and TELEGRAM_FAMILY_CHAT_ID (family group: energy reports) in
// include/WiFiConfig.h (git-ignored). Without a token the bot is disabled.
// Reply keyboard buttons of the private chat, they send their label as the message text.
#define TELEGRAM_BTN_WAKE    "Wake T580"
#define TELEGRAM_BTN_STATUS  "Status"

// Daily energy report to the family chat (to the private chat while TELEGRAM_FAMILY_CHAT_ID is
// not set): yesterday's consumption at this local time, on the 1st also the previous month.
#define ENERGY_REPORT_HOUR    7
#define ENERGY_REPORT_MINUTE  0
// electricity price per kWh, used for all costs in the messages
#define ENERGY_PRICE_PER_KWH  3.59
#define ENERGY_CURRENCY       "lei"

#endif /* _AppConfig_H_ */
