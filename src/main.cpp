/*
 * D1MiniProEnergyMeterV1 - LOLIN (WEMOS) D1 mini + PZEM-004T v3.0 energy meter.
 *
 * HTTP API:
 *   GET  /        - device info page + firmware upload form
 *   GET  /data    - PZEM readings as JSON
 *   GET  /health  - {"status":"UP"}
 *   POST /update  - OTA firmware update (ESP8266HTTPUpdateServer)
 */
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>

#include "WiFiHelper.h"
#include "ServerHelper.h"
#include "Logger.h"

#if __has_include("WiFiConfig.h")
#include "WiFiConfig.h"
#elif defined(REQUIRE_WIFI_CONFIG)
#error "include/WiFiConfig.h not found - OTA firmware with placeholder credentials would never reconnect"
#else
#warning "include/WiFiConfig.h not found - building with placeholder credentials from WiFiConfig.example.h"
#include "WiFiConfig.example.h"
#endif

void indexPage();
void handleNotFound();
void setupHTTPActions();
void setupAfterWiFiConnected();

void health();
void sendSensorData();
void writeJson(int httpStatus, const JsonDocument &doc);


WiFiHelper wiFiHelper;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
ServerHelper serverHelper("D1MiniProEnergyMeterV1.1", &server);
Logger logger;

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

	// Not found handler
	server.onNotFound(handleNotFound);

	// Setup HTTP Updater
	httpUpdater.setup(&server);

	server.begin();
}

void health() {
	JsonDocument rootDoc;
	rootDoc["status"] = "UP";
	writeJson(200, rootDoc);
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
}
