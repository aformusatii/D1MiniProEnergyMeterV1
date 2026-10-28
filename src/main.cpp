/*
 * D1MiniProEnergyMeterV1 - LOLIN (WEMOS) D1 mini + PZEM-004T v3.0 energy meter.
 *
 * HTTP API:
 *   GET  /        - device info page + firmware upload form
 *   GET  /data    - PZEM readings as JSON
 *   GET  /health  - {"status":"UP"} + time sync / MQTT / queue status
 *   GET  /mqtt/test[?msg=text] - publishes a test message to <base>/test (for testing the broker path)
 *   POST /update  - OTA firmware update (ESP8266HTTPUpdateServer)
 *
 * MQTT (see include/AppConfig.h, docs/MQTT.md): consumed energy per local hour, day, ISO week and
 * month, published at each period end once the clock is synced over NTP.
 */
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>
#include <coredecls.h>

#include "WiFiHelper.h"
#include "ServerHelper.h"
#include "Logger.h"
#include "EnergyPeriods.h"
#include "MqttPublisher.h"

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

// any time before this means NTP has not synced yet
#define TIME_VALID_AFTER  1700000000
#define ENERGY_CHECK_INTERVAL_MS  1000

// RTC user memory survives reset and OTA reboot (not power loss). The first 128 bytes
// are used by the OTA bootloader command, so start at block 64 (byte 256).
#define RTC_STATE_OFFSET  64
#define RTC_STATE_MAGIC   0x454E5031  // "ENP1"

void indexPage();
void handleNotFound();
void setupHTTPActions();
void setupAfterWiFiConnected();

void health();
void mqttTest();
void sendSensorData();
void writeJson(int httpStatus, const JsonDocument &doc);
void energyLoop(time_t now);
void loadTrackerState();
void saveTrackerState();


WiFiHelper wiFiHelper;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
ServerHelper serverHelper("D1MiniProEnergyMeterV1.2", &server);
Logger logger;
EnergyPeriods tracker;
MqttPublisher mqtt;

struct RtcBlob {
	uint32_t magic;
	EnergyTrackerState state;
	uint32_t crc;
};

unsigned long energy_check_timer = 0;
bool timeSynced = false;

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
	mqtt.begin(MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD, MQTT_BASE_TOPIC, &logger);

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
	char output[500];
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

	time_t now = time(nullptr);
	if (now < TIME_VALID_AFTER) {
		// no internet time yet: nothing is measured per period and nothing is published
		return;
	}

	if ((current_time - energy_check_timer) >= ENERGY_CHECK_INTERVAL_MS) {
		energy_check_timer = current_time;
		energyLoop(now);
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
