#include "MqttPublisher.h"

static const char ONLINE[] = "{\"state\":\"online\"}";
static const char OFFLINE[] = "{\"state\":\"offline\"}";

// only CONNACK / PUBACK are received (no subscriptions), records are ~170 bytes
MqttPublisher::MqttPublisher() : client(64, 256) {
	logger = nullptr;
	clientId = nullptr;
	user = nullptr;
	password = nullptr;
	baseTopic = nullptr;
	availabilityTopic[0] = 0;

	lastAttempt = 0;
	retryDelay = 0;
	wasConnected = false;
}

void MqttPublisher::begin(const char *host, uint16_t port, const char *_clientId, const char *_user,
		const char *_password, const char *_baseTopic, Logger *_logger) {
	clientId = _clientId;
	user = _user;
	password = _password;
	baseTopic = _baseTopic;
	logger = _logger;

	snprintf(availabilityTopic, sizeof(availabilityTopic), "%s/availability", baseTopic);

	net.setTimeout(MQTT_TIMEOUT_MS);
	client.begin(host, port, net);
	client.setOptions(MQTT_KEEPALIVE_S, true, MQTT_TIMEOUT_MS);
	client.setWill(availabilityTopic, OFFLINE, true, 1);
}

void MqttPublisher::loop(unsigned long current_time, EnergyPeriods &tracker) {
	if (!client.connected()) {
		if (wasConnected) {
			wasConnected = false;
			JsonDocument doc;
			doc["error"] = (int) client.lastError();
			logger->error(doc, "MQTT connection lost");
		}
		connect(current_time);
		return;
	}

	client.loop();

	EnergyRecord record;
	// one record per loop() keeps the web server responsive while a backlog drains
	if (tracker.peek(record)) {
		if (publishRecord(record)) {
			tracker.pop();
		} else {
			JsonDocument doc;
			doc["error"] = (int) client.lastError();
			logger->error(doc, "MQTT publish failed");
			// drop the connection, the record stays queued and goes out after reconnect
			client.disconnect();
		}
	}
}

void MqttPublisher::connect(unsigned long current_time) {
	// retryDelay is 0 before the first attempt
	if ((current_time - lastAttempt) < retryDelay) {
		return;
	}
	lastAttempt = current_time;

	if (client.connect(clientId, user, password)) {
		retryDelay = MQTT_RETRY_MIN_MS;
		wasConnected = true;
		client.publish(availabilityTopic, ONLINE, true, 1);

		JsonDocument doc;
		logger->info(doc, "MQTT connected");
		return;
	}

	retryDelay = (retryDelay == 0) ? MQTT_RETRY_MIN_MS : min(retryDelay * 2, (unsigned long) MQTT_RETRY_MAX_MS);

	JsonDocument doc;
	doc["error"] = (int) client.lastError();
	doc["returnCode"] = (int) client.returnCode();
	doc["retryMs"] = retryDelay;
	logger->error(doc, "MQTT connect failed");
}

bool MqttPublisher::publishRecord(const EnergyRecord &record) {
	const char *period = EnergyPeriods::periodName(record.period);

	char topic[64];
	snprintf(topic, sizeof(topic), "%s/energy/%s", baseTopic, period);

	char start[32];
	char end[32];
	formatTime(record.start, start, sizeof(start));
	formatTime(record.end, end, sizeof(end));

	char payload[200];
	snprintf(payload, sizeof(payload),
			"{\"period\":\"%s\",\"start\":\"%s\",\"end\":\"%s\",\"energy_wh\":%u,\"meter_wh\":%u,\"complete\":%s}",
			period, start, end, record.energyWh, record.meterWh, record.complete ? "true" : "false");

	return client.publish(topic, payload, false, 1);
}

bool MqttPublisher::publish(const char *subtopic, const char *payload) {
	if (!client.connected()) {
		return false;
	}

	char topic[64];
	snprintf(topic, sizeof(topic), "%s/%s", baseTopic, subtopic);
	return client.publish(topic, payload, false, 1);
}

// ISO 8601 local time with UTC offset, e.g. 2026-10-18T14:00:00+03:00
void MqttPublisher::formatTime(uint32_t t, char *buf, size_t size) {
	time_t tt = t;
	struct tm tm;
	localtime_r(&tt, &tm);

	// %z gives +0300, insert the colon for strict ISO 8601 / JavaScript Date parsing
	size_t n = strftime(buf, size, "%Y-%m-%dT%H:%M:%S%z", &tm);
	if (n >= 5 && n + 1 < size) {
		buf[n + 1] = 0;
		buf[n] = buf[n - 1];
		buf[n - 1] = buf[n - 2];
		buf[n - 2] = ':';
	}
}
