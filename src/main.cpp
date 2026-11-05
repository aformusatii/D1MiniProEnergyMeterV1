/*
 * D1MiniProEnergyMeterV1 - LOLIN (WEMOS) D1 mini + PZEM-004T v3.0 energy meter.
 *
 * HTTP API:
 *   GET  /        - device info page + firmware upload form
 *   GET  /data    - PZEM readings as JSON
 *   GET  /health  - {"status":"UP"} + time sync / MQTT / queue status
 *   GET  /mqtt/test[?msg=text] - publishes a test message to <base>/test (for testing the broker path)
 *   GET  /wol     - Wake-on-LAN target and whether it is up
 *   POST /wol     - sends the Wake-on-LAN magic packet to the target
 *   GET  /report  - preview of the daily energy report
 *   POST /report[?to=admin] - sends the daily energy report now (family chat, or the private chat)
 *   POST /update  - OTA firmware update (ESP8266HTTPUpdateServer)
 *
 * MQTT (see include/AppConfig.h, docs/MQTT.md): consumed energy per local hour, day, ISO week and
 * month, published at each period end once the clock is synced over NTP.
 *
 * Telegram bot (token and chat ids in include/WiFiConfig.h), one bot in two chats:
 *   private chat (TELEGRAM_CHAT_ID): /wol wakes the ThinkPad T580 and reports when it is up,
 *     /status shows whether it is up plus mains voltage and power
 *   family group (TELEGRAM_FAMILY_CHAT_ID): yesterday's consumption and cost every day at 07:00,
 *     on the 1st also the previous month
 *   both: /today, /yesterday, /month
 */
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiUdp.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>
#include <coredecls.h>
#include <EEPROM.h>

#include "WiFiHelper.h"
#include "ServerHelper.h"
#include "Logger.h"
#include "EnergyPeriods.h"
#include "EnergyReport.h"
#include "MqttPublisher.h"
#include "TelegramBot.h"
#include "WakeOnLan.h"

#if __has_include("WiFiConfig.h")
#include "WiFiConfig.h"
#elif defined(REQUIRE_WIFI_CONFIG)
#error "include/WiFiConfig.h not found - OTA firmware with placeholder credentials would never reconnect"
#else
#warning "include/WiFiConfig.h not found - building with placeholder credentials from WiFiConfig.example.h"
#include "WiFiConfig.example.h"
#endif

#include "AppConfig.h"

#ifndef MQTT_USER
#define MQTT_USER nullptr
#define MQTT_PASSWORD nullptr
#endif

#ifndef TELEGRAM_BOT_TOKEN
#define TELEGRAM_BOT_TOKEN nullptr
#define TELEGRAM_CHAT_ID   0
#endif

#ifndef TELEGRAM_FAMILY_CHAT_ID
#define TELEGRAM_FAMILY_CHAT_ID  0
#endif

#define WOL_UDP_PORT          9
#define WOL_REPEAT            3
#define WOL_CHECK_TIMEOUT_MS  1500
// after /wol from Telegram, check this often whether the target came up, and for how long
#define WOL_FOLLOWUP_INTERVAL_MS   20000
#define WOL_FOLLOWUP_WINDOW_MS    300000

// any time before this means NTP has not synced yet
#define TIME_VALID_AFTER  1700000000
#define ENERGY_CHECK_INTERVAL_MS  1000

// RTC user memory survives reset and OTA reboot (not power loss). The first 128 bytes
// are used by the OTA bootloader command, so start at block 64 (byte 256).
#define RTC_STATE_OFFSET  64
#define RTC_STATE_MAGIC   0x454E5031  // "ENP1"

// The flash copy (EEPROM sector at 0x3FB000, above the OTA staging area) survives a power cut:
// day/week/month in progress and the report history. Written ~2 times a day.
#define FLASH_STATE_MAGIC 0x454E5232  // "ENR2"

void indexPage();
void handleNotFound();
void setupHTTPActions();
void setupAfterWiFiConnected();

void health();
void mqttTest();
void sendSensorData();
void writeJson(int httpStatus, const JsonDocument &doc);
void energyLoop(time_t now);
void onEnergyRecord(const EnergyRecord &record);
void loadTrackerState();
void saveTrackerState();
void loadFlashState();
void saveFlashState();

void reportPreview();
void reportSend();
void reportLoop(time_t now);

void wolStatus();
void wolWake();
bool sendWakeOnLan();
bool targetOnline();
void telegramCommand(int64_t chatId, const char *text);
bool energyCommand(int64_t chatId, const char *command);
void adminCommand(const char *command);
void telegramWake();
void telegramStatus();
void wolFollowupLoop(unsigned long current_time);


WiFiHelper wiFiHelper;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
ServerHelper serverHelper("D1MiniProEnergyMeterV1.4", &server);
Logger logger;
EnergyPeriods tracker;
EnergyReport report(ENERGY_PRICE_PER_KWH, ENERGY_CURRENCY, ENERGY_REPORT_HOUR, ENERGY_REPORT_MINUTE);
MqttPublisher mqtt;
TelegramBot telegram;
WiFiUDP wolUdp;

struct RtcBlob {
	uint32_t magic;
	EnergyTrackerState state;
	uint32_t crc;
};

struct FlashBlob {
	uint32_t magic;
	EnergyTrackerState tracker;
	EnergyReportState report;
	uint32_t crc;
};

unsigned long energy_check_timer = 0;
bool timeSynced = false;
// the day in progress as of the last flash write, and new report history since
uint32_t flashDayStart = 0;
bool flashDirty = false;

// One bot, two chats: Wake-on-LAN only in the private (admin) chat, energy in both.
// The reports go to the family group, or to the private chat until the group is set up.
static const int64_t ADMIN_CHAT = TELEGRAM_CHAT_ID;
static const int64_t FAMILY_CHAT = TELEGRAM_FAMILY_CHAT_ID;
static const int64_t REPORT_CHAT = (FAMILY_CHAT != 0) ? FAMILY_CHAT : ADMIN_CHAT;

uint8_t wolMac[WOL_MAC_LEN];
bool wolMacValid = false;
bool wolFollowup = false;
unsigned long wolSentAt = 0;
unsigned long wolFollowupTimer = 0;

static const char TELEGRAM_KEYBOARD[] =
		"{\"keyboard\":[[\"" TELEGRAM_BTN_WAKE "\",\"" TELEGRAM_BTN_STATUS "\"]],\"resize_keyboard\":true,\"is_persistent\":true}";

#define ENERGY_COMMANDS_HELP \
		"/today - energy used today so far\n" \
		"/yesterday - yesterday's consumption\n" \
		"/month - this month so far, forecast and last month"

static const char TELEGRAM_HELP[] =
		"Commands:\n"
		"/wol - wake the " WOL_TARGET_NAME " (Wake-on-LAN)\n"
		"/status - is the " WOL_TARGET_NAME " up, mains voltage and power, device uptime\n"
		ENERGY_COMMANDS_HELP;

// in a group the bot only sees commands (privacy mode), so the buttons are commands too
static const char FAMILY_KEYBOARD[] =
		"{\"keyboard\":[[\"/today\",\"/yesterday\",\"/month\"]],\"resize_keyboard\":true}";

static const char FAMILY_HELP[] =
		"Home energy meter. Every morning I post yesterday's consumption and cost here, "
		"on the 1st also the total for the previous month.\n"
		ENERGY_COMMANDS_HELP;

// PZEM is wired to a software serial port, the hardware UART (USB) stays free for debug output
#define PZEM_RX_PIN D5
#define PZEM_TX_PIN D6

SoftwareSerial pzemSWSerial(PZEM_RX_PIN, PZEM_TX_PIN);
PZEM004Tv30 pzem(pzemSWSerial);

//The setup function is called once at startup of the sketch
void setup()
{
	Serial.begin(115200);

	// Configures static IP address
	// https://docs.google.com/spreadsheets/d/15TZZKE4YDxZscbjAQP_kMxA631p_ueJwKnBA6VqtsIE/edit#gid=0
	if (!wiFiHelper.configureStaticIp(192, 168, 100, 21)) {
		logger.error("STA Failed to configure");
	}

	wiFiHelper.begin(LOCAL2_SSID, LOCAL2_KEY, setupAfterWiFiConnected);
	wiFiHelper.setupLedStatus(LED_BUILTIN);

	// SNTP starts by itself once WiFi is up and re-syncs periodically
	configTime(TIME_ZONE, NTP_SERVER_1, NTP_SERVER_2);

	loadTrackerState();
	loadFlashState();
	tracker.setListener(onEnergyRecord);
	mqtt.begin(MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD, MQTT_BASE_TOPIC, &logger);

	wolMacValid = WakeOnLan::parseMac(WOL_TARGET_MAC, wolMac);

	telegram.begin(TELEGRAM_BOT_TOKEN, &logger, telegramCommand);
	telegram.addChat(ADMIN_CHAT);
	if (FAMILY_CHAT != ADMIN_CHAT) {
		telegram.addChat(FAMILY_CHAT);
	}

	// "Power On" here means the mains came back, the target may need a /wol
	char started[160];
	snprintf(started, sizeof(started), "D1MiniProEnergyMeterV1 started (reset reason: %s).\nSend /wol to wake the " WOL_TARGET_NAME ".",
			ESP.getResetReason().c_str());
	telegram.send(ADMIN_CHAT, started, TELEGRAM_KEYBOARD);

	Serial.println("Started D1MiniProEnergyMeterV1!");
}

/* ------------------------------------------------------------------------- */
/*  HTTP Handlers  */
void indexPage() {
	serverHelper.indexPage();
}

void handleNotFound() {
	serverHelper.handleNotFound();
}

void setupHTTPActions() {
	server.on("/", HTTP_GET, indexPage);

	server.on("/data", HTTP_GET, sendSensorData);
	server.on("/health", HTTP_GET, health);
	server.on("/mqtt/test", HTTP_ANY, mqttTest);
	server.on("/wol", HTTP_GET, wolStatus);
	server.on("/wol", HTTP_POST, wolWake);
	server.on("/report", HTTP_GET, reportPreview);
	server.on("/report", HTTP_POST, reportSend);

	// Not found handler
	server.onNotFound(handleNotFound);

	// Setup HTTP Updater
	httpUpdater.setup(&server);

	server.begin();
}

void health() {
	JsonDocument rootDoc;
	rootDoc["status"] = "UP";
	rootDoc["time_synced"] = timeSynced;
	rootDoc["mqtt"] = mqtt.connected();
	rootDoc["queued"] = tracker.queued();
	rootDoc["dropped"] = tracker.dropped();
	rootDoc["heap"] = ESP.getFreeHeap();
	rootDoc["telegram"] = telegram.ok();
	rootDoc["telegram_error"] = telegram.lastError();
	rootDoc["telegram_unknown_chat"] = telegram.lastUnknownChat();
	rootDoc["report_due"] = report.due(time(nullptr));
	writeJson(200, rootDoc);
}

// longer messages are cut so the payload fits the 256 byte MQTT write buffer
#define MQTT_TEST_MSG_MAX  100

void mqttTest() {
	char now[32];
	MqttPublisher::formatTime((uint32_t) time(nullptr), now, sizeof(now));

	JsonDocument msg;
	msg["test"] = true;
	msg["time"] = now;
	msg["time_synced"] = timeSynced;
	msg["uptime_ms"] = millis();
	msg["heap"] = ESP.getFreeHeap();
	if (server.hasArg("msg")) {
		msg["msg"] = server.arg("msg").substring(0, MQTT_TEST_MSG_MAX);
	}

	char payload[230];
	serializeJson(msg, payload, sizeof(payload));

	JsonDocument rootDoc;
	rootDoc["topic"] = MQTT_BASE_TOPIC "/test";
	rootDoc["payload"] = payload;

	int status = 200;
	if (!mqtt.connected()) {
		// the connection is only opened once NTP has synced
		status = 503;
		rootDoc["success"] = false;
		rootDoc["error"] = timeSynced ? "MQTT not connected" : "MQTT not connected (waiting for NTP time)";
	} else if (!mqtt.publish("test", payload)) {
		status = 502;
		rootDoc["success"] = false;
		rootDoc["error"] = "MQTT publish failed";
		rootDoc["mqtt_error"] = mqtt.lastError();
	} else {
		rootDoc["success"] = true;
	}

	writeJson(status, rootDoc);

	JsonDocument logDoc;
	logDoc["success"] = rootDoc["success"];
	logger.info(logDoc, "MQTT test publish");
}

void sendSensorData() {
	JsonDocument rootDoc;

	// Read the data from the sensor
	float voltage = pzem.voltage();
	float current = pzem.current();
	float power = pzem.power();
	float energy = pzem.energy();
	float frequency = pzem.frequency();
	float pf = pzem.pf();

	rootDoc["success"] = false;

	// Check if the data is valid
	if (isnan(voltage)) {
		rootDoc["error"] = "Error reading voltage";
	} else if (isnan(current)) {
		rootDoc["error"] = "Error reading current";
	} else if (isnan(power)) {
		rootDoc["error"] = "Error reading power";
	} else if (isnan(energy)) {
		rootDoc["error"] = "Error reading energy";
	} else if (isnan(frequency)) {
		rootDoc["error"] = "Error reading frequency";
	} else if (isnan(pf)) {
		rootDoc["error"] = "Error reading power factor";
	} else {
		rootDoc["success"] = true;
		rootDoc["voltage"] = voltage;
		rootDoc["current"] = current;
		rootDoc["power"] = power;
		rootDoc["energy"] = energy;
		rootDoc["frequency"] = frequency;
		rootDoc["pf"] = pf;
	}

	writeJson(200, rootDoc);
}

void writeJson(int httpStatus, const JsonDocument &doc) {
	// the report preview is longer than a fixed buffer would hold
	String output;
	output.reserve(measureJson(doc) + 1);
	serializeJson(doc, output);
	server.send(httpStatus, "application/json", output);
}

void setupAfterWiFiConnected() {
	JsonDocument rootDoc;
	rootDoc["IP"] = WiFi.localIP().toString();
	logger.info(rootDoc, "Connected to WiFi");

	Serial.println(WiFi.localIP());

	setupHTTPActions();
}

// The loop function is called in an endless loop
void loop()
{
	unsigned long current_time = millis();

	wiFiHelper.loop(current_time);

	if (wiFiHelper.wiFiOk) {
		server.handleClient();
	}

	// the bot does not need NTP time, it must work even when the clock is not synced
	if (wiFiHelper.wiFiOk && WiFi.status() == WL_CONNECTED) {
		telegram.loop(current_time);
		wolFollowupLoop(current_time);
	}

	time_t now = time(nullptr);
	if (now < TIME_VALID_AFTER) {
		// no internet time yet: nothing is measured per period and nothing is published
		return;
	}

	if ((current_time - energy_check_timer) >= ENERGY_CHECK_INTERVAL_MS) {
		energy_check_timer = current_time;
		energyLoop(now);
		reportLoop(now);
	}

	if (wiFiHelper.wiFiOk && WiFi.status() == WL_CONNECTED) {
		mqtt.loop(current_time, tracker);
	}
}

/* ------------------------------------------------------------------------- */
/*  Energy per period  */
void energyLoop(time_t now) {
	if (!timeSynced) {
		timeSynced = true;
		JsonDocument doc;
		doc["time"] = (uint32_t) now;
		logger.info(doc, "Time synced");
	}

	// the clock keeps running without WiFi, so boundaries are still caught and queued offline
	if (!tracker.needsReading(now)) {
		return;
	}

	float energy = pzem.energy();  // kWh, 1 Wh resolution
	if (isnan(energy)) {
		// retried on the next check
		return;
	}

	uint8_t queuedBefore = tracker.queued();
	if (tracker.update(now, (uint32_t) lroundf(energy * 1000.0f))) {
		saveTrackerState();

		JsonDocument doc;
		doc["queued"] = tracker.queued();
		doc["new"] = tracker.queued() - queuedBefore;
		logger.info(doc, "Energy period boundary");
	}

	// once a day (and right after the first reading), not every hour: flash sectors wear out
	if (flashDirty || tracker.state.periods[PERIOD_DAY].start != flashDayStart) {
		saveFlashState();
	}
}

// every finished period, called from tracker.update()
void onEnergyRecord(const EnergyRecord &record) {
	if (report.add(record)) {
		flashDirty = true;
	}
}

void loadTrackerState() {
	RtcBlob blob;
	ESP.rtcUserMemoryRead(RTC_STATE_OFFSET, (uint32_t *) &blob, sizeof(blob));

	if (blob.magic == RTC_STATE_MAGIC && blob.crc == crc32(&blob.state, sizeof(blob.state))) {
		tracker.state = blob.state;
	}
}

void saveTrackerState() {
	RtcBlob blob;
	blob.magic = RTC_STATE_MAGIC;
	blob.state = tracker.state;
	blob.crc = crc32(&blob.state, sizeof(blob.state));
	ESP.rtcUserMemoryWrite(RTC_STATE_OFFSET, (uint32_t *) &blob, sizeof(blob));
}

// after loadTrackerState(): RTC memory wins (it has the hour too), the flash copy covers a power cut
void loadFlashState() {
	FlashBlob blob;
	EEPROM.begin(sizeof(blob));
	EEPROM.get(0, blob);
	EEPROM.end();

	if (blob.magic != FLASH_STATE_MAGIC || blob.crc != crc32(&blob.tracker, sizeof(blob.tracker) + sizeof(blob.report))) {
		logger.info("No energy state in flash");
		return;
	}

	report.state = blob.report;
	flashDayStart = blob.tracker.periods[PERIOD_DAY].start;

	JsonDocument doc;
	doc["days"] = report.state.dayCount;
	doc["months"] = report.state.monthCount;
	doc["resumed"] = !tracker.state.valid && blob.tracker.valid;
	if (!tracker.state.valid && blob.tracker.valid) {
		// power cut: the PZEM counter kept its value and nothing was consumed while the mains was
		// off, so day/week/month simply continue. The hour starts over (the copy is from midnight).
		tracker.state.periods[PERIOD_DAY] = blob.tracker.periods[PERIOD_DAY];
		tracker.state.periods[PERIOD_WEEK] = blob.tracker.periods[PERIOD_WEEK];
		tracker.state.periods[PERIOD_MONTH] = blob.tracker.periods[PERIOD_MONTH];
		tracker.state.valid = 1;
	}
	logger.info(doc, "Energy state loaded from flash");
}

void saveFlashState() {
	FlashBlob blob;
	blob.magic = FLASH_STATE_MAGIC;
	blob.tracker = tracker.state;
	blob.report = report.state;
	blob.crc = crc32(&blob.tracker, sizeof(blob.tracker) + sizeof(blob.report));

	EEPROM.begin(sizeof(blob));
	EEPROM.put(0, blob);
	bool ok = EEPROM.commit();
	EEPROM.end();

	flashDayStart = tracker.state.periods[PERIOD_DAY].start;
	flashDirty = false;

	JsonDocument doc;
	doc["ok"] = ok;
	logger.info(doc, "Energy state saved to flash");
}

/* ------------------------------------------------------------------------- */
/*  Daily energy report  */
void reportLoop(time_t now) {
	if (!telegram.enabled() || !report.due(now)) {
		return;
	}

	char msg[TELEGRAM_MSG_MAX];
	report.daily(msg, sizeof(msg), now, tracker.state.periods[PERIOD_MONTH]);
	if (!telegram.send(REPORT_CHAT, msg)) {
		// a reply is waiting to go out, try again on the next check
		return;
	}

	report.markReported();
	saveFlashState();

	JsonDocument doc;
	doc["chat_id"] = REPORT_CHAT;
	doc["day"] = report.state.reportedDayEnd;
	logger.info(doc, "Daily energy report queued");
}

void reportPreview() {
	time_t now = time(nullptr);
	char msg[TELEGRAM_MSG_MAX];

	JsonDocument rootDoc;
	rootDoc["chat_id"] = REPORT_CHAT;
	rootDoc["family_chat"] = FAMILY_CHAT != 0;
	rootDoc["due"] = report.due(now);
	rootDoc["days"] = report.state.dayCount;
	rootDoc["months"] = report.state.monthCount;
	if (report.daily(msg, sizeof(msg), now, tracker.state.periods[PERIOD_MONTH]) > 0) {
		rootDoc["text"] = (const char *) msg;
	} else {
		rootDoc["text"] = nullptr;
		rootDoc["error"] = "No finished day yet, the first one is ready after midnight";
	}
	writeJson(200, rootDoc);
}

// sends the preview without marking it as reported, the 07:00 message still goes out
void reportSend() {
	time_t now = time(nullptr);
	int64_t chat = (server.arg("to") == "admin") ? ADMIN_CHAT : REPORT_CHAT;
	char msg[TELEGRAM_MSG_MAX];

	JsonDocument rootDoc;
	rootDoc["chat_id"] = chat;
	int status = 200;
	if (report.daily(msg, sizeof(msg), now, tracker.state.periods[PERIOD_MONTH]) == 0) {
		status = 409;
		rootDoc["success"] = false;
		rootDoc["error"] = "No finished day yet, the first one is ready after midnight";
	} else if (!telegram.send(chat, msg)) {
		status = 503;
		rootDoc["success"] = false;
		rootDoc["error"] = telegram.enabled() ? "Another Telegram message is waiting, try again" : "Telegram bot disabled";
	} else {
		rootDoc["success"] = true;
	}
	writeJson(status, rootDoc);
}

/* ------------------------------------------------------------------------- */
/*  Wake-on-LAN  */
void wolStatus() {
	JsonDocument rootDoc;
	rootDoc["target"] = WOL_TARGET_NAME;
	rootDoc["mac"] = WOL_TARGET_MAC;
	rootDoc["ip"] = WOL_TARGET_IP;
	rootDoc["online"] = targetOnline();
	writeJson(200, rootDoc);
}

void wolWake() {
	JsonDocument rootDoc;
	rootDoc["target"] = WOL_TARGET_NAME;
	rootDoc["mac"] = WOL_TARGET_MAC;
	rootDoc["broadcast"] = WiFi.broadcastIP().toString();
	rootDoc["success"] = sendWakeOnLan();
	writeJson(rootDoc["success"] ? 200 : 500, rootDoc);
}

bool sendWakeOnLan() {
	if (!wolMacValid) {
		logger.error("Wake-on-LAN: invalid WOL_TARGET_MAC");
		return false;
	}

	uint8_t packet[WOL_PACKET_LEN];
	WakeOnLan::buildPacket(wolMac, packet);

	// the target is off, so it cannot answer ARP: only a broadcast reaches it
	IPAddress broadcast = WiFi.broadcastIP();
	int sent = 0;
	for (int i = 0; i < WOL_REPEAT; i++) {
		if (wolUdp.beginPacket(broadcast, WOL_UDP_PORT)
				&& wolUdp.write(packet, sizeof(packet)) == sizeof(packet)
				&& wolUdp.endPacket()) {
			sent++;
		}
	}

	JsonDocument doc;
	doc["mac"] = WOL_TARGET_MAC;
	doc["broadcast"] = broadcast.toString();
	doc["sent"] = sent;
	logger.info(doc, "Wake-on-LAN magic packet");

	return sent > 0;
}

// blocks for up to WOL_CHECK_TIMEOUT_MS when the target is down
bool targetOnline() {
	IPAddress ip;
	ip.fromString(WOL_TARGET_IP);

	WiFiClient probe;
	probe.setTimeout(WOL_CHECK_TIMEOUT_MS);
	bool up = probe.connect(ip, WOL_CHECK_PORT) == 1;
	probe.stop();
	return up;
}

/* ------------------------------------------------------------------------- */
/*  Telegram commands  */
// only allowed chats get here (TelegramBot drops the rest)
void telegramCommand(int64_t chatId, const char *text) {
	char command[TELEGRAM_TEXT_MAX];
	strlcpy(command, text, sizeof(command));

	// "/wol@SomeBot" when used in a group
	char *at = strchr(command, '@');
	if (command[0] == '/' && at != nullptr) {
		*at = 0;
	}

	if (energyCommand(chatId, command)) {
		return;
	}

	if (chatId == ADMIN_CHAT) {
		adminCommand(command);
	} else if (command[0] == '/') {
		// Wake-on-LAN stays in the private chat; also the answer to /start. Other group
		// messages (chatter, "X joined") get no answer.
		telegram.send(chatId, FAMILY_HELP, FAMILY_KEYBOARD);
	}
}

// /today, /yesterday, /month in any allowed chat, the answer goes back to that chat
bool energyCommand(int64_t chatId, const char *command) {
	bool today = strcasecmp(command, "/today") == 0;
	bool yesterday = strcasecmp(command, "/yesterday") == 0;
	bool month = strcasecmp(command, "/month") == 0;
	if (!today && !yesterday && !month) {
		return false;
	}

	time_t now = time(nullptr);
	char msg[TELEGRAM_MSG_MAX];
	if (now < TIME_VALID_AFTER) {
		strlcpy(msg, "The clock is not synced yet, try again in a minute.", sizeof(msg));
	} else if (yesterday) {
		report.yesterday(msg, sizeof(msg), now);
	} else {
		float energy = pzem.energy();
		if (isnan(energy)) {
			strlcpy(msg, "The energy meter (PZEM) does not answer, try again later.", sizeof(msg));
		} else if (today) {
			report.today(msg, sizeof(msg), now, (uint32_t) lroundf(energy * 1000.0f), pzem.power(),
					tracker.state.periods[PERIOD_DAY]);
		} else {
			report.month(msg, sizeof(msg), now, (uint32_t) lroundf(energy * 1000.0f), tracker.state.periods[PERIOD_MONTH]);
		}
	}

	telegram.send(chatId, msg);
	return true;
}

void adminCommand(const char *command) {
	if (strcasecmp(command, "/wol") == 0 || strcasecmp(command, TELEGRAM_BTN_WAKE) == 0) {
		telegramWake();
	} else if (strcasecmp(command, "/status") == 0 || strcasecmp(command, TELEGRAM_BTN_STATUS) == 0) {
		telegramStatus();
	} else {
		// also the answer to /start
		telegram.send(ADMIN_CHAT, TELEGRAM_HELP, TELEGRAM_KEYBOARD);
	}
}

void telegramWake() {
	if (targetOnline()) {
		telegram.send(ADMIN_CHAT, WOL_TARGET_NAME " is already up (SSH answers).");
		return;
	}

	if (!sendWakeOnLan()) {
		telegram.send(ADMIN_CHAT, "Sending the magic packet failed, see the logs.");
		return;
	}

	wolFollowup = true;
	wolSentAt = millis();
	wolFollowupTimer = wolSentAt;

	char msg[200];
	snprintf(msg, sizeof(msg), "Magic packet sent to " WOL_TARGET_NAME " (" WOL_TARGET_MAC "). "
			"I will tell you when it is up (watching for %lu min).", WOL_FOLLOWUP_WINDOW_MS / 60000UL);
	telegram.send(ADMIN_CHAT, msg);
}

void telegramStatus() {
	bool up = targetOnline();

	float voltage = pzem.voltage();
	float power = pzem.power();
	char mains[48];
	if (isnan(voltage) || isnan(power)) {
		strlcpy(mains, "PZEM not answering", sizeof(mains));
	} else {
		snprintf(mains, sizeof(mains), "%.1f V, %.0f W", voltage, power);
	}

	unsigned long s = millis() / 1000;
	char msg[300];
	snprintf(msg, sizeof(msg), WOL_TARGET_NAME ": %s%s\nMains: %s\nUptime: %lud %02lu:%02lu, heap %u B, WiFi %d dBm",
			up ? "up (SSH answers)" : "down (no answer on SSH)",
			(!up && wolFollowup) ? ", waiting for it after /wol" : "",
			mains, s / 86400, (s / 3600) % 24, (s / 60) % 60, ESP.getFreeHeap(), WiFi.RSSI());
	telegram.send(ADMIN_CHAT, msg);
}

void wolFollowupLoop(unsigned long current_time) {
	if (!wolFollowup || (current_time - wolFollowupTimer) < WOL_FOLLOWUP_INTERVAL_MS) {
		return;
	}
	wolFollowupTimer = current_time;

	unsigned long elapsed = current_time - wolSentAt;
	char msg[200];
	if (targetOnline()) {
		snprintf(msg, sizeof(msg), WOL_TARGET_NAME " is up, %lu s after the magic packet.", elapsed / 1000);
	} else if (elapsed >= WOL_FOLLOWUP_WINDOW_MS) {
		snprintf(msg, sizeof(msg), WOL_TARGET_NAME " still does not answer %lu min after the magic packet. "
				"Try /wol again, or check Wake-on-LAN in the BIOS.", elapsed / 60000);
	} else {
		return;
	}

	// if a reply is still waiting to go out, try again on the next check
	if (telegram.send(ADMIN_CHAT, msg)) {
		wolFollowup = false;
	}
}
