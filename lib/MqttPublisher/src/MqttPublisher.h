#ifndef _MqttPublisher_H_
#define _MqttPublisher_H_

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <MQTT.h>

#include "EnergyPeriods.h"
#include "Logger.h"

#define MQTT_RETRY_MIN_MS      2000
#define MQTT_RETRY_MAX_MS    120000
#define MQTT_KEEPALIVE_S         30
#define MQTT_TIMEOUT_MS        2000

/*
 * Keeps an MQTT connection and drains the EnergyPeriods queue with QoS 1.
 * A record leaves the queue only after the broker acknowledged it (PUBACK).
 * Connection failures back off exponentially, so a dead broker costs at most one
 * short blocking connect attempt every MQTT_RETRY_MAX_MS.
 *
 * Topics:
 *   <base>/availability     {"state":"online"|"offline"}  retained, offline via last will
 *   <base>/energy/<period>  one JSON record per finished period (hour, day, week, month)
 *   <base>/test             test message on demand (GET /mqtt/test), not retained
 */
class MqttPublisher {

	private:
		WiFiClient net;
		MQTTClient client;
		Logger *logger;

		const char *clientId;
		const char *user;
		const char *password;
		const char *baseTopic;
		char availabilityTopic[64];

		unsigned long lastAttempt;
		unsigned long retryDelay;
		bool wasConnected;

		void connect(unsigned long current_time);
		bool publishRecord(const EnergyRecord &record);

	public:
		MqttPublisher(void);

		void begin(const char *host, uint16_t port, const char *clientId, const char *user,
				const char *password, const char *baseTopic, Logger *logger);

		// Call from loop() while WiFi is connected and the clock is synced
		void loop(unsigned long current_time, EnergyPeriods &tracker);

		bool connected(void) { return client.connected(); }

		// Publishes payload to <base>/<subtopic> with QoS 1, not retained.
		// Returns false when not connected or not acknowledged.
		bool publish(const char *subtopic, const char *payload);
		int lastError(void) { return (int) client.lastError(); }

		static void formatTime(uint32_t t, char *buf, size_t size);
};

#endif /* _MqttPublisher_H_ */
