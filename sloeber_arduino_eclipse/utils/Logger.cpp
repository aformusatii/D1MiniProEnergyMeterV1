#include "Logger.h"

#define UDP_DST_HOST "192.168.100.134"
#define UDP_DST_PORT 5000

#define LOG_SYSTEM "D1MiniProEnergyMeterV1"

#define LEVEL_INFO  "INFO"
#define LEVEL_ERROR "ERROR"

WiFiUDP UDP;

void Logger::info(const char *msg) {
	StaticJsonDocument<256> doc;
	send(doc, LEVEL_INFO, msg);
}

void Logger::info(JsonDocument &doc, const char *msg) {
	send(doc, LEVEL_INFO, msg);
}

void Logger::error(const char *msg) {
	StaticJsonDocument<256> doc;
	send(doc, LEVEL_ERROR, msg);
}

void Logger::error(JsonDocument &doc, const char *msg) {
	send(doc, LEVEL_ERROR, msg);
}

void Logger::send(JsonDocument &doc, const char *level, const char *msg) {
	doc["system"] = LOG_SYSTEM;
	doc["level"] = level;
	doc["message"] = msg;

	char buffer[500];
	serializeJson(doc, buffer);

	UDP.beginPacket(UDP_DST_HOST, UDP_DST_PORT);
    UDP.write(buffer);
    UDP.endPacket();

	UDP.beginPacket(UDP_DST_HOST, UDP_DST_PORT + 10);
    UDP.write(buffer);
    UDP.write("\n");
    UDP.endPacket();

    Serial.println(msg);
}

void Logger::info2(const char *msg) {
	UDP.beginPacket(UDP_DST_HOST, UDP_DST_PORT + 10);
    UDP.write(msg);
    UDP.write("\n");
    UDP.endPacket();
}


